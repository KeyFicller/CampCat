#pragma once

#include "ccat_driver/ccat_script_profile.h"
#include "core/app_config.h"
#include "core/log_buffer.h"
#include "core/mumu_launcher.h"
#include "core/scheduler.h"

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>

namespace campcat {

/**
 * @brief The shell's mutable state and its background jobs.
 *
 * What used to be `main()`'s locals and its capturing lambdas, gathered here so
 * the pages can act on the same state. The members are public on purpose: the
 * shell and all four pages read and write them, and accessors would turn this
 * move into a rewrite. Tighten later if it ever earns it.
 *
 * No ImGui in here: this is state and jobs, not a view.
 */
class shell_state {
public:
  /**
   * @brief Read the config, load the .ccat bundle, seed the UI buffers, hook up
   * the log sink.
   * @param[in] _cfg_path Shell config file; File > Save config writes back here.
   */
  explicit shell_state(std::filesystem::path _cfg_path);

  shell_state(const shell_state &) = delete;
  shell_state &operator=(const shell_state &) = delete;

  /** @return Snapshot of the current config, under lock. */
  app_config snapshot_cfg();
  /** @return Snapshot of the current .ccat bundle, under lock. */
  ccat_script_profile snapshot_ccat_live();
  /** @brief Publish a fresh .ccat bundle after a script switch, under lock. */
  void set_ccat_live(const ccat_script_profile &_profile);

  /** @brief Emit one line to `automation_log`. */
  void append_log(const std::string &_s);

  /** @brief Fold the UI buffers into `_dst`. */
  void merge_buffers_into(app_config *_dst, const app_config &_ui);
  /** @brief Fold the UI buffers in and commit them as the live config. */
  void push_job_snapshot(app_config *_view, const ccat_script_profile &_ui);
  /** @brief Write the live config to disk. */
  bool persist(std::string *_error_out);
  /** @brief Fold the UI buffers in, write to disk, commit only on success. */
  void save_config(const app_config &_view, const ccat_script_profile &_ui);

  /** @brief Run one automation cycle. */
  void run_single_job();
  /** @brief Run a .ccat source snippet from the console. */
  void run_console_source_job(std::string _source);
  /** @brief Launch the MuMu instance and arm its watchdog. */
  void start_mumu_job();
  /**
   * @brief Launch the instance and wait until it is up, on the calling thread.
   *
   * Same sequence, watchdog, ownership record and Settings status line as
   * `start_mumu_job`; that job's thread body is this call. Blocking: up to 60s
   * for the app plus 180s for the instance. Refuses when another launch, or an
   * automation cycle, already owns the flags.
   *
   * @param[in] _should_stop Polled by the waits; true gives up early and comes
   * back as a failed status that says so. Empty never stops.
   */
  campcat::mumu_status start_mumu_blocking(
      const std::function<bool()> &_should_stop = {});
  /** @brief Reconfigure and arm the scheduler. */
  void refresh_scheduler_locked(bool _immediate_first_tick = false);

  /** @brief Exit path: request stop, stop the scheduler, join the worker. */
  void stop_workers();
  /** @brief Quit the instance and app this run started, then drop the watchdog. */
  void close_launched_mumu();

  const std::filesystem::path cfg_path;

  log_buffer log_lines;
  std::deque<std::string> log_scroll;
  std::atomic<bool> cycle_running{false};
  std::atomic<bool> stop_requested{false};
  /// Named `sched`, not `scheduler`: the member would shadow the type.
  campcat::scheduler sched;
  std::thread worker_thread;

  std::atomic<bool> mumu_launching{false};
  std::mutex mumu_mu;
  campcat::mumu_status mumu_report;
  std::optional<campcat::process::child> mumu_watchdog;
  std::string mumu_launched_path;
  int mumu_launched_index = -1;
  std::string mumu_app_started;

  char adb_path_buf[512]{};
  char serial_buf[256]{};
  char adb_connect_buf[256]{};
  char mumutool_buf[512]{};
  int mumu_index_buf = 0;
  /// Last active script id, so the combo can tell a real switch from a redraw.
  std::string last_script_seen;

private:
  /**
   * @brief The launch sequence itself, on the calling thread.
   *
   * Enquire, launch the app when it is down, `open`, wait for ready, arm the
   * watchdog, record what we started, and publish `mumu_report` on every exit
   * path. The caller owns `mumu_launching` and `cycle_running`; nothing here
   * touches `worker_thread` or ImGui.
   */
  campcat::mumu_status mumu_launch_sequence(
      const campcat::app_config &_shell,
      const std::function<bool()> &_should_stop);
  void run_automation_thread_body(campcat::app_config _snap_shell,
                                  const ccat_script_profile &_snap_ccat);
  std::tuple<campcat::app_config, ccat_script_profile> capture_bundle_snapshot();

  std::mutex m_cfg_mu;
  app_config m_cfg;
  ccat_script_profile m_ccat_live;
};

} // namespace campcat
