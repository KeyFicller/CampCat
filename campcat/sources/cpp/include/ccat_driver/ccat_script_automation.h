#pragma once

#include "core/automation/game_automation.h"

namespace campcat {

class adb_api;
struct app_config;
class ccat_script_profile;

/**
 * @brief game_automation specialization executing `.ccat` DSL programs each
 * worker tick.
 */
class ccat_script_automation final : public game_automation {
public:
    /**
     * @brief Capture immutable pointers reused for entire automation lifetime
     * (must outlive instance).
     * @param[in] _adb adb facade performing IO-bound gestures/screenshots.
     * @param[in] _cfg Shell knobs forwarded into interpreter layers
     * (thresholds/pacing).
     * @param[in] _profile Loaded bundle describing `.ccat` location plus PNG
     * roots.
     * @param[in] _ask Answers the script's `ask(...)`; empty means nobody can,
     * which stops the script there.
     */
    explicit ccat_script_automation(adb_api *_adb, const app_config *_cfg,
                                    const ccat_script_profile *_profile,
                                    ccat_lang::ask_handler _ask = {});

    /**
     * @brief Reload `.ccat` bytes from disk, parse once, interpret until
     * completion/stop/failure.
     * @param[in] _should_stop Cooperative cancellation mirrored from UI/worker
     * atomic flags.
     * @return Aggregate automation_cycle_result for Dear ImGui / log telemetry.
     */
    automation_cycle_result
    run_cycle(const std::function<bool()> &_should_stop) override;

private:
    adb_api *m_adb;
    const app_config *m_cfg;
    const ccat_script_profile *m_profile;
    ccat_lang::ask_handler m_ask;
};

} // namespace campcat
