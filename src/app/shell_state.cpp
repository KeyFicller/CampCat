#include "app/shell_state.h"

#include "ccat_driver/ccat_program_runner.h"
#include "core/adb_client.h"
#include "core/automation/game_automation.h"
#include "core/automation_log.h"
#include "core/script/ccat_parser.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

/// How long the start button waits for the instance to leave its not-ready
/// states before it gives up. Booting the VM is slow; this only bounds it.
constexpr int k_mumu_ready_timeout_ms = 180000;

/// How long the start button waits for MuMuPlayer's service to answer after the
/// app is launched. Shorter than a VM boot: this is just an app coming up.
constexpr int k_mumu_service_timeout_ms = 60000;

inline bool acquire_cycle(std::atomic<bool> &_cycle_running) {
    bool expected = false;
    return _cycle_running.compare_exchange_strong(expected, true);
}

inline void release_cycle(std::atomic<bool> &_cycle_running) {
    _cycle_running.store(false);
}

/// How often the device poll asks adb. Short enough that an emulator coming up
/// shows on the chip while the user is still watching, long enough that an
/// `adb devices` per tick is not noise. The wait is sliced, so a stop never has
/// to wait out a whole interval.
constexpr int k_poll_interval_ms = 2000;
constexpr int k_poll_slice_ms = 100;
/// Shorter than the interval: a poll that runs long is stale anyway, and at
/// exit `stop_workers` joins this thread before the window goes down, so a hung
/// `adb` would otherwise freeze the quit for its full 8s default.
constexpr int k_poll_adb_timeout_ms = 1500;

bool persist_bundle_to_disk(const campcat::app_config &_shell_written,
                            const campcat::ccat_script_profile &_ccat_snap,
                            const std::filesystem::path &_shell_path,
                            std::string *_error_out = nullptr) {
    if (!_ccat_snap.has_script_source()) {
        if (_error_out) {
            *_error_out =
                "ccat script source path is empty; set .ccat path before save";
        }
        return false;
    }
    try {
        std::filesystem::create_directories(_shell_path.parent_path());
        campcat::app_config tmp_shell = _shell_written;
        tmp_shell.save_to_file(_shell_path);
        const std::filesystem::path bundle_json =
            std::filesystem::path(_shell_written.config_home) /
            campcat::k_ccat_bundle_rel;
        _ccat_snap.save_to_bundle_path(bundle_json, _shell_written.config_home);
        return true;
    } catch (const std::exception &ex) {
        if (_error_out) {
            *_error_out = ex.what();
        }
        return false;
    }
}

} // namespace

namespace campcat {

shell_state::shell_state(std::filesystem::path _cfg_path)
    : cfg_path(std::move(_cfg_path)),
      m_cfg(campcat::app_config::load_from_file(cfg_path)) {
    m_ccat_live = campcat::ccat_script_profile::load_from_bundle_path(
        std::filesystem::path(m_cfg.config_home) / k_ccat_bundle_rel,
        m_cfg.config_home);

    // Seed the Settings fields once here instead of on the first frame.
    adb_path_buf = m_cfg.adb.path;
    serial_buf = m_cfg.adb.serial;
    adb_connect_buf = m_cfg.adb.connect_address;
    mumutool_buf = m_cfg.mumu.tool_path;
    mumu_index_buf = m_cfg.mumu.device_index;

    campcat::automation_log::set_sink([this](const std::string &_s) {
        const auto now = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[64]{};
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        log_lines.push(std::string("[") + buf + "] " + _s);
    });
}

app_config shell_state::snapshot_cfg() {
    std::lock_guard<std::mutex> lk(m_cfg_mu);
    return m_cfg;
}

ccat_script_profile shell_state::snapshot_ccat_live() {
    std::lock_guard<std::mutex> lk(m_cfg_mu);
    return m_ccat_live;
}

void shell_state::set_ccat_live(const ccat_script_profile &_profile) {
    std::lock_guard<std::mutex> lk(m_cfg_mu);
    m_ccat_live = _profile;
}

void shell_state::append_log(const std::string &_s) {
    campcat::automation_log::emit(_s);
}

void shell_state::merge_buffers_into(app_config *_dst, const app_config &_ui) {
    *_dst = _ui;
    _dst->adb.path = adb_path_buf;
    _dst->adb.serial = serial_buf;
    _dst->adb.connect_address = adb_connect_buf;
    _dst->mumu.tool_path = mumutool_buf;
    _dst->mumu.device_index = mumu_index_buf;
}

void shell_state::push_job_snapshot(app_config *_view,
                                    const ccat_script_profile &_ui) {
    merge_buffers_into(_view, *_view);
    std::lock_guard<std::mutex> lk(m_cfg_mu);
    m_cfg = *_view;
    m_ccat_live = _ui;
}

bool shell_state::persist(std::string *_error_out) {
    return persist_bundle_to_disk(snapshot_cfg(), snapshot_ccat_live(),
                                  cfg_path, _error_out);
}

void shell_state::save_config(const app_config &_view,
                              const ccat_script_profile &_ui) {
    std::string err;
    campcat::app_config to_write{};
    merge_buffers_into(&to_write, _view);
    // Persist first (outside m_cfg_mu); commit to memory only on success so a
    // failed write cannot desync the automation config from disk.
    const bool ok = persist_bundle_to_disk(to_write, _ui, cfg_path, &err);
    if (ok) {
        {
            std::lock_guard<std::mutex> lk(m_cfg_mu);
            m_cfg = to_write;
            m_ccat_live = _ui;
        }
        append_log("[ui] saved to shell: " + cfg_path.string());
        append_log(
            "[ui] saved to ccat bundle: " +
            (std::filesystem::path(to_write.config_home) / k_ccat_bundle_rel)
                .string());
        if (!snapshot_cfg().scheduler.enabled) {
            sched.stop();
            append_log("[sched] stopped (scheduler.disabled in saved config)");
        }
    } else {
        append_log(std::string("[ui] save failed: ") + err);
    }
}

void shell_state::run_automation_thread_body(
    const campcat::app_config _snap_shell,
    const campcat::ccat_script_profile &_snap_ccat) {
    auto split_log = [&](const char *prefix, const std::string &block) {
        std::istringstream iss(block);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty()) {
                append_log(std::string(prefix) + line);
            }
        }
    };
    try {
        campcat::adb_client adb(_snap_shell.adb.path, _snap_shell.adb.serial);
        std::string co, ce;
        const bool ok_c = adb.connect_remote(_snap_shell.adb.connect_address,
                                             15000, &co, &ce);
        split_log("[adb connect] ", co);
        split_log("[adb connect err] ", ce);
        if (!_snap_shell.adb.connect_address.empty() && !ok_c) {
            append_log("[adb connect] failed or timeout");
        }
        auto automator =
            campcat::make_game_automation(&adb, &_snap_shell, &_snap_ccat);
        const auto should_stop = [this]() { return stop_requested.load(); };
        const auto res = automator->run_cycle(should_stop);
        append_log(std::string("[fsm] ok=") + (res.ok ? "true" : "false") +
                   " | " + res.message);
    } catch (const std::exception &ex) {
        append_log(std::string("[fsm] exception: ") + ex.what());
    }
    release_cycle(cycle_running);
}

std::tuple<campcat::app_config, campcat::ccat_script_profile>
shell_state::capture_bundle_snapshot() {
    std::lock_guard<std::mutex> lk(m_cfg_mu);
    return std::tuple<campcat::app_config, campcat::ccat_script_profile>(
        m_cfg, m_ccat_live);
}

void shell_state::run_single_job() {
    if (!acquire_cycle(cycle_running)) {
        append_log("[job] busy");
        return;
    }
    stop_requested.store(false);

    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    try {
        auto snap = capture_bundle_snapshot();
        worker_thread =
            std::thread([this, shell = std::move(std::get<0>(snap)),
                         ccat_bundle = std::move(std::get<1>(snap))]() mutable {
                run_automation_thread_body(std::move(shell),
                                           std::move(ccat_bundle));
            });
    } catch (const std::exception &ex) {
        release_cycle(cycle_running);
        append_log(std::string("[job] failed to start worker: ") + ex.what());
    }
}

void shell_state::run_console_source_job(std::string _source) {
    if (!acquire_cycle(cycle_running)) {
        append_log("[console] busy");
        return;
    }
    stop_requested.store(false);
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
    try {
        auto snap = capture_bundle_snapshot();
        worker_thread = std::thread([this, source = std::move(_source),
                                     shell = std::move(std::get<0>(snap)),
                                     ccat_bundle = std::move(
                                         std::get<1>(snap))]() mutable {
            try {
                campcat::adb_client adb(shell.adb.path, shell.adb.serial);
                std::string co, ce;
                adb.connect_remote(shell.adb.connect_address, 15000, &co, &ce);
                auto prog = campcat::ccat_lang::parse_program(source);
                const auto images = ccat_bundle.images_base(shell.config_home);
                std::filesystem::path script_abs =
                    ccat_bundle.script_path_absolute;
                if (script_abs.empty()) {
                    script_abs = std::filesystem::path(shell.config_home) /
                                 ccat_bundle.source_rel;
                }
                const auto should_stop = [this]() {
                    return stop_requested.load();
                };
                const auto res = campcat::run_ccat_program(
                    &adb, &shell, images, script_abs, *prog, should_stop);
                append_log(std::string("[console] ok=") +
                           (res.ok ? "true" : "false") + " | " + res.message);
            } catch (const campcat::ccat_lang::parse_error &ex) {
                append_log(std::string("[console] parse error: ") + ex.what());
            } catch (const std::exception &ex) {
                append_log(std::string("[console] exception: ") + ex.what());
            }
            release_cycle(cycle_running);
        });
    } catch (const std::exception &ex) {
        release_cycle(cycle_running);
        append_log(std::string("[console] failed to start worker: ") +
                   ex.what());
    }
}

void shell_state::start_mumu_job() {
    if (!acquire_cycle(cycle_running)) {
        append_log("[mumu] busy");
        return;
    }
    stop_requested.store(false);
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
    const campcat::app_config shell = snapshot_cfg();
    mumu_launching.store(true);
    try {
        worker_thread = std::thread([this, shell] {
            mumu_launch_sequence(shell, {});
            mumu_launching.store(false);
            release_cycle(cycle_running);
        });
    } catch (const std::exception &ex) {
        mumu_launching.store(false);
        release_cycle(cycle_running);
        append_log(std::string("[mumu] failed to start worker: ") + ex.what());
    }
}

campcat::mumu_status
shell_state::start_mumu_blocking(const std::function<bool()> &_should_stop) {
    if (mumu_launching.exchange(true)) {
        return {campcat::mumu_state::failed,
                "another launch is already in flight"};
    }
    if (!acquire_cycle(cycle_running)) {
        mumu_launching.store(false);
        return {campcat::mumu_state::failed, "an automation cycle is running"};
    }
    campcat::mumu_status st =
        mumu_launch_sequence(snapshot_cfg(), _should_stop);
    mumu_launching.store(false);
    release_cycle(cycle_running);
    return st;
}

campcat::mumu_status
shell_state::mumu_launch_sequence(const campcat::app_config &shell,
                                  const std::function<bool()> &_should_stop) {
    const std::string tool = shell.mumu.tool_path;
    const std::string app = campcat::mumu_app_path(tool);
    const int index = shell.mumu.device_index;
    // The app this run launched, if any; feeds the watchdog and is what
    // the shutdown path quits. Empty keeps somebody else's app alive.
    std::string app_owned;
    auto publish = [this](const campcat::mumu_status &_st) {
        std::lock_guard<std::mutex> lk(mumu_mu);
        mumu_report = _st;
    };

    campcat::mumu_status st = campcat::mumu_query_state(tool, index);
    if (st.state == campcat::mumu_state::service_down) {
        // mumutool is only a client of MuMuPlayer's service: with the app down
        // it cannot answer `info` at all, so the app has to come first.
        st = campcat::mumu_launch_app(app);
        if (st.state != campcat::mumu_state::failed) {
            app_owned = app;
            {
                std::lock_guard<std::mutex> lk(mumu_mu);
                mumu_app_started = app;
            }
            st = campcat::mumu_wait_service(
                tool, index, k_mumu_service_timeout_ms, _should_stop);
        }
    }
    if (st.state == campcat::mumu_state::failed) {
        publish(st);
        return st;
    }
    if (st.state != campcat::mumu_state::stopped) {
        // Somebody already had it up. Leave it alone and arm no watchdog:
        // that would close an instance we did not start. An app we just
        // launched is still ours to quit; `mumu_app_started` holds that.
        st.message = "already up";
        publish(st);
        return st;
    }

    st = campcat::mumu_open(tool, index);
    if (st.state == campcat::mumu_state::failed) {
        publish(st);
        return st;
    }
    st = campcat::mumu_wait_ready(tool, index, k_mumu_ready_timeout_ms,
                                  _should_stop);
    // A cancelled wait still leaves an instance we opened booting, so it stays
    // ours to close on exit. A plain failure arms nothing.
    const bool cancelled = _should_stop && _should_stop();
    if (st.state == campcat::mumu_state::failed && !cancelled) {
        publish(st);
        return st;
    }

    // Spawn first: the watchdog then covers the gap before anything is
    // recorded. Record either way, so an instance we started is still closed
    // on a clean exit when no watchdog could be armed.
    auto wd = campcat::mumu_spawn_watchdog(
        tool, index, static_cast<long>(::getpid()), app_owned);
    const bool armed = wd.has_value();
    {
        std::lock_guard<std::mutex> lk(mumu_mu);
        mumu_launched_path = tool;
        mumu_launched_index = index;
        mumu_watchdog = std::move(wd);
    }
    if (!cancelled && !armed) {
        st.message = "started, but no watchdog could be armed";
    }
    publish(st);
    return st;
}

void shell_state::refresh_scheduler_locked(bool _immediate_first_tick) {
    sched.stop();
    const auto snap = snapshot_cfg();
    if (!snap.scheduler.enabled) {
        append_log("[sched] disabled");
        return;
    }

    sched.configure(std::chrono::seconds(snap.scheduler.interval_seconds),
                    std::chrono::seconds(snap.scheduler.jitter_seconds));

    sched.start(
        [this]() {
            const auto shell = snapshot_cfg();
            if (shell.scheduler.skip_if_busy && cycle_running.load()) {
                append_log("[sched] skip tick (busy)");
                return;
            }
            run_single_job();
        },
        _immediate_first_tick);

    if (_immediate_first_tick) {
        append_log("[sched] armed - immediate tick once, then "
                   "sleep(interval+jitter) and repeat until exit/disable");
    } else {
        append_log("[sched] armed - sleep(interval+jitter) then tick, repeat "
                   "until exit/disable");
    }
}

void shell_state::stop_workers() {
    stop_requested.store(true);
    sched.stop();

    device_poll_stop.store(true);
    if (device_poll_thread.joinable()) {
        device_poll_thread.join();
    }

    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void shell_state::start_device_poll() {
    device_poll_stop.store(false);
    device_poll_thread = std::thread([this]() {
        while (!device_poll_stop.load()) {
            std::string adb_path;
            std::string serial;
            {
                // The UI pushes this snapshot every frame, so it already
                // carries what the Settings fields say.
                std::lock_guard<std::mutex> lk(m_cfg_mu);
                adb_path = m_cfg.adb.path;
                serial = m_cfg.adb.serial;
            }
            bool online = false;
            if (!adb_path.empty()) {
                online = campcat::device_online(
                    campcat::adb_client(adb_path, serial)
                        .list_devices(k_poll_adb_timeout_ms),
                    serial);
            }
            device_online.store(online);
            for (int waited = 0;
                 waited < k_poll_interval_ms && !device_poll_stop.load();
                 waited += k_poll_slice_ms) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(k_poll_slice_ms));
            }
        }
    });
}

void shell_state::close_launched_mumu() {
    // Close the instance this run started, quit the app it started, then drop
    // the watchdog so it cannot race us. Order is load-bearing: with the app
    // gone, mumutool cannot close anything. Dropping the watchdog last keeps it
    // covering a death mid-close, and both of its steps are idempotent, so a
    // concurrent fire is harmless. Both names stay empty when we started
    // nothing, which is what keeps somebody else's instance and app alive.
    std::lock_guard<std::mutex> lk(mumu_mu);
    if (mumu_launched_index >= 0) {
        campcat::mumu_close(mumu_launched_path, mumu_launched_index);
    }
    if (!mumu_app_started.empty()) {
        campcat::mumu_quit_app(mumu_app_started);
    }
    mumu_watchdog.reset();
}

} // namespace campcat
