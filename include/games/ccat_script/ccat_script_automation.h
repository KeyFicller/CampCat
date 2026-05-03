#pragma once

#include "core/automation/game_automation.h"

namespace campcat {

class adb_client;
struct app_config;
class ccat_script_profile;

/**
 * @brief game_automation specialization executing `.ccat` DSL programs each worker tick.
 */
class ccat_script_automation final : public game_automation {
public:
  /**
   * @brief Capture immutable pointers reused for entire automation lifetime (must outlive instance).
   * @param[in] _adb adb_client performing IO-bound gestures/screenshots.
   * @param[in] _cfg Shell knobs forwarded into interpreter layers (thresholds/pacing).
   * @param[in] _profile Loaded bundle describing `.ccat` location plus PNG roots.
   * @param[in] _log Logging functor bridged into DSL `log(...)` plus interpreter diagnostics.
   */
  explicit ccat_script_automation(adb_client *_adb, const app_config *_cfg,
                                  const ccat_script_profile *_profile,
                                  log_fn _log);

  /**
   * @brief Reload `.ccat` bytes from disk, parse once, interpret until completion/stop/failure.
   * @param[in] _should_stop Cooperative cancellation mirrored from UI/worker atomic flags.
   * @return Aggregate automation_cycle_result identical semantics to STZB driver messaging.
   */
  automation_cycle_result
  run_cycle(const std::function<bool()> &_should_stop) override;

private:
  adb_client *m_adb;
  const app_config *m_cfg;
  const ccat_script_profile *m_profile;
  log_fn m_log;
};

} // namespace campcat
