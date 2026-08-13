#pragma once

namespace campcat {

struct app_config;
struct ccat_script_profile;

} // namespace campcat

/**
 * @brief Release GL texture used by the template capture panel (call before GL context teardown).
 */
void template_capture_shutdown_gl();

/**
 * @brief CampCat-only: ADB screenshot + drag ROI → save PNG beside the `.ccat`.
 *
 * @param[in,out] cfg Shell snapshot (adb paths, config_home).
 * @param[in,out] ccat_ui Bundle with script source; PNG dir is the script folder.
 * @param[in] disable_capture When true, Capture button is disabled (automation busy).
 *
 * Log lines use campcat::automation_log::emit (shell sets sink in main).
 */
void template_capture_draw_panel(campcat::app_config &cfg,
                                 campcat::ccat_script_profile *ccat_ui,
                                 bool disable_capture);
