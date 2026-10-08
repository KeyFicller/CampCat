#include "app/llm_ui.h"

#include "app/native_file_dialog.h"
#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "core/llm/llm_host.h"

#include <imgui.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

GLuint g_tex = 0;
int g_tex_w = 0;
int g_tex_h = 0;
cv::Mat g_bgr;
std::string g_source_label = "(none)";

std::atomic<bool> g_running{false};
std::thread g_thread;
campcat::llm_host g_host;
std::mutex g_result_mu;
campcat::llm_result g_last{};
bool g_has_result = false;

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

void set_image(cv::Mat &&_bgr, std::string _label) {
  g_bgr = std::move(_bgr);
  g_source_label = std::move(_label);
  if (!upload_bgr(g_bgr)) {
    release_tex();
    campcat::automation_log::emit("[llm] texture upload failed");
  }
}

void capture_from_adb(campcat::app_config &_cfg) {
  campcat::adb_client adb(_cfg.adb_path, _cfg.adb_serial);
  const bool connected = adb.connect_remote(_cfg.adb_connect_address);
  cv::Mat cap;
  std::string diag;
  if (!adb.screencap_png(&cap, 45000, &diag)) {
    if (!connected) {
      diag += " (adb connect " + _cfg.adb_connect_address + " failed)";
    }
    campcat::automation_log::emit("[llm] screencap failed " + diag);
    return;
  }
  campcat::automation_log::emit(
      "[llm] captured " + std::to_string(cap.cols) + "x" +
      std::to_string(cap.rows));
  set_image(std::move(cap), "emulator screenshot");
}

void pick_local_image() {
  auto picked = campcat::pick_open_file(std::filesystem::current_path(), "png",
                                        "Select a screenshot");
  if (!picked) {
    return;
  }
  cv::Mat img = cv::imread(picked->string(), cv::IMREAD_COLOR);
  if (img.empty()) {
    campcat::automation_log::emit("[llm] cannot read image: " +
                                  picked->string());
    return;
  }
  campcat::automation_log::emit("[llm] loaded " + picked->string());
  set_image(std::move(img), picked->filename().string());
}

void start_describe(const std::filesystem::path &_repo_root, cv::Mat _bgr) {
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_running.store(true);
  g_thread = std::thread([root = _repo_root, img = std::move(_bgr)]() {
    const campcat::llm_result res = g_host.describe(root, img);
    {
      std::lock_guard<std::mutex> lk(g_result_mu);
      g_last = res;
      g_has_result = true;
    }
    if (res.ok) {
      campcat::automation_log::emit("[llm] ok: " + res.text);
    } else {
      campcat::automation_log::emit("[llm] failed: " + res.error);
    }
    g_running.store(false);
  });
}

} // namespace

void llm_ui_shutdown_gl() {
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_host.stop();
  g_bgr.release();
  release_tex();
}

void llm_ui_draw_panel(campcat::app_config &_cfg, bool _disable_capture) {
  ImGui::TextWrapped(
      "Send a screenshot to the sidecar and read back a description.");
  ImGui::TextDisabled("Model settings live in llm/.env and llm/models.py.");
  ImGui::Spacing();

  const bool busy = g_running.load();

  if (_disable_capture || busy) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Capture from emulator (ADB)")) {
    capture_from_adb(_cfg);
  }
  if (_disable_capture || busy) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (busy) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Open image file...")) {
    pick_local_image();
  }
  if (busy) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  const bool no_image = (g_tex == 0 || g_bgr.empty());
  if (no_image || busy) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Describe")) {
    start_describe(_cfg.config_home.parent_path(), g_bgr.clone());
  }
  if (no_image || busy) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  ImGui::TextDisabled("%s", g_source_label.c_str());

  ImGui::Separator();
  if (no_image) {
    ImGui::TextDisabled("(no screenshot yet)");
  } else {
    constexpr float k_viewport_h = 340.0f;
    ImGui::BeginChild("llm_preview", ImVec2(0.0F, k_viewport_h),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(g_tex)),
                 ImVec2(static_cast<float>(g_tex_w),
                        static_cast<float>(g_tex_h)),
                 ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
    ImGui::EndChild();
  }

  ImGui::Separator();
  if (busy) {
    ImGui::TextColored(ImVec4(0.95F, 0.75F, 0.35F, 1.0F), "Running...");
    return;
  }

  std::string text;
  std::string error;
  bool ok = false;
  bool has = false;
  {
    std::lock_guard<std::mutex> lk(g_result_mu);
    has = g_has_result;
    ok = g_last.ok;
    text = g_last.text;
    error = g_last.error;
  }

  if (!has) {
    ImGui::TextDisabled("Press Describe to request a description.");
  } else if (ok) {
    ImGui::TextUnformatted("Description");
    ImGui::BeginChild("llm_result", ImVec2(0.0F, 0.0F),
                      ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::EndChild();
  } else {
    ImGui::TextColored(ImVec4(0.95F, 0.45F, 0.35F, 1.0F), "Error: %s",
                       error.c_str());
  }
}
