#include "core/adb_client.h"

#include "games/stzb_auto_assemble/campcat_stzb_auto_assemble.h"


#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <opencv2/imgproc.hpp>

namespace campcat {

namespace {

cv::Rect norm_rect_px(const cv::Size &sz, const rect_norm &rn) {
  int x = static_cast<int>(std::lround(rn.x * static_cast<double>(sz.width)));
  int y = static_cast<int>(std::lround(rn.y * static_cast<double>(sz.height)));
  int w = static_cast<int>(std::lround(rn.w * static_cast<double>(sz.width)));
  int h = static_cast<int>(std::lround(rn.h * static_cast<double>(sz.height)));
  x = std::max(0, std::min(x, std::max(0, sz.width - 1)));
  y = std::max(0, std::min(y, std::max(0, sz.height - 1)));
  w = std::max(1, std::min(w, sz.width - x));
  h = std::max(1, std::min(h, sz.height - y));
  return {x, y, w, h};
}

std::chrono::milliseconds pause_after_tap(const app_config &cfg) {
  return std::chrono::milliseconds(
      std::max(cfg.tap_delay_ms, cfg.action_gap_ms));
}

/// Extra cooldown after leaving recruitment detail before probing the next
/// slot.
std::chrono::milliseconds pause_after_team_detail_exit(const app_config &cfg) {
  return std::chrono::milliseconds(
      std::max(cfg.tap_delay_ms * 2, cfg.action_gap_ms));
}

double roi_fill_heuristic(const cv::Mat &roi_bgr) {
  if (roi_bgr.empty()) {
    return 0.0;
  }
  cv::Mat hsv;
  cv::cvtColor(roi_bgr, hsv, cv::COLOR_BGR2HSV);
  std::vector<cv::Mat> ch;
  cv::split(hsv, ch);
  cv::Scalar m = cv::mean(ch[1]);
  return std::clamp(m[0] / 255.0, 0.0, 1.0);
}

} // namespace

} // namespace campcat

namespace campcat_stzb {

namespace {

std::string state_label(team_recruit_state _s) {
  switch (_s) {
  case team_recruit_state::unknown:
    return "unknown";
  case team_recruit_state::idle:
    return "idle";
  case team_recruit_state::in_progress:
    return "in_progress";
  case team_recruit_state::full:
    return "full";
  }
  return "unknown";
}

} // namespace

using namespace campcat;

campcat_stzb_auto_assemble::campcat_stzb_auto_assemble(
    adb_client *_adb, const app_config *_cfg,
    const stzb_auto_assemble_profile *_profile, game_automation::log_fn _log)
    : m_adb(_adb), m_cfg(_cfg), m_profile(_profile), m_log(std::move(_log)),
      m_matcher(_cfg ? _cfg->match_threshold : 0.82,
                _cfg && _cfg->match_multiscale) {
  if (!m_adb || !m_cfg || !m_profile) {
    throw std::invalid_argument(
        "campcat_stzb_auto_assemble requires non-null adb, shell config and "
        "stzb_auto_assemble_profile");
  }
}

std::filesystem::path
campcat_stzb_auto_assemble::resolve_template(const std::string &_relative_name) const {
  return m_profile->template_resolution_dir() / _relative_name;
}

bool campcat_stzb_auto_assemble::snapshot_match_relative_template(
    const std::filesystem::path &resolved_png, cv::Rect roi, double threshold,
    match_result *out_mr) const {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(resolved_png, ec)) {
    return false;
  }
  cv::Mat screen;
  if (!m_adb->screencap_png(&screen)) {
    return false;
  }
  const auto mr =
      m_matcher.match_file(screen, resolved_png.string(), threshold, roi);
  if (!mr || !mr->found) {
    return false;
  }
  if (out_mr) {
    *out_mr = *mr;
  }
  return true;
}

bool campcat_stzb_auto_assemble::wait_for_template(
    const std::string &logical_template_key, cv::Rect roi,
    std::chrono::milliseconds timeout,
    const std::function<bool()> &should_stop, match_result *out_match) {
  const std::string &rel = m_profile->tmpl(logical_template_key);
  if (rel.empty()) {
    std::ostringstream oss;
    oss << "[fsm] template key \"" << logical_template_key << "\" not set";
    m_log(oss.str());
    return false;
  }
  return wait_for_relative_template(rel, roi, timeout, should_stop,
                                    out_match);
}

bool campcat_stzb_auto_assemble::wait_for_relative_template(
    const std::string &relative_png, cv::Rect roi,
    std::chrono::milliseconds timeout,
    const std::function<bool()> &should_stop, match_result *out_match) {
  const auto path = resolve_template(relative_png);
  if (!std::filesystem::exists(path)) {
    std::ostringstream oss;
    oss << "[fsm] template missing, skipping wait: " << path.string();
    m_log(oss.str());
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!should_stop() && std::chrono::steady_clock::now() < deadline) {
    cv::Mat screen;
    if (!m_adb->screencap_png(&screen)) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(m_cfg->step_retry_interval_ms));
      continue;
    }

    const auto mr = m_matcher.match_file(screen, path.string(),
                                         m_cfg->match_threshold, roi);
    if (mr && mr->found) {
      if (out_match) {
        *out_match = *mr;
      }
      return true;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(m_cfg->step_retry_interval_ms));
  }
  return false;
}

bool campcat_stzb_auto_assemble::dismiss_notice_loop(const std::function<bool()> &should_stop) {
  if (m_profile->tmpl("dismiss_notice").empty()) {
    return true;
  }
  const auto path = resolve_template(m_profile->tmpl("dismiss_notice"));
  if (!std::filesystem::exists(path)) {
    m_log("[fsm] dismiss_notice template not found; skipping popup sweep");
    return true;
  }

  for (int i = 0; i < m_cfg->max_step_retries && !should_stop(); ++i) {
    cv::Mat screen;
    if (!m_adb->screencap_png(&screen)) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(m_cfg->step_retry_interval_ms));
      continue;
    }

    const auto mr =
        m_matcher.match_file(screen, path.string(), m_cfg->match_threshold, {});
    if (!mr || !mr->found) {
      return true;
    }

    match_result tap_mr{};
    if (!snapshot_match_relative_template(path, {}, m_cfg->match_threshold,
                                          &tap_mr)) {
      return true;
    }

    std::ostringstream oss;
    oss << "[fsm] closing popup (match=" << std::fixed << std::setprecision(3)
        << tap_mr.confidence << ")";
    m_log(oss.str());

    if (!m_adb->tap(tap_mr.center.x, tap_mr.center.y)) {
      m_log("[fsm] tap failed during popup dismiss");
      return false;
    }
    m_adb->delay_after_action(pause_after_tap(*m_cfg));
  }

  m_log("[fsm] dismiss_notice loop exceeded retries");
  return true;
}

bool campcat_stzb_auto_assemble::retreat_to_main_ui(const std::function<bool()> &should_stop) {
  const auto &gm_rel = m_profile->tmpl("game_main");
  if (gm_rel.empty()) {
    m_log("[fsm] game_main unset; skip retreat-to-main");
    return true;
  }

  const auto gm_path = resolve_template(gm_rel);
  if (!std::filesystem::exists(gm_path)) {
    m_log("[fsm] game_main template missing; skip retreat-to-main");
    return true;
  }

  auto game_main_visible = [&]() -> bool {
    cv::Mat screen;
    if (!m_adb->screencap_png(&screen)) {
      return false;
    }
    if (const auto mr = m_matcher.match_file(screen, gm_path.string(),
                                             m_cfg->match_threshold, {})) {
      return mr->found;
    }
    return false;
  };

  m_log("[fsm] retreat_to_main_ui: notices / back buttons until game_main");

  if (!dismiss_notice_loop(should_stop)) {
    return false;
  }
  if (should_stop()) {
    return false;
  }
  if (game_main_visible()) {
    m_log("[fsm] already on main UI (game_main)");
    return true;
  }

  constexpr int k_passes = 12;
  for (int pass = 0; pass < k_passes && !should_stop(); ++pass) {
    if (!tap_recruit_back_until_gone(should_stop)) {
      m_log("[fsm] retreat_to_main_ui: back sweep failed");
      return false;
    }
    if (!dismiss_notice_loop(should_stop)) {
      return false;
    }
    if (game_main_visible()) {
      m_log("[fsm] main UI reached (game_main) after pass " +
            std::to_string(pass));
      return true;
    }
    m_adb->delay_after_action(
        std::chrono::milliseconds(m_cfg->step_retry_interval_ms));
  }

  m_log("[fsm] retreat_to_main_ui: game_main not detected after retries");
  return false;
}

bool campcat_stzb_auto_assemble::ensure_main_ui(const std::function<bool()> &should_stop) {
  const auto &gm_rel = m_profile->tmpl("game_main");
  if (!gm_rel.empty()) {
    const auto gm_path = resolve_template(gm_rel);
    if (std::filesystem::exists(gm_path)) {
      const bool ok = wait_for_template("game_main", {},
                                        std::chrono::seconds(20), should_stop,
                                        nullptr);
      if (!ok || should_stop()) {
        m_log("[fsm] game_main not detected within timeout");
        return false;
      }
      m_log("[fsm] main UI confirmed (game_main)");
      return true;
    }
  }

  if (m_profile->tmpl("main_menu_anchor").empty()) {
    m_log("[fsm] main_menu_anchor not configured; assuming already in game");
    return true;
  }

  const auto anchor_path = resolve_template(m_profile->tmpl("main_menu_anchor"));
  if (!std::filesystem::exists(anchor_path)) {
    m_log("[fsm] main_menu_anchor template missing; skipping detection");
    return true;
  }

  const bool ok =
      wait_for_template("main_menu_anchor", {}, std::chrono::seconds(20),
                        should_stop, nullptr);
  if (!ok || should_stop()) {
    m_log("[fsm] main UI anchor not detected within timeout");
    return false;
  }

  m_log("[fsm] main UI anchor detected");
  return true;
}

bool campcat_stzb_auto_assemble::navigate_to_main_city(const std::function<bool()> &should_stop) {
  const auto &loc = m_profile->tmpl("enter_city_location");
  const auto &city = m_profile->tmpl("enter_city_city");
  const auto &detail = m_profile->tmpl("enter_city_detail");

  if (loc.empty() && city.empty() && detail.empty()) {
    m_log("[fsm] enter-city templates not configured; skipping navigation");
    return true;
  }
  if (loc.empty() || city.empty() || detail.empty()) {
    m_log("[fsm] enter-city incomplete: need enter_city_location, "
          "enter_city_city, "
          "enter_city_detail");
    return false;
  }

  auto tap_step = [&](const std::string &relative_png,
                      const char *step_label) -> bool {
    const auto path = resolve_template(relative_png);
    if (!std::filesystem::exists(path)) {
      std::ostringstream oss;
      oss << "[fsm] enter-city template missing (" << step_label
          << "): " << path.string();
      m_log(oss.str());
      return false;
    }

    match_result mr{};
    const bool ok = wait_for_relative_template(
        relative_png, {}, std::chrono::seconds(25), should_stop, &mr);
    if (!ok || should_stop()) {
      std::ostringstream oss;
      oss << "[fsm] enter-city: '" << step_label << "' not found in time";
      m_log(oss.str());
      return false;
    }

    if (!snapshot_match_relative_template(path, {}, m_cfg->match_threshold,
                                          &mr)) {
      std::ostringstream oss;
      oss << "[fsm] enter-city: '" << step_label
          << "' not visible at tap time (stale wait)";
      m_log(oss.str());
      return false;
    }

    if (!m_adb->tap(mr.center.x, mr.center.y)) {
      std::ostringstream oss;
      oss << "[fsm] enter-city tap failed at step " << step_label;
      m_log(oss.str());
      return false;
    }
    m_adb->delay_after_action(pause_after_tap(*m_cfg));
    std::ostringstream oss;
    oss << "[fsm] enter-city: tapped " << step_label << " (match=" << std::fixed
        << std::setprecision(3) << mr.confidence << ")";
    m_log(oss.str());
    return true;
  };

  if (!tap_step(loc, "location")) {
    return false;
  }
  if (!tap_step(city, "city")) {
    return false;
  }
  if (!tap_step(detail, "city_detail")) {
    return false;
  }

  m_log("[fsm] navigated to main city");
  return true;
}

bool campcat_stzb_auto_assemble::inspect_team_recruitment_slot(
    size_t team_index, const std::function<bool()> &should_stop,
    team_recruit_status *out_status) {
  if (out_status) {
    *out_status = team_recruit_status{};
  }

  cv::Mat main_screen;
  if (!m_adb->screencap_png(&main_screen)) {
    m_log("[fsm] screencap failed before opening team slot");
    return false;
  }

  const cv::Rect roi_px =
      norm_rect_px(main_screen.size(), m_profile->team_rois[team_index]);
  const cv::Point tap_pt(roi_px.x + roi_px.width / 2,
                         roi_px.y + roi_px.height / 2);

  if (!m_adb->tap(tap_pt.x, tap_pt.y)) {
    std::ostringstream oss;
    oss << "[fsm] team " << (team_index + 1) << ": tap slot center failed";
    m_log(oss.str());
    return false;
  }
  m_adb->delay_after_action(pause_after_tap(*m_cfg));

  bool on_detail = false;
  const auto &rd = m_profile->tmpl("recruit_detail");
  if (!rd.empty()) {
    const auto path_rd = resolve_template(rd);
    if (std::filesystem::exists(path_rd)) {
      on_detail =
          wait_for_relative_template(rd, {}, std::chrono::seconds(12),
                                    should_stop, nullptr);
    } else {
      std::ostringstream oss;
      oss << "[fsm] team " << (team_index + 1)
          << ": recruit_detail missing on disk (" << path_rd.string() << ")";
      m_log(oss.str());
    }
  } else {
    m_log("[fsm] recruit_detail not configured; cannot verify detail page");
  }

  {
    std::ostringstream oss;
    oss << "[fsm] team " << (team_index + 1) << ": recruitment detail "
        << (on_detail ? "OPEN (recruit_detail matched)"
                      : "NOT confirmed (no template match)");
    m_log(oss.str());
  }

  if (out_status && on_detail) {
    cv::Mat detail_screen;
    if (m_adb->screencap_png(&detail_screen)) {
      const rect_norm full_viewport{0.0, 0.0, 1.0, 1.0};

      const auto &asm_rel = m_profile->tmpl("assemble");
      if (!asm_rel.empty()) {
        const auto path_asm = resolve_template(asm_rel);
        if (std::filesystem::exists(path_asm)) {
          match_result asm_mr{};
          if (snapshot_match_relative_template(path_asm, {},
                                               m_cfg->match_threshold,
                                               &asm_mr)) {
            if (m_adb->tap(asm_mr.center.x, asm_mr.center.y)) {
              m_log("set to auto assemble.");
              m_adb->delay_after_action(pause_after_tap(*m_cfg));
              (void)m_adb->screencap_png(&detail_screen);
            }
          }
        }
      }

      *out_status = classify_team_roi(detail_screen, full_viewport);
    }
  }

  if (on_detail) {
    bool closed = false;
    const auto path_ok = [&](const std::string &rel) -> bool {
      if (rel.empty()) {
        return false;
      }
      return std::filesystem::exists(resolve_template(rel));
    };
    const bool backs_on_disk =
        path_ok(m_profile->tmpl("recruit_back")) ||
        path_ok(m_profile->tmpl("recruit_back_2"));

    if (backs_on_disk) {
      auto tap_back_if_visible = [&]() -> bool {
        cv::Mat s1;
        if (!m_adb->screencap_png(&s1)) {
          return false;
        }
        match_result mr1{};
        if (!pick_best_recruit_back(s1, m_cfg->match_threshold, &mr1)) {
          return false;
        }
        cv::Mat s2;
        if (!m_adb->screencap_png(&s2)) {
          return false;
        }
        match_result mr2{};
        if (!pick_best_recruit_back(s2, m_cfg->match_threshold, &mr2)) {
          return false;
        }
        return m_adb->tap(mr2.center.x, mr2.center.y);
      };

      closed = tap_back_if_visible();

      if (!closed) {
        bool anchor_still_visible = true;
        const auto &rd_name = rd;
        if (!rd_name.empty()) {
          const auto path_rd = resolve_template(rd_name);
          cv::Mat anchor_snap;
          if (std::filesystem::exists(path_rd) &&
              m_adb->screencap_png(&anchor_snap)) {
            anchor_still_visible = false;
            if (const auto rd_hit = m_matcher.match_file(
                    anchor_snap, path_rd.string(), m_cfg->match_threshold,
                    {})) {
              anchor_still_visible = rd_hit->found;
            }
          }
        }

        if (!anchor_still_visible) {
          closed = tap_back_if_visible();
          if (!closed) {
            m_log("[fsm] recruit_detail anchor gone before back "
                  "(e.g. closed by assemble); skip back wait");
            closed = true;
          }
        } else {
          if (wait_for_recruit_back_visible(std::chrono::seconds(12),
                                            should_stop, nullptr)) {
            cv::Mat s_tap;
            if (m_adb->screencap_png(&s_tap)) {
              match_result mr_tap{};
              if (pick_best_recruit_back(s_tap, m_cfg->match_threshold,
                                         &mr_tap)) {
                closed = m_adb->tap(mr_tap.center.x, mr_tap.center.y);
              }
            }
          }
        }
      }

      if (!closed) {
        m_log("[fsm] recruit_back / recruit_back_2 not matched / tap failed; "
              "no fallback (exit manually if stuck on detail)");
      }
    }
    m_adb->delay_after_action(pause_after_team_detail_exit(*m_cfg));
  } else {
    m_adb->delay_after_action(std::chrono::milliseconds(m_cfg->action_gap_ms));
  }

  return true;
}

team_recruit_status campcat_stzb_auto_assemble::classify_team_roi(const cv::Mat &screen,
                                                const rect_norm &rn) {
  team_recruit_status st{};
  const cv::Rect roi_px = norm_rect_px(screen.size(), rn);
  const cv::Mat roi = screen(roi_px);

  const double h = roi_fill_heuristic(roi);
  st.progress_estimate = h;
  if (h < 0.12) {
    st.state = team_recruit_state::idle;
  } else {
    st.state = team_recruit_state::unknown;
  }
  return st;
}

bool campcat_stzb_auto_assemble::pick_best_recruit_back(const cv::Mat &snap, double threshold,
                                      match_result *out_mr) const {
  match_result best{};
  bool any = false;
  auto consider = [&](const std::string &rel) {
    if (rel.empty()) {
      return;
    }
    const auto p = resolve_template(rel);
    if (!std::filesystem::exists(p)) {
      return;
    }
    if (const auto mr = m_matcher.match_file(snap, p.string(), threshold, {})) {
      if (mr->found && (!any || mr->confidence > best.confidence)) {
        best = *mr;
        any = true;
      }
    }
  };
  consider(m_profile->tmpl("recruit_back"));
  consider(m_profile->tmpl("recruit_back_2"));
  if (any && out_mr) {
    *out_mr = best;
  }
  return any;
}

bool campcat_stzb_auto_assemble::wait_for_recruit_back_visible(
    std::chrono::milliseconds timeout,
    const std::function<bool()> &should_stop, match_result *out_match) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!should_stop() && std::chrono::steady_clock::now() < deadline) {
    cv::Mat screen;
    if (!m_adb->screencap_png(&screen)) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(m_cfg->step_retry_interval_ms));
      continue;
    }
    match_result mr{};
    if (pick_best_recruit_back(screen, m_cfg->match_threshold, &mr)) {
      if (out_match) {
        *out_match = mr;
      }
      return true;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(m_cfg->step_retry_interval_ms));
  }
  return false;
}

bool campcat_stzb_auto_assemble::tap_recruit_back_until_gone(
    const std::function<bool()> &should_stop) {
  const auto path_ok = [&](const std::string &rel) -> bool {
    if (rel.empty()) {
      return false;
    }
    return std::filesystem::exists(resolve_template(rel));
  };
  const bool have_b1 = path_ok(m_profile->tmpl("recruit_back"));
  const bool have_b2 = path_ok(m_profile->tmpl("recruit_back_2"));

  if (!have_b1 && !have_b2) {
    if (m_profile->tmpl("recruit_back").empty() &&
        m_profile->tmpl("recruit_back_2").empty()) {
      m_log("[fsm] recruit_back / recruit_back_2 unset; skip back-to-main sweep");
    } else {
      m_log("[fsm] recruit_back / recruit_back_2 missing on disk; skip "
            "back-to-main sweep");
    }
    return true;
  }

  // Looser than cfg match_threshold so returning screens still pick up backs.
  constexpr double k_back_sweep_threshold = 0.6;
  constexpr int k_max_steps = 32;
  int taps = 0;

  while (!should_stop()) {
    cv::Mat snap;
    if (!m_adb->screencap_png(&snap)) {
      m_log("[fsm] screencap failed during back-to-main sweep");
      return false;
    }

    if (!pick_best_recruit_back(snap, k_back_sweep_threshold, nullptr)) {
      if (taps > 0) {
        m_log("[fsm] back buttons gone after " + std::to_string(taps) +
              " tap(s); assuming main UI");
      } else {
        m_log("[fsm] back buttons not visible after team loop (already main?)");
      }
      return true;
    }

    if (taps >= k_max_steps) {
      m_log("[fsm] back-to-main sweep: back still matches after " +
            std::to_string(k_max_steps) + " taps; giving up");
      return false;
    }

    cv::Mat pre_tap;
    if (!m_adb->screencap_png(&pre_tap)) {
      m_log("[fsm] screencap failed before verified back tap");
      return false;
    }
    match_result tap_mr{};
    if (!pick_best_recruit_back(pre_tap, k_back_sweep_threshold, &tap_mr)) {
      if (taps > 0) {
        m_log("[fsm] back buttons gone before tap; assuming main UI");
        return true;
      }
      m_log("[fsm] back buttons not visible before tap (unexpected)");
      return true;
    }

    if (!m_adb->tap(tap_mr.center.x, tap_mr.center.y)) {
      m_log("[fsm] back tap failed during back-to-main sweep");
      return false;
    }
    ++taps;
    (void)m_adb->delay_after_action(pause_after_tap(*m_cfg));
  }

  m_log("[fsm] back-to-main sweep aborted (stop requested)");
  return false;
}

campcat::automation_cycle_result
campcat_stzb_auto_assemble::run_cycle(const std::function<bool()> &should_stop) {
  campcat::automation_cycle_result result{};

  m_log("[fsm] --- cycle start ---");
  if (should_stop()) {
    result.message = "stopped before start";
    return result;
  }

  if (!retreat_to_main_ui(should_stop)) {
    result.message = "retreat to main UI failed";
    m_log("[fsm] cycle aborted: " + result.message);
    return result;
  }

  if (!dismiss_notice_loop(should_stop)) {
    result.message = "dismiss_notice failed";
    m_log("[fsm] cycle aborted: " + result.message);
    return result;
  }

  if (!ensure_main_ui(should_stop)) {
    result.message = "main UI not detected";
    m_log("[fsm] cycle aborted: " + result.message);
    return result;
  }

  if (!navigate_to_main_city(should_stop)) {
    result.message = "main city navigation failed";
    m_log("[fsm] cycle aborted: " + result.message);
    return result;
  }

  (void)m_adb->delay_after_action(
      std::chrono::milliseconds(m_cfg->action_gap_ms));

  const size_t n_teams = static_cast<size_t>(std::clamp(
      m_profile->team_count, 1,
      static_cast<int>(m_profile->team_rois.size())));
  m_log("[fsm] team slots to inspect: " + std::to_string(n_teams));

  for (size_t i = 0; i < n_teams; ++i) {
    if (should_stop()) {
      result.message = "stopped during team inspection";
      m_log("[fsm] cycle aborted: " + result.message);
      return result;
    }

    team_recruit_status st{};
    if (!inspect_team_recruitment_slot(i, should_stop, &st)) {
      result.message = "team inspection failed (adb)";
      m_log("[fsm] cycle aborted: " + result.message);
      return result;
    }

    std::ostringstream oss;
    oss << "[fsm] team " << (i + 1)
        << " classification: " << state_label(st.state)
        << " (~p=" << std::fixed << std::setprecision(2)
        << st.progress_estimate << ")";
    m_log(oss.str());
  }

  if (should_stop()) {
    result.message = "stopped before back-to-main";
    m_log("[fsm] cycle aborted: " + result.message);
    return result;
  }

  if (!tap_recruit_back_until_gone(should_stop)) {
    result.message = "back-to-main sweep failed";
    m_log("[fsm] cycle aborted: " + result.message);
    return result;
  }

  result.ok = true;
  result.message = "ok";
  m_log("[fsm] --- cycle end ---");
  return result;
}

} // namespace campcat_stzb
