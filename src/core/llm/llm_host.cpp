#include "core/llm/llm_host.h"

#include "core/automation_log.h"
#include "core/llm/llm_protocol.h"

#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace campcat {

namespace {

constexpr int k_poll_slice_ms = 100;
constexpr char k_default_script_rel[] = "llm/main.py";
constexpr char k_venv_python_rel[] = ".venv/bin/python3";

/// Generous enough to cover a cold Python + LangChain import.
constexpr int k_startup_timeout_ms = 60000;
/// One model round trip; a vision call on a large screenshot is not fast.
constexpr int k_request_timeout_ms = 120000;

/// The interpreter and script are found by convention, never configured.
void resolve_sidecar_paths(const std::filesystem::path &_repo_root,
                           std::filesystem::path *_python,
                           std::filesystem::path *_script) {
  const std::filesystem::path venv = _repo_root / k_venv_python_rel;
  std::error_code ec;
  *_python = std::filesystem::exists(venv, ec) ? venv
                                               : std::filesystem::path("python3");
  *_script = _repo_root / k_default_script_rel;
}

} // namespace

llm_host::~llm_host() { stop(); }

#ifndef _WIN32

void llm_host::handle_line(const std::string &_line) {
  llm_protocol::message msg;
  if (!llm_protocol::parse_line(_line, &msg)) {
    return;
  }

  switch (msg.type) {
  case llm_protocol::message::kind::ready:
    {
      std::lock_guard<std::mutex> lk(m_mu);
      m_ready = true;
    }
    m_cv.notify_all();
    break;
  case llm_protocol::message::kind::log:
    automation_log::emit("[llm] " + msg.log_message);
    break;
  case llm_protocol::message::kind::result:
    {
      std::lock_guard<std::mutex> lk(m_mu);
      if (msg.id == m_pending_id) {
        m_result.ok = msg.ok;
        m_result.text = msg.text;
        m_result.error = msg.error;
        m_result.turns = msg.turns;
        m_has_result = true;
      }
    }
    m_cv.notify_all();
    break;
  case llm_protocol::message::kind::chunk:
    {
      // Deliberately no notify: the UI polls per frame, so waking the waiter
      // on every delta would only churn.
      std::lock_guard<std::mutex> lk(m_mu);
      if (msg.id == m_pending_id) {
        m_stream += msg.text;
        m_thinking = m_thinking || msg.thinking;
      }
    }
    break;
  case llm_protocol::message::kind::unknown:
  default:
    break;
  }
}

std::string llm_host::streaming_text() const {
  std::lock_guard<std::mutex> lk(m_mu);
  return m_stream;
}

bool llm_host::is_thinking() const {
  std::lock_guard<std::mutex> lk(m_mu);
  return m_thinking;
}

void llm_host::clear_stream() {
  std::lock_guard<std::mutex> lk(m_mu);
  m_stream.clear();
  m_thinking = false;
}

void llm_host::pump(int _fd, bool _is_stderr) {
  if (_fd < 0) {
    return;
  }
  std::string buffer;
  while (true) {
    {
      std::lock_guard<std::mutex> lk(m_mu);
      if (m_stopping) {
        return;
      }
    }
    pollfd pfd{};
    pfd.fd = _fd;
    pfd.events = POLLIN;
    const int pr = poll(&pfd, 1, k_poll_slice_ms);
    if (pr < 0) {
      break;
    }
    if (pr == 0) {
      continue;
    }
    char chunk[65536];
    const ssize_t n = read(_fd, chunk, sizeof(chunk));
    if (n <= 0) {
      break;
    }
    buffer.append(chunk, static_cast<std::size_t>(n));
    for (const std::string &line : process::take_lines(&buffer)) {
      if (_is_stderr) {
        if (!line.empty()) {
          automation_log::emit("[llm][py] " + line);
        }
      } else {
        handle_line(line);
      }
    }
  }

  // EOF on stdout: the child is gone or closed it. Fail any in-flight request.
  if (!_is_stderr) {
    {
      std::lock_guard<std::mutex> lk(m_mu);
      m_broken = true;
    }
    m_cv.notify_all();
  }
}

bool llm_host::write_all(const std::string &_data, std::string *_error_out) {
  const int fd = m_child.in();
  if (fd < 0) {
    if (_error_out) {
      *_error_out = "sidecar stdin is closed";
    }
    return false;
  }
  std::size_t written = 0;
  while (written < _data.size()) {
    const ssize_t n = write(fd, _data.data() + written, _data.size() - written);
    if (n > 0) {
      written += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EINTR)) {
      continue;
    }
    if (_error_out) {
      *_error_out = "failed to write to the sidecar";
    }
    return false;
  }
  return true;
}

bool llm_host::ensure_running(const std::filesystem::path &_repo_root,
                              std::string *_error_out) {
  {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_broken && m_ready && m_child.pid() > 0) {
      return true;
    }
  }
  teardown();

  std::filesystem::path python;
  std::filesystem::path script;
  resolve_sidecar_paths(_repo_root, &python, &script);

  std::error_code ec;
  if (!std::filesystem::exists(script, ec)) {
    if (_error_out) {
      *_error_out = "sidecar script not found: " + script.string();
    }
    return false;
  }

  const std::vector<std::string> argv = {python.string(), script.string()};
  std::optional<process::child> spawned =
      process::spawn(python.string(), argv, _error_out);
  if (!spawned.has_value()) {
    return false;
  }

  int out_fd = -1;
  int err_fd = -1;
  {
    std::lock_guard<std::mutex> lk(m_mu);
    m_child = std::move(*spawned);
    out_fd = m_child.out();
    err_fd = m_child.err();
  }
  m_reader = std::thread([this, out_fd]() { pump(out_fd, false); });
  m_stderr_reader = std::thread([this, err_fd]() { pump(err_fd, true); });

  std::unique_lock<std::mutex> lk(m_mu);
  const bool ready = m_cv.wait_for(
      lk, std::chrono::milliseconds(k_startup_timeout_ms),
      [this]() { return m_ready || m_broken; });
  if (!ready) {
    lk.unlock();
    teardown();
    if (_error_out) {
      *_error_out = "sidecar did not become ready in time (check " +
                    python.string() + " and `uv pip install -r "
                    "requirements.txt`)";
    }
    return false;
  }
  if (m_broken || !m_ready) {
    lk.unlock();
    teardown();
    if (_error_out) {
      *_error_out = "sidecar exited during startup (see [llm][py] log lines)";
    }
    return false;
  }
  return true;
}

long llm_host::next_request_id() {
  std::lock_guard<std::mutex> lk(m_mu);
  const long id = m_next_id++;
  m_pending_id = id;
  m_has_result = false;
  m_result = llm_result{};
  m_stream.clear();
  m_thinking = false;
  return id;
}

llm_result llm_host::exchange(const std::string &_request_line) {
  llm_result r;
  std::string err;
  if (!write_all(_request_line, &err)) {
    teardown();
    r.error = err;
    return r;
  }

  std::unique_lock<std::mutex> lk(m_mu);
  const bool got = m_cv.wait_for(
      lk, std::chrono::milliseconds(k_request_timeout_ms),
      [this]() { return m_has_result || m_broken; });
  if (!got) {
    lk.unlock();
    teardown();
    r.error = "sidecar timed out after " +
              std::to_string(k_request_timeout_ms) + " ms";
    return r;
  }
  if (!m_has_result) {
    lk.unlock();
    teardown();
    r.error = "sidecar exited before answering";
    return r;
  }
  return m_result;
}

llm_result llm_host::describe(const std::filesystem::path &_repo_root,
                              const cv::Mat &_bgr,
                              const std::string &_prompt) {
  llm_result r;
  if (_bgr.empty() && _prompt.empty()) {
    r.error = "empty request";
    return r;
  }

  // A text-only turn needs no image, so encoding is skipped when there is none.
  std::string b64;
  if (!_bgr.empty()) {
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", _bgr, png) || png.empty()) {
      r.error = "failed to encode image as PNG";
      return r;
    }
    b64 = llm_protocol::base64_encode(png.data(), png.size());
  }

  std::string err;
  if (!ensure_running(_repo_root, &err)) {
    r.error = err;
    return r;
  }

  const long id = next_request_id();
  std::string request = llm_protocol::build_describe_request(b64, id, _prompt);
  request.push_back('\n');
  return exchange(request);
}

llm_result llm_host::reset(const std::filesystem::path &_repo_root) {
  llm_result r;
  {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_child.pid() <= 0 || m_broken || !m_ready) {
      // No child, so no history: report success without spawning one.
      r.ok = true;
      r.turns = 0;
      return r;
    }
  }
  (void)_repo_root;

  const long id = next_request_id();
  std::string request = llm_protocol::build_reset_request(id);
  request.push_back('\n');
  return exchange(request);
}

void llm_host::teardown() {
  {
    std::lock_guard<std::mutex> lk(m_mu);
    m_stopping = true;
  }
  m_cv.notify_all();

  // Closing the pipes first unblocks the readers still waiting in poll().
  m_child.close_pipes();
  if (m_reader.joinable()) {
    m_reader.join();
  }
  if (m_stderr_reader.joinable()) {
    m_stderr_reader.join();
  }
  m_child.terminate();

  std::lock_guard<std::mutex> lk(m_mu);
  m_ready = false;
  m_broken = false;
  m_stopping = false;
  m_has_result = false;
  m_stream.clear();
  m_thinking = false;
}

#else // _WIN32

void llm_host::handle_line(const std::string &) {}
void llm_host::pump(int, bool) {}

std::string llm_host::streaming_text() const { return std::string(); }
bool llm_host::is_thinking() const { return false; }
void llm_host::clear_stream() {}

long llm_host::next_request_id() { return 0; }

llm_result llm_host::exchange(const std::string &) {
  llm_result r;
  r.error = "the LLM sidecar is not supported on Windows";
  return r;
}

bool llm_host::write_all(const std::string &, std::string *_error_out) {
  if (_error_out) {
    *_error_out = "the LLM sidecar is not supported on Windows";
  }
  return false;
}

bool llm_host::ensure_running(const std::filesystem::path &,
                              std::string *_error_out) {
  if (_error_out) {
    *_error_out = "the LLM sidecar is not supported on Windows";
  }
  return false;
}

llm_result llm_host::describe(const std::filesystem::path &, const cv::Mat &,
                              const std::string &) {
  llm_result r;
  r.error = "the LLM sidecar is not supported on Windows";
  return r;
}

llm_result llm_host::reset(const std::filesystem::path &) {
  llm_result r;
  r.error = "the LLM sidecar is not supported on Windows";
  return r;
}

void llm_host::teardown() {}

#endif // _WIN32

void llm_host::stop() { teardown(); }

} // namespace campcat
