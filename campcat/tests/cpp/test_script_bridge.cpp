// The MCP bridge: a worker runs a saved bundle, the dispatch side turns each
// `ask` into an `input_required` round and hands the answer back. No device:
// the bundles here only log and ask, which is exactly the path a client sees.

#include <core/adb_client.h>
#include <core/app_config.h>
#include <core/automation_log.h>
#include <core/llm/llm_tools.h>
#include <core/mcp/script_server.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using campcat::automation_log;
using campcat::llm::tool_context;
using campcat::script_bridge;

class StubAdb final : public campcat::adb_api {
public:
    /// When set, `screencap_bgr` answers with a tiny frame instead of failing.
    bool has_screen = false;
    std::vector<std::string> list_devices(int) override { return {}; }
    void set_serial(const std::string &) override {}
    bool run(const std::vector<std::string> &, std::string *, std::string *,
             int) override {
        return false;
    }
    bool connect_remote(std::string_view, int, std::string *,
                        std::string *) override {
        return false;
    }
    bool tap(int, int) override { return false; }
    bool swipe(int, int, int, int, int) override { return false; }
    bool keyevent(int) override { return false; }
    bool home_and_kill_all(int) override { return false; }
    bool screencap_bgr(cv::Mat *_out, int, std::string *) override {
        if (!has_screen) {
            return false;
        }
        *_out = cv::Mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30));
        return true;
    }
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }
};

/// Writes each `name`/`source` pair as `<dir>/<name>/main.ccat`, which is the
/// layout `run_bundle` reads.
std::filesystem::path write_library(
    const std::vector<std::pair<std::string, std::string>> &_bundles) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "campcat_script_bridge_test";
    std::filesystem::remove_all(dir);
    for (const auto &[name, source] : _bundles) {
        std::filesystem::create_directories(dir / name);
        std::ofstream out(dir / name / "main.ccat");
        out << source;
    }
    return dir;
}

/// Adds the `golden.md` that makes a bundle an anchor; `write_library` writes
/// only the script.
void write_anchor(const std::filesystem::path &_lib, const std::string &_bundle,
                  const std::string &_description) {
    std::ofstream out(_lib / _bundle / "golden.md");
    out << "---\nkind: page\n---\n" << _description << "\n";
}

/// What a retrying client sends back: the capability it declared, plus the
/// answer. `elicit` is the key the server hangs its one question under.
nlohmann::json capable(std::optional<nlohmann::json> _answer = std::nullopt) {
    nlohmann::json params = {
        {"_meta",
         {{"io.modelcontextprotocol/clientCapabilities",
           {{"elicitation", {{"form", nlohmann::json::object()}}}}}}}};
    if (_answer) {
        params["inputResponses"] = {{"elicit", std::move(*_answer)}};
    }
    return params;
}

nlohmann::json accept(bool _answer) {
    return {{"action", "accept"}, {"content", {{"answer", _answer}}}};
}

/// Stands in for the kit's per-request scope, which is what tells the bridge a
/// client is present at all.
class elicit_scope {
public:
    explicit elicit_scope(const nlohmann::json &_params) : m_scope{&_params} {
        mcp::detail::t_elicit = &m_scope;
    }
    ~elicit_scope() { mcp::detail::t_elicit = nullptr; }
    elicit_scope(const elicit_scope &) = delete;
    elicit_scope &operator=(const elicit_scope &) = delete;

private:
    mcp::detail::ElicitScope m_scope;
};

class ScriptBridge : public testing::Test {
protected:
    void SetUp() override {
        automation_log::clear_sink();
        m_cfg = campcat::app_config{};
        m_ctx = tool_context{};
        m_ctx.adb = &m_adb;
        m_ctx.cfg = &m_cfg;
        campcat::llm::set_context(m_ctx);
    }
    void TearDown() override {
        campcat::llm::set_context(tool_context{});
        automation_log::clear_sink();
        if (!m_dir.empty()) {
            std::filesystem::remove_all(m_dir);
        }
    }

    /// Points the tools at `_lib` and wires `ask` to `_bridge`.
    void use(const std::filesystem::path &_lib, script_bridge &_bridge) {
        m_dir = _lib;
        m_ctx.script_dir = _lib;
        m_ctx.ask = [&_bridge](std::string_view _question) {
            return _bridge.ask(_question);
        };
        campcat::llm::set_context(m_ctx);
    }

    StubAdb m_adb;
    campcat::app_config m_cfg;
    tool_context m_ctx;
    std::filesystem::path m_dir;
};

TEST_F(ScriptBridge, OneQuestionTakesOneRound) {
    const auto lib = write_library(
        {{"asking", "// asks once\nlog(\"before\")\n"
                    "if (ask(\"Is there a claim button?\")) { log(\"yes\") } "
                    "else { log(\"no\") }\n"}});
    script_bridge bridge;
    use(lib, bridge);

    const nlohmann::json first = capable();
    elicit_scope scope(first);
    EXPECT_THROW(bridge.run("asking", first), mcp::detail::ElicitAsk);

    // The retry carries the answer; the script resumes after the question
    // instead of starting over.
    const nlohmann::json retry = capable(accept(true));
    elicit_scope retry_scope(retry);
    const std::string out = bridge.run("asking", retry);

    EXPECT_NE(out.find("script ran ok"), std::string::npos) << out;
    EXPECT_NE(out.find("yes"), std::string::npos) << out;
    EXPECT_EQ(out.find("before"), out.rfind("before")) << out;
}

TEST_F(ScriptBridge, ARunLooksForTemplatesInItsOwnBundle) {
    // `tap("probe.png")` is relative to the bundle, as the GUI's Run has it. A
    // run that resolved from the library root reported "template file missing"
    // for a file sitting right next to main.ccat.
    const auto lib = write_library({{"probe", "// one tap\ntap(\"probe.png\")\n"}});
    ASSERT_TRUE(cv::imwrite((lib / "probe" / "probe.png").string(),
                            cv::Mat(4, 4, CV_8UC3, cv::Scalar(10, 20, 30))));
    script_bridge bridge;
    use(lib, bridge);

    const nlohmann::json params = capable();
    elicit_scope scope(params);
    try {
        bridge.run("probe", params);
    } catch (const std::exception &_e) {
        EXPECT_EQ(std::string(_e.what()).find("template file missing"),
                  std::string::npos)
            << _e.what();
    }
}

TEST_F(ScriptBridge, TheRequestIsAFlatBooleanForm) {
    const auto lib =
        write_library({{"asking", "if (ask(\"Is it?\")) { log(\"yes\") }\n"}});
    script_bridge bridge;
    use(lib, bridge);

    const nlohmann::json params = capable();
    elicit_scope scope(params);
    try {
        bridge.run("asking", params);
        FAIL() << "no question was asked";
    } catch (const mcp::detail::ElicitAsk &_ask) {
        const nlohmann::json &req = _ask.request;
        EXPECT_EQ(req.at("method"), "elicitation/create");
        EXPECT_EQ(req.at("params").at("mode"), "form");
        EXPECT_EQ(req.at("params").at("message"), "Is it?");
        const nlohmann::json &schema = req.at("params").at("requestedSchema");
        EXPECT_EQ(schema.at("type"), "object");
        EXPECT_EQ(schema.at("properties").at("answer").at("type"), "boolean");
        EXPECT_EQ(schema.at("required"), nlohmann::json::array({"answer"}));
    }
}

TEST_F(ScriptBridge, AClientWithoutTheCapabilityIsToldSo) {
    const auto lib =
        write_library({{"asking", "if (ask(\"Is it?\")) { log(\"yes\") }\n"}});
    script_bridge bridge;
    use(lib, bridge);

    const nlohmann::json bare = nlohmann::json::object();
    elicit_scope scope(bare);
    EXPECT_THROW(bridge.run("asking", bare), std::runtime_error);
}

TEST_F(ScriptBridge, ADeclineIsANo) {
    const auto lib = write_library(
        {{"asking", "if (ask(\"Is it?\")) { log(\"yes\") } else { log(\"no\") }\n"}});
    script_bridge bridge;
    use(lib, bridge);

    const nlohmann::json first = capable();
    {
        elicit_scope scope(first);
        EXPECT_THROW(bridge.run("asking", first), mcp::detail::ElicitAsk);
    }
    const nlohmann::json retry =
        capable(nlohmann::json{{"action", "decline"}});
    elicit_scope retry_scope(retry);
    const std::string out = bridge.run("asking", retry);
    EXPECT_NE(out.find("no"), std::string::npos) << out;
}

TEST_F(ScriptBridge, TwoQuestionsTakeTwoRounds) {
    const auto lib = write_library(
        {{"two", "// two asks\nlog(\"before\")\n"
                 "if (ask(\"First?\")) { log(\"first-yes\") } else { log(\"first-no\") }\n"
                 "if (ask(\"Second?\")) { log(\"second-yes\") } else { log(\"second-no\") }\n"}});
    script_bridge bridge;
    use(lib, bridge);

    const nlohmann::json first = capable();
    std::string ask;
    {
        elicit_scope scope(first);
        try {
            bridge.run("two", first);
            FAIL() << "no question was asked";
        } catch (const mcp::detail::ElicitAsk &_ask) {
            ask = _ask.request.at("params").at("message");
        }
    }
    EXPECT_EQ(ask, "First?");

    // The second question must not be answered with the first answer, which the
    // retry still carries.
    const nlohmann::json retry = capable(accept(true));
    {
        elicit_scope scope(retry);
        try {
            bridge.run("two", retry);
            FAIL() << "the second question was skipped";
        } catch (const mcp::detail::ElicitAsk &_ask) {
            ask = _ask.request.at("params").at("message");
        }
    }
    EXPECT_EQ(ask, "Second?");

    const nlohmann::json done = capable(accept(false));
    elicit_scope done_scope(done);
    const std::string out = bridge.run("two", done);
    EXPECT_NE(out.find("first-yes"), std::string::npos) << out;
    EXPECT_NE(out.find("second-no"), std::string::npos) << out;
}

TEST_F(ScriptBridge, ADifferentBundleReplacesThePendingRun) {
    const auto lib = write_library(
        {{"asking", "if (ask(\"Is it?\")) { log(\"yes\") }\n"},
         {"plain", "// no questions\nlog(\"plain ran\")\n"}});
    script_bridge bridge;
    use(lib, bridge);

    const nlohmann::json first = capable();
    {
        elicit_scope scope(first);
        EXPECT_THROW(bridge.run("asking", first), mcp::detail::ElicitAsk);
    }
    // The old run is cancelled and joined rather than left waiting forever.
    const nlohmann::json second = capable();
    elicit_scope scope(second);
    const std::string out = bridge.run("plain", second);
    EXPECT_NE(out.find("plain ran"), std::string::npos) << out;
}

TEST_F(ScriptBridge, TheMcpSurfaceIsTheLibraryAndOneDevice) {
    campcat::script_server server;
    EXPECT_THROW(server.call_tool("run_script", nlohmann::json::object()),
                 std::runtime_error);
    EXPECT_THROW(server.call_tool("save_script", nlohmann::json::object()),
                 std::runtime_error);

    const nlohmann::json tools =
        nlohmann::json::parse(server.tools_as_openai_json());
    std::vector<std::string> names;
    for (const nlohmann::json &tool : tools) {
        names.push_back(tool.at("function").at("name"));
    }
    EXPECT_EQ(names,
              std::vector<std::string>({"list_scripts", "read_graph", "locate",
                                        "run_bundle", "screenshot",
                                        "start_emulator"}));
}

TEST_F(ScriptBridge, TheGuideServesOneGraph) {
    campcat::script_server server("stzb");
    const std::string guide(server.instructions());
    EXPECT_NE(guide.find("stzb"), std::string::npos) << guide;
    EXPECT_NE(guide.find("locate"), std::string::npos) << guide;
    EXPECT_EQ(guide.find("returns the source"), std::string::npos) << guide;
    // A run reports where it landed, so nothing should send the client to
    // locate afterwards -- that was one picture-sized round trip per run.
    EXPECT_NE(guide.find("do not locate to confirm one"), std::string::npos)
        << guide;
    EXPECT_EQ(guide.find("after one to see where it stopped"),
              std::string::npos)
        << guide;

    const nlohmann::json tools =
        nlohmann::json::parse(server.tools_as_openai_json());
    for (const nlohmann::json &tool : tools) {
        if (tool.at("function").at("name") != "locate") {
            continue;
        }
        const std::string description =
            tool.at("function").at("description");
        EXPECT_NE(description.find("only when you do not already know"),
                  std::string::npos)
            << description;
        EXPECT_EQ(description.find("after one to see where it stopped"),
                  std::string::npos)
            << description;
    }
}

/// A fresh server answering `_lines`, one parsed reply per request.
std::vector<nlohmann::json> serve_lines(const std::string &_lines) {
    campcat::script_server server;
    std::istringstream in(_lines);
    std::ostringstream out;
    EXPECT_EQ(server.serve(in, out), 0);
    std::vector<nlohmann::json> replies;
    std::string line;
    std::istringstream written(out.str());
    while (std::getline(written, line)) {
        replies.push_back(nlohmann::json::parse(line));
    }
    return replies;
}

TEST_F(ScriptBridge, TheGuideIsServedAtTheHandshake) {
    const auto replies = serve_lines(
        R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})"
        "\n");
    ASSERT_EQ(replies.size(), 1u);
    const std::string instructions =
        replies.at(0).at("result").at("instructions");
    EXPECT_NE(instructions.find("run_bundle"), std::string::npos);
    EXPECT_NE(instructions.find("read_graph"), std::string::npos);
    EXPECT_NE(instructions.find("goto"), std::string::npos);
}

TEST_F(ScriptBridge, TheGuideIsAlsoAPrompt) {
    const auto replies = serve_lines(
        R"({"jsonrpc":"2.0","id":1,"method":"prompts/list","params":{}})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"prompts/get","params":{"name":"guide","arguments":{}}})"
        "\n");
    ASSERT_EQ(replies.size(), 2u);
    const nlohmann::json prompts = replies.at(0).at("result").at("prompts");
    ASSERT_EQ(prompts.size(), 1u);
    EXPECT_EQ(prompts.at(0).at("name"), "guide");

    const nlohmann::json messages = replies.at(1).at("result").at("messages");
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages.at(0).at("role"), "user");
    EXPECT_EQ(messages.at(0).at("content").at("text"),
              std::string(campcat::script_guide()));
}

TEST_F(ScriptBridge, HttpServesTheSameSurface) {
    campcat::script_server server;
    std::thread serving([&server] { server.serve_http(0); });
    for (int waited = 0; waited < 300 && server.http_port() < 0; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Joining before any assertion fails: a bound server left running would
    // outlive the fixture and its stub device.
    const int port = server.http_port();
    ASSERT_GE(port, 0) << "the server never bound a port";

    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(5);
    const auto post = [&client](const std::string &_body) {
        return client.Post("/mcp", {{"Accept", "application/json, text/event-stream"},
                                    {"Content-Type", "application/json"}},
                           _body, "application/json");
    };

    const auto discover =
        post(R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})");
    ASSERT_TRUE(discover) << "no answer from the http server";
    EXPECT_EQ(discover->status, 200);
    const nlohmann::json found = nlohmann::json::parse(discover->body);
    EXPECT_NE(found.at("result").at("instructions").get<std::string>().find(
                  "run_bundle"),
              std::string::npos);

    const auto guide = post(
        R"({"jsonrpc":"2.0","id":2,"method":"prompts/get","params":{"name":"guide","arguments":{}}})");
    ASSERT_TRUE(guide);
    const nlohmann::json messages =
        nlohmann::json::parse(guide->body).at("result").at("messages");
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages.at(0).at("content").at("text"),
              std::string(campcat::script_guide()));

    server.stop_http();
    serving.join();
}

TEST_F(ScriptBridge, ReadGraphUsesTheServersGraph) {
    const auto lib = write_library(
        {{"g.entry", "if (has(\"entry.png\")) { return true }\nreturn false\n"},
         {"g.city", "if (has(\"city.png\")) { return true }\nreturn false\n"},
         {"g.entry-city", "// the move\nlog(\"walk\")\n"}});
    write_anchor(lib, "g.entry", "the start screen");
    write_anchor(lib, "g.city", "the city");
    m_dir = lib;
    m_ctx.script_dir = lib;
    campcat::llm::set_context(m_ctx);

    campcat::script_server server("g");
    const std::string out = server.call_tool("read_graph", nlohmann::json::object());
    EXPECT_NE(out.find("g.city"), std::string::npos) << out;
    EXPECT_NE(out.find("entry -> city"), std::string::npos) << out;
}

TEST_F(ScriptBridge, ReadGraphForwardsTheError) {
    m_dir = write_library({{"plain", "log(\"x\")\n"}});
    m_ctx.script_dir = m_dir;
    campcat::llm::set_context(m_ctx);

    campcat::script_server server("nope");
    try {
        server.call_tool("read_graph", nlohmann::json::object());
        FAIL() << "an unknown graph came back as a summary";
    } catch (const std::exception &_e) {
        EXPECT_NE(std::string(_e.what()).find("no graph"), std::string::npos)
            << _e.what();
    }
}

TEST_F(ScriptBridge, RunBundleRefusesAnotherGraph) {
    const auto lib = write_library({{"other.x", "// elsewhere\nlog(\"x\")\n"}});
    script_bridge bridge;
    use(lib, bridge);
    m_ctx.graph = "g";
    campcat::llm::set_context(m_ctx);

    campcat::script_server server("g");
    try {
        server.call_tool("run_bundle", {{"name", "other.x"}});
        FAIL() << "another graph's bundle ran";
    } catch (const std::exception &_e) {
        EXPECT_NE(std::string(_e.what()).find("not part of graph \"g\""),
                  std::string::npos)
            << _e.what();
    }
}

TEST_F(ScriptBridge, ListScriptsHidesAnotherGraph) {
    const auto lib = write_library(
        {{"g.map", "// the map\nlog(\"x\")\n"},
         {"other.map", "// theirs\nlog(\"x\")\n"},
         {"loose", "// unowned\nlog(\"x\")\n"}});
    m_dir = lib;
    m_ctx.script_dir = lib;
    m_ctx.graph = "g";
    campcat::llm::set_context(m_ctx);

    campcat::script_server server("g");
    const std::string out = server.call_tool("list_scripts", {});
    EXPECT_NE(out.find("g.map"), std::string::npos) << out;
    EXPECT_EQ(out.find("other.map"), std::string::npos) << out;
    EXPECT_EQ(out.find("loose"), std::string::npos) << out;
}

TEST_F(ScriptBridge, ARunThatSucceedsNamesWhereItLanded) {
    // The name already says where an edge ends, so the reply says it too and
    // the client never has to screenshot or locate to find out.
    const auto lib = write_library({{"g.entry", "return true\n"},
                                    {"g.map", "return true\n"},
                                    {"g.entry-map", "log(\"moved\")\n"},
                                    {"g.map.claim", "log(\"claimed\")\n"},
                                    {"plain", "log(\"x\")\n"}});
    write_anchor(lib, "g.entry", "the start screen");
    write_anchor(lib, "g.map", "the map");
    script_bridge bridge;
    use(lib, bridge);
    const nlohmann::json params = capable();
    elicit_scope scope(params);

    const std::string edge = bridge.run("g.entry-map", params);
    EXPECT_NE(edge.find("[now on g.map]"), std::string::npos) << edge;

    const std::string action = bridge.run("g.map.claim", params);
    EXPECT_NE(action.find("[now on g.map]"), std::string::npos) << action;

    const std::string anchor = bridge.run("g.map", params);
    EXPECT_NE(anchor.find("[now on g.map]"), std::string::npos) << anchor;

    // A bundle that belongs to no graph lands nowhere it can name.
    const std::string plain = bridge.run("plain", params);
    EXPECT_EQ(plain.find("now on"), std::string::npos) << plain;
}

TEST_F(ScriptBridge, LocateAnswersInOneLine) {
    const auto lib = write_library({{"g.entry", "return false\n"},
                                    {"g.map", "return true\n"}});
    write_anchor(lib, "g.entry", "the start screen");
    write_anchor(lib, "g.map", "the map");
    m_dir = lib;
    m_cfg.config_home = lib.string();
    m_ctx.script_dir = lib;
    m_ctx.graph = "g";
    m_adb.has_screen = true;
    campcat::llm::set_context(m_ctx);

    campcat::script_server server("g");
    EXPECT_EQ(server.call_tool("locate", {}), "g.map (page)");
}

TEST_F(ScriptBridge, LocateReportsAnAmbiguousGraph) {
    const auto lib = write_library({{"g.entry", "return false\n"},
                                    {"g.a", "return true\n"},
                                    {"g.b", "return true\n"}});
    write_anchor(lib, "g.entry", "the start screen");
    write_anchor(lib, "g.a", "one of two");
    write_anchor(lib, "g.b", "the other of two");
    m_dir = lib;
    m_cfg.config_home = lib.string();
    m_ctx.script_dir = lib;
    m_ctx.graph = "g";
    m_adb.has_screen = true;
    campcat::llm::set_context(m_ctx);

    campcat::script_server server("g");
    const std::string out = server.call_tool("locate", {});
    EXPECT_EQ(out.rfind("ambiguous:", 0), 0u) << out;
    EXPECT_NE(out.find("a"), std::string::npos) << out;
    EXPECT_NE(out.find("b"), std::string::npos) << out;
}

TEST_F(ScriptBridge, PlayOnlyDoesNotOfferTheViewers) {
    const auto lib =
        write_library({{"probe", "// one tap\ntap(\"nope.png\")\n"}});
    script_bridge bridge;
    use(lib, bridge);

    m_ctx.play_only = true;
    campcat::llm::set_context(m_ctx);
    const nlohmann::json params = capable();
    elicit_scope scope(params);
    try {
        bridge.run("probe", params);
        FAIL() << "a missing template was not a failure";
    } catch (const std::exception &_e) {
        EXPECT_EQ(std::string(_e.what()).find("view_template"),
                  std::string::npos)
            << _e.what();
    }

    m_ctx.play_only = false;
    campcat::llm::set_context(m_ctx);
    script_bridge again;
    const nlohmann::json retry = capable();
    elicit_scope retry_scope(retry);
    try {
        again.run("probe", retry);
        FAIL() << "a missing template was not a failure";
    } catch (const std::exception &_e) {
        EXPECT_NE(std::string(_e.what()).find("view_template"), std::string::npos)
            << _e.what();
    }
}

TEST_F(ScriptBridge, ScreenshotReturnsAnImageBlock) {
    m_adb.has_screen = true;
    campcat::llm::set_context(m_ctx);

    const auto replies = serve_lines(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"screenshot","arguments":{}}})"
        "\n");
    ASSERT_EQ(replies.size(), 1u);
    const nlohmann::json content = replies.at(0).at("result").at("content");
    ASSERT_EQ(content.size(), 1u) << content.dump();
    EXPECT_EQ(content.at(0).at("type"), "image");
    EXPECT_EQ(content.at(0).at("mimeType"), "image/png");
    EXPECT_FALSE(content.at(0).at("data").get<std::string>().empty());
}

} // namespace
