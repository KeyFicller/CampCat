#pragma once

namespace campcat {
class shell_state;
} // namespace campcat

/**
 * @brief Height the collapsed log box occupies.
 *
 * The nav block above sizes itself to stop short of it. Derived, not measured:
 * the header is one framed row, so the box is `GetFrameHeight() +
 * WindowPadding.y * 2` -- 38.0 under the 16px base body font. The identity is
 * font independent, the absolute number is not: a 13px font gives 35.0. A
 * second row in the header would make this too small, and the collapsed log
 * would be pushed below the column.
 */
float log_panel_collapsed_height();

/**
 * @brief Whether the log is expanded.
 *
 * Read it before drawing the nav block: that block needs to know how much room
 * the log is going to take.
 */
bool log_panel_open();

/**
 * @brief Draw the log box: the icon-button header, then the lines.
 *
 * Folds newly emitted lines into what it shows, so a collapsed log still drains
 * the buffer instead of letting it grow. The expand/collapse toggle happens
 * inside the header.
 *
 * @param[in,out] _st Shell state: the log buffer and the on-screen lines.
 */
void log_panel_draw(campcat::shell_state &_st);
