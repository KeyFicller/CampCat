#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/mumu_launcher.h"
#include "core/process.h"

namespace campcat {

/**
 * @brief Where one launch reports, so the GUI and the headless server can keep
 * different books over the same sequence.
 *
 * Every hook may be empty, and the GUI fills all three: it writes the log, it
 * remembers the app it started so `close_launched_mumu` can quit it, and it
 * keeps the watchdog so a later shutdown can kill it before it fires.
 */
struct launch_sink {
    /// One diagnostic line, without the `[mumu] ` prefix.
    std::function<void(std::string_view)> log;
    /// The MuMuPlayer app this launch started; empty when one was already up.
    std::function<void(const std::string &app)> app_launched;
    /// What is now ours to close: the tool, the device index, and the watchdog
    /// (empty when none could be armed).
    std::function<void(const std::string &tool, int index,
                       std::optional<process::child> watchdog)>
        opened;
};

/**
 * @brief Boot the emulator `_cfg` names and block until Android answers.
 *
 * The sequence the GUI's Start button runs: mumutool service, app, `open`, VM
 * ready, then `adb` until `sys.boot_completed`. Reports through `_sink` and
 * never touches anything else, so a caller without a shell can run it too.
 *
 * @param[in] _cfg Config snapshot naming mumutool, instance index, and adb.
 * @param[in] _should_stop Polled by the waits; true gives up early and comes
 * back as a failed status that says so. Empty never stops.
 * @param[in] _sink Hooks for the log, the app started, and the watchdog.
 * @return The last observed status; `running` once Android answers, or
 * `already up` in `message` when an instance was already there. That one is
 * still a success.
 */
mumu_status launch_and_wait(const app_config &_cfg,
                            const std::function<bool()> &_should_stop,
                            const launch_sink &_sink);

/**
 * @brief The last step of the launch: poll `adb` until Android answers.
 *
 * Also re-adds the TCP transport when it stops answering, because adb keeps a
 * dead one listed as `offline` and `adb connect` alone does not revive it.
 * Exposed for the test that proves the revive; callers use `launch_and_wait`.
 *
 * @param[in,out] _adb Client to poll with.
 * @param[in] _deadline_ms Budget for the whole wait.
 * @param[in] _should_stop Polled between attempts; true gives up early.
 * @param[in] _address The emulator's `host:port` transport, or empty.
 * @param[out] _why Last observation, for the failure message.
 * @return True once Android has finished booting.
 */
bool wait_device_ready(adb_api &_adb, int _deadline_ms,
                       const std::function<bool()> &_should_stop,
                       std::string_view _address, std::string *_why);

} // namespace campcat
