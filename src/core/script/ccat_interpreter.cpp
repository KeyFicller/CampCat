#include "core/script/ccat_interpreter.h"

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/template_matcher.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace campcat::ccat_lang {

namespace {

std::chrono::milliseconds pause_after_tap(const app_config &_cfg) {
  return std::chrono::milliseconds(
      std::max(_cfg.tap_delay_ms, _cfg.action_gap_ms));
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
                                 std::filesystem::path _images_base,
                                 game_automation::log_fn _log)
    : m_adb(_adb), m_cfg(_cfg), m_matcher(std::move(_matcher)),
      m_images_base(std::move(_images_base)), m_log(std::move(_log)) {
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
    m_log("[ccat] screencap failed");
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
    m_log("[ccat] if: template file missing " + abs_path.string());
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
    m_log("[ccat] template unreadable or missing: " + abs_path.string());
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

std::optional<std::string> CcatInterpreter::exec_stmt(
    const Stmt *_stmt, const std::function<bool()> &_should_stop) {
  if (!_stmt) {
    return std::nullopt;
  }
  if (_should_stop()) {
    return std::string("stopped");
  }

  if (const auto *blk = dynamic_cast<const BlockStmt *>(_stmt)) {
    for (const auto &child : blk->body) {
      if (auto err = exec_stmt(child.get(), _should_stop)) {
        return err;
      }
    }
    return std::nullopt;
  }

  if (const auto *tap = dynamic_cast<const TapStmt *>(_stmt)) {
    return tap_image(tap->image_path, _should_stop);
  }

  if (const auto *w = dynamic_cast<const WaitStmt *>(_stmt)) {
    return cooperative_sleep_ms(w->milliseconds, _should_stop);
  }

  if (const auto *lg = dynamic_cast<const LogStmt *>(_stmt)) {
    m_log(std::string("[ccat] ") + lg->message);
    return std::nullopt;
  }

  if (const auto *ifs = dynamic_cast<const IfStmt *>(_stmt)) {
    bool found = false;
    if (!image_matches(ifs->image_path, _should_stop, &found)) {
      if (_should_stop()) {
        return std::string("stopped");
      }
      return std::string("screencap failed in if condition");
    }
    const Stmt *branch =
        found ? ifs->then_branch.get() : ifs->else_branch.get();
    return exec_stmt(branch, _should_stop);
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
