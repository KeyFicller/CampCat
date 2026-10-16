#pragma once

namespace campcat {
class shell_state;
struct app_config;
} // namespace campcat

/**
 * @brief Draw the Console page: the .ccat history, the input box, Clear.
 *
 * With no bundle selected, the run still goes out; its paths resolve at
 * `config_home` (the `ccat_library` root).
 *
 * @param[in,out] _st Shell state: the run job and the log.
 * @param[in,out] _cfg_view This frame's config view, committed before a run.
 * @param[in] _busy_now A cycle is running, so the input and Clear are disabled.
 */
void console_panel_draw(campcat::shell_state &_st,
                        campcat::app_config &_cfg_view, bool _busy_now);
