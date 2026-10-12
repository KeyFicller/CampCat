#include "app/ui/icon_texture.h"

#include "core/automation_log.h"

#include <opencv2/imgcodecs.hpp>

#include <map>
#include <string>

namespace {

struct icon_slot {
    GLuint tex = 0;
    int w = 0;
    int h = 0;
    /// An upload is attempted once, so a missing file is not re-read every
    /// frame.
    bool tried = false;
};

/// Keyed by path and never erased. A std::map hands out stable references and
/// does not move its elements when it grows, which is what lets the texture ids
/// live in here rather than in each panel.
std::map<std::string, icon_slot> g_icons;

} // namespace

namespace campcat::app {

GLuint load_icon(const char *_path) {
    icon_slot &slot = g_icons[_path];
    if (slot.tried) {
        return slot.tex;
    }
    slot.tried = true;
    const cv::Mat icon = cv::imread(_path, cv::IMREAD_UNCHANGED);
    if (icon.empty()) {
        automation_log::emit(std::string("[ui] icon did not load: ") + _path);
        return 0;
    }
    slot.tex = upload_bgra(icon, &slot.w, &slot.h);
    return slot.tex;
}

void release_icons() {
    for (auto &entry : g_icons) {
        delete_tex(&entry.second.tex, &entry.second.w, &entry.second.h);
    }
}

} // namespace campcat::app
