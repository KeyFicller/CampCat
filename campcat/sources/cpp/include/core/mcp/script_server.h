#pragma once

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <llm/reflection_mcp.h>

#include "core/llm/llm_tools.h"
#include "core/script/ccat_ast.h"

namespace campcat {

/**
 * @brief One `run_bundle`: the worker runs the script, the dispatch thread only
 * talks to the client.
 *
 * The two threads meet at `ask`. A script stopped at a question leaves
 * `m_ask_pending` set; the dispatch thread turns that into an `input_required`
 * result and, when the client retries with the answer, hands it back. The
 * script then continues from where it stopped -- nothing before the question
 * runs twice.
 */
class script_bridge {
public:
    script_bridge() = default;
    ~script_bridge();
    script_bridge(const script_bridge &) = delete;
    script_bridge &operator=(const script_bridge &) = delete;

    /**
     * @brief The dispatch thread's side of one `tools/call run_bundle`.
     * @param[in] _bundle Bundle name the client asked for.
     * @param[in] _params This call's `params`, where a retry carries the answer.
     * @return The script's output.
     * @throws mcp::detail::ElicitAsk when the script is waiting for an answer
     *         the client has not given yet; the kit turns that into
     *         `input_required`.
     * @throws std::runtime_error when the script failed.
     */
    std::string run(std::string _bundle, const nlohmann::json &_params);

    /// The worker thread's side -- the answerer handed to `tool_context.ask`.
    /// Blocks until the client answers or the run is cancelled.
    ccat_lang::ask_result ask(std::string_view _question);

private:
    void worker_main(std::string _bundle);
    /// Cancels the worker and waits for it, so its adb use ends before the next
    /// run starts.
    void abandon();

    std::mutex m_mu;
    std::condition_variable m_cv;
    std::thread m_worker;
    std::string m_bundle;
    /// The question the worker is waiting on; read by the dispatch thread.
    std::string m_question;
    ccat_lang::ask_result m_answer;
    /// The `inputResponses.elicit` already handed to the worker, verbatim. A
    /// retry that carries no new answer must not feed the same one again.
    std::string m_consumed;
    bool m_ask_pending = false;
    bool m_answered = false;
    bool m_done = false;
    bool m_cancel = false;
    llm::tool_reply m_reply;
};

/// The guide: served as `instructions` at the handshake, and as the `guide`
/// prompt when a client wants to read it again.
/// @param[in] _graph The one graph this server serves; empty describes the
/// whole library, which is what the tests and the default constructor use.
std::string script_guide(std::string_view _graph = {});

/// The MCP face of CampCat: list the library, read a graph, locate the device
/// and run one bundle. There is no way in to write a bundle or to run source,
/// so a client cannot use this to author skills -- only to play the ones
/// already saved. A server serves exactly one graph, named at startup.
class script_server final : public mcp::Server<script_server> {
public:
    /// @param[in] _graph The one graph this server serves; empty serves the
    /// whole library (the tests' default).
    explicit script_server(std::string _graph = {});

    std::string_view server_name() const override { return "campcat"; }
    std::string_view server_version() const override;
    std::string_view instructions() const override { return m_guide; }

    [[= js::doc{
        .text = js::str(
            "Lists the saved .ccat bundles, one \"name -- description\" line "
            "each. Use it to find a skill to run.")}]]
    [[= mcp::tool{.read_only = true, .destructive = false}]] std::string
    list_scripts();

    [[= js::doc{
        .text = js::str(
            "Returns a graph as its bundles describe it: every anchor with its "
            "kind and the first line of its description, every edge, every "
            "action, and every problem that would make goto refuse the graph. "
            "Read it before you pick a bundle to run.")}]]
    [[= mcp::tool{.read_only = true, .destructive = false}]] std::string
    read_graph();

    [[= js::doc{
        .text = js::str(
            "Answers which screen of this graph the device is on, in one line. "
            "Use it only when you do not already know -- before a run to see "
            "where it starts, or after one to see where it stopped. It taps "
            "nothing. For a question a script asked, use screenshot.")}]]
    [[= mcp::tool{.read_only = true, .destructive = false}]] std::string
    locate();

    [[= mcp::tool{}]]
    [[= js::doc{
        .text = js::str(
            "Runs a saved bundle on the device by name. The script may pause "
            "to ask a yes/no question about the screen; answer it and the call "
            "resumes from where it stopped.")}]]
    [[= js::param_docs(
        js::str("Bundle name, as list_scripts shows it."))]] std::string
    run_bundle(std::string name);

    [[= js::doc{
        .text = js::str(
            "Returns the screen as it is now, as a PNG. Use it when a script "
            "stops and asks a question: the question alone may not be enough "
            "to answer it. To find out which screen the device is on, use "
            "locate instead -- it answers in one line and returns no picture. "
            "It does not tap anything.")}]]
    [[= mcp::tool{.read_only = true, .destructive = false}]] mcp::content
    screenshot();

    [[= js::doc{
        .text = js::str(
            "Boots the Android emulator this shell is configured for and waits "
            "until it is up, up to about four minutes. Use it when a device "
            "call fails because the device is not reachable. When the emulator "
            "is already up it says so and starts nothing.")}]]
    [[= mcp::tool{
        .read_only = false, .destructive = false, .idempotent = true}]] std::string
    start_emulator();

    [[= mcp::prompt{}]]
    [[= js::doc{
        .text = js::str(
            "How to use this library: what a graph's anchors, edges and actions "
            "are, how to find and read one, how to run it, and how a script's "
            "yes/no question comes back.")}]] mcp::message guide();

private:
    /// Every tool goes through the same dispatcher the GUI uses, so the
    /// wording a client reads is the wording the model reads.
    std::string forward(std::string_view _tool,
                        const nlohmann::json &_args = nlohmann::json::object());

    /// The one graph this server serves; empty means the whole library.
    std::string m_graph;
    /// `script_guide(m_graph)`, computed once: the handshake and the prompt
    /// must hand out the same text.
    std::string m_guide;
};

/**
 * @brief Serves the tools over stdio, or over Streamable HTTP at `/mcp` on the
 * loopback address. Blocks until the client closes the pipe, or until
 * `stop_http()`.
 * @param[in] _config_path Shell config; its `config_home` is the library root.
 * @param[in] _graph The one graph this server serves; must exist.
 * @param[in] _http_port When set, serve HTTP on 127.0.0.1 at this port instead
 * of stdio; 0 lets the kernel pick one, and the chosen port goes to stderr.
 * @return Process exit code: 2 for a name that is not `[a-z0-9_]+`, 1 for a
 * graph the library does not have.
 */
int serve_script_mcp(const std::filesystem::path &_config_path,
                     std::string _graph,
                     std::optional<int> _http_port = std::nullopt);

} // namespace campcat
