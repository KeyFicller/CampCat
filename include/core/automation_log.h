#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace campcat {

/**
 * @brief Process-wide log fan-out for automation/UI threads (replaces threaded
 * log_fn plumbing).
 *
 * The shell installs a sink once (typically timestamps then pushes to
 * `log_buffer`); drivers and interpreters call `emit()` from any thread. A tool
 * that wants the lines one call produced registers a second sink with
 * `add_sink`, which stays registered only as long as its handle lives.
 */
class automation_log {
public:
    using sink_fn = std::function<void(const std::string &)>;

    /// Marks the line a script's own `log(...)` asked for, so a capture can take
    /// just those while the interpreter's diagnostics stay untagged.
    static constexpr std::string_view k_script_log_tag = "[script] ";

    /**
     * @brief One registered sink, unregistered when the handle dies.
     *
     * Move-only: a copy would unregister the sink twice.
     */
    class handle {
    public:
        handle() noexcept = default;
        handle(handle &&_other) noexcept;
        handle &operator=(handle &&_other) noexcept;
        ~handle();

        handle(const handle &) = delete;
        handle &operator=(const handle &) = delete;

    private:
        friend class automation_log;

        explicit handle(std::size_t _id) noexcept : m_id(_id) {}

        std::size_t m_id = 0; ///< 0 means no sink
    };

    /// @brief Registers a sink that lives as long as the returned handle.
    static handle add_sink(sink_fn _fn);

    /// @brief Replaces every registered sink.
    static void set_sink(sink_fn _fn);

    /// @brief Removes every registered sink.
    static void clear_sink();

    /** @brief Thread-safe; dropped silently while nothing is registered. */
    static void emit(const std::string &_line);

private:
    static void remove(std::size_t _id);
};

/// The text a script's own `log(...)` carried, or nothing when the line came
/// from elsewhere. The one reader of `automation_log::k_script_log_tag`, whose
/// one writer is `LogStmt::exec`; kept next to it so the two cannot drift.
inline std::optional<std::string> script_log_text(const std::string &_line) {
    if (!_line.starts_with(automation_log::k_script_log_tag)) {
        return std::nullopt;
    }
    return _line.substr(automation_log::k_script_log_tag.size());
}

/// Caps what a script's logs can add to a tool result: a `while` loop can log
/// without bound, and the result goes into the request.
///
/// ponytail: raise these if a real script's tail turns out to matter. The lines
/// dropped here are still in the log panel and the log file.
inline constexpr std::size_t k_max_script_log_lines = 40;
inline constexpr std::size_t k_max_script_log_line_chars = 200;

/// The logs a script produced, one per line under the summary, or nothing at
/// all when it logged none. Pairs with `script_log_text`.
inline std::string script_log_suffix(const std::vector<std::string> &_lines) {
    if (_lines.empty()) {
        return {};
    }
    std::string out;
    const std::size_t shown = _lines.size() < k_max_script_log_lines
                                  ? _lines.size()
                                  : k_max_script_log_lines;
    for (std::size_t i = 0; i < shown; ++i) {
        std::string line = _lines[i];
        if (line.size() > k_max_script_log_line_chars) {
            line.resize(k_max_script_log_line_chars);
            line += "...";
        }
        out += "\n" + line;
    }
    if (_lines.size() > shown) {
        out += "\n... and " + std::to_string(_lines.size() - shown) +
               " more log lines";
    }
    return out;
}

} // namespace campcat
