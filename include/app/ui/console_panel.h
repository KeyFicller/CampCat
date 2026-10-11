#pragma once

struct ImFont; // only ever held as a pointer here, so imgui.h stays out

namespace campcat {
class shell_state;
struct app_config;
struct ccat_script_profile;
} // namespace campcat

/**
 * @brief Draw the Console page: the .ccat history, the input box, Clear.
 *
 * @param[in,out] _st Shell state: the run job and the log.
 * @param[in,out] _cfg_view This frame's config view, committed before a run.
 * @param[in] _ccat_ui This frame's .ccat view, read for `has_script_source`.
 * @param[in] _busy_now A cycle is running, so the input and Clear are disabled.
 * @param[in] _font_bold Bold face for the empty-state title.
 */
void console_panel_draw(campcat::shell_state &_st, campcat::app_config &_cfg_view,
                       const campcat::ccat_script_profile &_ccat_ui,
                       bool _busy_now, ImFont *_font_bold);
