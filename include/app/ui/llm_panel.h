#pragma once

#include "app/ui/activity_status.h"

struct ImFont; // only ever held as a pointer here, so imgui.h stays out

namespace campcat {
class shell_state;
struct app_config;
}

/**
 * @brief Hand the panel the bold font, used to render `**strong**` runs.
 *
 * Call once after the fonts are uploaded. Passing null makes bold fall back to
 * the body font instead of crashing.
 */
void llm_panel_set_bold_font(ImFont *_font);

/**
 * @brief Stop the LLM page's worker and sidecar (call before GL teardown, with
 * the other panels' shutdowns).
 *
 * Starts the last memory consolidation but does not wait for it: the wait would
 * otherwise block while the window is still up. `llm_panel_shutdown_finish` is what
 * waits, and it must be called after the window is destroyed.
 */
void llm_panel_shutdown_gl();

/**
 * @brief Let the shutdown consolidation finish, then stop the sidecar.
 *
 * Call after `glfwDestroyWindow`, so the wait happens with nothing on screen to
 * freeze. Without it the sidecar is killed with the write still in flight.
 */
void llm_panel_shutdown_finish();

/**
 * @brief Draw the LLM page: type an instruction, watch the agent work.
 *
 * @param[in,out] _st Shell state, which owns the emulator launch the tools may
 * ask for.
 * @param[in,out] _cfg Shell snapshot (adb paths + llm settings).
 * @param[in] _adb_busy True while automation owns ADB, which makes the device
 * tools refuse instead of contending with it.
 */
void llm_panel_draw_panel(campcat::shell_state &_st, campcat::app_config &_cfg,
                          bool _adb_busy);

/**
 * @brief What the LLM page is doing, for the shell's status chip.
 *
 * UI thread only: it reads the same flags the page's own draw reads, and `main`
 * asks before this page has drawn for the frame.
 */
campcat::activity_status llm_panel_activity();

/**
 * @brief Draw the box-annotation window for a pending approval, if one is open.
 *
 * Called every frame regardless of the active page: it is a top-level float, and
 * the user may well switch pages mid-edit. Whether there is anything to show is
 * the window's own business.
 */
void llm_panel_draw_overlay();
