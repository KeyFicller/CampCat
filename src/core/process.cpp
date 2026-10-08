#include "core/process.h"

#ifndef _WIN32
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace campcat::process {

std::vector<std::string> take_lines(std::string *_buffer) {
  std::vector<std::string> lines;
  if (_buffer == nullptr) {
    return lines;
  }
  std::size_t start = 0;
  while (true) {
    const std::size_t nl = _buffer->find('\n', start);
    if (nl == std::string::npos) {
      break;
    }
    lines.push_back(_buffer->substr(start, nl - start));
    start = nl + 1;
  }
  if (start > 0) {
    _buffer->erase(0, start);
  }
  return lines;
}

#ifndef _WIN32

namespace {

/// Close a descriptor if open, then mark it closed.
void close_fd(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

bool read_pipe_nonblocking(int fd, std::string *acc, int timeout_ms,
                           bool *eof_reached) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  int pr = poll(&pfd, 1, timeout_ms);
  if (pr == 0) {
    return true; // timeout, not necessarily finished
  }
  if (pr < 0) {
    return false;
  }
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0 && acc) {
      acc->append(buf, static_cast<size_t>(n));
    }
    *eof_reached = (n == 0);
    return true;
  }
  char buf[8192];
  ssize_t n = read(fd, buf, sizeof(buf));
  if (n > 0) {
    if (acc) {
      acc->append(buf, static_cast<size_t>(n));
    }
    return true;
  }
  if (n == 0) {
    *eof_reached = true;
    return true;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return true; // transient: retry on the next slice
  }
  *eof_reached = true; // real read error: stop retrying this fd
  return false;
}

/// Drain one pipe after `poll` marked it readable / hung up (large stdout must
/// not stall).
bool drain_pipe_after_poll(int fd, short revents, std::string *acc,
                           bool *eof_reached) {
  if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0 && acc) {
      acc->append(buf, static_cast<size_t>(n));
    }
    if (n <= 0) {
      *eof_reached = true;
    }
    return true;
  }
  if (!(revents & POLLIN)) {
    return false;
  }
  char buf[65536];
  ssize_t n = read(fd, buf, sizeof(buf));
  if (n > 0) {
    if (acc) {
      acc->append(buf, static_cast<size_t>(n));
    }
    return true;
  }
  if (n == 0) {
    *eof_reached = true;
    return true;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return true;
  }
  *eof_reached = true;
  return false;
}

} // namespace

void child::adopt(child &_other) noexcept {
  m_pid = _other.m_pid;
  m_in = _other.m_in;
  m_out = _other.m_out;
  m_err = _other.m_err;
  _other.m_pid = -1;
  _other.m_in = -1;
  _other.m_out = -1;
  _other.m_err = -1;
}

void child::close_pipes() {
  close_fd(&m_in);
  close_fd(&m_out);
  close_fd(&m_err);
}

void child::terminate() {
  if (m_pid > 0) {
    kill(static_cast<pid_t>(m_pid), SIGKILL);
    waitpid(static_cast<pid_t>(m_pid), nullptr, 0);
    m_pid = -1;
  }
}

void child::release() {
  close_pipes();
  terminate();
}

std::optional<child> spawn(const std::string &_exe,
                           const std::vector<std::string> &_argv,
                           std::string *_error_out) {
  int in_pipe[2]{};
  int out_pipe[2]{};
  int err_pipe[2]{};
  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    if (_error_out) {
      *_error_out = "failed to create pipes for the child process";
    }
    close_fd(&in_pipe[0]);
    close_fd(&in_pipe[1]);
    close_fd(&out_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&err_pipe[0]);
    close_fd(&err_pipe[1]);
    return std::nullopt;
  }

  // Built before fork(): see the note on spawn() in the header.
  std::vector<char *> ptrs;
  ptrs.reserve(_argv.size() + 1);
  for (const auto &s : _argv) {
    ptrs.push_back(const_cast<char *>(s.data()));
  }
  ptrs.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    if (_error_out) {
      *_error_out = "fork failed for the child process";
    }
    close_fd(&in_pipe[0]);
    close_fd(&in_pipe[1]);
    close_fd(&out_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&err_pipe[0]);
    close_fd(&err_pipe[1]);
    return std::nullopt;
  }

  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close_fd(&in_pipe[0]);
    close_fd(&in_pipe[1]);
    close_fd(&out_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&err_pipe[0]);
    close_fd(&err_pipe[1]);
    signal(SIGPIPE, SIG_DFL);
    execvp(_exe.c_str(), ptrs.data());
    _exit(127);
  }

  close_fd(&in_pipe[0]);  // child's stdin read end
  close_fd(&out_pipe[1]); // child's stdout write end
  close_fd(&err_pipe[1]); // child's stderr write end
  return child(static_cast<long>(pid), in_pipe[1], out_pipe[0], err_pipe[0]);
}

bool run_process_posix(const std::string &exe,
                       const std::vector<std::string> &argv,
                       std::string *stdout_out, std::string *stderr_out,
                       int timeout_ms, int *exit_code) {
  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0) {
    return false;
  }
  if (pipe(err_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }

  // Build the exec argv before fork(): the child must avoid heap allocation
  // (another thread may hold the allocator lock at fork time).
  std::vector<char *> ptrs;
  ptrs.reserve(argv.size() + 1);
  for (const auto &s : argv) {
    ptrs.push_back(const_cast<char *>(s.data()));
  }
  ptrs.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return false;
  }

  if (pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);

    signal(SIGPIPE, SIG_DFL);
    execvp(exe.c_str(), ptrs.data());
    _exit(127);
  }

  close(out_pipe[1]);
  close(err_pipe[1]);

  if (stdout_out) {
    stdout_out->clear();
  }
  if (stderr_out) {
    stderr_out->clear();
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  bool out_eof = false;
  bool err_eof = false;

  while (true) {
    int remaining_ms =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count());
    if (remaining_ms < 0) {
      remaining_ms = 0;
    }

    int status = 0;
    const pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      if (exit_code) {
        if (WIFEXITED(status)) {
          *exit_code = WEXITSTATUS(status);
        } else {
          *exit_code = -1;
        }
      }
      break;
    }

    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
      close(out_pipe[0]);
      close(err_pipe[0]);
      return false;
    }

    int slice = std::min(std::max(remaining_ms, 1), 50);
    pollfd fds[2]{};
    int nfds = 0;
    if (!out_eof) {
      fds[nfds].fd = out_pipe[0];
      fds[nfds].events = POLLIN;
      ++nfds;
    }
    if (!err_eof) {
      fds[nfds].fd = err_pipe[0];
      fds[nfds].events = POLLIN;
      ++nfds;
    }

    if (nfds == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    const int pr = poll(fds, static_cast<unsigned int>(nfds), slice);
    if (pr < 0) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
      close(out_pipe[0]);
      close(err_pipe[0]);
      return false;
    }

    for (int i = 0; i < nfds; ++i) {
      if (fds[i].revents != 0) {
        if (fds[i].fd == out_pipe[0]) {
          drain_pipe_after_poll(fds[i].fd, fds[i].revents,
                                stdout_out ? stdout_out : nullptr, &out_eof);
        } else {
          drain_pipe_after_poll(fds[i].fd, fds[i].revents,
                                stderr_out ? stderr_out : nullptr, &err_eof);
        }
      }
    }
  }

  while (!out_eof) {
    bool ok = read_pipe_nonblocking(
        out_pipe[0], stdout_out ? stdout_out : nullptr, 50, &out_eof);
    if (!ok) {
      break;
    }
  }
  while (!err_eof) {
    bool ok = read_pipe_nonblocking(
        err_pipe[0], stderr_out ? stderr_out : nullptr, 50, &err_eof);
    if (!ok) {
      break;
    }
  }

  close(out_pipe[0]);
  close(err_pipe[0]);
  return true;
}

#else // _WIN32

void child::adopt(child &) noexcept {}
void child::close_pipes() {}
void child::terminate() {}
void child::release() {}

std::optional<child> spawn(const std::string &, const std::vector<std::string> &,
                           std::string *_error_out) {
  if (_error_out) {
    *_error_out = "process spawning is not supported on Windows";
  }
  return std::nullopt;
}

bool run_process_posix(const std::string &, const std::vector<std::string> &,
                       std::string *, std::string *, int, int *) {
  return false;
}

#endif // _WIN32

} // namespace campcat::process
