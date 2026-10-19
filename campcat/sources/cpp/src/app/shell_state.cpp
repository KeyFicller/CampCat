#include "app/shell_state.h"

#include "ccat_driver/ccat_program_runner.h"
#include "core/adb_client.h"
#include "core/automation/game_automation.h"
#include "core/automation_log.h"
#include "core/llm/llm_consult.h"
#include "core/llm/llm_host.h"
#include "core/mumu_launch.h"
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

namespace {

/// Where the log file goes, relative to the working directory the app was
/// started from (the same base `campcat/sources/cpp/assets/` is resolved against). One file per
/// run, named for the moment it opened.
constexpr char k_log_dir_rel[] = "logs";

/// The local wall clock, filled the platform's thread-safe way. The log sink
/// runs on worker threads, so `std::localtime` is not an option here.
std::tm local_now() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return tm;
}

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

bool persist_config_to_disk(const campcat::app_config &_shell_written,
                            const std::filesystem::path &_shell_path,
                            std::string *_error_out = nullptr) {
    try {
        // `save_to_file` creates the parent directory itself. Calling it here
        // too threw on the default bare `config.json`, whose parent is "".
        _shell_written.save_to_file(_shell_path);
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

void shell_state::open_log_file() {
    const std::tm tm = local_now();
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

    std::error_code ec;
    std::filesystem::create_directories(k_log_dir_rel, ec);
    m_log_file.open(std::filesystem::path(k_log_dir_rel) /
                        (std::string(stamp) + ".log"),
                    std::ios::app);
    m_log_file_dead = !m_log_file;
}

void shell_state::write_log_line(const std::string &_line) {
    if (m_log_file_dead) {
        return;
    }
    std::lock_guard<std::mutex> lk(m_log_file_mu);
    m_log_file << _line << '\n';
    m_log_file.flush();
    if (!m_log_file) {
        // Full disk or a directory that turned read-only mid-run: stop trying
        // rather than paying for a failed write per line. Reported once, by the
        // constructor; the sink cannot report it without recursing.
        m_log_file_dead = true;
    }
}

shell_state::shell_state(std::filesystem::path _cfg_path)
    : cfg_path(std::move(_cfg_path)),
      m_cfg(campcat::app_config::load_from_file(cfg_path)) {
    m_ccat_live = campcat::ccat_script_profile::load(m_cfg.config_home,
                                                    m_cfg.script_bundle);

    // Seed the Settings fields once here instead of on the first frame.
    adb_path_buf = m_cfg.adb.path;
    serial_buf = m_cfg.adb.serial;
    mumutool_buf = m_cfg.mumu.tool_path;
    mumu_index_buf = m_cfg.mumu.device_index;

    open_log_file();
    campcat::automation_log::set_sink([this](const std::string &_s) {
        const std::tm tm = local_now();
        char buf[64]{};
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        log_lines.push(std::string("[") + buf + "] " + _s);

        // The file carries the date too: it outlives this session, and a bare
        // time cannot place a line on a day.
        char file_buf[64]{};
        std::strftime(file_buf, sizeof(file_buf), "%Y-%m-%d %H:%M:%S", &tm);
        write_log_line(std::string(file_buf) + " " + _s);
    });
    if (m_log_file_dead) {
        // Through the sink on purpose: it skips the file while dead, so this
        // cannot recurse.
        append_log(std::string("[log] cannot write under ") + k_log_dir_rel +
                   "/; logging to the UI only");
    }
}

app_config shell_state::snapshot_cfg() {
    std::lock_guard<std::mutex> lk(m_cfg_mu);
    return m_cfg;
}

ccat_script_profile shell_state::snapshot_ccat_live() {
    std::lock_guard<std::mutex> lk(m_cfg_mu);
    return m_ccat_live;
}

void shell_state::set_script_bundle(std::string _bundle) {
    std::lock_guard<std::mutex> lk(m_cfg_mu);
    m_cfg.script_bundle = std::move(_bundle);
    m_ccat_live = campcat::ccat_script_profile::load(m_cfg.config_home,
                                                     m_cfg.script_bundle);
}

void shell_state::append_log(const std::string &_s) {
    campcat::automation_log::emit(_s);
}

void shell_state::merge_buffers_into(app_config *_dst, const app_config &_ui) {
    *_dst = _ui;
    _dst->adb.path = adb_path_buf;
    _dst->adb.serial = serial_buf;
    _dst->mumu.tool_path = mumutool_buf;
    _dst->mumu.device_index = mumu_index_buf;
}

void shell_state::push_job_snapshot(app_config *_view) {
    merge_buffers_into(_view, *_view);
    std::lock_guard<std::mutex> lk(m_cfg_mu);
    m_cfg = *_view;
}

void shell_state::save_config(const app_config &_view) {
    std::string err;
    campcat::app_config to_write{};
    merge_buffers_into(&to_write, _view);
    // Persist first (outside m_cfg_mu); commit to memory only on success so a
    // failed write cannot desync the automation config from disk.
    if (persist_config_to_disk(to_write, cfg_path, &err)) {
        {
            std::lock_guard<std::mutex> lk(m_cfg_mu);
            m_cfg = to_write;
        }
        append_log("[ui] saved to shell: " + cfg_path.string());
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
        const std::string addr = campcat::mumu_adb_address(
            _snap_shell.mumu.tool_path, _snap_shell.mumu.device_index);
        campcat::adb_client adb(_snap_shell.adb.path,
                                addr.empty() ? _snap_shell.adb.serial : addr);
        std::string co, ce;
        const bool ok_c = adb.connect_remote(addr, 15000, &co, &ce);
        split_log("[adb connect] ", co);
        split_log("[adb connect err] ", ce);
        if (!addr.empty() && !ok_c) {
            append_log("[adb connect] failed or timeout");
        }
        auto automator = campcat::make_game_automation(
            &adb, &_snap_shell, &_snap_ccat,
            campcat::make_consult_ask(
                &adb, &campcat::consult_host(),
                std::filesystem::path(_snap_shell.config_home)
                    .parent_path(),
                _snap_shell.llm.provider,
                [this]() { return stop_requested.load(); }));
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
                const std::string addr = campcat::mumu_adb_address(
                    shell.mumu.tool_path, shell.mumu.device_index);
                campcat::adb_client adb(shell.adb.path,
                                        addr.empty() ? shell.adb.serial : addr);
                std::string co, ce;
                adb.connect_remote(addr, 15000, &co, &ce);
                auto prog = campcat::ccat_lang::parse_program(source);
                const std::filesystem::path home(shell.config_home);
                const std::filesystem::path images =
                    ccat_bundle.console_root(home);
                std::filesystem::path script_abs =
                    ccat_bundle.script_path_absolute;
                if (!ccat_bundle.has_script_source()) {
                    script_abs = home;
                } else if (script_abs.empty()) {
                    script_abs = home / ccat_bundle.source_rel;
                }
                const auto should_stop = [this]() {
                    return stop_requested.load();
                };
                const auto ask = campcat::make_consult_ask(
                    &adb, &campcat::consult_host(),
                    std::filesystem::path(shell.config_home).parent_path(),
                    shell.llm.provider, should_stop);
                const auto res = campcat::run_ccat_program(
                    &adb, &shell, images, script_abs, *prog, should_stop, ask);
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
shell_state::mumu_launch_sequence(const campcat::app_config &_shell,
                                  const std::function<bool()> &_should_stop) {
    // The sequence lives in `core` so the headless MCP entry point can run it
    // too; what is the GUI's alone is the bookkeeping below.
    return campcat::launch_and_wait(
        _shell, _should_stop,
        campcat::launch_sink{
            .log = [this](std::string_view _line) {
                append_log("[mumu] " + std::string(_line));
            },
            .app_launched = [this](const std::string &_app) {
                std::lock_guard<std::mutex> lk(mumu_mu);
                mumu_app_started = _app;
            },
            .opened = [this](const std::string &_tool, int _index,
                             std::optional<campcat::process::child> _wd) {
                std::lock_guard<std::mutex> lk(mumu_mu);
                mumu_launched_path = _tool;
                mumu_launched_index = _index;
                mumu_watchdog = std::move(_wd);
            }});
}

void shell_state::refresh_scheduler_locked(bool _immediate_first_tick) {
    sched.stop();
    const auto snap = snapshot_cfg();

    sched.configure(std::chrono::seconds(snap.scheduler.interval_seconds),
                    std::chrono::seconds(snap.scheduler.jitter_seconds));

    sched.start(
        [this]() {
            // run_single_job() would catch this too, but its "[job] busy" reads
            // as a UI murmur; a missed tick is worth an error.
            if (cycle_running.load()) {
                append_log("[sched] ERROR: previous cycle still running; tick "
                           "skipped, waiting for the next interval");
                return;
            }
            run_single_job();
        },
        _immediate_first_tick);

    if (_immediate_first_tick) {
        append_log("[sched] armed - immediate tick once, then "
                   "sleep(interval+jitter) and repeat until Stop or exit");
    } else {
        append_log("[sched] armed - sleep(interval+jitter) then tick, repeat "
                   "until Stop or exit");
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
            std::string tool;
            int index = 0;
            {
                // The UI pushes this snapshot every frame, so it already
                // carries what the Settings fields say.
                std::lock_guard<std::mutex> lk(m_cfg_mu);
                adb_path = m_cfg.adb.path;
                serial = m_cfg.adb.serial;
                tool = m_cfg.mumu.tool_path;
                index = m_cfg.mumu.device_index;
            }
            bool online = false;
            if (!adb_path.empty()) {
                // The instance is a TCP device: adb lists it as `device` only
                // after somebody connects, and it picks a new port every
                // launch. Asking mumutool and connecting here is what keeps the
                // chip honest -- reading `adb devices` alone reports "not
                // connected" for an instance that is up and usable.
                const std::string addr =
                    campcat::mumu_adb_address(tool, index);
                const std::string target = addr.empty() ? serial : addr;
                campcat::adb_client adb(adb_path, target);
                if (!addr.empty()) {
                    adb.connect_remote(addr, k_poll_adb_timeout_ms);
                }
                online = campcat::device_online(
                    adb.list_devices(k_poll_adb_timeout_ms), target);
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
