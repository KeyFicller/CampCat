#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace campcat {

/// The latest follow-up, shared by the UI and the single LLM worker.
class llm_prompt_queue {
public:
    bool replace(std::string _prompt) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return false;
        }
        m_next = std::move(_prompt);
        return true;
    }

    std::optional<std::string> take() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping || m_next.empty()) {
            return std::nullopt;
        }
        return std::exchange(m_next, {});
    }

    /// True when a message was still queued, rather than already running.
    bool stop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        const bool cancelled = !m_next.empty();
        m_next.clear();
        return cancelled;
    }

    void restart() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = false;
    }

    bool stopping() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping;
    }

private:
    mutable std::mutex m_mutex;
    std::string m_next;
    bool m_stopping = false;
};

} // namespace campcat
