#include "core/automation/game_automation.h"

#include <memory>
#include <string>

#include "core/adb_client.h"
#include "core/app_config.h"
#include "games/ccat_script/ccat_script_automation.h"
#include "games/ccat_script/ccat_script_profile.h"
#include "games/stzb_auto_assemble/campcat_stzb_auto_assemble.h"
#include "games/stzb_auto_assemble/stzb_auto_assemble_profile.h"

namespace campcat {

namespace {

class unknown_automation_game final : public game_automation {
  std::string m_msg;

public:
  explicit unknown_automation_game(std::string msg)
      : m_msg(std::move(msg)) {}

  automation_cycle_result
  run_cycle(const std::function<bool()> &) override {
    automation_cycle_result r{};
    r.ok = false;
    r.message = m_msg;
    return r;
  }
};

constexpr const char k_stzb_bundle_id[] = "stzb_auto_assemble";
constexpr const char k_ccat_bundle_id[] = "ccat_script";

} // namespace

std::unique_ptr<game_automation>
make_game_automation(adb_client *adb, const app_config *cfg,
                     const automation_profile *script_bundle,
                     game_automation::log_fn log) {
  if (!adb || !cfg) {
    return std::make_unique<unknown_automation_game>(
        "adb_client or app_config missing");
  }

  const std::string &sid = cfg->active_script_id;
  if (sid.empty() || sid == "none") {
    return std::make_unique<unknown_automation_game>(
        "no script selected (active_script='" + sid + "')");
  }

  if (sid == k_ccat_bundle_id) {
    const auto *typed =
        script_bundle ? dynamic_cast<const ccat_script_profile *>(script_bundle)
                      : nullptr;
    if (!typed) {
      return std::make_unique<unknown_automation_game>(
          "missing ccat_script bundle JSON (load or save profile)");
    }
    return std::make_unique<ccat_script_automation>(adb, cfg, typed,
                                                     std::move(log));
  }

  if (sid == k_stzb_bundle_id) {
    const auto *typed =
        script_bundle
            ? dynamic_cast<const stzb_auto_assemble_profile *>(script_bundle)
            : nullptr;
    if (!typed) {
      return std::make_unique<unknown_automation_game>(
          "missing typed script bundle for active script (load or save profile JSON)");
    }

    return std::make_unique<::campcat_stzb::campcat_stzb_auto_assemble>(
        adb, cfg, typed, std::move(log));
  }

  return std::make_unique<unknown_automation_game>("unsupported script: " +
                                                   sid);
}

} // namespace campcat
