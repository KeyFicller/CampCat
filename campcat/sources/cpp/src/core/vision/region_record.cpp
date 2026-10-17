#include "core/region_record.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#include "core/automation_log.h"

namespace campcat {
namespace {

/// Only ever goes up, so two samples written in the same millisecond still get
/// different folder names.
std::atomic<unsigned> g_seq{0};

/// `<milliseconds>-<sequence>`: sorts by time and never collides within one
/// process, which is all a folder name has to do without hashing the picture.
std::string sample_name() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return std::to_string(ms) + "-" + std::to_string(++g_seq);
}

/// The shape a region is, spelled the same way the model names it, so a reader
/// of meta.json knows which of the fields below to read.
nlohmann::json region_json(const llm::region &_region) {
    return std::visit(
        [](const auto &_r) -> nlohmann::json {
            using shape = std::decay_t<decltype(_r)>;
            if constexpr (std::is_same_v<shape, llm::box>) {
                return {{"shape", "box"},
                        {"x", _r.x},
                        {"y", _r.y},
                        {"w", _r.w},
                        {"h", _r.h}};
            } else if constexpr (std::is_same_v<shape, llm::circle>) {
                return {{"shape", "circle"},
                        {"cx", _r.cx},
                        {"cy", _r.cy},
                        {"r", _r.r}};
            } else {
                return {{"shape", "diamond"},
                        {"cx", _r.cx},
                        {"cy", _r.cy},
                        {"rx", _r.rx},
                        {"ry", _r.ry}};
            }
        },
        _region);
}

void warn(const std::string &_why) {
    automation_log::emit("[region_score] sample not recorded: " + _why);
}

/// A region that covers nothing teaches nothing: a redraw dragged to zero
/// width, or a disc of radius zero.
bool plausible(const llm::region &_region) {
    if (const llm::box *b = std::get_if<llm::box>(&_region)) {
        return b->w > 0 && b->h > 0;
    }
    if (const llm::circle *c = std::get_if<llm::circle>(&_region)) {
        return c->r > 0;
    }
    const llm::diamond &d = std::get<llm::diamond>(_region);
    return d.rx > 0 && d.ry > 0;
}

} // namespace

bool record_region_sample(const std::filesystem::path &_root,
                          const region_sample &_sample) {
    if (_sample.frame.empty()) {
        warn("nothing was shown");
        return false;
    }
    if (!plausible(_sample.given) || !plausible(_sample.human)) {
        warn("a region has no area");
        return false;
    }

    const std::filesystem::path dir = _root / sample_name();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        warn("cannot create " + dir.string() + ": " + ec.message());
        return false;
    }

    // The picture first, then meta.json: a folder without meta.json is a sample
    // that was cut short, which a reader can skip, while the reverse would look
    // complete and point at no image.
    if (!cv::imwrite((dir / "frame.png").string(), _sample.frame)) {
        warn("could not write " + (dir / "frame.png").string());
        return false;
    }

    const nlohmann::json meta = {
        {"given", region_json(_sample.given)},
        {"human", region_json(_sample.human)},
    };
    std::ofstream out(dir / "meta.json", std::ios::trunc);
    if (!out) {
        warn("cannot open " + (dir / "meta.json").string());
        return false;
    }
    out << meta.dump(2) << '\n';
    out.flush();
    if (!out) {
        warn("could not write " + (dir / "meta.json").string());
        return false;
    }
    return true;
}

} // namespace campcat
