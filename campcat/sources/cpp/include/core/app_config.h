#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "core/box_snap.h"
#include "core/llm/llm_fuse.h"

namespace campcat {

/// The script library's directory name, sibling to the config file. One
/// directory holds every bundle plus the runtime artifacts (memory, match
/// dumps), and `config_home` points at it.
inline constexpr std::string_view k_library_dir_name = "ccat_library";

/**
 * @brief Cadence of the GUI-driven periodic automation worker.
 *
 * No on/off here: arming is a runtime action (the Script page's Start
 * schedule), not saved state, and the worker never starts by itself.
 */
struct scheduler_config {
    int interval_seconds = 3600;
    int jitter_seconds = 90;
};

/// Locating and talking to the device bridge.
struct adb_config {
    std::string path = "/Applications/MuMuPlayer.app/Contents/MacOS/"
                       "MuMuEmulator.app/Contents/MacOS/tools/adb";
    std::string serial{};
};

/// The MuMu `mumutool` binary: it can start and stop the emulator instance,
/// which adb cannot, and it is what lets the instance outlive nobody.
struct mumu_config {
    std::string tool_path =
        "/Applications/MuMuPlayer.app/Contents/MacOS/mumutool";
    /// Which MuMu instance the start button acts on (`mumutool show`).
    int device_index = 0;
};

/// How synthetic input is paced.
struct input_config {
    int tap_delay_ms = 150;
    int action_gap_ms = 1000;
    int swipe_duration_ms = 300;
};

/// Template matching.
struct match_config {
    double box_threshold = 0.82;
    double circle_threshold = 0.96;
    bool multiscale = false;
    /// When true, dump annotated screencaps for each match under `debug_dir`.
    bool debug = false;
    /// Relative to `config_home` (e.g. `match_debug`).
    std::string debug_dir = "match_debug";
};

/// Per-tool approval gates, consulted only while `approval.require` is on. The
/// names are the tools' own: each reports the same string in its approval
/// request (`llm_tools.cpp`). Every one defaults on, so a config that predates
/// this block keeps exactly the behaviour the master switch alone gave it.
struct approval_gates {
    bool tap = true;
    bool run_script = true;
    bool save_script = true;
    bool save_template = true;
    bool update_script = true;
    bool delete_script = true;
    bool rename_script = true;
};

/// Whether the model's mutating tools ask first, and which ones.
struct approval_config {
    /// When true, the tools that change the device or write files (`tap`,
    /// `run_script`, `save_script`, `save_template`) show the user what they
    /// are about to do and wait for approval. Off by default: they then act
    /// without asking. See llm_tools.h / llm_host.h.
    bool require = false;
    /// Which tools ask while the master is on.
    approval_gates gates{};
};

/// Which model provider the sidecar talks to. Sent with every request, so a
/// change takes effect on the next turn rather than the next restart.
struct llm_config {
    std::string provider = "deepseek"; ///< "deepseek" or "qwen"
    /// Per-turn tool-call cap. At or above `k_fuse_unlimited` the bar draws ∞.
    int max_tool_calls = llm::k_default_max_tool_calls;
    /// Per-turn model-round cap, sent on the turn request. Must not exceed
    /// `max_tool_calls`: one round can issue several calls.
    int max_tool_rounds = llm::k_default_max_tool_rounds;
};

/// Pulling the boxes the model gives onto the element's boundary, on the way to
/// acting on them. See `core/box_snap.h` for what a snap does and what it
/// costs.
struct box_snap_config {
    /// Which strategy, `none` included: "off" and "which strategy" are one
    /// question, so one field answers both. Off by default -- `otsu` and `step`
    /// each improve some elements and worsen others on the test set, not worth
    /// a failure on an element nobody measured.
    box_snap_strategy strategy = box_snap_strategy::none;
};

/**
 * @brief Shell JSON document loaded from disk: adb settings, matchers
 * heuristic defaults, scheduler, persistence paths.
 */
struct app_config {
    /**
     * @brief Where the config file lives -- the root every relative path above
     * resolves against.
     *
     * Not the config file's own directory: the file sits at the repo root and
     * this points at the sibling `ccat_library/`, so that `parent_path()` is
     * still the repo root and the scripts, memory and match dumps all land in
     * one directory. Set by `load_from_file`, never authored by hand.
     */
    std::string config_home{};

    adb_config adb{};
    mumu_config mumu{};
    input_config input{};
    match_config match{};
    approval_config approval{};
    llm_config llm{};
    box_snap_config box_snap{};
    scheduler_config scheduler{};
    /// Script 页选中的 bundle 名（`<config_home>/<name>/main.ccat`）；空 =
    /// 未选。
    std::string script_bundle{};

    /**
     * @brief Factory defaults mirrored when load_from_file fails.
     * @return Default shell record.
     */
    static app_config defaults();

    /**
     * @brief Parse json from disk merging onto defaults().
     * @param[in] _path Shell configuration file (`config.json`).
     * @return Populated shell struct with `config_home` parent bound.
     */
    static app_config load_from_file(const std::filesystem::path &_path);

    /**
     * @brief Persist `*this` to `_path`.
     * @param[in] _path Destination writable json path on disk.
     */
    void save_to_file(const std::filesystem::path &_path) const;
};

} // namespace campcat
