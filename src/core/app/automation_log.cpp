#include "core/automation_log.h"

#include <mutex>

namespace campcat {

namespace {

std::mutex g_mu;
automation_log::sink_fn g_sink;

} // namespace

void automation_log::set_sink(sink_fn _fn) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_sink = std::move(_fn);
}

void automation_log::clear_sink() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_sink = {};
}

void automation_log::emit(const std::string &_line) {
  sink_fn fn;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    fn = g_sink;
  }
  if (fn) {
    fn(_line);
  }
}

} // namespace campcat
