#pragma once

#include <functional>
#include <memory>
#include <string>

// Logging uses campcat::automation_log::emit from worker code paths.

namespace campcat {

class adb_client;
class automation_profile;
struct app_config;

/**
 * @brief Shell-level outcome of one automation tick (opaque to scripted payloads).
 */
struct automation_cycle_result {
  bool ok = false;
  std::string message;
};

/**
 * @brief Polymorphic hook representing one automation profile runnable per shell tick.
 */
class game_automation {
public:
  virtual ~game_automation() = default;

  /**
   * @brief Execute one scripted pass until completion or cooperative stop hooks.
   * @param[in] should_stop Predicate polled inside tight loops (e.g. atomic UI cancel).
   * @return Consolidated telemetry for Dear ImGui / logs.
   */
  virtual automation_cycle_result
  run_cycle(const std::function<bool()> &should_stop) = 0;
};

/**
 * @brief Factory wiring shell + optional script bundle into an automation driver.
 * @param[in] adb Live adb facade (must outlive automation instance usage).
 * @param[in] cfg Loaded shell persistence snapshot.
 * @param[in] script_bundle Concrete profile type decided by active script id; nullable.
 * @return Unique owning pointer honoring `cfg->active_script_id`.
 */
std::unique_ptr<game_automation>
make_game_automation(adb_client *adb, const app_config *cfg,
                     const automation_profile *script_bundle);

} // namespace campcat
