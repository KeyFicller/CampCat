#pragma once

#include "core/template_matcher.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace campcat::ccat_lang {

/**
 * @brief One template in an anchor signature. `roi` is normalized `[x, y, w, h]`
 * against the frame; empty means the whole frame.
 */
struct anchor_template {
    std::string png;
    bool has_roi = false;
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
};

struct anchor_def {
    std::string id;
    std::vector<anchor_template> must;
    std::vector<anchor_template> must_not;
    bool transient = false;
};

struct graph_edge {
    std::string from;
    std::string to;
    std::string script;
    int timeout_ms = 20000;
};

struct graph_interrupt {
    std::string anchor;
    std::string script;
};

/**
 * @brief One game's screen graph, loaded from `<library>/<name>/graph.json`.
 */
struct screen_graph {
    std::string name;
    std::filesystem::path dir;
    std::string root;
    std::string recover;
    std::vector<anchor_def> anchors;
    std::vector<graph_edge> edges;
    std::vector<graph_interrupt> interrupts;
};

struct loaded_graph {
    bool ok = false;
    screen_graph graph;
    /// Every problem, not just the first, joined by newlines.
    std::string error;
};

/**
 * @brief Load and statically check a graph. A failure lists every problem.
 *
 * @param[in] _library The script library (`config_home`).
 * @param[in] _name    Bundle directory name; also the graph's name.
 */
loaded_graph load_graph(const std::filesystem::path &_library,
                        std::string_view _name);

/**
 * @brief The same, for text that is not on disk yet.
 *
 * `update_graph` checks a submission with this before it writes anything, so a
 * refusal leaves the file alone.
 */
loaded_graph load_graph_text(const std::filesystem::path &_library,
                             std::string_view _name, std::string_view _text);

/// `"graph:anchor"` with a single colon and a safe name on each side.
bool split_anchor_ref(std::string_view _ref, std::string *_graph,
                      std::string *_anchor);

enum class locate_kind {
    page,       ///< a stable anchor
    transient,  ///< a loading / transition anchor
    interrupt,  ///< a popup anchor, checked before pages
    unknown,
    ambiguous,  ///< two page anchors match; the graph cannot tell them apart
    error
};

struct locate_hit {
    locate_kind kind = locate_kind::unknown;
    std::string id;
    std::string detail;
};

/**
 * @brief Score `_frame` against every anchor. One frame, no capture.
 *
 * Interrupt anchors are tried in declaration order and the first hit wins.
 * Two matching page anchors is `ambiguous`.
 */
locate_hit locate(const screen_graph &_graph, const template_matcher &_matcher,
                  const cv::Mat &_frame);

/**
 * @brief The four budgets `goto` uses. Defaults are the language's; tests pass
 * shorter ones. Not read from config.
 *
 * `unknown_ms == 0` and `timeout_ms == 0` take one look and then give up, so a
 * test does not sleep. A `graph.json` timeout must be at least 1.
 */
struct nav_limits {
    int unknown_ms = 20000;
    int poll_ms = 200;
    int max_failures = 3;
    int max_interrupts = 10;
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
 * Failures replan from a fresh locate. After `max_failures` the recover bundle
 * runs once, if the graph has one. The error names the hops that were tried.
 */
nav_result navigate(const screen_graph &_graph, const std::string &_target,
                    const template_matcher &_matcher, const nav_io &_io,
                    nav_limits _limits = {});

/// Bundle names a `graph.json` names as `script` or `recover`.
/// `nullopt` when the file is not JSON; `_err` says why. A missing file is an
/// empty list.
std::optional<std::vector<std::string>>
graph_script_names(const std::filesystem::path &_graph_json, std::string *_err);

/**
 * @brief PNG names a `graph.json` declares in `must` / `must_not`, sorted and
 * without duplicates.
 *
 * A graph keeps its own templates, so `update_script` must count these as
 * declared or the declared set deletes the anchors. `nullopt` when the file is
 * not JSON (`_err` says why) -- a caller must not read that as "nothing to
 * keep". A missing file is an empty list.
 */
std::optional<std::vector<std::string>>
graph_template_names(const std::filesystem::path &_graph_json,
                     std::string *_err);

/// What `list_scripts` says about a bundle beyond its description.
struct bundle_tag {
    bool graph = false;
    int anchors = 0;
    int edges = 0;
    std::string edge_of; ///< the graph whose edge this bundle is, else empty
};

/**
 * @brief Tag one bundle. Reads `graph.json` for the counts, and, for anything
 * else, looks for a graph that names this bundle as an edge.
 */
bundle_tag bundle_tags(const std::filesystem::path &_library,
                       std::string_view _name);

/**
 * @brief The header `read_graph` puts above a loaded graph's own text: what the
 * graph holds, which of its templates are on disk, and how many golden shots
 * each anchor has.
 *
 * `_loaded` must be a graph that loaded `ok`.
 */
std::string graph_summary(const loaded_graph &_loaded);

/**
 * @brief Replace exact `script` / `recover` bundle names.
 * @return The rewritten text, or nullopt when nothing matched or the text is
 * not JSON (`_err` set).
 */
std::optional<std::string> rewrite_graph_scripts(std::string_view _text,
                                                 std::string_view _from,
                                                 std::string_view _to,
                                                 std::string *_err);

/// Empty when every golden shot locates as its own anchor.
///
/// A golden shot is a crop plus a `<n>.json` sidecar saying where on the screen
/// it was cut from; the check pastes the crop back onto a blank frame of the
/// recorded size and runs `locate` on that. The pasted-back position is what
/// keeps a normalized `roi` meaning the same region it did on the real screen.
/// Outside the crop the frame is blank, so a conflict out there cannot be seen:
/// cut the widest box you can, and a whole screen has no blind spot. A crop
/// with no sidecar, or one that will not parse, is a mismatch of its own.
std::vector<std::string>
golden_mismatches(const screen_graph &_graph,
                  const template_matcher &_matcher);

/**
 * @brief Where a golden crop sat on the screen it was cut from, in the screen's
 * pixels.
 */
struct golden_meta {
    int screen_w = 0;
    int screen_h = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

std::string golden_meta_to_text(const golden_meta &_meta);

/// `nullopt` when the text is not a sidecar this program wrote; `_err` says why.
std::optional<golden_meta> golden_meta_from_text(std::string_view _text,
                                                 std::string *_err);

} // namespace campcat::ccat_lang
