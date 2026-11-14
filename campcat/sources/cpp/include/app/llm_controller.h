#pragma once

#include "app/device_task_lease.h"
#include "app/llm_prompt_queue.h"
#include "core/llm/llm_host.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace campcat {

class shell_state;

/// ImGui-free request lifecycle. The panel owns presentation and GL resources.
class llm_controller {
public:
    /// Promotion shares the completion stream so fast turns cannot overtake it.
    enum class completion_kind { turn, memory, reset, promotion };
    struct completion {
        completion_kind kind = completion_kind::turn;
        llm_result result;
        llm_host::timeline timeline;
    };
    using job = std::function<llm_result(const device_task_lease::owner &)>;
    using turn_job = std::function<llm_result(
        const std::string &, const device_task_lease::owner &)>;
    struct request {
        completion_kind kind = completion_kind::turn;
        job work;
        turn_job followup;
    };

    explicit llm_controller(llm_host &_host) : m_host(_host) {}
    ~llm_controller();
    llm_controller(const llm_controller &) = delete;
    llm_controller &operator=(const llm_controller &) = delete;

    /// Acquire before starting the worker; callbacks also support headless
    /// jobs.
    bool start(device_task_lease &_tasks, request _request);
    bool start_turn(shell_state &_st, const app_config &_cfg,
                    std::string _prompt);
    bool start_memorize(shell_state &_st, const app_config &_cfg);
    bool start_reset(shell_state &_st, const app_config &_cfg);

    bool queue_prompt(std::string _prompt, bool *_replaced = nullptr);
    /// Returns whether a queued message was cancelled. Running work keeps its
    /// lease.
    bool request_stop();
    bool running() const { return m_running.load(); }
    bool resetting() const { return m_resetting.load(); }
    bool memorizing() const { return m_memorizing.load(); }
    bool stopping() const { return m_prompts.stopping(); }
    std::optional<completion> take_completion();

    /// Join active work, then start the final pass without waiting for it.
    void begin_shutdown(std::function<void()> _final = {});
    void begin_shutdown(const app_config &_cfg);
    /// Called after window teardown, so the final pass cannot freeze the
    /// window.
    void finish_shutdown();

private:
    turn_job make_turn_job(shell_state &_st, app_config _cfg);
    void run(request _request, device_task_lease::owner _owner);
    void interrupt_work();
    void memorize_both(const app_config &_cfg, int _timeout_ms,
                       llm_result *_operator_out);
    void post(completion_kind _kind, llm_result _result);

    llm_host &m_host;
    llm_prompt_queue m_prompts;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_resetting{false};
    std::atomic<bool> m_memorizing{false};
    /// Serializes start, enqueue and completion, never the job itself.
    std::mutex m_lifecycle_mu;
    bool m_accepting = true;
    bool m_shutdown_started = false;
    std::thread m_worker;
    std::thread m_shutdown_worker;
    std::mutex m_done_mu;
    std::deque<completion> m_done;
};

} // namespace campcat
