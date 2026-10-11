#include "core/mumu_launcher.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#ifndef _WIN32

namespace campcat {

namespace {

/// One mumutool call is a short local round trip; this is only a hang guard.
constexpr int k_timeout_ms = 15000;
constexpr int k_poll_ms = 1000;
/// `close` must come before the app is quit: with the app gone, mumutool has
/// nothing to talk to and the instance would be left running.
constexpr char k_watchdog_script[] =
    "trap '' INT TERM; "
    "while kill -0 \"$1\" 2>/dev/null; do sleep 1; done; "
    "\"$2\" close \"$3\"; "
    "[ -z \"$4\" ] || /usr/bin/osascript -e \"quit app \\\"$4\\\"\"";

/// What mumutool writes to stderr (exit 1) when no MuMuPlayer is running to
/// answer it. ponytail: its stderr text is the only signal it gives; if MuMu
/// rewords this, the state falls back to `failed`, which stops instead of
/// launching something on a guess.
constexpr char k_service_down_marker[] = "invalidPort";

/// The states mumutool reports while the instance cannot be used yet.
/// ponytail: `starting` is a guess (only `stopped` was observed). If a boot
/// shows another value, add it here -- otherwise a boot reads as ready.
bool not_ready(mumu_state _state) {
    return _state == mumu_state::stopped || _state == mumu_state::starting;
}

bool run_mumutool(const std::string &_mumutool,
                  const std::vector<std::string> &_args, std::string *_out,
                  std::string *_message) {
    std::vector<std::string> argv{_mumutool};
    argv.insert(argv.end(), _args.begin(), _args.end());
    std::string err;
    int code = 0;
    if (!process::run_process_posix(_mumutool, argv, _out, &err, k_timeout_ms,
                                    &code)) {
        *_message = "mumutool did not run: " + err;
        return false;
    }
    if (code != 0) {
        *_message = "mumutool exited " + std::to_string(code) + ": " + err;
        return false;
    }
    return true;
}

/// Every mumutool answer is `{"errcode":0,...}`; anything else is the failure.
bool check_errcode(const std::string &_out, std::string *_message) {
    const auto j = nlohmann::json::parse(_out, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        *_message = "unreadable mumutool output";
        return false;
    }
    const int errcode = j.value("errcode", -1);
    if (errcode != 0) {
        *_message = "mumutool errcode " + std::to_string(errcode) + ": " +
                    j.value("message", std::string{});
        return false;
    }
    return true;
}

} // namespace

mumu_status mumu_query_state(const std::string &_mumutool, int _index) {
    mumu_status st;
    std::string out;
    if (!run_mumutool(_mumutool, {"info", std::to_string(_index)}, &out,
                      &st.message) ||
        !check_errcode(out, &st.message)) {
        st.state = st.message.find(k_service_down_marker) != std::string::npos
                       ? mumu_state::service_down
                       : mumu_state::failed;
        return st;
    }
    const auto j = nlohmann::json::parse(out, nullptr, false);
    const std::string raw = j.contains("return")
                                ? j["return"].value("state", std::string{})
                                : std::string{};
    // The raw string rides along whatever the mapping says, so a state this
    // file has never seen is visible in the UI instead of being silently
    // misread.
    st.message = raw;
    if (raw.empty()) {
        st.state = mumu_state::unknown;
    } else if (raw == "stopped") {
        st.state = mumu_state::stopped;
    } else if (raw == "starting") {
        st.state = mumu_state::starting;
    } else {
        st.state = mumu_state::running;
    }
    return st;
}

mumu_status mumu_open(const std::string &_mumutool, int _index) {
    mumu_status st;
    std::string out;
    if (!run_mumutool(_mumutool, {"open", std::to_string(_index)}, &out,
                      &st.message) ||
        !check_errcode(out, &st.message)) {
        st.state = mumu_state::failed;
        return st;
    }
    // Accepted, not up: the caller waits on `mumu_wait_ready`.
    st.state = mumu_state::starting;
    return st;
}

mumu_status mumu_close(const std::string &_mumutool, int _index) {
    mumu_status st;
    std::string out;
    if (!run_mumutool(_mumutool, {"close", std::to_string(_index)}, &out,
                      &st.message) ||
        !check_errcode(out, &st.message)) {
        st.state = mumu_state::failed;
        return st;
    }
    st.state = mumu_state::stopped;
    return st;
}

mumu_status mumu_wait_ready(const std::string &_mumutool, int _index,
                            int _deadline_ms,
                            const std::function<bool()> &_should_stop) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(_deadline_ms);
    mumu_status st = mumu_query_state(_mumutool, _index);
    while (not_ready(st.state) && std::chrono::steady_clock::now() < deadline &&
           !(_should_stop && _should_stop())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(k_poll_ms));
        st = mumu_query_state(_mumutool, _index);
    }
    if (_should_stop && _should_stop()) {
        // Before the timeout wording below: the caller reads this to tell "the
        // human gave up" from "it never came up".
        st.message = "stopped by the user";
        st.state = mumu_state::failed;
        return st;
    }
    if (st.state == mumu_state::running) {
        return st;
    }
    // A timeout, an unreadable state and a mumutool failure are the same thing
    // to the caller, which only distinguishes "up" from "not up".
    if (st.state != mumu_state::failed) {
        st.message =
            "not up after " + std::to_string(_deadline_ms / 1000) +
            "s (last state: " + (st.message.empty() ? "none" : st.message) +
            ")";
    }
    st.state = mumu_state::failed;
    return st;
}

std::string mumu_app_path(const std::string &_mumutool) {
    return std::filesystem::path(_mumutool)
        .parent_path()
        .parent_path()
        .parent_path()
        .string();
}

mumu_status mumu_launch_app(const std::string &_app_path) {
    mumu_status st;
    std::error_code ec;
    // `open` accepts any path, so a derived path that came out wrong must be
    // stopped here rather than handed to LaunchServices on a guess.
    if (!std::filesystem::is_directory(_app_path, ec) ||
        std::filesystem::path(_app_path).extension() != ".app") {
        st.state = mumu_state::failed;
        st.message = "not an app bundle: " +
                     (_app_path.empty() ? std::string("(empty)") : _app_path);
        return st;
    }
    std::string out;
    std::string err;
    int code = 0;
    if (!process::run_process_posix("/usr/bin/open",
                                    {"/usr/bin/open", "-g", _app_path}, &out,
                                    &err, k_timeout_ms, &code) ||
        code != 0) {
        st.state = mumu_state::failed;
        st.message =
            "open failed: " + (err.empty() ? std::string("no detail") : err);
        return st;
    }
    st.state = mumu_state::starting;
    st.message = "launching MuMuPlayer";
    return st;
}

mumu_status mumu_wait_service(const std::string &_mumutool, int _index,
                              int _deadline_ms,
                              const std::function<bool()> &_should_stop) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(_deadline_ms);
    // Retry on *any* failure, not just `service_down`. While the app comes up
    // mumutool moves from `invalidPort` to a plain connection error
    // ("URLSessionTask failed ... 无法连接服务器"), and both just mean "not
    // listening yet". Only ever called after `mumu_launch_app` accepted the
    // bundle, so there is no hopeless case here worth failing fast on.
    auto not_listening = [](const mumu_status &_st) {
        return _st.state == mumu_state::service_down ||
               _st.state == mumu_state::failed;
    };
    mumu_status st = mumu_query_state(_mumutool, _index);
    while (not_listening(st) && std::chrono::steady_clock::now() < deadline &&
           !(_should_stop && _should_stop())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(k_poll_ms));
        st = mumu_query_state(_mumutool, _index);
    }
    if (_should_stop && _should_stop()) {
        // Before the timeout wording below: the caller reads this to tell "the
        // human gave up" from "it never came up".
        st.message = "stopped by the user";
        st.state = mumu_state::failed;
        return st;
    }
    if (not_listening(st)) {
        st.message = "no MuMu service after " +
                     std::to_string(_deadline_ms / 1000) +
                     "s (last error: " + st.message + ")";
        st.state = mumu_state::failed;
    }
    return st;
}

bool mumu_quit_app(const std::string &_app_path) {
    if (_app_path.empty()) {
        return true;
    }
    std::string out;
    std::string err;
    int code = 0;
    return process::run_process_posix(
               "/usr/bin/osascript",
               {"/usr/bin/osascript", "-e", "quit app \"" + _app_path + "\""},
               &out, &err, k_timeout_ms, &code) &&
           code == 0;
}

std::optional<process::child>
mumu_spawn_watchdog(const std::string &_mumutool, int _index, long _parent_pid,
                    const std::string &_app_path) {
    // Positional parameters, not string interpolation: the paths contain
    // spaces.
    const std::vector<std::string> argv = {"/bin/sh",
                                           "-c",
                                           k_watchdog_script,
                                           "sh",
                                           std::to_string(_parent_pid),
                                           _mumutool,
                                           std::to_string(_index),
                                           _app_path};
    std::string err;
    return process::spawn("/bin/sh", argv, &err);
}

} // namespace campcat

#else // _WIN32: the emulator and mumutool are macOS-only.

namespace campcat {

namespace {

mumu_status unavailable() {
    mumu_status st;
    st.state = mumu_state::failed;
    st.message = "mumutool is only used on macOS";
    return st;
}

} // namespace

mumu_status mumu_query_state(const std::string &, int) { return unavailable(); }
mumu_status mumu_open(const std::string &, int) { return unavailable(); }
mumu_status mumu_close(const std::string &, int) { return unavailable(); }
mumu_status mumu_wait_ready(const std::string &, int, int,
                            const std::function<bool()> &) {
    return unavailable();
}
std::string mumu_app_path(const std::string &) { return {}; }
mumu_status mumu_launch_app(const std::string &) { return unavailable(); }
mumu_status mumu_wait_service(const std::string &, int, int,
                              const std::function<bool()> &) {
    return unavailable();
}
bool mumu_quit_app(const std::string &) { return false; }
std::optional<process::child> mumu_spawn_watchdog(const std::string &, int,
                                                  long, const std::string &) {
    return std::nullopt;
}

} // namespace campcat

#endif
