#include "app/template_capture_ui.h"

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "games/ccat_script/ccat_script_profile.h"

#include <imgui.h>

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
#include <cstring>
#include <filesystem>

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
int g_sel_x0 = 0;
int g_sel_y0 = 0;
int g_sel_x1 = 0;
int g_sel_y1 = 0;

char g_filename[256] = "template.png";

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
  glBindTexture(GL_TEXTURE_2D, 0);
  g_tex_w = rgba.cols;
  g_tex_h = rgba.rows;
  return true;
}

bool safe_leaf_name(const char *_s) {
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

void img_px_from_screen(const ImVec2 &_screen, const ImVec2 &_img_min,
                        const ImVec2 &_img_max, int *_ix, int *_iy) {
  const float denom_x = std::max(1e-6f, _img_max.x - _img_min.x);
  const float denom_y = std::max(1e-6f, _img_max.y - _img_min.y);
  float u = (_screen.x - _img_min.x) / denom_x;
  float v = (_screen.y - _img_min.y) / denom_y;
  u = std::clamp(u, 0.0f, 1.0f);
  v = std::clamp(v, 0.0f, 1.0f);
  *_ix = static_cast<int>(
      std::floor(u * static_cast<float>(std::max(1, g_tex_w - 1))));
  *_iy = static_cast<int>(
      std::floor(v * static_cast<float>(std::max(1, g_tex_h - 1))));
  *_ix = std::clamp(*_ix, 0, std::max(0, g_tex_w - 1));
  *_iy = std::clamp(*_iy, 0, std::max(0, g_tex_h - 1));
}

ImVec2 screen_from_img_px(int _ix, int _iy, const ImVec2 &_img_min,
                          const ImVec2 &_img_max) {
  const float u =
      (static_cast<float>(_ix) + 0.5f) / static_cast<float>(std::max(1, g_tex_w));
  const float v =
      (static_cast<float>(_iy) + 0.5f) / static_cast<float>(std::max(1, g_tex_h));
  return ImVec2(_img_min.x + u * (_img_max.x - _img_min.x),
                _img_min.y + v * (_img_max.y - _img_min.y));
}

void capture_clicked(campcat::app_config &_cfg) {
  campcat::adb_client adb(_cfg.adb_path, _cfg.adb_serial);
  (void)adb.connect_remote(_cfg.adb_connect_address);
  cv::Mat cap;
  std::string diag;
  if (!adb.screencap_png(&cap, 45000, &diag)) {
    campcat::automation_log::emit("[capture] screencap failed " + diag);
    return;
  }
  g_bgr = cap.clone();
  g_dragging = false;
  g_has_sel = false;
  if (!upload_bgr(g_bgr)) {
    campcat::automation_log::emit("[capture] texture upload failed");
    return;
  }
  campcat::automation_log::emit("[capture] frame " + std::to_string(cap.cols) +
                                "x" + std::to_string(cap.rows));
}

void clear_capture() {
  g_bgr.release();
  g_dragging = false;
  g_has_sel = false;
  release_tex();
}

} // namespace

void template_capture_shutdown_gl() {
  clear_capture();
}

void template_capture_draw_panel(campcat::app_config &_cfg,
                                 campcat::ccat_script_profile *_ccat_ui,
                                 bool _disable_capture) {
  if (!_ccat_ui) {
    ImGui::TextDisabled("(CampCat bundle missing)");
    return;
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Template from screenshot (CampCat)");
  ImGui::TextDisabled(
      "Drag on image after Capture to select ROI. Preview is 1:1 device "
      "pixels; scroll when larger than viewport.");

  if (_disable_capture) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Capture##adb_tpl")) {
    capture_clicked(_cfg);
  }
  if (_disable_capture) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (ImGui::Button("Clear##adb_tpl")) {
    clear_capture();
  }

  ImGui::InputText("png filename##adb_tpl", g_filename,
                   IM_ARRAYSIZE(g_filename));

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

  ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(g_tex)),
               disp, uv0, uv1);

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
      g_sel_x0 = g_anchor_ix;
      g_sel_y0 = g_anchor_iy;
      g_sel_x1 = g_cur_ix;
      g_sel_y1 = g_cur_iy;
      g_has_sel = true;
    }
  }

  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (g_dragging || g_has_sel) {
    const int x0 = g_dragging ? g_anchor_ix : g_sel_x0;
    const int y0 = g_dragging ? g_anchor_iy : g_sel_y0;
    const int x1 = g_dragging ? g_cur_ix : g_sel_x1;
    const int y1 = g_dragging ? g_cur_iy : g_sel_y1;
    const ImVec2 s0 = screen_from_img_px(
        std::min(x0, x1), std::min(y0, y1), img_min, img_max);
    const ImVec2 s1 = screen_from_img_px(
        std::max(x0, x1), std::max(y0, y1), img_min, img_max);
    dl->AddRectFilled(s0, s1, IM_COL32(80, 200, 255, 55));
    dl->AddRect(s0, s1, IM_COL32(255, 220, 80, 255), 0.0f, 0, 2.0f);
  }

  ImGui::EndChild();

  ImGui::TextDisabled("size: %dx%d px", g_tex_w, g_tex_h);

  if (ImGui::Button("Save crop##adb_tpl")) {
    if (!g_has_sel) {
      campcat::automation_log::emit(
          "[capture] drag on image and release to finalize selection");
    } else if (!safe_leaf_name(g_filename)) {
      campcat::automation_log::emit(
          "[capture] invalid filename (no path separators)");
    } else if (!_ccat_ui->has_script_source()) {
      campcat::automation_log::emit(
          "[capture] set .ccat source path before saving templates");
    } else if (_cfg.config_home.empty()) {
      campcat::automation_log::emit(
          "[capture] CampCat images dir unresolved (config_home)");
    } else {
      const cv::Rect roi = inclusive_rect_to_roi(
          g_sel_x0, g_sel_y0, g_sel_x1, g_sel_y1, g_bgr.cols, g_bgr.rows);
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
              "[capture] mkdir failed: " + dir.string() + " (" + ec.message() +
              ")");
        } else {
          const std::filesystem::path out = dir / g_filename;
          cv::Mat patch = g_bgr(roi).clone();
          if (!cv::imwrite(out.string(), patch)) {
            campcat::automation_log::emit("[capture] imwrite failed: " +
                                          out.string());
          } else {
            campcat::automation_log::emit("[capture] wrote " + out.string());
          }
        }
      }
    }
  }

  ImGui::TextDisabled("PNG files are saved next to the .ccat script.");
}
