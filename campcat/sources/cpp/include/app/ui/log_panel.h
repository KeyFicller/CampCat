#pragma once

namespace campcat {
class shell_state;
} // namespace campcat

/**
 * @brief Draw the Log dock window: the icon-button header, then the lines.
 *
 * Folds newly emitted lines into what it shows, so a collapsed log still drains
 * the buffer instead of letting it grow. The chevron only shows or hides the
 * lines; the dock split is what gives the window its height.
 *
 * @param[in,out] _st Shell state: the log buffer and the on-screen lines.
 */
void log_panel_draw(campcat::shell_state &_st);
