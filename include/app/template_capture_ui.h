#pragma once

#include <functional>
#include <string>

namespace campcat {

struct app_config;
struct stzb_auto_assemble_profile;
struct ccat_script_profile;

} // namespace campcat

/**
 * @brief Release GL texture used by the template capture panel (call before GL context teardown).
 */
void template_capture_shutdown_gl();

/**
 * @brief Draw ADB screenshot + drag-rect crop UI under the script bundle pane.
 * @param[in] stzb_mode When true, save under STZB resolution dir and optional slot assign.
 * @param[in,out] cfg Shell snapshot (adb paths, config_home, project root).
 * @param[in,out] stzb_ui Required when stzb_mode; templates map updated on assign save.
 * @param[in,out] ccat_ui Required when !stzb_mode.
 * @param[in] disable_capture When true, Capture button is disabled (automation busy).
 * @param[in] log_fn Lines appended to shell log (e.g. append_log).
 */
void template_capture_draw_panel(
    bool stzb_mode, campcat::app_config &cfg,
    campcat::stzb_auto_assemble_profile *stzb_ui,
    campcat::ccat_script_profile *ccat_ui, bool disable_capture,
    const std::function<void(std::string)> &log_fn);
