#include "core/llm/llm_memory.h"

#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

namespace campcat::llm {

namespace {

/// The UI thread (the Memory window) and the LLM worker (memorize) both touch
/// this file, so every path through it takes this lock.
std::mutex &file_mutex() {
    static std::mutex mu;
    return mu;
}

/// A fixed name rather than a unique one: `file_mutex` already keeps two
/// writers apart, and a fixed name cannot leave a trail of temp files behind. A
/// stale one from a crash is simply truncated by the next write.
constexpr char k_temp_name[] = "memory.md.tmp";

/// Caller holds `file_mutex()`.
bool read_unlocked(const std::filesystem::path &_path, std::string *_out) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(_path, ec);
    if (ec) {
        // Cannot even stat it: unreadable, not empty. Saying "empty" here would
        // let the next write erase notes we simply failed to look at.
        return false;
    }
    if (!exists) {
        return true; // first run
    }

    std::ifstream in(_path, std::ios::binary);
    if (!in) {
        return false;
    }
    _out->assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
    return !in.bad();
}

} // namespace

std::filesystem::path memory_path(const std::filesystem::path &_config_home,
                                  memory_kind _kind) {
    const char *name =
        _kind == memory_kind::scripter_notes ? "scripter.md" : "operator.md";
    return _config_home / name;
}

const char *memory_agent_name(memory_kind _kind) {
    return _kind == memory_kind::scripter_notes ? "scripter" : "operator";
}

/// Caller holds `file_mutex()`. `operator.md` wins when both names exist.
static bool adopt_legacy_operator_file(const std::filesystem::path &_config_home) {
    const std::filesystem::path current =
        memory_path(_config_home, memory_kind::operator_notes);
    const std::filesystem::path legacy = _config_home / "memory.md";
    std::error_code ec;
    const bool current_exists = std::filesystem::exists(current, ec);
    if (ec) {
        return false;
    }
    ec.clear();
    const bool legacy_exists = std::filesystem::exists(legacy, ec);
    if (ec) {
        return false;
    }
    if (current_exists || !legacy_exists) {
        return true;
    }
    std::filesystem::rename(legacy, current, ec);
    return !ec;
}

bool read_memory(const std::filesystem::path &_config_home, memory_kind _kind,
                 std::string *_out) {
    if (_out == nullptr) {
        return false;
    }
    _out->clear();
    if (_config_home.empty()) {
        return true; // no config, so no notes to have
    }

    std::lock_guard<std::mutex> lk(file_mutex());
    if (_kind == memory_kind::operator_notes &&
        !adopt_legacy_operator_file(_config_home)) {
        return false;
    }
    return read_unlocked(memory_path(_config_home, _kind), _out);
}

bool write_memory(const std::filesystem::path &_config_home, memory_kind _kind,
                  const std::string &_text) {
    if (_config_home.empty() || _text.empty() ||
        _text.size() > k_memory_max_chars) {
        return false;
    }

    std::lock_guard<std::mutex> lk(file_mutex());
    const std::filesystem::path p = memory_path(_config_home, _kind);

    // Refuse to replace notes we could not read: an unreadable file must not
    // become an empty one.
    std::string current;
    if (!read_unlocked(p, &current)) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) {
        return false;
    }

    const std::filesystem::path tmp = p.parent_path() / k_temp_name;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << _text;
        out.flush();
        if (!out) {
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            return false;
        }
    }

    // The rename is what makes this atomic: a reader sees the old file or the
    // new one, never a partial write.
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        return false;
    }
    return true;
}

bool clear_memory(const std::filesystem::path &_config_home, memory_kind _kind) {
    if (_config_home.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(file_mutex());
    std::error_code ec;
    // A file that was never there reports no error, which is the success we
    // want.
    std::filesystem::remove(memory_path(_config_home, _kind), ec);
    return !ec;
}

} // namespace campcat::llm
