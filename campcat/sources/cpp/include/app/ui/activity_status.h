#pragma once

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
/// The two colours the chrome did not have -- amber (waiting on something) and
/// red (failed) -- are written out here as literals; the rest are the style's
/// own values (ink_dim, accent_hi, and the green the idle label used to show
/// for a busy shell), which live inside the style function. It sits in this
/// header rather than next to the chip because panels also colour the hints
/// that wait on the human with `waiting`.
inline ImVec4 activity_color(activity_level _level) {
    switch (_level) {
    case activity_level::ok:
        return ImVec4(0.450F, 0.850F, 0.550F, 1.0F);
    case activity_level::busy:
        return ImVec4(0.302F, 0.478F, 0.616F, 1.0F);
    case activity_level::waiting:
        return ImVec4(0.850F, 0.680F, 0.300F, 1.0F);
    case activity_level::failed:
        return ImVec4(0.850F, 0.420F, 0.420F, 1.0F);
    case activity_level::idle:
        break;
    }
    return ImVec4(0.478F, 0.510F, 0.549F, 1.0F);
}

} // namespace campcat
