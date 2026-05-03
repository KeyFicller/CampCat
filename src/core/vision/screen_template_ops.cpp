#include "core/screen_template_ops.h"

#include "core/adb_client.h"
#include "core/app_config.h"

#include <chrono>

#include <opencv2/imgcodecs.hpp>

namespace campcat {

namespace {

std::filesystem::path resolve_under_resolution_dir(
    const std::filesystem::path &_template_resolution_dir,
    std::string_view _relative_name) {
  return _template_resolution_dir / std::string(_relative_name);
}

void adb_connect_if_configured(adb_client &_adb, const app_config &_cfg,
                               int _timeout_ms) {
  if (_cfg.adb_connect_address.empty()) {
    return;
  }
  std::string co, ce;
  (void)_adb.run({"connect", _cfg.adb_connect_address}, &co, &ce,
                 _timeout_ms);
}

} // namespace

screen_template_detect_result detect_template_on_screen(
    adb_client &_adb, const app_config &_shell_timing_only,
    const std::filesystem::path &_template_resolution_dir,
    std::string_view _relative_template_filename,
    const screen_template_ops_options &_opts) {
  screen_template_detect_result r;
  if (_relative_template_filename.empty()) {
    r.error = screen_template_error::template_missing;
    return r;
  }

  r.template_path = resolve_under_resolution_dir(_template_resolution_dir,
                                               _relative_template_filename);

  if (!std::filesystem::exists(r.template_path)) {
    r.error = screen_template_error::template_missing;
    return r;
  }

  adb_connect_if_configured(_adb, _shell_timing_only,
                            _opts.adb_connect_timeout_ms);

  cv::Mat scr;
  if (!_adb.screencap_png(&scr, _opts.screencap_timeout_ms,
                          &r.screencap_diagnostic)) {
    r.error = screen_template_error::screencap_failed;
    return r;
  }

  template_matcher matcher(_shell_timing_only.match_threshold,
                           _shell_timing_only.match_multiscale);
  const auto mr =
      matcher.match_file(scr, r.template_path.string(),
                         _shell_timing_only.match_threshold, _opts.roi_px);
  if (!mr) {
    r.error = screen_template_error::template_unreadable;
    return r;
  }

  r.match = *mr;
  r.matched = mr->found;
  r.error = screen_template_error::ok;

  if (mr->found && !_opts.save_annotated_png.empty()) {
    const auto parent = _opts.save_annotated_png.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    const cv::Mat vis =
        template_matcher::annotate(scr, *mr, cv::Scalar(0, 255, 0));
    cv::imwrite(_opts.save_annotated_png.string(), vis);
  }

  return r;
}

screen_template_tap_result tap_template_on_screen(
    adb_client &_adb, const app_config &_shell_timing_only,
    const std::filesystem::path &_template_resolution_dir,
    std::string_view _relative_template_filename,
    const screen_template_ops_options &_opts) {
  screen_template_tap_result out;

  const screen_template_detect_result det =
      detect_template_on_screen(_adb, _shell_timing_only,
                                _template_resolution_dir,
                                _relative_template_filename, _opts);

  out.template_path = det.template_path;
  out.screencap_diagnostic = det.screencap_diagnostic;
  out.match = det.match;

  if (det.error != screen_template_error::ok) {
    out.error = det.error;
    return out;
  }

  if (!det.matched) {
    out.error = screen_template_error::ok;
    out.tapped = false;
    return out;
  }

  if (!_adb.tap(det.match.center.x, det.match.center.y)) {
    out.error = screen_template_error::tap_failed;
    out.tapped = false;
    return out;
  }

  (void)_adb.delay_after_action(
      std::chrono::milliseconds(_shell_timing_only.tap_delay_ms));
  out.error = screen_template_error::ok;
  out.tapped = true;
  return out;
}

} // namespace campcat
