#pragma once

#include <atomic>
#include <optional>
#include <utility>

namespace campcat {

/// One device job at a time. Its owner may lend the same token to nested tools.
/// The lease must outlive its owners; shell_state joins its workers before exit.
class device_task_lease {
public:
    class owner {
    public:
        owner(const owner &) = delete;
        owner &operator=(const owner &) = delete;
        owner(owner &&_other) noexcept
            : m_lease(std::exchange(_other.m_lease, nullptr)) {}
        owner &operator=(owner &&_other) noexcept {
            if (this != &_other) {
                release();
                m_lease = std::exchange(_other.m_lease, nullptr);
            }
            return *this;
        }
        ~owner() { release(); }

    private:
        friend class device_task_lease;
        explicit owner(device_task_lease *_lease) : m_lease(_lease) {}
        void release() {
            if (m_lease) {
                m_lease->m_busy.store(false);
                m_lease = nullptr;
            }
        }
        device_task_lease *m_lease;
    };

    std::optional<owner> try_acquire() {
        bool expected = false;
        if (!m_busy.compare_exchange_strong(expected, true)) {
            return std::nullopt;
        }
        return owner(this);
    }
    bool busy() const { return m_busy.load(); }
    bool owns(const owner &_owner) const { return _owner.m_lease == this; }

private:
    std::atomic<bool> m_busy{false};
};

} // namespace campcat
