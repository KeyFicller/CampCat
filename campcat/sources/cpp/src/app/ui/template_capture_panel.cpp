#include "app/ui/template_capture_panel.h"

#include "app/ui/fonts.h"
#include "ccat_driver/ccat_script_profile.h"
#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/mumu_launcher.h"
#include "core/automation_log.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

namespace {

GLuint g_tex = 0;
int g_tex_w = 0;
int g_tex_h = 0;
cv::Mat g_bgr;

bool g_dragging = false;
int g_anchor_ix = 0;
int g_anchor_iy = 0;
int g_cur_ix = 0;
int g_cur_iy = 0;
bool g_has_sel = false;

/// Which shape the drag selects. A box is two corners; a circle is the centre
/// and the drag as the radius; a diamond is the centre and the drag's
/// horizontal and vertical reach as the two half-diagonals.
enum class capture_shape { box, circle, diamond };
capture_shape g_shape = capture_shape::box;

int g_sel_x0 = 0;
int g_sel_y0 = 0;
int g_sel_x1 = 0;
int g_sel_y1 = 0;
int g_sel_cx = 0; ///< committed centre, when the shape is a circle or diamond
int g_sel_cy = 0;
int g_sel_r = 0;
int g_sel_rx = 0; ///< committed diamond half-diagonals
int g_sel_ry = 0;

std::string g_filename = "template.png";

void release_tex() {
    if (g_tex != 0) {
        glDeleteTextures(1, &g_tex);
        g_tex = 0;
    }
    g_tex_w = 0;
    g_tex_h = 0;
}

bool upload_bgr(const cv::Mat &_bgr) {
    if (_bgr.empty() || _bgr.type() != CV_8UC3) {
        return false;
    }
    cv::Mat rgba;
    cv::cvtColor(_bgr, rgba, cv::COLOR_BGR2RGBA);
    release_tex();
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.cols, rgba.rows, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.ptr());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_tex_w = rgba.cols;
    g_tex_h = rgba.rows;
    return true;
}

bool safe_leaf_name(std::string_view _s) {
    const std::string s(_s);
    if (s.empty()) {
        return false;
    }
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) {
        return false;
    }
    if (s == "." || s == "..") {
        return false;
    }
    return true;
}

cv::Rect inclusive_rect_to_roi(int _x0, int _y0, int _x1, int _y1, int _w,
                               int _h) {
    const int xa = std::clamp(std::min(_x0, _x1), 0, std::max(0, _w - 1));
    const int ya = std::clamp(std::min(_y0, _y1), 0, std::max(0, _h - 1));
    const int xb = std::clamp(std::max(_x0, _x1), 0, std::max(0, _w - 1));
    const int yb = std::clamp(std::max(_y0, _y1), 0, std::max(0, _h - 1));
    const int rw = std::max(1, xb - xa + 1);
    const int rh = std::max(1, yb - ya + 1);
    return {xa, ya, rw, rh};
}

/// The radius the current drag sets: away from the centre, or the longest
/// distance that keeps every covered pixel in the frame, whichever is smaller.
/// A disc that hangs over an edge would otherwise write a template whose shape
/// its own pixels do not agree with.
int drag_radius(int _cx, int _cy, int _ix, int _iy) {
    const double want = std::hypot(static_cast<double>(_ix - _cx),
                                  static_cast<double>(_iy - _cy));
    const int room = std::min({_cx, _cy, g_bgr.cols - 1 - _cx,
                               g_bgr.rows - 1 - _cy});
    return std::max(0, std::min(static_cast<int>(std::lround(want)), room));
}

/// The circular selection as a 4-channel crop: alpha 255 inside the disc and 0
/// outside. The same form `save_template` writes for a circle, so a template cut
/// here matches under the same rule -- the alpha is the shape.
cv::Mat circle_crop(int _cx, int _cy, int _r) {
    cv::Mat out(2 * _r + 1, 2 * _r + 1, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    for (int y = 0; y < out.rows; ++y) {
        auto *row = out.ptr<cv::Vec4b>(y);
        for (int x = 0; x < out.cols; ++x) {
            const int dx = x - _r;
            const int dy = y - _r;
            if (dx * dx + dy * dy > _r * _r) {
                continue;
            }
            const int ix = _cx + dx;
            const int iy = _cy + dy;
            if (ix < 0 || iy < 0 || ix >= g_bgr.cols || iy >= g_bgr.rows) {
                continue;
            }
            const cv::Vec3b &src = g_bgr.at<cv::Vec3b>(iy, ix);
            row[x] = cv::Vec4b(src[0], src[1], src[2], 255);
        }
    }
    return out;
}

/// The diamond selection as a 4-channel crop: alpha 255 inside
/// `|dx|/rx + |dy|/ry <= 1`, 0 outside. Same mask rule as a circle.
cv::Mat diamond_crop(int _cx, int _cy, int _rx, int _ry) {
    cv::Mat out(2 * _ry + 1, 2 * _rx + 1, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    for (int y = 0; y < out.rows; ++y) {
        auto *row = out.ptr<cv::Vec4b>(y);
        for (int x = 0; x < out.cols; ++x) {
            const long long dx = x - _rx;
            const long long dy = y - _ry;
            const long long adx = dx < 0 ? -dx : dx;
            const long long ady = dy < 0 ? -dy : dy;
            if (adx * _ry + ady * _rx > static_cast<long long>(_rx) * _ry) {
                continue;
            }
            const int ix = _cx + static_cast<int>(dx);
            const int iy = _cy + static_cast<int>(dy);
            if (ix < 0 || iy < 0 || ix >= g_bgr.cols || iy >= g_bgr.rows) {
                continue;
            }
            const cv::Vec3b &src = g_bgr.at<cv::Vec3b>(iy, ix);
            row[x] = cv::Vec4b(src[0], src[1], src[2], 255);
        }
    }
    return out;
}

/// Half-diagonals the current drag sets, each clamped so that vertex stays in
/// the frame. A diamond that hangs over an edge would write a template whose
/// shape its own pixels do not agree with.
void drag_diamond(int _cx, int _cy, int _ix, int _iy, int *_rx, int *_ry) {
    const int room_x = std::min(_cx, g_bgr.cols - 1 - _cx);
    const int room_y = std::min(_cy, g_bgr.rows - 1 - _cy);
    *_rx = std::max(0, std::min(std::abs(_ix - _cx), room_x));
    *_ry = std::max(0, std::min(std::abs(_iy - _cy), room_y));
}

void img_px_from_screen(const ImVec2 &_screen, const ImVec2 &_img_min,
                        const ImVec2 &_img_max, int *_ix, int *_iy) {
    const float denom_x = std::max(1e-6f, _img_max.x - _img_min.x);
    const float denom_y = std::max(1e-6f, _img_max.y - _img_min.y);
    float u = (_screen.x - _img_min.x) / denom_x;
    float v = (_screen.y - _img_min.y) / denom_y;
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    *_ix = static_cast<int>(
        std::floor(u * static_cast<float>(std::max(1, g_tex_w))));
    *_iy = static_cast<int>(
        std::floor(v * static_cast<float>(std::max(1, g_tex_h))));
    *_ix = std::clamp(*_ix, 0, std::max(0, g_tex_w - 1));
    *_iy = std::clamp(*_iy, 0, std::max(0, g_tex_h - 1));
}

ImVec2 screen_from_img_px(int _ix, int _iy, const ImVec2 &_img_min,
                          const ImVec2 &_img_max) {
    const float u = (static_cast<float>(_ix) + 0.5f) /
                    static_cast<float>(std::max(1, g_tex_w));
    const float v = (static_cast<float>(_iy) + 0.5f) /
                    static_cast<float>(std::max(1, g_tex_h));
    return ImVec2(_img_min.x + u * (_img_max.x - _img_min.x),
                  _img_min.y + v * (_img_max.y - _img_min.y));
}

void capture_clicked(campcat::app_config &_cfg) {
    const std::string addr = campcat::mumu_adb_address(_cfg.mumu.tool_path,
                                                      _cfg.mumu.device_index);
    campcat::adb_client adb(_cfg.adb.path,
                            addr.empty() ? _cfg.adb.serial : addr);
    const bool connected = adb.connect_remote(addr);
    cv::Mat cap;
    std::string diag;
    if (!adb.screencap_bgr(&cap, 45000, &diag)) {
        if (!connected) {
            diag += " (adb connect " + addr + " failed)";
        }
        campcat::automation_log::emit("[capture] screencap failed " + diag);
        return;
    }
    const int frame_w = cap.cols;
    const int frame_h = cap.rows;
    g_bgr = std::move(cap);
    g_dragging = false;
    g_has_sel = false;
    if (!upload_bgr(g_bgr)) {
        release_tex();
        campcat::automation_log::emit("[capture] texture upload failed");
        return;
    }
    campcat::automation_log::emit("[capture] frame " + std::to_string(frame_w) +
                                  "x" + std::to_string(frame_h));
}

void clear_capture() {
    g_bgr.release();
    g_dragging = false;
    g_has_sel = false;
    release_tex();
}

/// The toolkit ships no help marker; this is the one from its own demo. A
/// dimmed question mark whose answer appears on hover, wrapped, because these
/// answers are a sentence rather than a word.
void help_marker(const char *_text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0F);
        ImGui::TextUnformatted(_text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// What the panel used to print above and below the image, merged: both are
/// reference rather than instruction, so neither needs the line it cost.
constexpr char k_help_text[] =
    "Drag on image after Capture to select ROI. Box selects two corners. "
    "Circle takes the press as the centre and the drag as the radius. Diamond "
    "takes the press as the centre; horizontal reach is rx and vertical reach "
    "is ry. A circle or a diamond is saved with a transparent outside, so "
    "matching ignores it. Preview is "
    "1:1 device pixels; scroll when larger than viewport. PNG files are saved "
    "next to the .ccat script.";

} // namespace

void template_capture_panel_shutdown_gl() { clear_capture(); }

void template_capture_panel_draw(campcat::app_config &_cfg,
                                 const campcat::ccat_script_profile *_ccat_ui,
                                 bool _disable_capture) {
    if (!_ccat_ui) {
        ImGui::TextDisabled("(CampCat bundle missing)");
        return;
    }

    // No title and no separator here: the caller draws this inside the
    // "Screenshot crop (ADB)" collapsing header, which already names it.
    if (_disable_capture) {
        ImGui::BeginDisabled();
    }
    WITH_FONT(bold, {
        if (ImGui::Button("Capture##adb_tpl")) {
            capture_clicked(_cfg);
        }
    })
    if (_disable_capture) {
        ImGui::EndDisabled();
    }

    // Read after Capture, so a click that drops the old selection also drops
    // the Save enabled by it.
    const bool can_save = g_has_sel && !g_bgr.empty();
    ImGui::SameLine();
    if (!can_save) {
        ImGui::BeginDisabled();
    }
    bool save_crop = false;
    WITH_FONT(bold, { save_crop = ImGui::Button("Save##adb_tpl"); })
    if (!can_save) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    WITH_FONT(bold, {
        if (ImGui::Button("Clear##adb_tpl")) {
            clear_capture();
        }
    })

    ImGui::SameLine();
    help_marker(k_help_text);

    // The field's own name, moved out of the row so the name stops competing
    // with the value for it.
    ImGui::InputText("##adb_tpl_name", &g_filename);
    ImGui::SameLine();
    help_marker("Template File Name");

    // Switching shape drops the selection: what was drawn as one shape must not
    // be saved as the other.
    if (ImGui::RadioButton("Box##adb_tpl_shape",
                           g_shape == capture_shape::box)) {
        g_shape = capture_shape::box;
        g_has_sel = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Circle##adb_tpl_shape",
                           g_shape == capture_shape::circle)) {
        g_shape = capture_shape::circle;
        g_has_sel = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Diamond##adb_tpl_shape",
                           g_shape == capture_shape::diamond)) {
        g_shape = capture_shape::diamond;
        g_has_sel = false;
    }
    ImGui::SameLine();
    help_marker("Shape to save the crop as.");

    if (g_tex == 0 || g_bgr.empty()) {
        ImGui::TextDisabled("(no screenshot)");
        return;
    }

    constexpr float k_viewport_h = 420.0f;
    const float iw = static_cast<float>(g_tex_w);
    const float ih = static_cast<float>(g_tex_h);
    const ImVec2 disp(iw, ih);

    const ImVec2 uv0(0.0f, 0.0f);
    const ImVec2 uv1(1.0f, 1.0f);

    ImGui::BeginChild("adb_tpl_scroll", ImVec2(0.0f, k_viewport_h),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(g_tex)), disp,
                 uv0, uv1);

    const ImVec2 img_min = ImGui::GetItemRectMin();
    const ImVec2 img_max = ImGui::GetItemRectMax();
    ImGuiIO &io = ImGui::GetIO();

    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_dragging = true;
        img_px_from_screen(io.MousePos, img_min, img_max, &g_anchor_ix,
                           &g_anchor_iy);
        g_cur_ix = g_anchor_ix;
        g_cur_iy = g_anchor_iy;
        g_has_sel = false;
    }

    if (g_dragging) {
        img_px_from_screen(io.MousePos, img_min, img_max, &g_cur_ix, &g_cur_iy);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_dragging = false;
            if (g_shape == capture_shape::circle) {
                const int r =
                    drag_radius(g_anchor_ix, g_anchor_iy, g_cur_ix, g_cur_iy);
                if (r > 0) {
                    g_sel_cx = g_anchor_ix;
                    g_sel_cy = g_anchor_iy;
                    g_sel_r = r;
                    g_has_sel = true;
                }
            } else if (g_shape == capture_shape::diamond) {
                int rx = 0;
                int ry = 0;
                drag_diamond(g_anchor_ix, g_anchor_iy, g_cur_ix, g_cur_iy, &rx,
                             &ry);
                if (rx > 0 && ry > 0) {
                    g_sel_cx = g_anchor_ix;
                    g_sel_cy = g_anchor_iy;
                    g_sel_rx = rx;
                    g_sel_ry = ry;
                    g_has_sel = true;
                }
            } else {
                g_sel_x0 = g_anchor_ix;
                g_sel_y0 = g_anchor_iy;
                g_sel_x1 = g_cur_ix;
                g_sel_y1 = g_cur_iy;
                g_has_sel = true;
            }
        }
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (g_dragging || g_has_sel) {
        constexpr ImU32 k_fill = IM_COL32(80, 200, 255, 55);
        constexpr ImU32 k_stroke = IM_COL32(255, 220, 80, 255);
        // One screen pixel per image pixel, so the radius converts by the same
        // factor the position does.
        const float scale =
            (img_max.x - img_min.x) / static_cast<float>(std::max(1, g_tex_w));
        if (g_shape == capture_shape::circle) {
            const int cx = g_dragging ? g_anchor_ix : g_sel_cx;
            const int cy = g_dragging ? g_anchor_iy : g_sel_cy;
            const int r = g_dragging
                              ? drag_radius(cx, cy, g_cur_ix, g_cur_iy)
                              : g_sel_r;
            const ImVec2 centre = screen_from_img_px(cx, cy, img_min, img_max);
            dl->AddCircleFilled(centre, r * scale, k_fill);
            dl->AddCircle(centre, r * scale, k_stroke, 0, 2.0f);
        } else if (g_shape == capture_shape::diamond) {
            const int cx = g_dragging ? g_anchor_ix : g_sel_cx;
            const int cy = g_dragging ? g_anchor_iy : g_sel_cy;
            int rx = g_sel_rx;
            int ry = g_sel_ry;
            if (g_dragging) {
                drag_diamond(cx, cy, g_cur_ix, g_cur_iy, &rx, &ry);
            }
            const ImVec2 top = screen_from_img_px(cx, cy - ry, img_min, img_max);
            const ImVec2 right =
                screen_from_img_px(cx + rx, cy, img_min, img_max);
            const ImVec2 bottom =
                screen_from_img_px(cx, cy + ry, img_min, img_max);
            const ImVec2 left =
                screen_from_img_px(cx - rx, cy, img_min, img_max);
            dl->AddQuadFilled(top, right, bottom, left, k_fill);
            dl->AddQuad(top, right, bottom, left, k_stroke, 2.0f);
        } else {
            const int x0 = g_dragging ? g_anchor_ix : g_sel_x0;
            const int y0 = g_dragging ? g_anchor_iy : g_sel_y0;
            const int x1 = g_dragging ? g_cur_ix : g_sel_x1;
            const int y1 = g_dragging ? g_cur_iy : g_sel_y1;
            const ImVec2 s0 = screen_from_img_px(std::min(x0, x1),
                                                 std::min(y0, y1), img_min,
                                                 img_max);
            const ImVec2 s1 = screen_from_img_px(std::max(x0, x1),
                                                 std::max(y0, y1), img_min,
                                                 img_max);
            dl->AddRectFilled(s0, s1, k_fill);
            dl->AddRect(s0, s1, k_stroke, 0.0f, 0, 2.0f);
        }
    }

    ImGui::EndChild();

    ImGui::TextDisabled("size: %dx%d px", g_tex_w, g_tex_h);

    // A click cannot arrive with no selection or no frame: Save is disabled
    // without both.
    if (save_crop) {
        if (!safe_leaf_name(g_filename)) {
            campcat::automation_log::emit(
                "[capture] invalid filename (no path separators)");
        } else if (!_ccat_ui->has_script_source()) {
            campcat::automation_log::emit(
                "[capture] set .ccat source path before saving templates");
        } else if (_cfg.config_home.empty()) {
            campcat::automation_log::emit(
                "[capture] CampCat images dir unresolved (config_home)");
        } else {
            const std::filesystem::path dir =
                _ccat_ui->images_base(_cfg.config_home);
            if (dir.empty()) {
                campcat::automation_log::emit(
                    "[capture] script folder unresolved; check source path");
            } else {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                if (ec) {
                    campcat::automation_log::emit(
                        "[capture] mkdir failed: " + dir.string() + " (" +
                        ec.message() + ")");
                } else {
                    const std::filesystem::path out = dir / g_filename;
                    const cv::Mat patch =
                        g_shape == capture_shape::circle
                            ? circle_crop(g_sel_cx, g_sel_cy, g_sel_r)
                        : g_shape == capture_shape::diamond
                            ? diamond_crop(g_sel_cx, g_sel_cy, g_sel_rx,
                                           g_sel_ry)
                            : g_bgr(inclusive_rect_to_roi(
                                        g_sel_x0, g_sel_y0, g_sel_x1, g_sel_y1,
                                        g_bgr.cols, g_bgr.rows))
                                  .clone();
                    if (!cv::imwrite(out.string(), patch)) {
                        campcat::automation_log::emit(
                            "[capture] imwrite failed: " + out.string());
                    } else {
                        campcat::automation_log::emit("[capture] wrote " +
                                                      out.string());
                    }
                }
            }
        }
    }
}
