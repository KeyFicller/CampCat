#pragma once

namespace campcat {
class shell_state;
struct app_config;
} // namespace campcat

/**
 * @brief Draw the Settings page: timing and matching, ADB, and the emulator
 * launcher.
 *
 * @param[in,out] _st Shell state: the adb/mumu buffers and the launch job.
 * @param[in,out] _cfg_view This frame's config view.
 */
void settings_panel_draw(campcat::shell_state &_st,
                         campcat::app_config &_cfg_view);
