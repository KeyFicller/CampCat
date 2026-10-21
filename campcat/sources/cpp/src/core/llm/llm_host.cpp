#include "core/llm/llm_host.h"

#include "core/automation_log.h"
#include "core/llm/llm_memory.h"
#include "core/llm/llm_protocol.h"

#include <chrono>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace campcat {

namespace {

constexpr int k_poll_slice_ms = 100;
constexpr char k_default_script_rel[] = "campcat/sources/py/main.py";
constexpr char k_venv_python_rel[] = ".venv/bin/python3";

/// Generous enough to cover a cold Python + LangChain import.
constexpr int k_startup_timeout_ms = 60000;
/// The whole exchange, tool calls included, so this has to cover every round
/// the sidecar allows (`MAX_MODEL_ROUNDS` in graph.py). Too short and a long
/// but healthy turn dies as "sidecar timed out" rather than finishing.
constexpr int k_request_timeout_ms = 86400000;

/// How many of the turn's newest screenshots keep their bytes. Past this the
/// oldest is dropped and its timeline step renders as unavailable.
constexpr int k_event_image_stash = 32;

/// The interpreter and script are found by convention, never configured.
void resolve_sidecar_paths(const std::filesystem::path &_repo_root,
                           std::filesystem::path *_python,
                           std::filesystem::path *_script) {
    const std::filesystem::path venv = _repo_root / k_venv_python_rel;
    std::error_code ec;
    *_python = std::filesystem::exists(venv, ec)
                   ? venv
                   : std::filesystem::path("python3");
    *_script = _repo_root / k_default_script_rel;
}

/// Collapses a run of blank lines to one. The Markdown renderer spends a full
/// empty text row on each blank line, so a payload that arrives with a run of
/// them opens a hole in the bubble.
std::string collapse_blank_lines(const std::string &_text) {
    std::string out;
    bool previous_blank = false;
    for (std::size_t pos = 0; pos < _text.size();) {
        const std::size_t newline = _text.find('\n', pos);
        const std::size_t stop =
            newline == std::string::npos ? _text.size() : newline;
        const std::string line = _text.substr(pos, stop - pos);
        // Whitespace-only counts as blank: it renders as an empty row too.
        const bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
        if (!blank || !previous_blank) {
            out += line;
            if (newline != std::string::npos) {
                out += '\n';
            }
        }
        previous_blank = blank;
        pos = stop + 1;
    }
    return out;
}

} // namespace

std::string tool_note(const std::string &_name, const char *_status,
                      const std::string &_body) {
    return collapse_blank_lines(
        "**" + _name + "** " + _status +
        (_body.empty() ? std::string() : "\n\n" + _body));
}

llm_host::~llm_host() { stop(); }

#ifndef _WIN32

void llm_host::handle_line(const std::string &_line) {
    llm_protocol::message msg;
    if (!llm_protocol::parse_line(_line, &msg)) {
        return;
    }

    switch (msg.type) {
    case llm_protocol::message::kind::ready: {
        std::lock_guard<std::mutex> lk(m_mu);
        if (msg.protocol != llm_protocol::k_protocol_version) {
            // Reported instead of ignored: an older sidecar silently knows
            // nothing about tool calls, and a newer one may expect fields we do
            // not send.
            m_protocol_mismatch =
                "sidecar speaks protocol " + std::to_string(msg.protocol) +
                ", host speaks " +
                std::to_string(llm_protocol::k_protocol_version);
        }
        m_ready = true;
    }
        m_cv.notify_all();
        break;
    case llm_protocol::message::kind::log:
        automation_log::emit("[llm] " + msg.log_message);
        break;
    case llm_protocol::message::kind::note:
        record_note(msg.text);
        break;
    case llm_protocol::message::kind::result: {
        std::lock_guard<std::mutex> lk(m_mu);
        if (msg.id == m_pending_id) {
            m_result.ok = msg.ok;
            m_result.interrupted = msg.interrupted;
            m_result.text = msg.text;
            m_result.error = msg.error;
            m_result.turns = msg.turns;
            m_has_result = true;
            llm_protocol::apply_usage(&m_usage, msg);
        }
    }
        m_cv.notify_all();
        break;
    case llm_protocol::message::kind::chunk: {
        // Deliberately no notify: the UI polls per frame, so waking the waiter
        // on every delta would only churn.
        std::lock_guard<std::mutex> lk(m_mu);
        if (msg.id == m_pending_id) {
            llm_events_append_text(m_events, msg.text);
            m_thinking = m_thinking || msg.thinking;
        }
    } break;
    case llm_protocol::message::kind::consult_result: {
        std::lock_guard<std::mutex> lk(m_mu);
        if (msg.id == m_consult_pending_id) {
            m_consult_ok = msg.ok;
            m_consult_answer = msg.answer;
            m_consult_error = msg.error;
            m_has_consult = true;
        }
    }
        m_cv.notify_all();
        break;
    case llm_protocol::message::kind::tool_call: {
        // Queued, not executed: this is the reader thread, and running the tool
        // here would block the very pipe that delivers its result. The waiter
        // in `exchange` picks it up and runs it off the lock.
        std::lock_guard<std::mutex> lk(m_mu);
        if (msg.id == m_pending_id) {
            m_tool_call = msg;
            m_has_tool = true;
            llm_protocol::apply_usage(&m_usage, msg);
        }
    }
        m_cv.notify_all();
        break;
    case llm_protocol::message::kind::unknown:
    default:
        break;
    }
}

void llm_events_append_text(std::vector<llm_event> &_events,
                            std::string_view _delta) {
    if (_delta.empty()) {
        return;
    }
    if (_events.empty() || _events.back().type != llm_event::kind::text) {
        llm_event e;
        e.type = llm_event::kind::text;
        _events.push_back(std::move(e));
    }
    _events.back().text.append(_delta);
}

std::vector<llm_event> llm_host::events() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_events;
}

std::string llm_host::event_image_png(long _image_id) const {
    std::lock_guard<std::mutex> lk(m_mu);
    for (const auto &entry : m_event_images) {
        if (entry.first == _image_id) {
            return entry.second;
        }
    }
    return {};
}

bool llm_host::is_thinking() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_thinking;
}

void llm_host::clear_stream() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_events.clear();
    m_event_images.clear();
    m_thinking = false;
}

void llm_host::pump(int _fd, bool _is_stderr) {
    if (_fd < 0) {
        return;
    }
    std::string buffer;
    while (true) {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_stopping) {
                return;
            }
        }
        pollfd pfd{};
        pfd.fd = _fd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, k_poll_slice_ms);
        if (pr < 0) {
            break;
        }
        if (pr == 0) {
            continue;
        }
        char chunk[65536];
        const ssize_t n = read(_fd, chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
        for (const std::string &line : process::take_lines(&buffer)) {
            if (_is_stderr) {
                if (!line.empty()) {
                    automation_log::emit("[llm][py] " + line);
                }
            } else {
                handle_line(line);
            }
        }
    }

    // EOF on stdout: the child is gone or closed it. Fail any in-flight
    // request.
    if (!_is_stderr) {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_broken = true;
        }
        m_cv.notify_all();
    }
}

bool llm_host::write_all(const std::string &_data, std::string *_error_out) {
    // One whole line at a time: a script's `ask` and a turn can write from two
    // threads, and interleaved halves of two lines break the stream for both.
    std::lock_guard<std::mutex> write_lk(m_write_mu);
    const int fd = m_child.in();
    if (fd < 0) {
        if (_error_out) {
            *_error_out = "sidecar stdin is closed";
        }
        return false;
    }
    std::size_t written = 0;
    while (written < _data.size()) {
        const ssize_t n =
            write(fd, _data.data() + written, _data.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR)) {
            continue;
        }
        if (_error_out) {
            *_error_out = "failed to write to the sidecar";
        }
        return false;
    }
    return true;
}

bool llm_host::ensure_running(const std::filesystem::path &_repo_root,
                              std::string *_error_out) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_broken && m_ready && m_child.pid() > 0) {
            return true;
        }
    }
    teardown();

    std::filesystem::path python;
    std::filesystem::path script;
    resolve_sidecar_paths(_repo_root, &python, &script);

    std::error_code ec;
    if (!std::filesystem::exists(script, ec)) {
        if (_error_out) {
            *_error_out = "sidecar script not found: " + script.string();
        }
        return false;
    }

    const std::vector<std::string> argv = {python.string(), script.string()};
    std::optional<process::child> spawned =
        process::spawn(python.string(), argv, _error_out);
    if (!spawned.has_value()) {
        return false;
    }

    int out_fd = -1;
    int err_fd = -1;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_child = std::move(*spawned);
        out_fd = m_child.out();
        err_fd = m_child.err();
    }
    m_reader = std::thread([this, out_fd]() { pump(out_fd, false); });
    m_stderr_reader = std::thread([this, err_fd]() { pump(err_fd, true); });

    std::unique_lock<std::mutex> lk(m_mu);
    const bool ready =
        m_cv.wait_for(lk, std::chrono::milliseconds(k_startup_timeout_ms),
                      [this]() { return m_ready || m_broken; });
    if (!ready) {
        lk.unlock();
        teardown();
        if (_error_out) {
            *_error_out = "sidecar did not become ready in time (check " +
                          python.string() +
                          " and `uv pip install -r "
                          "requirements.txt`)";
        }
        return false;
    }
    if (!m_protocol_mismatch.empty()) {
        const std::string mismatch = m_protocol_mismatch;
        lk.unlock();
        teardown();
        if (_error_out) {
            *_error_out = mismatch;
        }
        return false;
    }
    if (m_broken || !m_ready) {
        lk.unlock();
        teardown();
        if (_error_out) {
            *_error_out =
                "sidecar exited during startup (see [llm][py] log lines)";
        }
        return false;
    }
    return true;
}

long llm_host::next_request_id() {
    std::lock_guard<std::mutex> lk(m_mu);
    const long id = m_next_id++;
    m_pending_id = id;
    m_has_result = false;
    m_result = llm_result{};
    m_thinking = false;
    m_has_tool = false;
    m_tool_calls = 0;
    m_view_crop_streak = 0;
    m_events.clear();
    m_event_images.clear();
    return id;
}

void llm_host::set_tool_context(const llm::tool_context &_ctx) {
    m_tool_ctx = _ctx;
}

/// A step in the timeline that is not the model's own words: a tool call's
/// note, or a line the host itself wants shown.
void llm_host::record_note(const std::string &_note) {
    std::lock_guard<std::mutex> lk(m_mu);
    llm_event e;
    e.type = llm_event::kind::tool;
    e.text = _note;
    m_events.push_back(std::move(e));
}

std::string llm_host::run_tool(const llm_protocol::message &_call) {
    int call_index = 0;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        call_index = ++m_tool_calls;
    }
    if (call_index > m_turn_max_dispatches) {
        const std::string error = "dispatch limit reached (" +
                                  std::to_string(m_turn_max_dispatches) + ")";
        record_note(tool_note(_call.tool_name, "error", error));
        return llm_protocol::build_tool_result(_call.call_id, false, "", error,
                                               "");
    }
    if (_call.tool_name == "view_crop") {
        ++m_view_crop_streak;
    } else {
        m_view_crop_streak = 0;
    }
    const std::string alarm = llm::dispatch_alarm(
        call_index, m_turn_max_dispatches, m_view_crop_streak);

    const llm::tool_reply reply =
        llm::dispatch(_call.tool_name, _call.tool_args_json);
    std::string text = reply.text;
    std::string error = reply.error;
    if (!alarm.empty()) {
        std::string &dest = reply.ok ? text : error;
        if (!dest.empty()) {
            dest += "\n";
        }
        dest += alarm;
    }
    if (reply.ok) {
        std::string shown =
            reply.quiet_note.empty() ? text : std::string(reply.quiet_note);
        if (!alarm.empty() && !reply.quiet_note.empty()) {
            shown += "\n" + alarm;
        }
        record_note(tool_note(_call.tool_name, "ok", shown));
    } else {
        record_note(tool_note(_call.tool_name, "error", error));
    }
    if (!reply.image_png.empty()) {
        std::lock_guard<std::mutex> lk(m_mu);
        llm_event e;
        e.type = llm_event::kind::image;
        e.image_id = ++m_tool_image_id;
        m_events.push_back(std::move(e));
        // Kept raw for the UI to draw; the wire is the only place base64
        // happens. Bounded, and dropped oldest first, because the UI may not be
        // looking at the page while the model takes screenshot after
        // screenshot.
        m_event_images.emplace_back(m_tool_image_id, reply.image_png);
        while (static_cast<int>(m_event_images.size()) > k_event_image_stash) {
            m_event_images.pop_front();
        }
    }
    const std::string image_b64 =
        reply.image_png.empty() ? std::string()
                                : llm_protocol::base64_encode(
                                      reinterpret_cast<const unsigned char *>(
                                          reply.image_png.data()),
                                      reply.image_png.size());
    return llm_protocol::build_tool_result(_call.call_id, reply.ok, text, error,
                                           image_b64, reply.shot);
}

llm_result llm_host::exchange(const std::string &_request_line,
                              int _timeout_ms) {
    llm_result r;
    std::string err;
    if (!write_all(_request_line, &err)) {
        teardown();
        r.error = err;
        return r;
    }

    // One deadline for the whole exchange, tool calls included: a model that
    // keeps tapping must not extend its own budget indefinitely.
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(_timeout_ms);
    std::unique_lock<std::mutex> lk(m_mu);
    while (true) {
        const bool signalled = m_cv.wait_until(lk, deadline, [this]() {
            return m_has_result || m_broken || m_has_tool;
        });
        if (!signalled) {
            lk.unlock();
            teardown();
            r.error = "sidecar timed out after " + std::to_string(_timeout_ms) +
                      " ms";
            return r;
        }
        if (m_has_result) {
            return m_result;
        }
        if (m_broken) {
            lk.unlock();
            teardown();
            r.error = "sidecar exited before answering";
            return r;
        }

        const llm_protocol::message call = m_tool_call;
        const long id = m_pending_id;
        m_has_tool = false;
        // A message the user already sent stops the turn here: the tool has
        // not started, so it does not run. The sidecar is blocked in this
        // call and reads `interrupt` instead of a result.
        const bool stop_first = m_interrupt.load();
        lk.unlock();
        std::string reply;
        if (stop_first) {
            reply = llm_protocol::build_interrupt_request(id);
        } else {
            reply = run_tool(call);
            // The tool that was already running finishes, then the same line
            // replaces its result. Checked after, so a stop that arrives
            // mid-tool is not lost.
            if (m_interrupt.load()) {
                reply = llm_protocol::build_interrupt_request(id);
            }
        }
        // Time spent waiting on a human does not count against the request
        // budget: the worker was blocked inside service_approval, so wait_until
        // never ran, and without this top-up the next wait_until would time out
        // immediately and tear the sidecar down.
        deadline += take_approval_wait();
        if (!write_all(reply + "\n", &err)) {
            teardown();
            r.error = err;
            return r;
        }
        lk.lock();
    }
}

llm_protocol::usage_stats llm_host::usage() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_usage;
}

llm_result llm_host::run_turn(const std::filesystem::path &_config_home,
                              const std::string &_provider,
                              const std::string &_prompt, int _max_dispatches,
                              int _max_model_rounds, bool _operator_only,
                              const std::filesystem::path &_repo_root) {
    llm_result r;
    m_turn_max_dispatches =
        _max_dispatches >= 1 ? _max_dispatches : llm::k_default_max_dispatches;
    if (_max_model_rounds < 1) {
        _max_model_rounds = llm::k_default_max_model_rounds;
    }
    if (_prompt.empty()) {
        r.error = "empty request";
        return r;
    }

    // One context per turn, carrying the host's own approval callback: the gate
    // has to have somebody to ask before it can work. The screenshot is
    // injected from `m_screen` rather than taken fresh: it belongs to the
    // session, so coordinates taken from it stay valid until the model asks for
    // a new one.
    llm::tool_context ctx = m_tool_ctx;
    ctx.request_approval = [this](const llm::approval_request &_req) {
        return service_approval(_req);
    };
    ctx.screen = m_screen;
    llm::set_context(ctx);

    std::string err;
    if (!ensure_running(_repo_root.empty() ? _config_home.parent_path()
                                           : _repo_root,
                        &err)) {
        r.error = err;
        return r;
    }

    // Read fresh every turn rather than cached: the file is small, and editing
    // it outside the app then has to show up on the very next request.
    std::string memory;
    if (!llm::read_memory(_config_home, llm::memory_kind::operator_notes,
                          &memory)) {
        memory.clear();
        record_note("operator long-term memory file is unreadable; continuing "
                    "without it");
    }
    std::string scripter_memory;
    if (!llm::read_memory(_config_home, llm::memory_kind::scripter_notes,
                          &scripter_memory)) {
        scripter_memory.clear();
        record_note("scripter long-term memory file is unreadable; continuing "
                    "without it");
    }

    const long id = next_request_id();
    std::string request = llm_protocol::build_turn_request(
        id, _prompt, llm::tools_json(), memory, scripter_memory, _provider,
        _max_model_rounds, _operator_only);
    request.push_back('\n');
    const llm_result turn = exchange(request, k_request_timeout_ms);
    // This turn may have replaced the screenshot; take it back for the next one
    // to inject. A turn that never called `screenshot` hands back what it was
    // given, so this cannot blank the session's picture.
    llm::current_screen(&m_screen);
    return turn;
}

llm_result llm_host::memorize(const std::filesystem::path &_config_home,
                              const std::string &_provider,
                              llm::memory_kind _kind, int _timeout_ms) {
    llm_result r;

    std::string current;
    if (!llm::read_memory(_config_home, _kind, &current)) {
        r.error = std::string(llm::memory_agent_name(_kind)) +
                  " long-term memory file is unreadable; not overwriting it";
        return r;
    }

    std::string err;
    if (!ensure_running(_config_home.parent_path(), &err)) {
        r.error = err;
        return r;
    }

    const long id = next_request_id();
    std::string request = llm_protocol::build_memorize_request(
        id, current, _provider, llm::memory_agent_name(_kind));
    request.push_back('\n');
    const llm_result out = exchange(request, _timeout_ms);
    if (!out.ok) {
        return out;
    }
    // The user sent a message while this pass ran. The notes stay as they
    // were; the message is the next turn. No `interrupt` line: a memorize
    // request has no tool call to replace.
    if (m_interrupt.load()) {
        r.ok = true;
        r.text = current;
        return r;
    }

    // Nothing new since the last pass: the sidecar hands the notes straight
    // back, and rewriting an identical file would only move its timestamp.
    if (out.text == current) {
        r.ok = true;
        r.text = out.text;
        return r;
    }

    if (!llm::write_memory(_config_home, _kind, out.text)) {
        r.error =
            std::string(llm::memory_agent_name(_kind)) +
            " long-term memory was not written (unreadable, empty, or over " +
            std::to_string(llm::k_memory_max_chars) + " characters)";
        return r;
    }
    r.ok = true;
    r.text = out.text;
    return r;
}

ccat_lang::ask_result
llm_host::consult(const std::filesystem::path &_repo_root,
                  std::string_view _provider, std::string_view _question,
                  std::string_view _image_b64,
                  const std::function<bool()> &_should_stop) {
    ccat_lang::ask_result answer;
    std::string err;
    // The Run button can be the first thing a session does, so this starts the
    // sidecar exactly as a turn would.
    if (!ensure_running(_repo_root, &err)) {
        answer.error = "ask: no model to ask (" + err + ")";
        return answer;
    }

    long id = 0;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        id = m_next_consult_id++;
        m_consult_pending_id = id;
        m_has_consult = false;
        m_consult_ok = false;
        m_consult_answer.clear();
        m_consult_error.clear();
    }
    if (!write_all(llm_protocol::build_consult(id, _provider, _question,
                                               _image_b64) +
                       "\n",
                   &err)) {
        teardown();
        answer.error = "ask: no model to ask (" + err + ")";
        return answer;
    }

    std::unique_lock<std::mutex> lk(m_mu);
    // Polled rather than waited on once: a model can take a while, and Stop has
    // to be able to say so before the reply lands.
    while (!m_has_consult && !m_broken &&
           !(_should_stop && _should_stop())) {
        m_cv.wait_for(lk, std::chrono::milliseconds(k_poll_slice_ms));
    }
    if (!m_has_consult) {
        const bool broken = m_broken;
        m_consult_pending_id = 0;
        lk.unlock();
        if (broken) {
            teardown();
            answer.error = "ask: the sidecar exited before answering";
        } else {
            answer.error = "ask: stopped";
        }
        return answer;
    }
    const bool ok = m_consult_ok;
    const std::string said = m_consult_answer;
    const std::string why = m_consult_error;
    m_consult_pending_id = 0;
    lk.unlock();

    if (!ok) {
        answer.error = "ask: " + (why.empty() ? std::string("the model did "
                                                           "not answer")
                                              : why);
        return answer;
    }
    // Only the two words count. Anything else is `ask` refusing to guess,
    // which is the whole reason the model is told to say "unsure".
    if (said == "yes") {
        answer.value = true;
    } else if (said == "no") {
        answer.value = false;
    } else {
        answer.error = "ask: the answer was not yes or no";
    }
    return answer;
}

llm_host &consult_host() {
    static llm_host host;
    return host;
}

llm_result llm_host::reset(const std::filesystem::path &_repo_root) {
    llm_result r;
    // The session is over: the screenshot goes with it, and so do the numbered
    // frames the tools kept, at the same moment the sidecar drops its history.
    // Before the early return below -- no child means there was no history, and
    // a stale screenshot or frame would only mislead.
    m_screen = llm::screen_state{};
    llm::forget_frames();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_child.pid() <= 0 || m_broken || !m_ready) {
            // No child, so no history: report success without spawning one.
            r.ok = true;
            r.turns = 0;
            m_usage = {};
            return r;
        }
    }
    (void)_repo_root;

    const long id = next_request_id();
    std::string request = llm_protocol::build_reset_request(id);
    request.push_back('\n');
    llm_result out = exchange(request, k_request_timeout_ms);
    if (out.ok) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_usage = {};
    }
    return out;
}

void llm_host::teardown() {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_stopping = true;
    }
    m_cv.notify_all();

    // Closing the pipes first unblocks the readers still waiting in poll().
    m_child.close_pipes();
    if (m_reader.joinable()) {
        m_reader.join();
    }
    if (m_stderr_reader.joinable()) {
        m_stderr_reader.join();
    }
    m_child.terminate();

    std::lock_guard<std::mutex> lk(m_mu);
    m_ready = false;
    m_broken = false;
    m_stopping = false;
    m_has_result = false;
    m_has_tool = false;
    m_has_consult = false;
    m_consult_pending_id = 0;
    m_protocol_mismatch.clear();
    m_events.clear();
    m_event_images.clear();
    m_thinking = false;
}

#else // _WIN32

void llm_host::handle_line(const std::string &) {}
void llm_host::pump(int, bool) {}

std::vector<llm_event> llm_host::events() const { return {}; }

std::string llm_host::event_image_png(long) const { return {}; }

bool llm_host::is_thinking() const { return false; }
void llm_host::clear_stream() {}

long llm_host::next_request_id() { return 0; }

std::string llm_host::run_tool(const llm_protocol::message &) {
    return std::string();
}

void llm_host::set_tool_context(const llm::tool_context &) {}

void llm_host::record_note(const std::string &) {}

llm_result llm_host::exchange(const std::string &, int) {
    llm_result r;
    r.error = "the LLM sidecar is not supported on Windows";
    return r;
}

bool llm_host::write_all(const std::string &, std::string *_error_out) {
    if (_error_out) {
        *_error_out = "the LLM sidecar is not supported on Windows";
    }
    return false;
}

bool llm_host::ensure_running(const std::filesystem::path &,
                              std::string *_error_out) {
    if (_error_out) {
        *_error_out = "the LLM sidecar is not supported on Windows";
    }
    return false;
}

llm_protocol::usage_stats llm_host::usage() const { return {}; }

llm_result llm_host::run_turn(const std::filesystem::path &,
                              const std::string &, const std::string &, int,
                              int, bool, const std::filesystem::path &) {
    llm_result r;
    r.error = "the LLM sidecar is not supported on Windows";
    return r;
}

llm_result llm_host::memorize(const std::filesystem::path &,
                              const std::string &, llm::memory_kind, int) {
    llm_result r;
    r.error = "the LLM sidecar is not supported on Windows";
    return r;
}

ccat_lang::ask_result llm_host::consult(const std::filesystem::path &,
                                        std::string_view, std::string_view,
                                        std::string_view,
                                        const std::function<bool()> &) {
    ccat_lang::ask_result r;
    r.error = "ask: the LLM sidecar is not supported on Windows";
    return r;
}

llm_host &consult_host() {
    static llm_host host;
    return host;
}

llm_result llm_host::reset(const std::filesystem::path &) {
    // Unreachable in practice -- the Windows host never spawns a sidecar -- but
    // "a reset drops the frame table" holds on every platform, so it is not
    // conditional here.
    llm::forget_frames();
    llm_result r;
    r.error = "the LLM sidecar is not supported on Windows";
    return r;
}

void llm_host::teardown() {}

#endif // _WIN32

void llm_host::stop() { teardown(); }

void llm_host::request_interrupt() { m_interrupt.store(true); }

void llm_host::clear_interrupt() { m_interrupt.store(false); }

bool llm_host::interrupt_pending() const { return m_interrupt.load(); }

llm_host::timeline llm_host::copy_timeline() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return timeline{m_events, m_event_images};
}

void llm_host::release_approval() {
    std::lock_guard<std::mutex> lk(m_approval_mu);
    if (!m_approval_pending || m_approval_answered || m_approval_cancelled) {
        return;
    }
    m_approval_approved = false;
    m_approval_cancelled = true;
    m_approval_cv.notify_all();
}

llm::approval_decision
llm_host::service_approval(const llm::approval_request &_req) {
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(m_approval_mu);
    if (m_approval_abort) {
        return {};
    }
    m_approval_req = _req;
    m_approval_pending = true;
    m_approval_answered = false;
    m_approval_approved = false;
    m_approval_cancelled = false;
    m_approval_guidance
        .clear(); // a stale correction must not leak into this one
    m_approval_region.reset();
    ++m_next_approval_id;
    // Deliberately no notify: the UI polls pending_approval() per frame, so no
    // thread is waiting on publication.
    m_approval_cv.wait(
        lk, [this] { return m_approval_answered || m_approval_cancelled; });
    const bool approved = m_approval_approved && !m_approval_cancelled;
    const std::string guidance = approved ? std::string{} : m_approval_guidance;
    const std::optional<llm::region> redrawn =
        approved ? std::nullopt : m_approval_region;
    m_approval_pending = false;
    lk.unlock();
    m_approval_wait += std::chrono::steady_clock::now() - start;
    return {approved, guidance, redrawn};
}

llm_approval llm_host::pending_approval() const {
    std::lock_guard<std::mutex> lk(m_approval_mu);
    return llm_approval{m_approval_pending,         m_next_approval_id,
                        m_approval_req.tool,        m_approval_req.summary,
                        m_approval_req.highlight,   m_approval_req.editable,
                        m_approval_req.given_region};
}

std::string llm_host::approval_screen_png() const {
    std::lock_guard<std::mutex> lk(m_approval_mu);
    return m_approval_req.screen_png;
}

void llm_host::answer_approval(bool _approved, std::string _guidance,
                               std::optional<llm::region> _redrawn) {
    std::lock_guard<std::mutex> lk(m_approval_mu);
    if (!m_approval_pending || m_approval_answered || m_approval_cancelled) {
        return;
    }
    m_approval_approved = _approved;
    // An approval means "the region is right", so any text or redrawn region is
    // dropped. Enforced here rather than in the UI: no caller can sneak a
    // correction onto an approval.
    m_approval_guidance = _approved ? std::string{} : std::move(_guidance);
    m_approval_region = _approved ? std::nullopt : std::move(_redrawn);
    m_approval_answered = true;
    m_approval_cv.notify_all();
}

void llm_host::cancel_approval() {
    std::lock_guard<std::mutex> lk(m_approval_mu);
    m_approval_abort = true;
    if (!m_approval_pending) {
        return;
    }
    m_approval_cancelled = true;
    m_approval_cv.notify_all();
}

std::chrono::nanoseconds llm_host::take_approval_wait() {
    const std::chrono::nanoseconds waited = m_approval_wait;
    m_approval_wait = std::chrono::nanoseconds{0};
    return waited;
}

} // namespace campcat
