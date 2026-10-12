#pragma once

#include <opencv2/core.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace campcat {

/**
 * @brief The device operations the automation layers depend on.
 *
 * `adb_client` is the only implementation that ships; the seam exists so tests
 * can drive the layers above it with no device attached. It stops where the
 * module boundary already is: every `adb` invocation stays in `adb_client`.
 */
class adb_api {
public:
    virtual ~adb_api() = default;

    /// See `adb_client::list_devices`.
    virtual std::vector<std::string> list_devices(int _timeout_ms = 8000) = 0;

    /// See `adb_client::set_serial`.
    virtual void set_serial(const std::string &_serial) = 0;

    /// See `adb_client::run`.
    virtual bool run(const std::vector<std::string> &_args,
                     std::string *_stdout_out, std::string *_stderr_out,
                     int _timeout_ms) = 0;

    /// See `adb_client::connect_remote`.
    virtual bool connect_remote(std::string_view _address, int _timeout_ms = 15000,
                                std::string *_stdout_out = nullptr,
                                std::string *_stderr_out = nullptr) = 0;

    /// See `adb_client::tap`.
    virtual bool tap(int _x, int _y) = 0;

    /// See `adb_client::swipe`.
    virtual bool swipe(int _x1, int _y1, int _x2, int _y2,
                       int _duration_ms) = 0;

    /// See `adb_client::keyevent`.
    virtual bool keyevent(int _keycode) = 0;

    /// See `adb_client::home_and_kill_all`.
    virtual bool home_and_kill_all(int _gap_ms) = 0;

    /// See `adb_client::screencap_bgr`.
    virtual bool screencap_bgr(cv::Mat *_bgr_out, int _timeout_ms = 45000,
                               std::string *_diagnostic_out = nullptr) = 0;

    /// See `adb_client::delay_after_action`.
    virtual bool delay_after_action(
        std::chrono::milliseconds _tap_delay) const = 0;
};

/**
 * @brief Thin wrapper around the host `adb` binary: device enumeration, invoke
 * adb with arguments, screencap, tap/swipe helpers.
 */
class adb_client : public adb_api {
public:
    /**
     * @brief Construct a client pinned to binaries `adb`.
     * @param[in] _adb_path Filesystem path to the `adb` executable.
     * @param[in] _serial Empty for default transport, or emulator/device
     * serial.
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
     * @param[in] _timeout_ms Budget for the subprocess. The device poll
     * shortens it: this call sits on a 2s timer, and an `adb` that hangs must
     * not make the poll -- or the join on it at exit -- wait four intervals.
     * @return Serialized device descriptors (similar to adb output tokens).
     */
    std::vector<std::string> list_devices(int _timeout_ms = 8000) override;

    /**
     * @brief Emulator/device serial (if any).
     * @return The active serial identifier.
     */
    const std::string &serial() const { return m_serial; }

    /**
     * @brief Switch target device for subsequent adb calls before `run(...)`.
     * @param[in] _serial Emulator/device serial; empty restores default
     * semantics.
     */
    void set_serial(const std::string &_serial) override { m_serial = _serial; }

    /**
     * @brief Run `adb` with additional arguments forwarded after base prefix.
     * @param[in] _args Extra argv tokens (omit leading `adb` path).
     * @param[out] _stdout_out Optional captured stdout buffer.
     * @param[out] _stderr_out Optional captured stderr buffer.
     * @param[in] _timeout_ms Milliseconds allowed before cancelling the
     * subprocess.
     * @return True if the subprocess finished without transport errors.
     */
    bool run(const std::vector<std::string> &_args, std::string *_stdout_out,
             std::string *_stderr_out, int _timeout_ms) override;

    /**
     * @brief When `_address` is non-empty, run `adb connect <address>` (TCP
     * device).
     * @return False when the connection did not come up. `adb connect` exits 0
     * even on a refused connection, so the output is what decides; an empty
     * `_address` skips the call and returns true.
     */
    bool connect_remote(std::string_view _address, int _timeout_ms = 15000,
                        std::string *_stdout_out = nullptr,
                        std::string *_stderr_out = nullptr) override;

    /**
     * @brief Tap at normalized screen coordinates understood by adb.
     * @param[in] _x Horizontal pixel coordinate.
     * @param[in] _y Vertical pixel coordinate.
     * @return False when adb execution fails or returns error.
     */
    bool tap(int _x, int _y) override;

    /**
     * @brief Swipe from start to end with optional swipe duration hint.
     * @param[in] _x1 Start X.
     * @param[in] _y1 Start Y.
     * @param[in] _x2 End X.
     * @param[in] _y2 End Y.
     * @param[in] _duration_ms Swipe gesture duration forwarded to adb.
     * @return False when adb execution fails or returns error.
     */
    bool swipe(int _x1, int _y1, int _x2, int _y2, int _duration_ms) override;

    /**
     * @brief Send an Android keyevent (`adb shell input keyevent <code>`).
     * @param[in] _keycode Android KEYCODE_* integer (e.g. 3 = HOME).
     * @return False when adb execution fails or returns error.
     */
    bool keyevent(int _keycode) override;

    /**
     * @brief HOME, then `am force-stop` every non-home package in recents.
     * @param[in] _gap_ms Delay after HOME before dumpsys / force-stop.
     * @return False when HOME or dumpsys fails, or any force-stop fails.
     */
    bool home_and_kill_all(int _gap_ms) override;

    /**
     * @brief Capture the device screen as an OpenCV BGR image.
     *
     * Prefers the raw framebuffer (`adb exec-out screencap`) and repacks it on
     * the host; falls back to the PNG paths when the raw payload is not one we
     * can decode. All three paths yield a pixel-identical frame.
     *
     * @param[out] _bgr_out Output buffer written on success.
     * @param[in] _timeout_ms adb wait budget.
     * @param[out] _diagnostic_out Optional adb stderr/snippet collector; a
     * failed raw attempt leaves no noise here, the PNG paths do.
     * @return True when a frame was produced.
     */
    bool screencap_bgr(cv::Mat *_bgr_out, int _timeout_ms = 45000,
                       std::string *_diagnostic_out = nullptr) override;

    /**
     * @brief Sleeping helper aligning with configurable shell tap pauses.
     * @param[in] _tap_delay Sleep duration propagated to `sleep` adb pattern.
     * @return True when adb reported success.
     */
    bool delay_after_action(std::chrono::milliseconds _tap_delay) const override;

private:
    std::vector<std::string> base_prefix() const;

    std::string m_adb_path;
    std::string m_serial;
};

/// True when `adb devices` output lists `_serial`, or lists anything at all
/// when `_serial` is empty -- which is how a client with no `-s` picks its one
/// device. This is what the shell's device chip reports as connected. Offline
/// and unauthorized devices never reach `devices`: `list_devices` keeps only
/// the ones in state `device`.
bool device_online(const std::vector<std::string> &_devices,
                   const std::string &_serial);

} // namespace campcat
