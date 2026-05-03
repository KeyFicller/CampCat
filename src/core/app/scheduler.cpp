#include "core/scheduler.h"

namespace campcat {

scheduler::scheduler() = default;

scheduler::~scheduler() {
  stop();
}

void scheduler::configure(std::chrono::seconds _interval,
                          std::chrono::seconds _jitter) {
  std::lock_guard<std::mutex> lk(m_mu);
  m_interval = _interval;
  m_jitter = _jitter;
}

void scheduler::start(task_fn _task, bool _run_immediately_first) {
  stop();
  {
    std::lock_guard<std::mutex> lk(m_mu);
    m_task = std::move(_task);
    m_run_immediately_first = _run_immediately_first;
    m_waiting_for_tick = false;
  }
  m_stop_requested = false;
  m_thread = std::thread([this] { loop(); });
}

void scheduler::stop() {
  m_stop_requested = true;
  m_cv.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
  m_stop_requested = false;
  std::lock_guard<std::mutex> lk(m_mu);
  m_waiting_for_tick = false;
}

void scheduler::request_stop_cycle() {}

scheduler_tick_wait_progress scheduler::tick_wait_progress() const {
  std::lock_guard<std::mutex> lk(m_mu);
  scheduler_tick_wait_progress p{};
  if (!m_waiting_for_tick) {
    return p;
  }
  p.in_wait_phase = true;
  p.duration_seconds = m_wait_duration_sec;
  const auto now = std::chrono::steady_clock::now();
  p.elapsed_seconds =
      std::chrono::duration<double>(now - m_wait_started).count();
  return p;
}

void scheduler::loop() {
  // Runs until stop(): optional initial skip of presleep, then sleep(interval+jitter), task, repeat.
  std::uniform_int_distribution<int> dist(0, static_cast<int>(m_jitter.count()));

  bool skip_presleep = false;
  {
    std::lock_guard<std::mutex> lk(m_mu);
    skip_presleep = m_run_immediately_first;
  }

  while (!m_stop_requested.load()) {
    if (!skip_presleep) {
      std::chrono::seconds sleep_add{0};
      {
        std::lock_guard<std::mutex> lk(m_mu);
        const int j = m_jitter.count() > 0 ? dist(m_rng) : 0;
        sleep_add = m_interval + std::chrono::seconds(j);
      }

      const auto sleep_started = std::chrono::steady_clock::now();
      {
        std::lock_guard<std::mutex> lk(m_mu);
        m_wait_started = sleep_started;
        m_wait_duration_sec = static_cast<double>(sleep_add.count());
        m_waiting_for_tick = true;
      }

      bool wake_stop = false;
      {
        std::unique_lock<std::mutex> lk(m_mu);
        wake_stop =
            m_cv.wait_for(lk, sleep_add,
                          [&] { return m_stop_requested.load(); });
      }

      {
        std::lock_guard<std::mutex> lk(m_mu);
        m_waiting_for_tick = false;
      }

      if (wake_stop) {
        break;
      }
    }
    skip_presleep = false;

    task_fn local;
    {
      std::lock_guard<std::mutex> lk(m_mu);
      local = m_task;
    }
    if (local) {
      local();
    }

    if (m_stop_requested.load()) {
      break;
    }
  }
}

} // namespace campcat
