#include "core/adb_client.h"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace campcat {

namespace {

#ifndef _WIN32
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
  }
  return true;
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
  }
  return true;
}

bool run_process_posix(const std::string &exe,
                       const std::vector<std::string> &argv,
                       std::string *stdout_out, std::string *stderr_out,
                       int timeout_ms, int *exit_code) {
  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    return false;
  }

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

    std::vector<char *> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (auto &s : argv) {
      ptrs.push_back(const_cast<char *>(s.data()));
    }
    ptrs.push_back(nullptr);

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
#endif

} // namespace

adb_client::adb_client(const std::string &_adb_path,
                       const std::string &_serial)
    : m_adb_path(_adb_path), m_serial(_serial) {}

std::vector<std::string> adb_client::base_prefix() const {
  std::vector<std::string> v;
  v.push_back(m_adb_path);
  if (!m_serial.empty()) {
    v.emplace_back("-s");
    v.push_back(m_serial);
  }
  return v;
}

bool adb_client::run(const std::vector<std::string>& _args,
                     std::string* _stdout_out, std::string* _stderr_out,
                     int _timeout_ms) {
#ifndef _WIN32
  std::vector<std::string> argv = base_prefix();
  argv.insert(argv.end(), _args.begin(), _args.end());
  int code = 0;
  const bool ok = run_process_posix(m_adb_path, argv, _stdout_out, _stderr_out,
                                    _timeout_ms, &code);
  return ok && code == 0;
#else
  (void)_args;
  (void)_stdout_out;
  (void)_stderr_out;
  (void)_timeout_ms;
  return false;
#endif
}

bool adb_client::connect_remote(std::string_view _address, int _timeout_ms,
                                std::string *_stdout_out,
                                std::string *_stderr_out) {
  if (_address.empty()) {
    return true;
  }
  return run({"connect", std::string(_address)}, _stdout_out, _stderr_out,
             _timeout_ms);
}

std::vector<std::string> adb_client::list_devices() {
  std::vector<std::string> devices;
  std::string out;
#ifndef _WIN32
  std::vector<std::string> argv = {m_adb_path, "devices"};
  int code = 0;
  if (!run_process_posix(m_adb_path, argv, &out, nullptr, 8000, &code) ||
      code != 0) {
    return devices;
  }
#endif
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.find("List of devices") != std::string::npos) {
      continue;
    }
    if (line.empty()) {
      continue;
    }
    std::istringstream ls(line);
    std::string serial;
    std::string state;
    ls >> serial >> state;
    if (!serial.empty() && state == "device") {
      devices.push_back(serial);
    }
  }
  return devices;
}

bool adb_client::screencap_png(cv::Mat* _bgr_out, int _timeout_ms,
                               std::string* _diagnostic_out) {
  auto note = [&](std::string_view msg) {
    if (_diagnostic_out) {
      _diagnostic_out->append(std::string(msg));
    }
  };

  auto try_decode = [&](std::string&& bin) -> bool {
    if (bin.empty()) {
      return false;
    }
    std::vector<uint8_t> raw(bin.begin(), bin.end());
    cv::Mat decoded = cv::imdecode(raw, cv::IMREAD_COLOR);
    if (decoded.empty()) {
      note("imdecode failed (not PNG or corrupt); ");
      return false;
    }
    *_bgr_out = decoded;
    return true;
  };

  auto attempt = [&](const char* label,
                     const std::vector<std::string>& suffix) -> bool {
    std::vector<std::string> argv = base_prefix();
    argv.insert(argv.end(), suffix.begin(), suffix.end());
    std::string bin;
    std::string err;
    int code = 0;
    if (!run_process_posix(m_adb_path, argv, &bin, &err, _timeout_ms, &code)) {
      note(std::string(label) + ": subprocess failed/timeout; ");
      return false;
    }
    if (code != 0) {
      note(std::string(label) + ": exit " + std::to_string(code) + "; ");
    }
    if (!err.empty()) {
      note(std::string(label) + " stderr: " + err + "; ");
    }
    if (try_decode(std::move(bin))) {
      return true;
    }
    note(std::string(label) + ": decode failed; ");
    return false;
  };

#ifndef _WIN32
  if (attempt("exec-out screencap -p", {"exec-out", "screencap", "-p"})) {
    return true;
  }
  // MuMu / some images: shell path is more reliable than exec-out.
  if (attempt("shell screencap -p", {"shell", "screencap", "-p"})) {
    return true;
  }
#else
  (void)attempt;
  (void)note;
  (void)try_decode;
#endif
  return false;
}

bool adb_client::tap(int _x, int _y) {
  std::vector<std::string> args = {"shell", "input", "tap",
                                     std::to_string(_x), std::to_string(_y)};
  return run(args, nullptr, nullptr, 8000);
}

bool adb_client::swipe(int _x1, int _y1, int _x2, int _y2, int _duration_ms) {
  std::vector<std::string> args = {"shell",
                                   "input",
                                   "swipe",
                                   std::to_string(_x1),
                                   std::to_string(_y1),
                                   std::to_string(_x2),
                                   std::to_string(_y2),
                                   std::to_string(_duration_ms)};
  return run(args, nullptr, nullptr, 12000);
}

bool adb_client::delay_after_action(std::chrono::milliseconds _tap_delay) const {
  std::this_thread::sleep_for(_tap_delay);
  return true;
}

} // namespace campcat
