#include "core/automation_log.h"

#include <mutex>
#include <utility>
#include <vector>

namespace campcat {

namespace {

struct sink_entry {
    std::size_t id = 0;
    automation_log::sink_fn fn;
};

struct log_state {
    std::mutex mu;
    std::vector<sink_entry> sinks;
    std::size_t next_id = 1; ///< 0 is the empty handle, so ids start at 1
};

log_state &state() {
    // Intentionally leaked: a function-local static would still be destroyed
    // during shutdown, where worker threads may emit and use a destroyed mutex.
    static log_state *const s = new log_state{};
    return *s;
}

} // namespace

automation_log::handle::handle(handle &&_other) noexcept : m_id(_other.m_id) {
    _other.m_id = 0;
}

automation_log::handle &
automation_log::handle::operator=(handle &&_other) noexcept {
    if (this != &_other) {
        remove(m_id);
        m_id = _other.m_id;
        _other.m_id = 0;
    }
    return *this;
}

automation_log::handle::~handle() { remove(m_id); }

automation_log::handle automation_log::add_sink(sink_fn _fn) {
    std::lock_guard<std::mutex> lk(state().mu);
    const std::size_t id = state().next_id++;
    state().sinks.push_back(sink_entry{id, std::move(_fn)});
    return handle(id);
}

void automation_log::set_sink(sink_fn _fn) {
    std::lock_guard<std::mutex> lk(state().mu);
    state().sinks.clear();
    const std::size_t id = state().next_id++;
    state().sinks.push_back(sink_entry{id, std::move(_fn)});
}

void automation_log::clear_sink() {
    std::lock_guard<std::mutex> lk(state().mu);
    state().sinks.clear();
}

void automation_log::emit(const std::string &_line) {
    std::vector<sink_entry> copy;
    {
        std::lock_guard<std::mutex> lk(state().mu);
        copy = state().sinks;
    }
    // Outside the lock on purpose: a sink may emit again, or drop its own
    // handle, and either would deadlock if the lock were still held.
    for (const sink_entry &e : copy) {
        if (e.fn) {
            e.fn(_line);
        }
    }
}

void automation_log::remove(std::size_t _id) {
    if (_id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(state().mu);
    std::vector<sink_entry> &sinks = state().sinks;
    for (auto it = sinks.begin(); it != sinks.end(); ++it) {
        if (it->id == _id) {
            sinks.erase(it);
            return;
        }
    }
}

} // namespace campcat
