#include "core/script/ccat_interpreter.h"

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "core/template_matcher.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace campcat::ccat_lang {

namespace {

constexpr int k_do_while_max_iters = 50000;

std::chrono::milliseconds pause_after_tap(const app_config &_cfg) {
  return std::chrono::milliseconds(
      std::max(_cfg.tap_delay_ms, _cfg.action_gap_ms));
}

bool coord_normalized_ok(double _v) { return _v >= 0.0 && _v <= 1.0; }

int norm_axis_to_pixel(double _n, int _dim) {
  if (_dim <= 1) {
    return 0;
  }
  const double clamped = std::clamp(_n, 0.0, 1.0);
  return static_cast<int>(
      std::lround(clamped * static_cast<double>(_dim - 1)));
}

std::optional<std::string>
cooperative_sleep_ms(int _ms,
                     const std::function<bool()> &_should_stop) {
  constexpr int k_slice_ms = 50;
  int remaining = _ms;
  while (remaining > 0) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    const int step = std::min(remaining, k_slice_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(step));
    remaining -= step;
  }
  return std::nullopt;
}

} // namespace

CcatInterpreter::CcatInterpreter(adb_client *_adb, const app_config *_cfg,
                                 template_matcher _matcher,
                                 std::filesystem::path _images_base)
    : m_adb(_adb), m_cfg(_cfg), m_matcher(std::move(_matcher)),
      m_images_base(std::move(_images_base)) {
  if (!m_adb || !m_cfg) {
    throw std::invalid_argument("CcatInterpreter requires adb and cfg");
  }
}

std::filesystem::path
CcatInterpreter::resolve_image_path(const std::string &_rel) const {
  std::filesystem::path p(_rel);
  if (p.is_absolute()) {
    return p;
  }
  return m_images_base / p;
}

bool CcatInterpreter::capture_screen(
    cv::Mat *_out, const std::function<bool()> &_should_stop) {
  if (_should_stop()) {
    return false;
  }
  if (!m_adb->screencap_png(_out)) {
    automation_log::emit("[ccat] screencap failed");
    return false;
  }
  return !_should_stop();
}

bool CcatInterpreter::image_matches(const std::string &_rel_path,
                                    const std::function<bool()> &_should_stop,
                                    bool *_found) {
  const auto abs_path = resolve_image_path(_rel_path);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(abs_path, ec)) {
    automation_log::emit("[ccat] if: template file missing " + abs_path.string());
    *_found = false;
    return true;
  }
  cv::Mat screen;
  if (!capture_screen(&screen, _should_stop)) {
    return false;
  }
  const auto opt =
      m_matcher.match_file(screen, abs_path.string(), m_cfg->match_threshold);
  if (!opt.has_value()) {
    automation_log::emit("[ccat] template unreadable or missing: " +
                         abs_path.string());
    *_found = false;
    return true;
  }
  *_found = opt->found;
  return true;
}

std::optional<std::string>
CcatInterpreter::tap_image(const std::string &_rel_path,
                           const std::function<bool()> &_should_stop) {
  const auto abs_path = resolve_image_path(_rel_path);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(abs_path, ec)) {
    std::ostringstream oss;
    oss << "tap: template file missing " << abs_path.string();
    return oss.str();
  }
  cv::Mat screen;
  if (!capture_screen(&screen, _should_stop)) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    return std::string("screencap failed before tap");
  }
  const auto opt =
      m_matcher.match_file(screen, abs_path.string(), m_cfg->match_threshold);
  if (!opt.has_value()) {
    std::ostringstream oss;
    oss << "tap: unreadable template " << abs_path.string();
    return oss.str();
  }
  if (!opt->found) {
    std::ostringstream oss;
    oss << "tap: image not found " << abs_path.string()
        << " (threshold=" << m_cfg->match_threshold << ")";
    return oss.str();
  }

  cv::Mat screen_tap;
  if (!capture_screen(&screen_tap, _should_stop)) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    return std::string("screencap failed before tap verify");
  }
  const auto verify =
      m_matcher.match_file(screen_tap, abs_path.string(), m_cfg->match_threshold);
  if (!verify.has_value()) {
    std::ostringstream oss;
    oss << "tap: unreadable template at tap time " << abs_path.string();
    return oss.str();
  }
  if (!verify->found) {
    std::ostringstream oss;
    oss << "tap: image not found at tap time " << abs_path.string()
        << " (threshold=" << m_cfg->match_threshold << ")";
    return oss.str();
  }

  if (!m_adb->tap(verify->center.x, verify->center.y)) {
    return std::string("tap: adb tap failed");
  }
  std::this_thread::sleep_for(pause_after_tap(*m_cfg));
  return std::nullopt;
}

std::optional<std::string> CcatInterpreter::swipe_templates_impl(
    const std::string &_from_rel, const std::string &_to_rel,
    const std::function<bool()> &_should_stop) {
  const auto abs_from = resolve_image_path(_from_rel);
  const auto abs_to = resolve_image_path(_to_rel);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(abs_from, ec)) {
    std::ostringstream oss;
    oss << "swipe: from template missing " << abs_from.string();
    return oss.str();
  }
  if (!std::filesystem::is_regular_file(abs_to, ec)) {
    std::ostringstream oss;
    oss << "swipe: to template missing " << abs_to.string();
    return oss.str();
  }
  cv::Mat screen;
  if (!capture_screen(&screen, _should_stop)) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    return std::string("swipe: screencap failed");
  }
  const auto m_from =
      m_matcher.match_file(screen, abs_from.string(), m_cfg->match_threshold);
  const auto m_to =
      m_matcher.match_file(screen, abs_to.string(), m_cfg->match_threshold);
  if (!m_from.has_value() || !m_to.has_value()) {
    return std::string("swipe: template unreadable");
  }
  if (!m_from->found || !m_to->found) {
    return std::string(
        "swipe: one or both templates not found on current screen");
  }
  if (!m_adb->swipe(m_from->center.x, m_from->center.y, m_to->center.x,
                    m_to->center.y, m_cfg->swipe_duration_ms)) {
    return std::string("swipe: adb swipe failed");
  }
  std::this_thread::sleep_for(pause_after_tap(*m_cfg));
  return std::nullopt;
}

std::optional<std::string> CcatInterpreter::tap_at_impl(
    double _nx, double _ny, const std::function<bool()> &_should_stop) {
  if (!coord_normalized_ok(_nx) || !coord_normalized_ok(_ny)) {
    return std::string("tap_at: coordinates must be normalized [0,1]");
  }
  cv::Mat screen;
  if (!capture_screen(&screen, _should_stop)) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    return std::string("tap_at: screencap failed");
  }
  const int px = norm_axis_to_pixel(_nx, screen.cols);
  const int py = norm_axis_to_pixel(_ny, screen.rows);
  if (!m_adb->tap(px, py)) {
    return std::string("tap_at: adb tap failed");
  }
  std::this_thread::sleep_for(pause_after_tap(*m_cfg));
  return std::nullopt;
}

std::optional<std::string> CcatInterpreter::swipe_at_impl(
    double _x1, double _y1, double _x2, double _y2,
    const std::function<bool()> &_should_stop) {
  if (!coord_normalized_ok(_x1) || !coord_normalized_ok(_y1) ||
      !coord_normalized_ok(_x2) || !coord_normalized_ok(_y2)) {
    return std::string("swipe_at: coordinates must be normalized [0,1]");
  }
  cv::Mat screen;
  if (!capture_screen(&screen, _should_stop)) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    return std::string("swipe_at: screencap failed");
  }
  const int ax = norm_axis_to_pixel(_x1, screen.cols);
  const int ay = norm_axis_to_pixel(_y1, screen.rows);
  const int bx = norm_axis_to_pixel(_x2, screen.cols);
  const int by = norm_axis_to_pixel(_y2, screen.rows);
  if (!m_adb->swipe(ax, ay, bx, by, m_cfg->swipe_duration_ms)) {
    return std::string("swipe_at: adb swipe failed");
  }
  std::this_thread::sleep_for(pause_after_tap(*m_cfg));
  return std::nullopt;
}

std::optional<std::string> CcatInterpreter::wait_until_impl(
    const std::string &_rel_path, int _timeout_ms,
    const std::function<bool()> &_should_stop) {
  const auto abs_path = resolve_image_path(_rel_path);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(abs_path, ec)) {
    std::ostringstream oss;
    oss << "wait_until: template missing " << abs_path.string();
    return oss.str();
  }
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(std::max(_timeout_ms, 1));
  for (;;) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      std::ostringstream oss;
      oss << "wait_until: timeout after " << _timeout_ms << " ms for "
          << _rel_path;
      return oss.str();
    }
    bool found = false;
    if (!image_matches(_rel_path, _should_stop, &found)) {
      if (_should_stop()) {
        return std::string("stopped");
      }
      return std::string("wait_until: screencap failed");
    }
    if (found) {
      return std::nullopt;
    }
    if (auto sl = cooperative_sleep_ms(50, _should_stop)) {
      return sl;
    }
  }
}

std::optional<std::string> CcatInterpreter::exec_stmt(
    const Stmt *_stmt, const std::function<bool()> &_should_stop) {
  if (!_stmt) {
    return std::nullopt;
  }
  if (_should_stop()) {
    return std::string("stopped");
  }
  return _stmt->exec(*this, _should_stop);
}

std::optional<std::string>
BlockStmt::exec(CcatInterpreter &_interp,
                const std::function<bool()> &_should_stop) const {
  for (const auto &child : body) {
    if (auto err = _interp.exec_stmt(child.get(), _should_stop)) {
      return err;
    }
  }
  return std::nullopt;
}

std::optional<std::string>
TapStmt::exec(CcatInterpreter &_interp,
              const std::function<bool()> &_should_stop) const {
  return _interp.tap_image(image_path, _should_stop);
}

std::optional<std::string>
WaitStmt::exec(CcatInterpreter &_interp,
               const std::function<bool()> &_should_stop) const {
  return cooperative_sleep_ms(milliseconds, _should_stop);
}

std::optional<std::string>
LogStmt::exec(CcatInterpreter &_interp,
              const std::function<bool()> &/*_should_stop*/) const {
  automation_log::emit(std::string("[ccat] ") + message);
  return std::nullopt;
}

std::optional<std::string>
IfStmt::exec(CcatInterpreter &_interp,
             const std::function<bool()> &_should_stop) const {
  bool found = false;
  if (!_interp.image_matches(image_path, _should_stop, &found)) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    return std::string("screencap failed in if condition");
  }
  const Stmt *branch =
      found ? then_branch.get() : else_branch.get();
  return _interp.exec_stmt(branch, _should_stop);
}

std::optional<std::string>
SwipeTemplatesStmt::exec(CcatInterpreter &_interp,
                         const std::function<bool()> &_should_stop) const {
  return _interp.swipe_templates_impl(from_image_path, to_image_path,
                                      _should_stop);
}

std::optional<std::string>
TapAtStmt::exec(CcatInterpreter &_interp,
                const std::function<bool()> &_should_stop) const {
  return _interp.tap_at_impl(nx, ny, _should_stop);
}

std::optional<std::string>
SwipeAtStmt::exec(CcatInterpreter &_interp,
                  const std::function<bool()> &_should_stop) const {
  return _interp.swipe_at_impl(x1, y1, x2, y2, _should_stop);
}

std::optional<std::string>
WaitUntilStmt::exec(CcatInterpreter &_interp,
                    const std::function<bool()> &_should_stop) const {
  return _interp.wait_until_impl(image_path, timeout_ms, _should_stop);
}

std::optional<std::string>
RetryStmt::exec(CcatInterpreter &_interp,
                const std::function<bool()> &_should_stop) const {
  std::optional<std::string> last_err;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    last_err = _interp.exec_stmt(body.get(), _should_stop);
    if (!last_err.has_value()) {
      return std::nullopt;
    }
    if (attempt + 1 < attempts) {
      if (auto sl = cooperative_sleep_ms(50, _should_stop)) {
        return sl;
      }
    }
  }
  return last_err;
}

std::optional<std::string>
DoWhileStmt::exec(CcatInterpreter &_interp,
                  const std::function<bool()> &_should_stop) const {
  if (!body) {
    return std::nullopt;
  }
  for (int iter = 0;; ++iter) {
    if (iter >= k_do_while_max_iters) {
      return std::string("do_while: iteration limit exceeded");
    }
    if (_should_stop()) {
      return std::string("stopped");
    }
    if (auto err = _interp.exec_stmt(body.get(), _should_stop)) {
      return err;
    }
    bool found = false;
    if (!_interp.image_matches(condition_image_path, _should_stop, &found)) {
      if (_should_stop()) {
        return std::string("stopped");
      }
      return std::string("do_while: screencap failed in condition");
    }
    if (!found) {
      break;
    }
  }
  return std::nullopt;
}

std::optional<std::string>
LoopStmt::exec(CcatInterpreter &_interp,
               const std::function<bool()> &_should_stop) const {
  if (!body) {
    return std::nullopt;
  }
  for (int i = 0; i < repetitions; ++i) {
    if (_should_stop()) {
      return std::string("stopped");
    }
    if (auto err = _interp.exec_stmt(body.get(), _should_stop)) {
      return err;
    }
  }
  return std::nullopt;
}

automation_cycle_result
CcatInterpreter::run(const Program &_program,
                     const std::function<bool()> &_should_stop) {
  automation_cycle_result r{};
  for (const auto &st : _program.stmts) {
    if (auto err = exec_stmt(st.get(), _should_stop)) {
      r.ok = false;
      r.message = std::move(*err);
      return r;
    }
  }
  r.ok = true;
  r.message = "ok";
  return r;
}

} // namespace campcat::ccat_lang
