#pragma once

namespace campcat {
struct app_config;
}

/**
 * @brief Release GL texture owned by the LLM page (call before GL teardown).
 */
void llm_ui_shutdown_gl();

/**
 * @brief Draw the LLM page: capture/pick a screenshot, preview it, request a
 * description from the configured endpoint.
 *
 * @param[in,out] _cfg Shell snapshot (adb paths + llm settings).
 * @param[in] _disable_capture Disable the ADB capture button while automation
 * owns ADB.
 */
void llm_ui_draw_panel(campcat::app_config &_cfg, bool _disable_capture);
