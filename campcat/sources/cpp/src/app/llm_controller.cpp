#include "app/llm_controller.h"

#include "app/shell_state.h"
#include "core/adb_client.h"
#include "core/automation_log.h"
#include "core/mumu_launcher.h"

#include <exception>
#include <utility>

namespace campcat {

llm_controller::~llm_controller() {
    begin_shutdown();
    finish_shutdown();
}

bool llm_controller::start(device_task_lease &_tasks, request _request) {
    std::lock_guard<std::mutex> lock(m_lifecycle_mu);
    if (!m_accepting || m_running.load() || !_request.work ||
        _request.kind == completion_kind::promotion) {
        return false;
    }
    auto owner = _tasks.try_acquire();
    if (!owner) {
        return false;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_prompts.restart();
    m_host.clear_interrupt();
    if (_request.kind == completion_kind::turn) {
        m_host.clear_stream();
    }
    m_resetting.store(_request.kind == completion_kind::reset);
    m_memorizing.store(_request.kind != completion_kind::turn);
    m_running.store(true);
    try {
        m_worker = std::thread([this, request = std::move(_request),
                                owner = std::move(*owner)]() mutable {
            run(std::move(request), std::move(owner));
        });
    } catch (const std::exception &ex) {
        m_running.store(false);
        m_resetting.store(false);
        m_memorizing.store(false);
        automation_log::emit(std::string("[llm] failed to start worker: ") +
                              ex.what());
        return false;
    }
    return true;
}

void llm_controller::post(completion_kind _kind, llm_result _result) {
    completion done;
    done.kind = _kind;
    done.result = std::move(_result);
    if (_kind == completion_kind::turn) {
        done.timeline = m_host.copy_timeline();
    }
    std::lock_guard<std::mutex> lock(m_done_mu);
    m_done.push_back(std::move(done));
}

void llm_controller::run(request _request, device_task_lease::owner _owner) {
    std::optional<device_task_lease::owner> owner(std::move(_owner));
    auto kind = _request.kind;
    auto work = std::move(_request.work);
    while (true) {
        llm_result result;
        try {
            if (m_prompts.stopping()) {
                result.ok = true;
                result.interrupted = true;
            } else {
                result = work(*owner);
            }
        } catch (const std::exception &ex) {
            result.error = ex.what();
        } catch (...) {
            result.error = "unknown worker exception";
        }

        std::lock_guard<std::mutex> lock(m_lifecycle_mu);
        m_memorizing.store(false);
        if (kind != completion_kind::memory || !m_host.interrupt_pending()) {
            post(kind, std::move(result));
        }
        std::optional<std::string> next;
        if (kind != completion_kind::reset && _request.followup &&
            !m_prompts.stopping()) {
            // Stop and enqueue share this lock, so clearing an old interrupt
            // cannot erase a newer Stop or lose a just-accepted message.
            m_host.clear_interrupt();
            next = m_prompts.take();
        }
        if (!next) {
            owner.reset();
            m_resetting.store(false);
            m_running.store(false);
            return;
        }
        m_host.clear_stream();
        post(completion_kind::promotion, {});
        kind = completion_kind::turn;
        work = [followup = _request.followup,
                 prompt = std::move(*next)](const auto &_owner) {
            return followup(prompt, _owner);
        };
    }
}

void llm_controller::interrupt_work() {
    m_host.request_interrupt();
    llm::request_script_stop();
    llm::request_emulator_stop();
    m_host.release_approval();
}

bool llm_controller::queue_prompt(std::string _prompt, bool *_replaced) {
    std::lock_guard<std::mutex> lock(m_lifecycle_mu);
    if (!m_accepting || !m_running.load() || m_resetting.load() ||
        m_prompts.stopping()) {
        return false;
    }
    if (_replaced) {
        *_replaced = m_prompts.pending();
    }
    if (!m_prompts.replace(std::move(_prompt))) {
        return false;
    }
    interrupt_work();
    return true;
}

bool llm_controller::request_stop() {
    std::lock_guard<std::mutex> lock(m_lifecycle_mu);
    const bool cancelled = m_prompts.stop();
    interrupt_work();
    return cancelled;
}

std::optional<llm_controller::completion> llm_controller::take_completion() {
    std::lock_guard<std::mutex> lock(m_done_mu);
    if (m_done.empty()) {
        return std::nullopt;
    }
    auto done = std::move(m_done.front());
    m_done.pop_front();
    return done;
}

llm_controller::turn_job llm_controller::make_turn_job(shell_state &_st,
                                                       app_config _cfg) {
    return [this, st = &_st, cfg = std::move(_cfg)](
               const std::string &_prompt,
               const device_task_lease::owner &_owner) {
        const std::string addr =
            mumu_adb_address(cfg.mumu.tool_path, cfg.mumu.device_index);
        adb_client adb(cfg.adb.path, addr.empty() ? cfg.adb.serial : addr);
        adb.connect_remote(addr);
        llm::tool_context ctx;
        ctx.adb = &adb;
        ctx.adb_busy = [st, &_owner] { return !st->device_tasks.owns(_owner); };
        // Approval and snap settings stay live while the rest is a snapshot.
        ctx.needs_approval = [st](std::string_view _tool) {
            return llm::approval_gate_required(st->snapshot_cfg().approval, _tool);
        };
        ctx.box_snap = [st] { return st->snapshot_cfg().box_snap; };
        ctx.cfg = &cfg;
        ctx.script_dir = cfg.config_home;
        ctx.start_emulator = [st, &_owner](const auto &_should_stop) {
            return st->start_mumu_blocking(_should_stop, &_owner);
        };
        m_host.set_tool_context(ctx);
        struct context_cleanup {
            llm_host &host;
            ~context_cleanup() { host.set_tool_context({}); }
        } cleanup{m_host};
        return m_host.run_turn(cfg.config_home, cfg.llm.provider,
                               cfg.script_bundle, _prompt,
                               cfg.llm.max_dispatches, cfg.llm.max_model_rounds,
                               cfg.region_score.record);
    };
}

bool llm_controller::start_turn(shell_state &_st, const app_config &_cfg,
                                 std::string _prompt) {
    request next;
    next.followup = make_turn_job(_st, _cfg);
    next.work = [turn = next.followup,
                  prompt = std::move(_prompt)](const auto &_owner) {
        return turn(prompt, _owner);
    };
    return start(_st.device_tasks, std::move(next));
}

void llm_controller::memorize_both(const app_config &_cfg, int _timeout_ms,
                                   llm_result *_operator_out) {
    *_operator_out = m_host.memorize(
        _cfg.config_home, _cfg.llm.provider, _cfg.script_bundle,
        llm::memory_kind::operator_notes, _timeout_ms);
    if (m_host.interrupt_pending()) {
        return;
    }
    const auto scripter = m_host.memorize(
        _cfg.config_home, _cfg.llm.provider, _cfg.script_bundle,
        llm::memory_kind::scripter_notes, _timeout_ms);
    if (!scripter.ok) {
        automation_log::emit("[llm] scripter memory not saved: " + scripter.error);
    }
}

bool llm_controller::start_memorize(shell_state &_st, const app_config &_cfg) {
    request next;
    next.kind = completion_kind::memory;
    next.followup = make_turn_job(_st, _cfg);
    next.work = [this, cfg = _cfg](const auto &) {
        llm_result result;
        memorize_both(cfg, k_memorize_timeout_ms, &result);
        return result;
    };
    return start(_st.device_tasks, std::move(next));
}

bool llm_controller::start_reset(shell_state &_st, const app_config &_cfg) {
    request next;
    next.kind = completion_kind::reset;
    next.work = [this, cfg = _cfg](const auto &) {
        llm_result kept;
        memorize_both(cfg, k_memorize_timeout_ms, &kept);
        if (!kept.ok) {
            automation_log::emit("[llm] memory not saved: " + kept.error);
        }
        const std::filesystem::path home(cfg.config_home);
        return m_host.reset(home.parent_path(), home);
    };
    return start(_st.device_tasks, std::move(next));
}

void llm_controller::begin_shutdown(std::function<void()> _final) {
    {
        std::lock_guard<std::mutex> lock(m_lifecycle_mu);
        if (m_shutdown_started) {
            return;
        }
        m_shutdown_started = true;
        m_accepting = false;
    }
    request_stop();
    m_host.cancel_approval();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    if (_final) {
        m_host.clear_interrupt();
        m_shutdown_worker = std::thread([final = std::move(_final)] {
            try {
                final();
            } catch (const std::exception &ex) {
                automation_log::emit(std::string("[llm] shutdown memory failed: ") +
                                      ex.what());
            }
        });
    }
}

void llm_controller::begin_shutdown(const app_config &_cfg) {
    if (_cfg.config_home.empty()) {
        begin_shutdown();
        return;
    }
    begin_shutdown([this, cfg = _cfg] {
        llm_result result;
        memorize_both(cfg, k_shutdown_memorize_timeout_ms, &result);
        if (!result.ok) {
            automation_log::emit("[llm] memory not saved: " + result.error);
        }
    });
}

void llm_controller::finish_shutdown() {
    if (m_shutdown_worker.joinable()) {
        m_shutdown_worker.join();
    }
    m_host.stop();
}

} // namespace campcat
