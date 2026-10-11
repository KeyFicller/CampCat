#pragma once

namespace campcat {
class shell_state;
struct app_config;
struct ccat_script_profile;
} // namespace campcat

/**
 * @brief Draw the Script page: one cycle, stop, the scheduler, the active
 * script, and the .ccat paths plus screenshot crop for the picked script.
 *
 * @param[in,out] _st Shell state: jobs and the adb/mumu buffers.
 * @param[in,out] _cfg_view This frame's config view, holding the picked script
 * and the scheduler cadence the page edits.
 * @param[in,out] _ccat_ui This frame's .ccat view, holding `source_rel`.
 * @param[in] _lock_manual_single A cycle or the scheduler owns ADB: the actions
 * that start work are disabled and Stop is the only one left live. When false
 * Stop is moot, so it greys out instead.
 */
void script_panel_draw(campcat::shell_state &_st,
                       campcat::app_config &_cfg_view,
                       campcat::ccat_script_profile &_ccat_ui,
                       bool _lock_manual_single);
