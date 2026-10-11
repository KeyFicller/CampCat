#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace campcat {

/// One script registered with the shell: `id` is what `script.active_id` names,
/// `path` is resolved relative to `config_home`.
struct script_entry {
  std::string id{};
  std::string path{};
};

/**
 * @brief Controls the optional GUI-driven periodic automation worker.
 */
struct scheduler_config {
  bool enabled = false;
  int interval_seconds = 3600;
  int jitter_seconds = 90;
  bool skip_if_busy = true;
};

/// Locating and talking to the device bridge.
struct adb_config {
  std::string path = "/Applications/MuMuPlayer.app/Contents/MacOS/"
                     "MuMuEmulator.app/Contents/MacOS/tools/adb";
  std::string serial{};
  std::string connect_address{};
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
  double threshold = 0.82;
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
  /// `run_script`, `save_script`, `save_template`) show the user what they are
  /// about to do and wait for approval. Off by default: they then act without
  /// asking. See llm_tools.h / llm_host.h.
  bool require = false;
  /// Which tools ask while the master is on.
  approval_gates gates{};
};

/// Which script runs, and the ids it may name.
struct script_config {
  /// String id driving `make_game_automation`; empty or `"none"` keeps
  /// automation idle.
  std::string active_id = "ccat_script";
  /// In file order. Replaces the old id -> path map: the reflection layer has no
  /// map support, and a vector of {id, path} is the shape it can read and write.
  /// The declared default is the single entry the shell used to inject when the
  /// key was missing.
  std::vector<script_entry> registry{{"ccat_script",
                                      "scripts/ccat_script.json"}};
};

/**
 * @brief Shell JSON document loaded from disk: adb settings, matchers
 * heuristic defaults, scheduler, active script bookkeeping, persistence paths.
 */
struct app_config {
  /// Where the config file itself lives -- the root every relative path above
  /// resolves against. Set by `load_from_file`, never authored by hand.
  std::string config_home{};

  adb_config adb{};
  mumu_config mumu{};
  input_config input{};
  match_config match{};
  approval_config approval{};
  scheduler_config scheduler{};
  script_config script{};

  /**
   * @brief Resolve `{config_home}/{script.registry[id]}` for known ids.
   * @param[in] _id Logical script identifier registered inside json.
   * @return Canonical-ish absolute path token; empty if missing ids/paths.
   */
  std::filesystem::path resolve_script_json(const std::string &_id) const;

  /**
   * @brief Factory defaults mirrored when load_from_file fails.
   * @return Default shell record with scripted registry scaffolding.
   */
  static app_config defaults();

  /**
   * @brief Parse json from disk merging onto defaults().
   * @param[in] _path Shell configuration file (`default.json`).
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
