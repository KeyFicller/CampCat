#pragma once

#include <chrono>
#include <filesystem>
#include <functional>

#include <opencv2/core.hpp>

#include "core/automation/game_automation.h"
#include "core/app_config.h"
#include "core/template_matcher.h"

#include "games/stzb_auto_assemble/stzb_team_types.h"
#include "games/stzb_auto_assemble/stzb_auto_assemble_profile.h"

namespace campcat_stzb {

/**
 * @brief Rate game assemble automation; uses shell `campcat::app_config` plus a
 * separate `campcat::stzb_auto_assemble_profile` for templates/bundle paths.
 */
class campcat_stzb_auto_assemble final : public ::campcat::game_automation {
public:
  explicit campcat_stzb_auto_assemble(
      ::campcat::adb_client *adb, const ::campcat::app_config *cfg,
      const ::campcat::stzb_auto_assemble_profile *profile,
      ::campcat::game_automation::log_fn log);

  ::campcat::automation_cycle_result
  run_cycle(const std::function<bool()> &should_stop) override;

  bool navigate_to_main_city(const std::function<bool()> &should_stop);

private:
  bool wait_for_template(const std::string &logical_template_key,
                         cv::Rect roi, std::chrono::milliseconds timeout,
                         const std::function<bool()> &should_stop,
                         ::campcat::match_result *out_match);

  bool wait_for_relative_template(const std::string &relative_filename,
                                  cv::Rect roi,
                                  std::chrono::milliseconds timeout,
                                  const std::function<bool()> &should_stop,
                                  ::campcat::match_result *out_match);

  bool dismiss_notice_loop(const std::function<bool()> &should_stop);

  bool retreat_to_main_ui(const std::function<bool()> &should_stop);

  bool ensure_main_ui(const std::function<bool()> &should_stop);

  bool inspect_team_recruitment_slot(
      size_t team_index, const std::function<bool()> &should_stop,
      team_recruit_status *out_status);

  team_recruit_status classify_team_roi(const cv::Mat &screen,
                                       const ::campcat::rect_norm &rn);

  bool tap_recruit_back_until_gone(const std::function<bool()> &should_stop);

  bool pick_best_recruit_back(const cv::Mat &snap, double threshold,
                              ::campcat::match_result *out_mr) const;

  bool wait_for_recruit_back_visible(
      std::chrono::milliseconds timeout,
      const std::function<bool()> &should_stop,
      ::campcat::match_result *out_match);

  std::filesystem::path
  resolve_template(const std::string &relative_name) const;

  /**
   * @brief Capture current screen then match `resolved_png` (must exist on disk).
   * @return True when screencap and correlation succeed above threshold.
   */
  bool snapshot_match_relative_template(
      const std::filesystem::path &resolved_png, cv::Rect roi,
      double threshold, ::campcat::match_result *out_mr) const;

  ::campcat::adb_client *m_adb;
  const ::campcat::app_config *m_cfg;
  const ::campcat::stzb_auto_assemble_profile *m_profile;
  ::campcat::game_automation::log_fn m_log;
  ::campcat::template_matcher m_matcher;
};

} // namespace campcat_stzb
