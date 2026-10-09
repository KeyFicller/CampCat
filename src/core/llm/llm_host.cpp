#include "core/llm/llm_host.h"

#include "core/automation_log.h"
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
constexpr char k_default_script_rel[] = "llm/main.py";
constexpr char k_venv_python_rel[] = ".venv/bin/python3";

/// Generous enough to cover a cold Python + LangChain import.
constexpr int k_startup_timeout_ms = 60000;
/// The whole exchange, tool calls included, so this has to cover every round the
/// sidecar allows (`MAX_TOOL_ROUNDS` in llm/graph.py). Too short and a long but
/// healthy turn dies as "sidecar timed out" rather than finishing.
constexpr int k_request_timeout_ms = 900000;
/// Hard stop on a runaway tool loop, kept above the sidecar's round budget: a
/// round may issue several calls, and this must not cut a turn the sidecar still
/// considers legal. The deadline above is what bounds total time.
constexpr int k_max_tool_calls = 100;

/// The interpreter and script are found by convention, never configured.
void resolve_sidecar_paths(const std::filesystem::path &_repo_root,
                           std::filesystem::path *_python,
                           std::filesystem::path *_script) {
  const std::filesystem::path venv = _repo_root / k_venv_python_rel;
  std::error_code ec;
  *_python = std::filesystem::exists(venv, ec) ? venv
                                               : std::filesystem::path("python3");
  *_script = _repo_root / k_default_script_rel;
}

} // namespace

llm_host::~llm_host() { stop(); }

#ifndef _WIN32

void llm_host::handle_line(const std::string &_line) {
  llm_protocol::message msg;
  if (!llm_protocol::parse_line(_line, &msg)) {
    return;
  }

  switch (msg.type) {
  case llm_protocol::message::kind::ready:
    {
      std::lock_guard<std::mutex> lk(m_mu);
      if (msg.protocol != llm_protocol::k_protocol_version) {
        // Reported instead of ignored: an older sidecar silently knows nothing
        // about tool calls, and a newer one may expect fields we do not send.
        m_protocol_mismatch = "sidecar speaks protocol " + std::to_string(msg.protocol) +
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
  case llm_protocol::message::kind::result:
    {
      std::lock_guard<std::mutex> lk(m_mu);
      if (msg.id == m_pending_id) {
        m_result.ok = msg.ok;
        m_result.text = msg.text;
        m_result.error = msg.error;
        m_result.turns = msg.turns;
        m_has_result = true;
      }
    }
    m_cv.notify_all();
    break;
  case llm_protocol::message::kind::chunk:
    {
      // Deliberately no notify: the UI polls per frame, so waking the waiter
      // on every delta would only churn.
      std::lock_guard<std::mutex> lk(m_mu);
      if (msg.id == m_pending_id) {
        m_stream += msg.text;
        m_thinking = m_thinking || msg.thinking;
      }
    }
    break;
  case llm_protocol::message::kind::tool_call:
    {
      // Queued, not executed: this is the reader thread, and running the tool
      // here would block the very pipe that delivers its result. The waiter in
      // `exchange` picks it up and runs it off the lock.
      std::lock_guard<std::mutex> lk(m_mu);
      if (msg.id == m_pending_id) {
        m_tool_call = msg;
        m_has_tool = true;
      }
    }
    m_cv.notify_all();
    break;
  case llm_protocol::message::kind::unknown:
  default:
    break;
  }
}

std::string llm_host::streaming_text() const {
  std::lock_guard<std::mutex> lk(m_mu);
  return m_stream;
}

bool llm_host::is_thinking() const {
  std::lock_guard<std::mutex> lk(m_mu);
  return m_thinking;
}

void llm_host::clear_stream() {
  std::lock_guard<std::mutex> lk(m_mu);
  m_stream.clear();
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

  // EOF on stdout: the child is gone or closed it. Fail any in-flight request.
  if (!_is_stderr) {
    {
      std::lock_guard<std::mutex> lk(m_mu);
      m_broken = true;
    }
    m_cv.notify_all();
  }
}

bool llm_host::write_all(const std::string &_data, std::string *_error_out) {
  const int fd = m_child.in();
  if (fd < 0) {
    if (_error_out) {
      *_error_out = "sidecar stdin is closed";
    }
    return false;
  }
  std::size_t written = 0;
  while (written < _data.size()) {
    const ssize_t n = write(fd, _data.data() + written, _data.size() - written);
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
  const bool ready = m_cv.wait_for(
      lk, std::chrono::milliseconds(k_startup_timeout_ms),
      [this]() { return m_ready || m_broken; });
  if (!ready) {
    lk.unlock();
    teardown();
    if (_error_out) {
      *_error_out = "sidecar did not become ready in time (check " +
                    python.string() + " and `uv pip install -r "
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
      *_error_out = "sidecar exited during startup (see [llm][py] log lines)";
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
  m_stream.clear();
  m_thinking = false;
  m_has_tool = false;
  m_tool_calls = 0;
  m_notes.clear();
  return id;
}

void llm_host::set_tool_context(const llm::tool_context &_ctx) { m_tool_ctx = _ctx; }

std::vector<std::string> llm_host::tool_notes() const {
  std::lock_guard<std::mutex> lk(m_mu);
  return m_notes;
}

void llm_host::record_note(const std::string &_note) {
  std::lock_guard<std::mutex> lk(m_mu);
  m_notes.push_back(_note);
}

std::string llm_host::run_tool(const llm_protocol::message &_call) {
  int call_index = 0;
  {
    std::lock_guard<std::mutex> lk(m_mu);
    call_index = ++m_tool_calls;
  }
  if (call_index > k_max_tool_calls) {
    const std::string error = "tool call limit reached (" +
                              std::to_string(k_max_tool_calls) + ")";
    record_note(_call.tool_name + " error: " + error);
    return llm_protocol::build_tool_result(_call.call_id, false, "", error, "");
  }

  const llm::tool_reply reply = llm::dispatch(_call.tool_name, _call.tool_args_json);
  if (reply.ok) {
    record_note(_call.tool_name + " ok" +
                (reply.text.empty() ? std::string() : ": " + reply.text));
  } else {
    record_note(_call.tool_name + " error: " + reply.error);
  }
  return llm_protocol::build_tool_result(_call.call_id, reply.ok, reply.text,
                                         reply.error, reply.image_b64);
}

llm_result llm_host::exchange(const std::string &_request_line) {
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
                  std::chrono::milliseconds(k_request_timeout_ms);
  std::unique_lock<std::mutex> lk(m_mu);
  while (true) {
    const bool signalled = m_cv.wait_until(
        lk, deadline, [this]() { return m_has_result || m_broken || m_has_tool; });
    if (!signalled) {
      lk.unlock();
      teardown();
      r.error = "sidecar timed out after " +
                std::to_string(k_request_timeout_ms) + " ms";
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
    m_has_tool = false;
    lk.unlock();
    const std::string reply = run_tool(call);
    // Time spent waiting on a human does not count against the request budget:
    // the worker was blocked inside service_approval, so wait_until never ran,
    // and without this top-up the next wait_until would time out immediately and
    // tear the sidecar down.
    deadline += take_approval_wait();
    if (!write_all(reply + "\n", &err)) {
      teardown();
      r.error = err;
      return r;
    }
    lk.lock();
  }
}

llm_result llm_host::run_turn(const std::filesystem::path &_repo_root,
                              const std::string &_prompt) {
  llm_result r;
  if (_prompt.empty()) {
    r.error = "empty request";
    return r;
  }

  // A fresh context per turn, so the screen size starts unknown: coordinates are
  // only meaningful against a screenshot the model actually asked for, and the
  // `screenshot` tool is what fills these in.
  // One context per turn, carrying the host's own approval callback: the gate has
  // to have somebody to ask before it can work.
  llm::tool_context ctx = m_tool_ctx;
  ctx.request_approval = [this](const llm::approval_request &_req) {
    return service_approval(_req);
  };
  llm::set_context(ctx);

  std::string err;
  if (!ensure_running(_repo_root, &err)) {
    r.error = err;
    return r;
  }

  const long id = next_request_id();
  std::string request =
      llm_protocol::build_turn_request(id, _prompt, llm::tools_json());
  request.push_back('\n');
  return exchange(request);
}

llm_result llm_host::reset(const std::filesystem::path &_repo_root) {
  llm_result r;
  {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_child.pid() <= 0 || m_broken || !m_ready) {
      // No child, so no history: report success without spawning one.
      r.ok = true;
      r.turns = 0;
      return r;
    }
  }
  (void)_repo_root;

  const long id = next_request_id();
  std::string request = llm_protocol::build_reset_request(id);
  request.push_back('\n');
  return exchange(request);
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
  m_protocol_mismatch.clear();
  m_stream.clear();
  m_thinking = false;
}

#else // _WIN32

void llm_host::handle_line(const std::string &) {}
void llm_host::pump(int, bool) {}

std::string llm_host::streaming_text() const { return std::string(); }
bool llm_host::is_thinking() const { return false; }
void llm_host::clear_stream() {}

long llm_host::next_request_id() { return 0; }

std::string llm_host::run_tool(const llm_protocol::message &) {
  return std::string();
}

void llm_host::set_tool_context(const llm::tool_context &) {}

std::vector<std::string> llm_host::tool_notes() const { return {}; }

void llm_host::record_note(const std::string &) {}

llm_result llm_host::exchange(const std::string &) {
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

llm_result llm_host::run_turn(const std::filesystem::path &,
                              const std::string &) {
  llm_result r;
  r.error = "the LLM sidecar is not supported on Windows";
  return r;
}

llm_result llm_host::reset(const std::filesystem::path &) {
  llm_result r;
  r.error = "the LLM sidecar is not supported on Windows";
  return r;
}

void llm_host::teardown() {}

#endif // _WIN32

void llm_host::stop() { teardown(); }

llm::approval_decision llm_host::service_approval(
    const llm::approval_request &_req) {
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
  m_approval_guidance.clear(); // a stale correction must not leak into this one
  ++m_next_approval_id;
  // Deliberately no notify: the UI polls pending_approval() per frame, so no
  // thread is waiting on publication.
  m_approval_cv.wait(lk,
                     [this] { return m_approval_answered || m_approval_cancelled; });
  const bool approved = m_approval_approved && !m_approval_cancelled;
  const std::string guidance = approved ? std::string{} : m_approval_guidance;
  m_approval_pending = false;
  lk.unlock();
  m_approval_wait += std::chrono::steady_clock::now() - start;
  return {approved, guidance};
}

llm_approval llm_host::pending_approval() const {
  std::lock_guard<std::mutex> lk(m_approval_mu);
  return llm_approval{m_approval_pending, m_next_approval_id, m_approval_req.tool,
                      m_approval_req.summary, m_approval_req.highlight};
}

std::string llm_host::approval_screen_png() const {
  std::lock_guard<std::mutex> lk(m_approval_mu);
  return m_approval_req.screen_png;
}

void llm_host::answer_approval(bool _approved, std::string _guidance) {
  std::lock_guard<std::mutex> lk(m_approval_mu);
  if (!m_approval_pending || m_approval_answered || m_approval_cancelled) {
    return;
  }
  m_approval_approved = _approved;
  // An approval means "the box is right", so any text is dropped. Enforced here
  // rather than in the UI: no caller can sneak a correction onto an approval.
  m_approval_guidance = _approved ? std::string{} : std::move(_guidance);
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
