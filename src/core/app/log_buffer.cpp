#include "core/log_buffer.h"

namespace campcat {

void log_buffer::push(std::string _line) {
  std::lock_guard<std::mutex> lk(m_mu);
  m_lines.push_back(std::move(_line));
  while (m_lines.size() > k_max_lines) {
    m_lines.pop_front();
  }
}

void log_buffer::drain(std::vector<std::string>* _out) {
  std::lock_guard<std::mutex> lk(m_mu);
  _out->clear();
  _out->reserve(m_lines.size());
  while (!m_lines.empty()) {
    _out->push_back(std::move(m_lines.front()));
    m_lines.pop_front();
  }
}

void log_buffer::clear() {
  std::lock_guard<std::mutex> lk(m_mu);
  m_lines.clear();
}

size_t log_buffer::size_approx() const {
  std::lock_guard<std::mutex> lk(m_mu);
  return m_lines.size();
}

} // namespace campcat
