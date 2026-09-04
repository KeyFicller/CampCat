#pragma once

#include "core/automation/game_automation.h"
#include "core/script/ccat_ast.h"

#include <filesystem>
#include <functional>

namespace campcat {

class adb_client;
struct app_config;

/**
 * @brief Run an already-parsed Program with the same interpreter wiring as file cycles.
 */
automation_cycle_result run_ccat_program(
    adb_client *_adb, const app_config *_cfg,
    const std::filesystem::path &_images_base,
    const std::filesystem::path &_script_path,
    const ccat_lang::Program &_program,
    const std::function<bool()> &_should_stop);

} // namespace campcat
