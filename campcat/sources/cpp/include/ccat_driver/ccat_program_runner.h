#pragma once

#include "core/automation/game_automation.h"
#include "core/script/ccat_ast.h"
#include "core/script/ccat_graph.h"

#include <filesystem>
#include <functional>
#include <string>

namespace campcat {

class adb_api;
struct app_config;

/**
 * @brief Run an already-parsed Program with the same interpreter wiring as file
 * cycles.
 */
automation_cycle_result
run_ccat_program(adb_api *_adb, const app_config *_cfg,
                 const std::filesystem::path &_images_base,
                 const std::filesystem::path &_script_path,
                 const ccat_lang::Program &_program,
                 const std::function<bool()> &_should_stop);

/// Which anchor of `_graph` `_frame` is, with the interpreter wiring above.
ccat_lang::locate_hit locate_on_frame(adb_api *_adb, const app_config *_cfg,
                                      const std::string &_graph,
                                      const cv::Mat &_frame, std::string *_err);

} // namespace campcat
