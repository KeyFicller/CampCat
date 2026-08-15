#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace campcat {

/**
 * @brief Controls the optional GUI-driven periodic automation worker.
 */
struct scheduler_config {
  bool enabled = false;
  int interval_seconds = 3600;
  int jitter_seconds = 90;
  bool skip_if_busy = true;
};

/**
 * @brief Shell JSON document loaded from disk: adb settings, matchers
 * heuristic defaults, scheduler, active script bookkeeping, persistence paths.
 */
struct app_config {
  int version = 1;

  /// Directory containing the persisted shell `.json`; filled by load_from_file.
  std::filesystem::path config_home;

  std::string adb_path = "/Applications/MuMuPlayer.app/Contents/MacOS/"
                         "MuMuEmulator.app/Contents/MacOS/tools/adb";
  std::string adb_serial;
  std::string adb_connect_address;

  int tap_delay_ms = 150;
  int action_gap_ms = 1000;
  int swipe_duration_ms = 300;

  double match_threshold = 0.82;
  bool match_multiscale = false;

  /// When true, dump annotated screencaps for each template match under `match_debug_dir`.
  bool match_debug = false;
  /// Relative to `config_home` (e.g. `match_debug`).
  std::string match_debug_dir = "match_debug";

  int max_step_retries = 12;
  int step_retry_interval_ms = 500;

  scheduler_config scheduler{};

  /// String id driving `make_game_automation`; `"none"` keeps automation idle.
  std::string active_script_id = "ccat_script";

  /// Mapping script id → json path resolved relative to `config_home`.
  std::unordered_map<std::string, std::string> script_config_paths;

  /**
   * @brief Resolve `{config_home}/{script_config_paths[id]}` for known ids.
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

private:
  static void from_json(const nlohmann::json &_j, app_config &_c);
  static nlohmann::json to_json(const app_config &_c);
};

} // namespace campcat
