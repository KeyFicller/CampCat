#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace campcat {

/**
 * @brief Logging buffer.
 */
class log_buffer {
public:
  /**
   * @brief Push a line to the buffer.
   * @param[in] _line The line to push.
   */
  void push(std::string _line);

  /**
   * @brief Drain the buffer to a vector.
   * @param[out] _out The vector to drain to.
   */
  void drain(std::vector<std::string> *_out);

  /**
   * @brief Discard all queued lines (e.g. UI clear alongside on-screen deque).
   */
  void clear();

  /**
   * @brief Get the approximate size of the buffer.
   * @return The approximate size of the buffer.
   */
  size_t size_approx() const;

private:
  mutable std::mutex m_mu;
  std::deque<std::string> m_lines;

  static constexpr size_t k_max_lines = 4000;
};

} // namespace campcat
