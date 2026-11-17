#pragma once

#include <cstddef>
#include <string_view>

namespace campcat {
class shell_state;
} // namespace campcat

/// @brief Length of a leading "[hh:mm:ss]" (10), else 0.
inline std::size_t log_stamp_len(std::string_view _line) {
    constexpr std::size_t k_len = 10;
    if (_line.size() < k_len || _line[0] != '[' || _line[3] != ':' ||
        _line[6] != ':' || _line[9] != ']') {
        return 0;
    }
    for (const std::size_t i : {1U, 2U, 4U, 5U, 7U, 8U}) {
        if (_line[i] < '0' || _line[i] > '9') {
            return 0;
        }
    }
    return k_len;
}

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
