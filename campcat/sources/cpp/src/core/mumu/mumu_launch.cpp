#include "core/mumu_launch.h"

#include <chrono>
#include <thread>

#include "core/adb_client.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

/// How long to wait for the instance to leave its not-ready states before
/// giving up. Booting the VM is slow; this only bounds it.
constexpr int k_mumu_ready_timeout_ms = 180000;

/// How long to wait for MuMuPlayer's service to answer after the app is
/// launched. Shorter than a VM boot: this is just an app coming up.
constexpr int k_mumu_service_timeout_ms = 60000;

/// How often the device poll asks adb. Short enough that an emulator coming up
/// shows on the chip while the user is still watching, long enough that an
/// `adb devices` per tick is not noise. The wait is sliced, so a stop never has
/// to wait out a whole interval.
constexpr int k_poll_interval_ms = 2000;

/// How often a dead transport is dropped and re-added while waiting. One
/// `adb` per tick would be noise; this still recovers inside a boot.
constexpr int k_reconnect_interval_ms = 10000;

/**
 * @brief Wait until the device itself is usable, not merely booted.
 *
 * mumutool reports the instance `running` seconds after `open`; that is the
 * VM, not Android. Callers poll the screen the moment this returns, so
 * reporting early is exactly what makes them screenshot a black screen.
 *
 * @param[in,out] _adb Client to poll with.
 * @param[in] _deadline_ms Budget left, shared with the VM-ready wait.
 * @param[in] _should_stop Polled between attempts; true gives up early.
 * @param[in] _address The emulator's `host:port` transport, or empty when the
 * client already carries what it needs.
 * @param[out] _why Last observation, for the failure message.
 * @return True once `sys.boot_completed=1` and `init.svc.bootanim=stopped`.
 */
bool poll_device_ready(campcat::adb_api &_adb, int _deadline_ms,
                       const std::function<bool()> &_should_stop,
                       std::string_view _address, std::string *_why) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(_deadline_ms);
    std::string boot;
    std::string anim;
    std::string io_err;
    const auto prop = [&](const char *_key, std::string *_out) {
        std::string out;
        std::string err;
        if (!_adb.run({"shell", "getprop", _key}, &out, &err, 5000)) {
            _out->clear();
            io_err = err.empty() ? out : err;
            return false;
        }
        // getprop answers with a trailing newline, and CRLF on some images. An
        // unset key is an empty line -- that is "absent", not an error.
        const auto first = out.find_first_not_of(" \t\r\n");
        const auto last = out.find_last_not_of(" \t\r\n");
        *_out = first == std::string::npos ? std::string{}
                                           : out.substr(first, last - first + 1);
        return !_out->empty();
    };
    auto next_reconnect = std::chrono::steady_clock::now();
    while (true) {
        if (_should_stop && _should_stop()) {
            *_why = "stopped by the user";
            return false;
        }
        const bool booted = prop("sys.boot_completed", &boot) && boot == "1";
        // `init.svc.bootanim` does not exist on images that boot with the
        // animation disabled (MuMu sets `debug.sf.nobootanimation`), so it is
        // only consulted when it is there at all.
        const bool anim_done =
            !prop("init.svc.bootanim", &anim) || anim == "stopped";
        if (booted && anim_done) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *_why = "device not ready (boot_completed=" +
                    (boot.empty() ? std::string("?") : boot) + ", bootanim=" +
                    (anim.empty() ? std::string("?") : anim) + ")" +
                    (io_err.empty() ? std::string{}
                                    : ", adb said: " + io_err);
            return false;
        }
        // An emulator registers its TCP transport before adbd answers, and adb
        // keeps that dead transport listed as `offline`. `adb connect` on it
        // answers "already connected" without reviving it, so a device that is
        // already up would spin out this whole budget. Dropping and re-adding
        // the transport is what brings it back.
        if (!booted && !_address.empty() &&
            std::chrono::steady_clock::now() >= next_reconnect) {
            _adb.run({"disconnect", std::string(_address)}, nullptr, nullptr,
                     5000);
            _adb.connect_remote(_address, 5000);
            next_reconnect =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(k_reconnect_interval_ms);
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(k_poll_interval_ms));
    }
}

} // namespace

namespace campcat {

/// Exposed for the test that proves a dead adb transport is revived; the shell
/// and the headless server both go through `launch_and_wait`.
bool wait_device_ready(adb_api &_adb, int _deadline_ms,
                       const std::function<bool()> &_should_stop,
                       std::string_view _address, std::string *_why) {
    return poll_device_ready(_adb, _deadline_ms, _should_stop, _address, _why);
}


mumu_status launch_and_wait(const app_config &_cfg,
                            const std::function<bool()> &_should_stop,
                            const launch_sink &_sink) {
    const std::string tool = _cfg.mumu.tool_path;
    const std::string app = mumu_app_path(tool);
    const int index = _cfg.mumu.device_index;
    // The app this run launched, if any; feeds the watchdog and is what
    // the shutdown path quits. Empty keeps somebody else's app alive.
    std::string app_owned;
    // `k_mumu_ready_timeout_ms` is one budget for the whole launch, so the
    // device-ready wait that follows the VM wait gets what is left of it.
    const auto launch_start = std::chrono::steady_clock::now();

    mumu_status st = mumu_query_state(tool, index);
    if (st.state == mumu_state::service_down) {
        // mumutool is only a client of MuMuPlayer's service: with the app down
        // it cannot answer `info` at all, so the app has to come first.
        st = mumu_launch_app(app);
        if (st.state != mumu_state::failed) {
            app_owned = app;
            if (_sink.app_launched) {
                _sink.app_launched(app);
            }
            st = mumu_wait_service(tool, index, k_mumu_service_timeout_ms,
                                   _should_stop);
        }
    }
    if (st.state == mumu_state::failed) {
        return st;
    }
    if (st.state != mumu_state::stopped) {
        // Somebody already had it up. Leave it alone and arm no watchdog:
        // that would close an instance we did not start. An app we just
        // launched is still ours to quit; `app_launched` recorded that.
        st.message = "already up";
        return st;
    }

    st = mumu_open(tool, index);
    if (st.state == mumu_state::failed) {
        return st;
    }
    st = mumu_wait_ready(tool, index, k_mumu_ready_timeout_ms, _should_stop);
    // A cancelled wait still leaves an instance we opened booting, so it stays
    // ours to close on exit. A plain failure arms nothing.
    const bool cancelled = _should_stop && _should_stop();
    if (st.state == mumu_state::failed && !cancelled) {
        return st;
    }

    // The emulator picks its adb port, not us, so read it off mumutool and use
    // it from here on. 0 means this mumutool did not say; adb's default device
    // is then the best guess.
    if (!cancelled) {
        // The VM being `running` says nothing about Android, and callers poll
        // the screen the moment this returns.
        const int spent_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - launch_start)
                .count());
        // Asked of mumutool rather than remembered: the port changes with every
        // launch, so the answer is only good for as long as it is used.
        const std::string addr = mumu_adb_address(tool, index);
        const std::string target = addr.empty() ? _cfg.adb.serial : addr;
        if (target.empty()) {
            // With no `-s` the probes below read whatever device adb happens to
            // have: a stale `offline` entry makes them fail forever, and a lone
            // other device would have its boot state answer for ours.
            st.state = mumu_state::failed;
            st.message = "mumutool named no adb port and no serial is set, so "
                         "the device could not be confirmed ready";
            if (_sink.log) {
                _sink.log(st.message);
            }
            return st;
        }
        adb_client conn(_cfg.adb.path, target);
        std::string co;
        std::string ce;
        conn.connect_remote(addr, 15000, &co, &ce);
        std::string why;
        if (!wait_device_ready(conn, k_mumu_ready_timeout_ms - spent_ms,
                               _should_stop, addr, &why)) {
            st.state = mumu_state::failed;
            st.message = why;
            if (_sink.log) {
                _sink.log("launch failed: " + why);
            }
            return st;
        }
    }

    // Spawn first: the watchdog then covers the gap before anything is
    // recorded. Record either way, so an instance we started is still closed
    // on a clean exit when no watchdog could be armed.
    auto wd = mumu_spawn_watchdog(tool, index, static_cast<long>(::getpid()),
                                  app_owned);
    const bool armed = wd.has_value();
    if (_sink.opened) {
        _sink.opened(tool, index, std::move(wd));
    }
    if (!cancelled && !armed) {
        st.message = "started, but no watchdog could be armed";
    }
    return st;
}

} // namespace campcat
