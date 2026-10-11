#pragma once

#include <functional>
#include <optional>
#include <string>

#include "core/process.h"

namespace campcat {

/**
 * @brief Where the emulator instance is, as far as `mumutool info` can tell.
 *
 * `starting` is a guess: only `stopped` has been seen on a machine with the
 * instance down. It is treated as not-ready, which is the safe reading.
 *
 * `service_down` means mumutool ran but found no MuMuPlayer to talk to. It is
 * kept apart from `failed` because it is the one failure with a fix: start the
 * app (`mumu_launch_app`) and ask again.
 */
enum class mumu_state {
  unknown,
  stopped,
  starting,
  running,
  service_down,
  failed
};

/**
 * @brief One `mumutool` answer, ready for the UI to draw.
 */
struct mumu_status {
  mumu_state state = mumu_state::unknown;
  /// The raw `state` string from mumutool, or why the call failed.
  std::string message;
};

/**
 * @brief Read `info <index>` and map its `state` field.
 *
 * Safe to call with nothing running. A state this file does not know is
 * reported as `running` with the raw string in `message`, so a new value shows
 * up in the UI instead of being silently misread.
 *
 * @param[in] _mumutool Path to the `mumutool` executable.
 * @param[in] _index MuMu instance index.
 * @return Parsed state; `failed` when mumutool could not be run or answered,
 * `service_down` when it ran but found no MuMuPlayer to talk to.
 */
mumu_status mumu_query_state(const std::string &_mumutool, int _index);

/**
 * @brief Derive the MuMuPlayer bundle from a `mumutool` path.
 *
 * `mumutool` sits at `<app>/Contents/MacOS/mumutool`, so the bundle is three
 * levels up. A path not laid out that way yields nonsense, which
 * `mumu_launch_app` rejects before anything is started.
 *
 * @param[in] _mumutool Path to the `mumutool` executable.
 * @return The bundle path, or an empty/meaningless string if it cannot be read
 * off `_mumutool`.
 */
std::string mumu_app_path(const std::string &_mumutool);

/**
 * @brief Start MuMuPlayer.app (`open -g`) so its service starts answering.
 *
 * mumutool is only a client of that service: with the app down every call
 * fails, `info` included, so this is the step that actually lets an instance be
 * started from nothing. The service is not up when this returns -- poll
 * `mumu_wait_service`. `-g` keeps the app from stealing focus.
 *
 * @param[in] _app_path The bundle to launch.
 * @return `starting` once `open` accepted it; `failed` when the path is not a
 * directory ending in `.app`, or `open` itself failed.
 */
mumu_status mumu_launch_app(const std::string &_app_path);

/**
 * @brief Poll until mumutool answers with a real state, or time runs out.
 *
 * Pairs with `mumu_launch_app`: an app takes seconds to expose its service, and
 * mumutool signals "not listening yet" in more than one way (`invalidPort`
 * before the port exists, a connection error after). Every failure is retried,
 * so the wait is bounded by the deadline rather than by which wording mumutool
 * happens to use. `mumu_wait_ready` cannot stand in -- it waits for the
 * *instance*, which has not been started yet.
 *
 * @return The first answer with a real state; `failed` on timeout.
 *
 * @param[in] _should_stop Polled between attempts; true gives up early and
 * reports it, which is a stop rather than a failure to come up. Empty never
 * stops (the Settings button).
 */
mumu_status mumu_wait_service(const std::string &_mumutool, int _index,
                              int _deadline_ms,
                              const std::function<bool()> &_should_stop = {});

/**
 * @brief Ask MuMuPlayer.app to quit, by bundle path.
 *
 * Best effort, and it must run *after* `mumu_close`: with the app gone,
 * mumutool cannot close anything. osascript exits 0 whether or not the app was
 * running, so `true` only means the request went out.
 */
bool mumu_quit_app(const std::string &_app_path);

/**
 * @brief Ask for the instance to start (`open <index>`).
 *
 * Success means the request was accepted, not that the instance is usable:
 * readiness is `mumu_wait_ready`'s job.
 */
mumu_status mumu_open(const std::string &_mumutool, int _index);

/**
 * @brief Close the instance (`close <index>`). Idempotent.
 *
 * Closing an instance that is already down is not an error, which is what lets
 * both the explicit shutdown and the watchdog call it without coordinating.
 */
mumu_status mumu_close(const std::string &_mumutool, int _index);

/**
 * @brief Poll until the instance leaves the not-ready states, or time runs out.
 *
 * @param[in] _deadline_ms How long to wait before giving up.
 * @param[in] _should_stop Polled between attempts; true gives up early and
 * reports it, which is a stop rather than a failure to come up. Empty never
 * stops (the Settings button).
 * @return The last observed status; `failed` on timeout or on a mumutool error.
 */
mumu_status mumu_wait_ready(const std::string &_mumutool, int _index,
                            int _deadline_ms,
                            const std::function<bool()> &_should_stop = {});

/**
 * @brief Watch `_parent_pid` and close the instance when it goes away.
 *
 * Covers the deaths no destructor runs for (SIGKILL, crash). On a clean exit
 * the caller is expected to close the instance itself and then destroy this
 * handle, killing the watchdog before it can fire.
 *
 * @param[in] _parent_pid The process whose death ends the instance's life.
 * @param[in] _app_path Bundle to quit as well, after the instance is closed;
 * empty leaves the app alone.
 * @return The handle, or `std::nullopt` when the spawn failed.
 */
std::optional<process::child> mumu_spawn_watchdog(const std::string &_mumutool,
                                                 int _index, long _parent_pid,
                                                 const std::string &_app_path);

} // namespace campcat
