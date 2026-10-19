#include "core/mcp/script_server.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/mumu_launch.h"
#include "core/mumu_launcher.h"
#include "core/script/ccat_graph.h"

namespace campcat {

namespace {

/// The guide, in one place: `instructions` at the handshake and the `guide`
/// prompt both hand out this text, so the two can never drift apart. Built per
/// graph, because the whole point is that a client sees one graph and nothing
/// else: there is no "which graph" step left to describe.
std::string make_guide(std::string_view _graph) {
    const bool one = !_graph.empty();
    const std::string g = one ? std::string(_graph) : std::string("<graph>");
    const std::string scope =
        one ? "This server serves one graph, " + g +
                  ": every bundle it lists or runs is one of its " + g +
                  ".<part> bundles, and a goto, at or run that points at "
                  "another graph is refused.\n\n"
            : "A graph is the screens of one app, kept as bundles named "
              "<graph>.<part>:\n\n";
    return "CampCat plays saved skills on one Android emulator.\n\n" + scope +
           "  " + g + ".entry      the start anchor\n"
           "  " + g + ".<a>        an anchor: one screen, recognised by its "
           "templates\n"
           "  " + g + ".<a>-<b>    an edge: the move from anchor a to anchor b\n"
           "  " + g + ".<a>.<x>    an action: work that starts and ends on "
           "anchor a\n"
           "\n"
           "Using it:\n"
           "\n"
           "1. With no device, start_emulator boots it and waits until Android "
           "answers.\n"
           "2. list_scripts lists the bundles; read_graph returns the anchors, "
           "the edges, the actions and every problem that would make goto "
           "refuse the graph.\n"
           "3. locate answers which screen the device is on, in one line. Call "
           "it only when you do not already know: before a run to see where it "
           "starts, or after one to see where it stopped. Nothing changed "
           "means nothing to re-check, and \"unknown\" is an answer -- report "
           "it, do not retry.\n"
           "4. run_bundle <name> plays the skill. It taps the real screen.\n"
           "\n"
           "There is no goto tool: walking is written into the scripts, and "
           "run_bundle is how you start one from wherever the device is.\n"
           "\n"
           "A script may stop and ask a yes/no question about the screen. That "
           "comes back as resultType \"input_required\"; retry the same "
           "run_bundle with inputResponses.elicit "
           "{\"action\":\"accept\",\"content\":{\"answer\":true|false}}. "
           "\"decline\" means no; \"cancel\" ends the run. While a run waits on "
           "a question it touches nothing, so call screenshot to see the screen "
           "the question is about -- that is the only reason to pull a "
           "picture. Over HTTP, send Mcp-Method, MCP-Protocol-Version and "
           "Mcp-Name with every request: a client without Mcp-Method gets no "
           "questions at all.\n"
           "\n"
           "This server only plays " + g +
           ". It cannot write anything: no save_script, no save_anchor, no "
           "read_script, no run_script. A template that is missing or stale is "
           "something to report to the user, not to work around: there is no "
           "save_template and no tap_at here.\n";
}

/// The single field a client fills in. Flat, as `requestedSchema` demands.
struct ask_form {
    bool answer = false;
};

/// This call's `params`, or an empty object when the kit is not serving one
/// (a handshake-era client), so the bridge has nothing to read.
const nlohmann::json &request_params() {
    static const nlohmann::json k_empty = nlohmann::json::object();
    return mcp::detail::t_elicit != nullptr ? *mcp::detail::t_elicit->params
                                            : k_empty;
}

/// Whether the client on the other end can answer at all. Asked of this request
/// alone: a previous one says nothing about it.
bool client_can_answer(const nlohmann::json &_params) {
    if (mcp::detail::t_elicit == nullptr) {
        return false;
    }
    const nlohmann::json::json_pointer form(
        "/_meta/io.modelcontextprotocol~1clientCapabilities/elicitation/form");
    return _params.contains(form) && !_params.at(form).is_null();
}

/// One `inputResponses.elicit` as the script's `ask` reads it. `decline` is an
/// answer (no); everything else that is not a yes/no is a failure to answer,
/// and the script stops rather than picking a branch.
ccat_lang::ask_result read_answer(const nlohmann::json &_answer) {
    const std::string action = _answer.value("action", "");
    if (action == "decline") {
        return {false, ""};
    }
    if (action == "cancel") {
        return {std::nullopt, "ask: cancelled"};
    }
    if (action == "accept" && _answer.contains("content") &&
        _answer.at("content").is_object()) {
        const nlohmann::json &content = _answer.at("content");
        if (content.contains("answer") && content.at("answer").is_boolean()) {
            return {content.at("answer").get<bool>(), ""};
        }
    }
    return {std::nullopt, "ask: the client cannot answer"};
}

/// The request that ends this call with `input_required`. Built here rather
/// than through `elicit_form`, which asks at most once per dispatch and a script
/// may ask again after its first answer.
// ponytail: this leans on kit detail:: types; revisit on the next pin bump.
nlohmann::json ask_request(const std::string &_question) {
    return nlohmann::json{
        {"method", "elicitation/create"},
        {"params",
         {{"mode", "form"},
          {"message", _question},
          {"requestedSchema", mcp::detail::flat_schema<ask_form>()}}}};
}

/// The one run this process is hosting. A retry from the client has to find the
/// worker it left stopped, so it outlives the single call.
script_bridge &bridge() {
    static script_bridge instance;
    return instance;
}

} // namespace

std::string script_guide(std::string_view _graph) { return make_guide(_graph); }

script_server::script_server(std::string _graph)
    : m_graph(std::move(_graph)), m_guide(make_guide(m_graph)) {}


script_bridge::~script_bridge() { abandon(); }

void script_bridge::abandon() {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_cancel = true;
    }
    // A script in an adb wait would otherwise hold the join for up to 20s.
    llm::request_script_stop();
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

std::string script_bridge::run(std::string _bundle,
                               const nlohmann::json &_params) {
    for (;;) {
        std::unique_lock<std::mutex> lock(m_mu);
        if (m_worker.joinable() && (m_done || m_bundle != _bundle)) {
            lock.unlock();
            abandon();
            continue;
        }
        if (!m_worker.joinable()) {
            // Copied, not moved: the loop compares `_bundle` against `m_bundle`
            // on the next pass, and a moved-from name would read as a new run.
            m_bundle = _bundle;
            m_done = false;
            m_cancel = false;
            m_ask_pending = false;
            m_consumed.clear();
            m_reply = llm::tool_reply{};
            m_worker = std::thread(&script_bridge::worker_main, this, m_bundle);
        }
        m_cv.wait(lock, [this] { return m_ask_pending || m_done; });

        if (m_done) {
            llm::tool_reply reply = std::move(m_reply);
            std::thread worker = std::move(m_worker);
            m_bundle.clear();
            lock.unlock();
            worker.join();
            if (!reply.ok) {
                throw std::runtime_error(reply.error.empty() ? "the script failed"
                                                             : reply.error);
            }
            return reply.text;
        }

        // The script is stopped at a question. Only a response this run has not
        // consumed yet is an answer; the same object handed over twice would
        // answer a question the client never saw.
        const nlohmann::json *given = nullptr;
        if (_params.contains("inputResponses") &&
            _params.at("inputResponses").is_object()) {
            const nlohmann::json &responses = _params.at("inputResponses");
            if (responses.contains("elicit")) {
                given = &responses.at("elicit");
            }
        }
        if (given != nullptr && given->dump() != m_consumed) {
            m_consumed = given->dump();
            m_answer = read_answer(*given);
        } else if (!client_can_answer(_params)) {
            m_answer = {std::nullopt, "ask: the client cannot answer"};
        } else {
            throw mcp::detail::ElicitAsk{ask_request(m_question)};
        }
        m_answered = true;
        m_ask_pending = false;
        m_cv.notify_all();
    }
}

ccat_lang::ask_result script_bridge::ask(std::string_view _question) {
    std::unique_lock<std::mutex> lock(m_mu);
    m_question = std::string(_question);
    m_answered = false;
    m_ask_pending = true;
    m_cv.notify_all();
    m_cv.wait(lock, [this] { return m_answered || m_cancel; });
    m_answered = false;
    if (m_cancel) {
        return {std::nullopt, "ask: cancelled"};
    }
    return m_answer;
}

void script_bridge::worker_main(std::string _bundle) {
    llm::tool_reply reply;
    try {
        reply = llm::dispatch(
            "run_bundle", nlohmann::json{{"name", std::move(_bundle)}}.dump());
    } catch (const std::exception &_e) {
        reply.ok = false;
        reply.error = _e.what();
    }
    std::lock_guard<std::mutex> lock(m_mu);
    m_reply = std::move(reply);
    m_done = true;
    m_cv.notify_all();
}

std::string_view script_server::server_version() const { return "1"; }

std::string script_server::forward(std::string_view _tool,
                                   const nlohmann::json &_args) {
    const llm::tool_reply reply = llm::dispatch(_tool, _args.dump());
    if (!reply.ok) {
        throw std::runtime_error(reply.error.empty()
                                     ? std::string(_tool) + " failed"
                                     : reply.error);
    }
    return reply.text;
}

std::string script_server::list_scripts() { return forward("list_scripts"); }

std::string script_server::read_graph() {
    return forward("read_graph", nlohmann::json{{"name", m_graph}});
}

std::string script_server::locate() {
    // The picture is only the input of the answer: handing it over would give
    // the client a way to keep looking instead of reading the one line.
    forward("screenshot");
    return forward("locate", nlohmann::json{{"graph", m_graph}});
}

std::string script_server::run_bundle(std::string name) {
    return bridge().run(std::move(name), request_params());
}

mcp::content script_server::screenshot() {
    const llm::tool_reply reply = llm::dispatch("screenshot", "{}");
    if (!reply.ok) {
        throw std::runtime_error(reply.error.empty() ? "screenshot failed"
                                                     : reply.error);
    }
    mcp::content out;
    out.add(mcp::image{
        mcp::mime::png,
        std::vector<std::uint8_t>(reply.image_png.begin(),
                                  reply.image_png.end())});
    return out;
}

std::string script_server::start_emulator() { return forward("start_emulator"); }

mcp::message script_server::guide() {
    return mcp::message{mcp::role::user, mcp::text{m_guide}};
}

int serve_script_mcp(const std::filesystem::path &_config_path,
                     std::string _graph,
                     std::optional<int> _http_port) {
    const app_config cfg = app_config::load_from_file(_config_path);
    // Both refusals happen before the port is opened: a service that starts and
    // then fails every call is harder to notice than one that never starts.
    if (!ccat_lang::graph_part_name(_graph)) {
        std::fprintf(stderr,
                     "campcat: graph \"%s\" must be [a-z0-9_]+\n",
                     _graph.c_str());
        return 2;
    }
    if (!ccat_lang::scan_graph(cfg.config_home, _graph).exists) {
        const std::vector<std::string> graphs =
            ccat_lang::graph_names(cfg.config_home);
        std::string names = "none yet";
        for (const std::string &g : graphs) {
            names = names == "none yet" ? g : names + ", " + g;
        }
        std::fprintf(stderr,
                     "campcat: no graph \"%s\": it has no %s.entry anchor "
                     "(graphs: %s)\n",
                     _graph.c_str(), _graph.c_str(), names.c_str());
        return 1;
    }
    const std::string addr =
        mumu_adb_address(cfg.mumu.tool_path, cfg.mumu.device_index);
    adb_client adb(cfg.adb.path, addr.empty() ? cfg.adb.serial : addr);
    if (!addr.empty()) {
        adb.connect_remote(addr);
    }

    llm::tool_context ctx;
    ctx.adb = &adb;
    ctx.cfg = &cfg;
    ctx.script_dir = std::filesystem::path(cfg.config_home);
    ctx.graph = _graph;
    // Play-only: the writing and viewer tools are not here, so a template
    // failure must not advertise them.
    ctx.play_only = true;
    // No approval callback: running a saved bundle is what this server is for,
    // and there is no human on stdio to ask anyway.
    ctx.ask = [](std::string_view _question) {
        return bridge().ask(_question);
    };
    // The instance this server boots stays up with it: the watchdog is held
    // here for the life of the process, which is what closes it on a clean
    // exit. Nothing else outlives a launch.
    static std::optional<process::child> watchdog;
    ctx.start_emulator = [&cfg](const std::function<bool()> &_stop) {
        return launch_and_wait(
            cfg, _stop,
            launch_sink{
                .log = [](std::string_view _line) {
                    std::fprintf(stderr, "[mumu] %.*s\n",
                                 static_cast<int>(_line.size()),
                                 _line.data());
                },
                .opened = [](const std::string &, int,
                             std::optional<process::child> _wd) {
                    watchdog = std::move(_wd);
                }});
    };
    llm::set_context(ctx);
    script_server server(_graph);
    return _http_port ? server.serve_http(*_http_port) : server.stdio();
}

} // namespace campcat
