#pragma once

#include <opencv2/core.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace campcat {

/**
 * @brief Thin wrapper around the host `adb` binary: device enumeration, invoke
 * adb with arguments, screencap, tap/swipe helpers.
 */
class adb_client {
public:
  /**
   * @brief Construct a client pinned to binaries `adb`.
   * @param[in] _adb_path Filesystem path to the `adb` executable.
   * @param[in] _serial Empty for default transport, or emulator/device serial.
   */
  adb_client(const std::string &_adb_path, const std::string &_serial);

public:
  /**
   * @brief Absolute path previously passed to ctor.
   * @return The configured `adb` path string.
   */
  const std::string &adb_path() const { return m_adb_path; }

  /**
   * @brief List devices/emulators parsed from `adb devices`.
   * @return Serialized device descriptors (similar to adb output tokens).
   */
  std::vector<std::string> list_devices();

  /**
   * @brief Emulator/device serial (if any).
   * @return The active serial identifier.
   */
  const std::string &serial() const { return m_serial; }

  /**
   * @brief Switch target device for subsequent adb calls before `run(...)`.
   * @param[in] _serial Emulator/device serial; empty restores default semantics.
   */
  void set_serial(const std::string &_serial) { m_serial = _serial; }

  /**
   * @brief Run `adb` with additional arguments forwarded after base prefix.
   * @param[in] _args Extra argv tokens (omit leading `adb` path).
   * @param[out] _stdout_out Optional captured stdout buffer.
   * @param[out] _stderr_out Optional captured stderr buffer.
   * @param[in] _timeout_ms Milliseconds allowed before cancelling the subprocess.
   * @return True if the subprocess finished without transport errors.
   */
  bool run(const std::vector<std::string> &_args, std::string *_stdout_out,
           std::string *_stderr_out, int _timeout_ms);

  /**
   * @brief When `_address` is non-empty, run `adb connect <address>` (TCP device).
   * @return True if skipped (empty address) or subprocess exited 0; same contract as `run`.
   */
  bool connect_remote(std::string_view _address, int _timeout_ms = 15000,
                      std::string *_stdout_out = nullptr,
                      std::string *_stderr_out = nullptr);

  /**
   * @brief Tap at normalized screen coordinates understood by adb.
   * @param[in] _x Horizontal pixel coordinate.
   * @param[in] _y Vertical pixel coordinate.
   * @return False when adb execution fails or returns error.
   */
  bool tap(int _x, int _y);

  /**
   * @brief Swipe from start to end with optional swipe duration hint.
   * @param[in] _x1 Start X.
   * @param[in] _y1 Start Y.
   * @param[in] _x2 End X.
   * @param[in] _y2 End Y.
   * @param[in] _duration_ms Swipe gesture duration forwarded to adb.
   * @return False when adb execution fails or returns error.
   */
  bool swipe(int _x1, int _y1, int _x2, int _y2, int _duration_ms);

  /**
   * @brief Pipe `adb exec-out screencap -p` into an OpenCV BGR decode.
   * @param[out] _bgr_out Output buffer written on success.
   * @param[in] _timeout_ms adb wait budget.
   * @param[out] _diagnostic_out Optional adb stderr/snippet collector.
   * @return True when the png payload decodes cleanly.
   */
  bool screencap_png(cv::Mat *_bgr_out, int _timeout_ms = 45000,
                     std::string *_diagnostic_out = nullptr);

  /**
   * @brief Sleeping helper aligning with configurable shell tap pauses.
   * @param[in] _tap_delay Sleep duration propagated to `sleep` adb pattern.
   * @return True when adb reported success.
   */
  bool delay_after_action(std::chrono::milliseconds _tap_delay) const;

private:
  std::vector<std::string> base_prefix() const;

  std::string m_adb_path;
  std::string m_serial;
};

} // namespace campcat
