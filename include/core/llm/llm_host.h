#pragma once

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

#include "core/process.h"

namespace campcat {

/**
 * @brief One-shot outcome of a describe request.
 */
struct llm_result {
  bool ok = false;
  std::string text; ///< model output when `ok`
  std::string error; ///< human-readable failure detail
};

/**
 * @brief Owns the persistent Python sidecar and speaks NDJSON to it.
 *
 * The C++ side knows nothing about the model, endpoint or prompts: it hands
 * over an image and reads back text. Those settings live in `llm/` (see
 * `llm/models.py`), where the child process reads them itself.
 *
 * The child is started lazily on the first `describe` and reused afterwards.
 * `describe` never throws; every failure is reported through `llm_result`.
 */
class llm_host {
public:
  llm_host() = default;
  ~llm_host();

  llm_host(const llm_host &) = delete;
  llm_host &operator=(const llm_host &) = delete;

  /**
   * @brief Encode `_bgr` as PNG and ask the sidecar to describe it.
   * @param[in] _repo_root Directory holding `config/`; anchors `.venv` and `llm/`.
   * @param[in] _bgr Non-empty BGR frame.
   */
  llm_result describe(const std::filesystem::path &_repo_root,
                      const cv::Mat &_bgr);

  /// Terminate the child if running. Safe to call repeatedly.
  void stop();

private:
  bool ensure_running(const std::filesystem::path &_repo_root,
                      std::string *_error_out);
  /// Poll one child pipe until EOF, feeding lines to the stdout/stderr sink.
  void pump(int _fd, bool _is_stderr);
  void handle_line(const std::string &_line);
  bool write_all(const std::string &_data, std::string *_error_out);
  void teardown();

  /// Owns the sidecar pid and its stdio pipes; destroyed on restart/shutdown.
  process::child m_child;

  std::thread m_reader;
  std::thread m_stderr_reader;

  std::mutex m_mu;
  std::condition_variable m_cv;
  bool m_ready = false;
  bool m_broken = false;
  bool m_stopping = false;

  long m_next_id = 1;
  long m_pending_id = 0;
  bool m_has_result = false;
  llm_result m_result;
};

} // namespace campcat
