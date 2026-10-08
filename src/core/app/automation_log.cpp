#include "core/automation_log.h"

#include <mutex>

namespace campcat {

namespace {

struct log_state {
  std::mutex mu;
  automation_log::sink_fn sink;
};

log_state &state() {
  // Intentionally leaked: a function-local static would still be destroyed
  // during shutdown, where worker threads may emit and use a destroyed mutex.
  static log_state *const s = new log_state{};
  return *s;
}

} // namespace

void automation_log::set_sink(sink_fn _fn) {
  std::lock_guard<std::mutex> lk(state().mu);
  state().sink = std::move(_fn);
}

void automation_log::clear_sink() {
  std::lock_guard<std::mutex> lk(state().mu);
  state().sink = {};
}

void automation_log::emit(const std::string &_line) {
  sink_fn fn;
  {
    std::lock_guard<std::mutex> lk(state().mu);
    fn = state().sink;
  }
  if (fn) {
    fn(_line);
  }
}

} // namespace campcat
