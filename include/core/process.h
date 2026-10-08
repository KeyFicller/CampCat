#pragma once

#include <optional>
#include <string>
#include <vector>

namespace campcat::process {

/**
 * @brief RAII handle to a spawned child process and its stdio pipes.
 *
 * Owns the pid and the three descriptors. Destruction closes the pipes and
 * reaps the child, so a spawned process cannot outlive its handle. Move-only.
 */
class child {
public:
  child() = default;

  /**
   * @param[in] _pid Child process id.
   * @param[in] _in Write end feeding the child's stdin (`-1` if absent).
   * @param[in] _out Read end draining the child's stdout (`-1` if absent).
   * @param[in] _err Read end draining the child's stderr (`-1` if absent).
   */
  child(long _pid, int _in, int _out, int _err)
      : m_pid(_pid), m_in(_in), m_out(_out), m_err(_err) {}

  ~child() { release(); }

  child(const child &) = delete;
  child &operator=(const child &) = delete;

  child(child &&_other) noexcept { adopt(_other); }

  child &operator=(child &&_other) noexcept {
    if (this != &_other) {
      release();
      adopt(_other);
    }
    return *this;
  }

  /// @return Child pid, or `-1` when empty.
  long pid() const { return m_pid; }
  /// @return Write end for the child's stdin, or `-1`.
  int in() const { return m_in; }
  /// @return Read end for the child's stdout, or `-1`.
  int out() const { return m_out; }
  /// @return Read end for the child's stderr, or `-1`.
  int err() const { return m_err; }

  /// Close the stdio pipes, signalling EOF to the child. Idempotent.
  void close_pipes();

  /// SIGKILL the child if alive and reap it. Idempotent.
  void terminate();

private:
  void adopt(child &_other) noexcept;
  void release();

  long m_pid = -1;
  int m_in = -1;
  int m_out = -1;
  int m_err = -1;
};

/**
 * @brief Spawn `_exe` with `_argv`, piped on all three stdio streams.
 *
 * `_argv` is passed to `execvp` as-is, so `_argv[0]` should be the program
 * name. The argv vector is built before `fork`, keeping the child free of heap
 * allocation (another thread may hold the allocator lock at fork time).
 *
 * @param[in] _error_out Optional human-readable failure detail.
 * @return The child handle, or `std::nullopt` on failure.
 */
std::optional<child> spawn(const std::string &_exe,
                           const std::vector<std::string> &_argv,
                           std::string *_error_out);

/**
 * @brief Split `_buffer` into complete `\n`-terminated lines.
 *
 * The trailing partial line stays in `_buffer` for the next call, so a caller
 * can feed it arbitrary read chunks.
 */
std::vector<std::string> take_lines(std::string *_buffer);

/**
 * @brief Run `_exe` to completion, collecting stdout and stderr.
 *
 * Both pipes are polled together so a large stream cannot stall the other.
 *
 * @param[out] _stdout_out Cleared then filled; may be null.
 * @param[out] _stderr_out Cleared then filled; may be null.
 * @param[out] _exit_code Process exit status; may be null.
 * @return False on spawn failure, timeout (after SIGKILL), or a poll error.
 */
bool run_process_posix(const std::string &_exe,
                       const std::vector<std::string> &_argv,
                       std::string *_stdout_out, std::string *_stderr_out,
                       int _timeout_ms, int *_exit_code);

} // namespace campcat::process
