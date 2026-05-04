#include "core/automation/game_automation.h"

#include "core/automation/automation_driver.h"

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
  explicit unknown_automation_game(const std::string &msg) : m_msg(msg) {}

  automation_cycle_result
  run_cycle(const std::function<bool()> &) override {
    automation_cycle_result r{};
    r.ok = false;
    r.message = m_msg;
    return r;
  }
};

class ccat_automation_driver final : public automation_driver {
public:
  std::string_view script_id() const noexcept override {
    return shell_script_id::k_ccat_script;
  }

  std::unique_ptr<game_automation>
  create(adb_client *adb, const app_config *cfg,
         const automation_profile *bundle) const override {
    const auto *typed =
        bundle ? dynamic_cast<const ccat_script_profile *>(bundle) : nullptr;
    if (!typed) {
      return std::make_unique<unknown_automation_game>(
          "missing ccat_script bundle JSON (load or save profile)");
    }
    return std::make_unique<ccat_script_automation>(adb, cfg, typed);
  }
};

class stzb_automation_driver final : public automation_driver {
public:
  std::string_view script_id() const noexcept override {
    return shell_script_id::k_stzb_auto_assemble;
  }

  std::unique_ptr<game_automation>
  create(adb_client *adb, const app_config *cfg,
         const automation_profile *bundle) const override {
    const auto *typed =
        bundle ? dynamic_cast<const stzb_auto_assemble_profile *>(bundle)
               : nullptr;
    if (!typed) {
      return std::make_unique<unknown_automation_game>(
          "missing typed script bundle for active script (load or save profile "
          "JSON)");
    }
    return std::make_unique<::campcat_stzb::campcat_stzb_auto_assemble>(
        adb, cfg, typed);
  }
};

const ccat_automation_driver g_ccat_driver{};
const stzb_automation_driver g_stzb_driver{};

const automation_driver *const k_drivers[] = {
    &g_ccat_driver,
    &g_stzb_driver,
};

} // namespace

std::unique_ptr<game_automation>
make_game_automation(adb_client *adb, const app_config *cfg,
                     const automation_profile *script_bundle) {
  if (!adb || !cfg) {
    return std::make_unique<unknown_automation_game>(
        "adb_client or app_config missing");
  }

  const std::string &sid = cfg->active_script_id;
  if (sid.empty() || sid == shell_script_id::k_none) {
    return std::make_unique<unknown_automation_game>(
        "no script selected (active_script='" + sid + "')");
  }

  for (const automation_driver *drv : k_drivers) {
    if (drv->supports(sid)) {
      return drv->create(adb, cfg, script_bundle);
    }
  }

  return std::make_unique<unknown_automation_game>("unsupported script: " +
                                                   sid);
}

} // namespace campcat
