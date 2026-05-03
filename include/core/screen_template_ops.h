#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <opencv2/core.hpp>

#include "core/template_matcher.h"

namespace campcat {

class adb_client;
struct app_config;

/**
 * @brief Tweaks for adb screencap/matcher wrapper routines.
 */
struct screen_template_ops_options {
  int screencap_timeout_ms = 45000;
  int adb_connect_timeout_ms = 15000;
  cv::Rect roi_px{};
  std::filesystem::path save_annotated_png;
};

/**
 * @brief High-level statuses returned by templated adb helpers.
 */
enum class screen_template_error {
  ok,
  template_missing,
  template_unreadable,
  screencap_failed,
  tap_failed,
};

/**
 * @brief Result bundle for screenshot-only probing (no taps).
 */
struct screen_template_detect_result {
  screen_template_error error = screen_template_error::ok;
  bool matched = false;
  match_result match{};
  std::filesystem::path template_path;
  std::string screencap_diagnostic;
};

/**
 * @brief Capture frame, correlate template anchored under `_template_resolution_dir`.
 * @param[in] _adb Configured adb client for screencap/tunnel.
 * @param[in] _shell_timing_only Host shell fields (timeouts, matcher thresholds).
 * @param[in] _template_resolution_dir Directory containing templ png assets.
 * @param[in] _relative_template_filename Filename relative inside resolution folder.
 * @param[in] _opts Diagnostics / ROI overrides optional.
 * @return Populated diagnostics struct ready for callers.
 */
screen_template_detect_result detect_template_on_screen(
    adb_client &_adb,
    const app_config &_shell_timing_only,
    const std::filesystem::path &_template_resolution_dir,
    std::string_view _relative_template_filename,
    const screen_template_ops_options &_opts = {});

/**
 * @brief Companion struct tracking tap attempts after screenshot matching.
 */
struct screen_template_tap_result {
  screen_template_error error = screen_template_error::ok;
  bool tapped = false;
  match_result match{};
  std::filesystem::path template_path;
  std::string screencap_diagnostic;
};

/**
 * @brief Run `detect_template_on_screen`; when correlation passes threshold tap center.
 * @param[in] _adb Configured adb client for screencap/tunnel.
 * @param[in] _shell_timing_only Host shell fields including tap delay thresholds.
 * @param[in] _template_resolution_dir Directory containing templ png assets.
 * @param[in] _relative_template_filename Filename relative inside resolution folder.
 * @param[in] _opts Diagnostics / ROI overrides optional.
 * @return Tap diagnostics mirroring detector fields plus tapped flag semantics.
 */
screen_template_tap_result tap_template_on_screen(
    adb_client &_adb,
    const app_config &_shell_timing_only,
    const std::filesystem::path &_template_resolution_dir,
    std::string_view _relative_template_filename,
    const screen_template_ops_options &_opts = {});

} // namespace campcat
