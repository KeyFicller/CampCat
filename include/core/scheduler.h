#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <random>
#include <thread>

namespace campcat {

/**
 * @brief UI diagnostics snapshot describing scheduler idle/wait bookkeeping.
 */
struct scheduler_tick_wait_progress {
  bool in_wait_phase = false;
  double elapsed_seconds = 0;
  double duration_seconds = 1;
};

/**
 * @brief Dedicated worker thread looping `sleep(interval +/- jitter)` then fire a task functor.
 */
class scheduler {
public:
  using task_fn = std::function<void()>;

  /**
   * @brief Construct empty scheduler placeholder (stopped).
   */
  scheduler();

  /**
   * @brief Join the worker thread if still joinable.
   */
  ~scheduler();

  /**
   * @brief Set interval jitter before `start`.
   * @param[in] _interval Base interval between executions.
   * @param[in] _jitter Added random jitter [0,_jitter] seconds.
   */
  void configure(std::chrono::seconds _interval,
                 std::chrono::seconds _jitter);

  /**
   * @brief Spawn background loop honoring configure parameters.
   * @param[in] _task Callback executed after each timed wait until `stop`.
   * @param[in] _run_immediately_first When true skips first sleep slab.
   */
  void start(task_fn _task, bool _run_immediately_first = false);

  /**
   * @brief Request cooperative shutdown and join the worker thread.
   */
  void stop();

  /**
   * @brief Hint long-running `_task` to abort midflight (caller driven).
   */
  void request_stop_cycle();

  /**
   * @brief Whether the auxiliary thread owns an active scheduler loop handle.
   * @return True while background thread remained joinable.
   */
  bool running() const { return m_thread.joinable(); }

  /**
   * @brief Peek current wait-phase progress guarded by mutex.
   * @return Copy safe for Dear ImGui render thread consumption.
   */
  scheduler_tick_wait_progress tick_wait_progress() const;

private:
  void loop();

  mutable std::mutex m_mu;
  std::condition_variable m_cv;
  std::thread m_thread;
  std::atomic<bool> m_stop_requested{false};

  std::chrono::seconds m_interval{3600};
  std::chrono::seconds m_jitter{0};
  bool m_run_immediately_first{false};
  task_fn m_task;

  std::chrono::steady_clock::time_point m_wait_started{};
  double m_wait_duration_sec{1};
  bool m_waiting_for_tick{false};

  std::mt19937 m_rng{std::random_device{}()};
};

} // namespace campcat
