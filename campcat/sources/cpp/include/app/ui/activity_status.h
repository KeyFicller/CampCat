#pragma once

#include "app/ui/theme.h"

#include <imgui.h>

namespace campcat {

/// How the shell's status chip reads a page. The colour comes from the level
/// alone; the words come from whoever knows what is happening, because only the
/// page can tell "Thinking" from "Running".
enum class activity_level {
    idle,    ///< nothing to report
    busy,    ///< work in progress
    waiting, ///< waiting on the human, or on something slow they may want to
             ///< stop
    failed,  ///< the last thing this page did failed
    ok, ///< up and well (the device is there); not "busy", so its own level
};

/// One chip's answer. `label` is a literal: the chip measures it every frame,
/// and nothing here should allocate.
struct activity_status {
    activity_level level = activity_level::idle;
    const char *label = "Idle";
};

/// @brief The colour for a level.
///
/// It sits in this header rather than next to the chip because panels also
/// colour the hints that wait on the human with `waiting`.
inline ImVec4 activity_color(activity_level _level) {
    switch (_level) {
    case activity_level::ok:
        return app::theme::k_success;
    case activity_level::busy:
        return app::theme::k_accent_fg;
    case activity_level::waiting:
        return app::theme::k_warning;
    case activity_level::failed:
        return app::theme::k_danger;
    case activity_level::idle:
        break;
    }
    return app::theme::k_text_3;
}

} // namespace campcat
