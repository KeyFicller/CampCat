#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace campcat::llm {
struct screen_state { int w = 0; int h = 0; std::string png; };

/// Session-owned, disk-backed shots. Only the exchange worker accesses it.
class frame_store {
public:
    explicit frame_store(std::size_t _budget = 64U * 1024U * 1024U);
    ~frame_store();
    frame_store(const frame_store &) = delete;
    frame_store &operator=(const frame_store &) = delete;
    int put(const screen_state &_frame);
    std::optional<screen_state> get(int _shot) const;
    void clear();
    std::size_t memory_bytes() const { return m_bytes; }
private:
    struct entry { int w, h; std::string cached; };
    std::size_t m_budget, m_bytes = 0;
    int m_next = 0;
    std::filesystem::path m_dir;
    std::map<int, entry> m_frames;
};
}
