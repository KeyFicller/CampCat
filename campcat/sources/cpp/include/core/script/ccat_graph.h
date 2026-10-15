#pragma once

#include "core/script/ccat_ast.h"

#include <opencv2/core.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace campcat::ccat_lang {

enum class anchor_kind {
    page,      ///< a screen you can stay on
    transient, ///< a loading / transition screen
    popup      ///< a popup over any screen, closed by `close`
};

/// What an anchor's `golden.md` says: its front matter, then its description.
struct golden_doc {
    anchor_kind kind = anchor_kind::page;
    std::string close;       ///< popup only: the bundle that closes it
    std::string description; ///< the body after the front matter, trimmed
};

/**
 * @brief Parse `golden.md`. A UTF-8 BOM and CRLF line ends are accepted.
 * @return nullopt, with `*_err` set, when the front matter is missing or not
 * closed, a key is unknown, `kind` is not one of the three, or a popup has no
 * `close`.
 */
std::optional<golden_doc> parse_golden_md(std::string_view _text,
                                          std::string *_err);

/**
 * @brief Every reason `_program` cannot be an anchor script; empty when it can.
 *
 * Locating runs every anchor script, so one may only judge the screen: `has`,
 * `if` / `elif` / `else`, `return`, `log`, and at least one `return` -- a
 * script with none answers true on every screen.
 */
std::vector<std::string> anchor_script_problems(const Program &_program);

/// `[a-z0-9_]+`: a graph or anchor name, so `G.<a>-<b>` splits one way only.
bool graph_part_name(std::string_view _s);

/// `rest` of `G.<anchor>.<action>`: exactly one dot, both sides a part name.
bool split_action_name(std::string_view _rest, std::string *_anchor,
                       std::string *_action);

/// One anchor script's answer on one frame.
struct anchor_check {
    bool here = false;
    std::string error; ///< non-empty when the script failed; `here` is then false
};

/// Bundle `G.<id>` holding a `golden.md`.
struct anchor_def {
    std::string id;
    anchor_kind kind = anchor_kind::page;
    std::string close;
    std::string description;
};

/// Bundle `G.<from>-<to>` where both ends are anchors of `G`.
struct graph_edge {
    std::string from;
    std::string to;
    std::string script;
};

/// Bundle `G.<anchor>.<action>`: work that starts and ends on one page.
struct graph_action {
    std::string anchor;
    std::string name;
    std::string script;
};

/// One game's screen graph, read from the `G.*` bundles. No file of its own.
struct screen_graph {
    std::string name;
    std::vector<anchor_def> anchors; ///< sorted by id
    std::vector<graph_edge> edges;   ///< sorted by script
    std::vector<graph_action> actions; ///< sorted by script
};

struct scanned_graph {
    bool exists = false; ///< `G.entry/golden.md` is there
    screen_graph graph;  ///< whatever could be read, even with problems
    std::vector<std::string> problems;
};

/**
 * @brief Read graph `_name` from `_library`, listing every problem instead of
 * stopping at the first. For views and checks that must show a graph mid-build.
 */
scanned_graph scan_graph(const std::filesystem::path &_library,
                         std::string_view _name);

/// Why `save_script` / `update_script` must not write `_name`. Empty when they may.
/// An anchor script under `graph.id` needs `golden.md`, which only `save_anchor`
/// writes. An edge needs both ends to already be anchors.
std::string script_graph_block(const std::filesystem::path &_library,
                               const std::string &_name,
                               const Program &_program);

struct loaded_graph {
    bool ok = false;
    screen_graph graph;
    /// Every problem, not just the first, joined by newlines.
    std::string error;
};

/// `scan_graph`, but `ok` only when the graph exists and has no problem.
/// `goto` / `at` / running an edge use this one.
loaded_graph load_graph(const std::filesystem::path &_library,
                        std::string_view _name);

/// Every `G` with a `G.entry/golden.md`, sorted.
std::vector<std::string> graph_names(const std::filesystem::path &_library);

/// `"graph:anchor"` with a single colon and a safe name on each side.
bool split_anchor_ref(std::string_view _ref, std::string *_graph,
                      std::string *_anchor);

enum class locate_kind {
    page,       ///< a stable anchor
    transient,  ///< a loading / transition anchor
    interrupt,  ///< a popup anchor, checked before pages
    unknown,
    ambiguous,  ///< two page anchors answer true; the graph cannot tell them apart
    error
};

struct locate_hit {
    locate_kind kind = locate_kind::unknown;
    std::string id;
    std::string detail;
};

using anchor_check_fn =
    std::function<anchor_check(const anchor_def &, const cv::Mat &)>;

/**
 * @brief Run every anchor's check on `_frame`. One frame, no capture.
 *
 * Popups are tried by name and the first hit wins. Two page / transient hits is
 * `ambiguous`; a check that fails is `error` naming the anchor.
 */
locate_hit locate(const screen_graph &_graph, const anchor_check_fn &_check,
                  const cv::Mat &_frame);

/**
 * @brief The budgets `goto` and a direct edge run use. Defaults are the
 * language's; tests pass shorter ones. Not read from config.
 *
 * `unknown_ms == 0` and `edge_timeout_ms == 0` take one look and then give up,
 * so a test does not sleep.
 */
struct nav_limits {
    int unknown_ms = 20000;
    int poll_ms = 200;
    int max_failures = 3;
    int max_interrupts = 10;
    int edge_timeout_ms = 20000;
};

struct bundle_run {
    bool stopped = false;
    bool failed = false;
    std::string message;
};

struct nav_io {
    /// @return false when the capture failed. `should_stop` tells a stop from
    /// a dead device.
    std::function<bool(cv::Mat *)> capture;
    std::function<bool()> should_stop;
    anchor_check_fn check;
    std::function<bundle_run(const std::string &)> run_bundle;
    std::function<void(const std::string &)> log;
};

struct nav_result {
    bool ok = false;
    bool device = false;
    bool stopped = false;
    std::string message;
};

/**
 * @brief Walk `_graph` until `locate` reports `_target`.
 *
 * Failures replan from a fresh locate; `max_failures` of them is an error that
 * names every hop that was tried.
 */
nav_result navigate(const screen_graph &_graph, const std::string &_target,
                    const nav_io &_io, nav_limits _limits = {});

/**
 * @brief One hop with both ends checked: not at `from` fails without running
 * the script; after it, `to` must appear within `edge_timeout_ms`. No replan,
 * no retry.
 */
nav_result run_edge(const screen_graph &_graph, const graph_edge &_edge,
                    const nav_io &_io, nav_limits _limits = {});

/// One action: the same checks as an edge whose `from` and `to` are the anchor.
nav_result run_action(const screen_graph &_graph, const graph_action &_action,
                      const nav_io &_io, nav_limits _limits = {});

/// The edge or action bundle the hop in progress is running, or empty. The
/// script thread writes it for that hop, including a `goto`; the graph page
/// reads it.
std::string running_graph_bundle();
void set_running_graph_bundle(std::string _bundle);

/// What `list_scripts` says about a bundle beyond its description.
struct bundle_tag {
    std::string anchor_of; ///< the graph whose anchor this bundle is
    anchor_kind kind = anchor_kind::page;
    std::string edge_of;   ///< the graph whose edge this bundle is
    std::string action_of; ///< the graph whose action this bundle is
    std::string action_on; ///< the anchor that action runs on
    int anchors = 0;       ///< on `G.entry` only: the graph's counts
    int edges = 0;
    int actions = 0;
};

/// Tag one bundle from its name and the `golden.md` files next to it.
bundle_tag bundle_tags(const std::filesystem::path &_library,
                       std::string_view _name);

/// What `read_graph` shows: anchors with kind and first description line,
/// edges, and every problem.
std::string graph_summary(const scanned_graph &_scanned);

} // namespace campcat::ccat_lang
