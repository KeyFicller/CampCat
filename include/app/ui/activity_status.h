#pragma once

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

} // namespace campcat
