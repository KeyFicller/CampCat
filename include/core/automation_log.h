#pragma once

#include <functional>
#include <string>

namespace campcat {

/**
 * @brief Process-wide log sink for automation/UI threads (replaces threaded log_fn plumbing).
 *
 * The shell installs a sink once (typically timestamps then pushes to `log_buffer`);
 * drivers and interpreters call `emit()` from any thread.
 */
class automation_log {
public:
  using sink_fn = std::function<void(const std::string &)>;

  static void set_sink(sink_fn _fn);
  static void clear_sink();

  /** @brief Thread-safe; dropped silently until `set_sink` is called. */
  static void emit(const std::string &_line);
};

} // namespace campcat
