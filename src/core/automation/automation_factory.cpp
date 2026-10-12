#include "core/automation/game_automation.h"

#include <memory>
#include <string>

#include "ccat_driver/ccat_script_automation.h"
#include "ccat_driver/ccat_script_profile.h"
#include "core/adb_client.h"
#include "core/app_config.h"

namespace campcat {

namespace {

class unknown_automation_game final : public game_automation {
    std::string m_msg;

public:
    explicit unknown_automation_game(const std::string &msg) : m_msg(msg) {}

    automation_cycle_result run_cycle(const std::function<bool()> &) override {
        automation_cycle_result r{};
        r.ok = false;
        r.message = m_msg;
        return r;
    }
};

} // namespace

std::unique_ptr<game_automation>
make_game_automation(adb_api *adb, const app_config *cfg,
                     const ccat_script_profile *script_bundle) {
    if (!adb || !cfg) {
        return std::make_unique<unknown_automation_game>(
            "adb_client or app_config missing");
    }
    if (!script_bundle) {
        return std::make_unique<unknown_automation_game>(
            "missing ccat_script bundle JSON (load or save profile)");
    }
    return std::make_unique<ccat_script_automation>(adb, cfg, script_bundle);
}

} // namespace campcat
