#include "core/adb_client.h"
#include "core/process.h"

#include <opencv2/imgcodecs.hpp>

#include <cctype>
#include <chrono>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace campcat {

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
  const bool ok = process::run_process_posix(m_adb_path, argv, _stdout_out, _stderr_out,
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
  if (!process::run_process_posix(m_adb_path, argv, &out, nullptr, 8000, &code) ||
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
    if (!process::run_process_posix(m_adb_path, argv, &bin, &err, _timeout_ms, &code)) {
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

bool adb_client::keyevent(int _keycode) {
  std::vector<std::string> args = {"shell", "input", "keyevent",
                                   std::to_string(_keycode)};
  return run(args, nullptr, nullptr, 8000);
}

namespace {

/** Packages from `dumpsys activity recents` Task headers (skip type=home). */
std::vector<std::string>
packages_from_recents_dump(const std::string &_dump) {
  std::vector<std::string> pkgs;
  std::unordered_set<std::string> seen;
  std::istringstream iss(_dump);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.find("Recent #") == std::string::npos ||
        line.find("Task{") == std::string::npos) {
      continue;
    }
    if (line.find("type=home") != std::string::npos) {
      continue;
    }
    const auto apos = line.find("A=");
    if (apos == std::string::npos) {
      continue;
    }
    // Header form is `A=com.example.app` or `A=com.example.app/.Activity`; the
    // token ends at whitespace or the enclosing `}`, and the optional `/`
    // suffix names the activity.
    size_t i = apos + 2;
    size_t j = i;
    while (j < line.size()) {
      const unsigned char c = static_cast<unsigned char>(line[j]);
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '}') {
        break;
      }
      ++j;
    }
    if (i >= j) {
      continue;
    }
    std::string pkg = line.substr(i, j - i);
    const auto slash = pkg.find('/');
    if (slash != std::string::npos) {
      pkg.resize(slash);
    }
    if (pkg.empty()) {
      continue;
    }
    if (seen.insert(pkg).second) {
      pkgs.push_back(std::move(pkg));
    }
  }
  return pkgs;
}

} // namespace

bool adb_client::home_and_kill_all(int _gap_ms) {
  // KEYCODE_HOME == 3
  if (!keyevent(3)) {
    return false;
  }
  if (_gap_ms > 0) {
    (void)delay_after_action(std::chrono::milliseconds(_gap_ms));
  }

  // am kill-all only drops cached processes; recent apps stay alive on MuMu.
  // force-stop each non-home package listed in recents instead.
  std::string dump;
  if (!run({"shell", "dumpsys", "activity", "recents"}, &dump, nullptr,
           20000)) {
    return false;
  }
  const auto pkgs = packages_from_recents_dump(dump);
  for (const auto &pkg : pkgs) {
    if (!run({"shell", "am", "force-stop", pkg}, nullptr, nullptr, 15000)) {
      return false;
    }
  }
  return keyevent(3);
}

bool adb_client::delay_after_action(std::chrono::milliseconds _tap_delay) const {
  std::this_thread::sleep_for(_tap_delay);
  return true;
}

} // namespace campcat
