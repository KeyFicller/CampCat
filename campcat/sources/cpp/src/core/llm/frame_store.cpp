#include "core/llm/frame_store.h"
#include "core/file_io.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace campcat::llm {
frame_store::frame_store(std::size_t _budget) : m_budget(_budget) {}
frame_store::~frame_store() { clear(); }
int frame_store::put(const screen_state &_frame) {
    if (m_dir.empty()) {
        static std::atomic<unsigned long> serial{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        m_dir = std::filesystem::temp_directory_path() /
            ("campcat-frames-" + std::to_string(tick) + "-" + std::to_string(++serial));
    }
    const int shot = m_next + 1;
    write_file_atomic(m_dir / std::to_string(shot), _frame.png);
    entry e{_frame.w, _frame.h, {}};
    if (_frame.png.size() <= m_budget) {
        e.cached = _frame.png;
        m_bytes += e.cached.size();
    }
    m_frames.emplace(shot, std::move(e));
    m_next = shot;
    for (auto &[id, old] : m_frames) {
        if (m_bytes <= m_budget) break;
        m_bytes -= old.cached.size();
        std::string().swap(old.cached);
    }
    return shot;
}
std::optional<screen_state> frame_store::get(int _shot) const {
    const auto it = m_frames.find(_shot);
    if (it == m_frames.end()) return std::nullopt;
    const entry &e = it->second;
    if (!e.cached.empty()) return screen_state{e.w, e.h, e.cached};
    std::ifstream in(m_dir / std::to_string(_shot), std::ios::binary);
    if (!in) throw std::runtime_error("stored screenshot #" + std::to_string(_shot) + " is unreadable");
    std::string png{std::istreambuf_iterator<char>(in), {}};
    if (in.bad()) throw std::runtime_error("cannot read stored screenshot");
    return screen_state{e.w, e.h, std::move(png)};
}
void frame_store::clear() {
    m_frames.clear(); m_bytes = 0; m_next = 0;
    if (!m_dir.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(m_dir, ignored);
        m_dir.clear();
    }
}
}
