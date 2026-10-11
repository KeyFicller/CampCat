#include <algorithm>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "core/app_config.h"
#include "core/automation_log.h"
#include "app/shell_state.h"
#include "app/ui/template_capture_panel.h"
#include "app/ui/console_panel.h"
#include "app/ui/llm_panel.h"
#include "app/ui/script_panel.h"
#include "app/ui/settings_panel.h"
#include "app/ui/script_editor_panel.h"
#include "app/ui/macos_ime.h"
#include "ccat_driver/ccat_script_profile.h"

namespace {

constexpr char k_default_config_relative[] = "config/default.json";

} // namespace

int main(int argc, char **argv) {
  // The LLM sidecar can die between our readiness check and a write to its
  // stdin; that write would raise SIGPIPE and kill the whole app. Ignoring it
  // turns the case into an ordinary EPIPE that `llm_host` already reports.
  // The sidecar itself re-enables the default (see `process::spawn`).
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif

  const std::filesystem::path cfg_path =
      std::filesystem::path(argc >= 2 ? argv[1] : k_default_config_relative);

  // The shell's state and its jobs: what used to be this function's locals and
  // capturing lambdas, now shared with the pages.
  campcat::shell_state st(cfg_path);

  glfwSetErrorCallback([](int code, const char *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, desc ? desc : "(null)");
  });

  if (!glfwInit()) {
    return 1;
  }

#if defined(__APPLE__)
  const char *glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#else
  const char *glsl_version = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

  GLFWwindow *window =
      glfwCreateWindow(1180, 780, "CampCat Shell", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 2;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();
  {
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0F;
    style.ChildRounding = 6.0F;
    style.FrameRounding = 5.0F;
    style.GrabRounding = 4.0F;
    style.ScrollbarRounding = 6.0F;
    style.PopupRounding = 6.0F;
    style.WindowPadding = ImVec2(10.0F, 10.0F);
    style.FramePadding = ImVec2(10.0F, 6.0F);
    style.ItemSpacing = ImVec2(10.0F, 8.0F);
    style.ItemInnerSpacing = ImVec2(8.0F, 5.0F);
    style.ScrollbarSize = 12.0F;
    style.GrabMinSize = 12.0F;
    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.PopupBorderSize = 1.0F;
    style.DisabledAlpha = 0.50F;

    // One palette, every slot filled. A partly themed style leaves the rest on
    // ImGui's defaults -- bright scrollbars and checkmarks, a saturated blue
    // on every header -- and that mix is what reads as unfinished. Values are
    // low-contrast on purpose: the accent marks what is active, nothing shouts.
    const ImVec4 bg{0.086F, 0.094F, 0.106F, 1.00F};
    const ImVec4 panel{0.106F, 0.116F, 0.133F, 1.00F};
    const ImVec4 field{0.145F, 0.157F, 0.176F, 1.00F};
    const ImVec4 field_hi{0.176F, 0.192F, 0.216F, 1.00F};
    const ImVec4 line{0.196F, 0.212F, 0.239F, 1.00F};
    const ImVec4 line_hi{0.278F, 0.302F, 0.337F, 1.00F};
    const ImVec4 ink{0.847F, 0.871F, 0.898F, 1.00F};
    const ImVec4 ink_dim{0.478F, 0.510F, 0.549F, 1.00F};
    const ImVec4 accent{0.243F, 0.400F, 0.529F, 1.00F};
    const ImVec4 accent_hi{0.302F, 0.478F, 0.616F, 1.00F};
    const ImVec4 accent_lo{0.200F, 0.341F, 0.463F, 1.00F};
    // Headers and rows get a wash of the accent, not a solid fill, so an
    // expanded section reads as a group instead of a second toolbar.
    const ImVec4 wash{0.243F, 0.400F, 0.529F, 0.28F};
    const ImVec4 wash_hi{0.243F, 0.400F, 0.529F, 0.46F};

    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = ink;
    c[ImGuiCol_TextDisabled] = ink_dim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = ImVec4(0.118F, 0.129F, 0.149F, 0.98F);
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_FrameBg] = field;
    c[ImGuiCol_FrameBgHovered] = field_hi;
    c[ImGuiCol_FrameBgActive] = accent_lo;
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.129F, 0.145F, 0.169F, 1.00F);
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = ImVec4(0.098F, 0.106F, 0.122F, 1.00F);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.235F, 0.255F, 0.286F, 1.00F);
    c[ImGuiCol_ScrollbarGrabHovered] = line_hi;
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent_hi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    // A plain button is a surface, not a primary. Filled with the accent, every
    // button on a page looked equally important -- Run once, Stop and Send all
    // read as the one thing to press. The accent is spent on the single primary
    // per page instead; see the Send button in ui/llm_panel.cpp.
    c[ImGuiCol_Button] = field_hi;
    c[ImGuiCol_ButtonHovered] = line_hi;
    c[ImGuiCol_ButtonActive] = accent_lo;
    c[ImGuiCol_Header] = wash;
    c[ImGuiCol_HeaderHovered] = wash_hi;
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = line_hi;
    c[ImGuiCol_SeparatorActive] = accent_hi;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_ResizeGripActive] = accent_hi;
    c[ImGuiCol_TabHovered] = wash_hi;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabSelected] = accent_lo;
    c[ImGuiCol_TabSelectedOverline] = accent_hi;
    c[ImGuiCol_TabDimmed] = panel;
    c[ImGuiCol_TabDimmedSelected] = field_hi;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_PlotLines] = accent_hi;
    c[ImGuiCol_PlotLinesHovered] = accent_hi;
    c[ImGuiCol_PlotHistogram] = accent; // ProgressBar fill
    c[ImGuiCol_PlotHistogramHovered] = accent_hi;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.129F, 0.141F, 0.165F, 1.00F);
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.153F, 0.165F, 0.188F, 1.00F);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0F, 1.0F, 1.0F, 0.015F);
    c[ImGuiCol_TextLink] = accent_hi;
    c[ImGuiCol_TextSelectedBg] = wash_hi;
    c[ImGuiCol_DragDropTarget] = accent_hi;
    c[ImGuiCol_NavCursor] = accent_hi;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0F, 1.0F, 1.0F, 0.70F);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.45F);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.55F);
  }

  // Every UI font is one family: Arial for Latin, with the same CJK face merged
  // in so Chinese never lands on a font that lacks it. Sizes and weights stay
  // per-use (16 body, 16 bold, 20 titles).
  static const struct {
    const char *path;
    int face;
  } cjk_fonts[] = {
      // PingFang is the current macOS Chinese face, but here it is a
      // downloadable asset: it lives under an AssetsV2 hash rather than the
      // stable Fonts directory. Stable path first, hash second, and Hiragino
      // (older, always present) as the fallback. ponytail: an OS update that
      // rehashes the asset silently falls back to Hiragino -- cosmetic only.
      {"/System/Library/Fonts/PingFang.ttc", 3}, // PingFang SC / Regular
      {"/System/Library/AssetsV2/com_apple_MobileAsset_Font7/"
       "3419f2a427639ad8c8e139149a287865a90fa17e.asset/AssetData/PingFang.ttc",
       3},
      {"/System/Library/Fonts/Hiragino Sans GB.ttc", 0},
      {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 0},
  };

  // ImGui's ChineseFull set stops at the CJK blocks, so Unicode's Arrows block
  // (→, ←, ⇒) is never asked for and draws as nothing, even though both CJK
  // faces above have the glyph. Appended to ImGui's own list rather than copied,
  // so it cannot drift from it. `static` because ImGui keeps the pointer until
  // the atlas is built, not just for the call.
  static const std::vector<ImWchar> cjk_ranges = [&io] {
    std::vector<ImWchar> r;
    for (const ImWchar *p = io.Fonts->GetGlyphRangesChineseFull();
         p[0] != 0 && p[1] != 0; p += 2) {
      r.push_back(p[0]);
      r.push_back(p[1]);
    }
    r.push_back(0x2190); // Arrows
    r.push_back(0x21FF);
    r.push_back(0); // zero-terminated, as ImGui requires
    r.push_back(0);
    return r;
  }();

  // MergeMode appends to the font added just before it, so each merge is issued
  // right after its own base font. AddFontFromFileTTF asserts on a missing file,
  // so the CJK candidate is probed first. The full set (~21k hanzi) costs a few
  // tens of MB of atlas, but the 2500 common set left replies drawing "?" for
  // everyday words ("screen" is U+5C4F, outside it).
  //
  // `_cjk` is false for the 20px title only: the full set at 20px doubles the
  // atlas to 4096x16384 (+320 MB RSS, +128 MB VRAM) for strings that are all
  // ASCII today. A Chinese title would draw "?"; add the merge then if it
  // becomes worth that.
  auto load_font = [&io](const char *_path, float _size, bool _cjk,
                         int _face) -> ImFont * {
    ImFontConfig base;
    base.FontNo = _face; // a .ttc holds several faces
    ImFont *font = io.Fonts->AddFontFromFileTTF(_path, _size, &base);
    if (font == nullptr || !_cjk) {
      return font;
    }
    for (const auto &cjk : cjk_fonts) {
      std::error_code ec;
      if (!std::filesystem::exists(cjk.path, ec)) {
        continue;
      }
      ImFontConfig merge;
      merge.MergeMode = true; // add to that font instead of replacing it
      merge.FontNo = cjk.face; // a .ttc holds several faces
      io.Fonts->AddFontFromFileTTF(cjk.path, _size, &merge, cjk_ranges.data());
      break;
    }
    return font;
  };

  // Arial for Latin. San Francisco is unusable: it is a variable font and
  // ImGui's rasterizer ignores variation axes, so every weight -- titles
  // included -- would draw Regular. (Helvetica Neue has real faces if that
  // trade is ever worth making.)
  ImFont *font_body = load_font("/System/Library/Fonts/Supplemental/Arial.ttf",
                                16.0F, true, 0);
  if (font_body == nullptr) {
    font_body = io.Fonts->AddFontDefault();
  }
  ImFont *font_bold = load_font(
      "/System/Library/Fonts/Supplemental/Arial Bold.ttf", 16.0F, true, 0);
  if (font_bold == nullptr) {
    font_bold = font_body;
  }
  ImFont *font_title = load_font(
      "/System/Library/Fonts/Supplemental/Arial Bold.ttf", 20.0F, false, 0);
  if (font_title == nullptr) {
    font_title = font_bold;
  }
  llm_panel_set_bold_font(font_bold);

  // The script editor is a character grid, so it needs one width for every
  // glyph: with a proportional face the caret and the selection drift off the
  // text. CJK is merged in for the same reason as the other faces -- comments
  // in a script are written in whatever language its author thinks in.
  // Probed like the CJK faces below: AddFontFromFileTTF asserts on a missing
  // file, and this one is not guaranteed on every machine. The full merge
  // doubles the atlas to 4096x16384 (measured); the 2500 common set would keep
  // it at 4096x8192 but draw "?" for hanzi outside it, which the editor hits
  // while typing, not only while drawing.
  ImFont *font_mono = nullptr;
  const char *mono_path = "/System/Library/Fonts/Menlo.ttc";
  std::error_code mono_ec;
  if (std::filesystem::exists(mono_path, mono_ec)) {
    font_mono = load_font(mono_path, 16.0F, true, 0);
  }
  script_editor_panel_set_mono_font(font_mono);

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);
#if defined(__APPLE__)
  // Points the IME candidate window at the caret ImGui publishes, and keeps a
  // live composition out of ImGui's hands.
  macos_ime_attach(window);
#endif

  st.append_log("[ui] cfg=" + cfg_path.string());
  st.append_log("[ui] script combo: switching logs that script snapshot to the "
                "log pane.");

  campcat::ccat_script_profile ccat_ui = st.snapshot_ccat_live();

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);

    ImGui::Begin("campcat-shell", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar);

    campcat::app_config cfg_view = st.snapshot_cfg();

    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("File")) {
        const bool can_save = ccat_ui.has_script_source();
        if (!can_save) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Save config")) {
          st.save_config(cfg_view, ccat_ui);
        }
        if (!can_save) {
          ImGui::EndDisabled();
          ImGui::TextDisabled("Set a .ccat script path before saving.");
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

    {
      std::vector<std::string> drained;
      st.log_lines.drain(&drained);
      for (auto &l : drained) {
        st.log_scroll.push_back(std::move(l));
      }
      while (st.log_scroll.size() > 2000) {
        st.log_scroll.pop_front();
      }
    }

    const bool busy_now = st.cycle_running.load();
    const bool lock_manual_single = busy_now || st.sched.running();
    const char *status_label = busy_now ? "Running" : "Idle";

    enum class shell_page : int { Script = 0, Llm, Console, Settings };
    static shell_page page = shell_page::Script;

    auto nav_item = [&](const char *label, shell_page target) {
      const bool selected = (page == target);
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.28F, 0.45F, 0.58F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.32F, 0.50F, 0.64F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.24F, 0.40F, 0.52F, 1.0F));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.16F, 0.17F, 0.19F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.22F, 0.24F, 0.27F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.20F, 0.22F, 0.25F, 1.0F));
      }
      ImGui::PushFont(font_bold);
      if (ImGui::Button(label, ImVec2(-1.0F, 0.0F))) {
        page = target;
      }
      ImGui::PopFont();
      ImGui::PopStyleColor(3);
    };

    constexpr float k_left_w = 320.0F;
    static bool log_open = true;
    const float body_h = ImGui::GetContentRegionAvail().y;

    if (ImGui::BeginTable(
            "shell_layout", 2,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoPadOuterX |
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_SizingStretchProp,
            ImVec2(-1.0F, body_h))) {
      ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed,
                              k_left_w);
      ImGui::TableSetupColumn("content", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableNextRow(ImGuiTableRowFlags_None, body_h);
      ImGui::TableSetColumnIndex(0);

      ImGui::BeginChild("shell_nav", ImVec2(0.0F, 0.0F),
                        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
      ImGui::Spacing();
      ImGui::PushFont(font_title);
      ImGui::TextUnformatted("CampCat");
      ImGui::PopFont();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      nav_item("Script", shell_page::Script);
      nav_item("LLM", shell_page::Llm);
      nav_item("Console", shell_page::Console);
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      nav_item("Settings", shell_page::Settings);
      ImGui::EndChild();

      // Negative height consumes whatever the nav block left in the column;
      // collapsed, the box shrinks to its header instead.
      ImGui::BeginChild(
          "shell_log",
          ImVec2(0.0F,
                 log_open ? -ImGui::GetStyle().ItemSpacing.y : 0.0F),
          ImGuiChildFlags_Borders |
              (log_open ? 0 : ImGuiChildFlags_AutoResizeY));
      ImGui::PushFont(font_bold);
      if (ImGui::Button(log_open ? "[-] Log" : "[+] Log")) {
        log_open = !log_open;
      }
      ImGui::PopFont();
      ImGui::Separator();
      if (log_open) {
        ImGui::BeginChild("logpane", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None);
        for (const auto &line : st.log_scroll) {
          ImGui::TextWrapped("%s", line.c_str());
        }
        if (!st.log_scroll.empty() &&
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0F) {
          ImGui::SetScrollHereY(1.0F);
        }
        if (ImGui::BeginPopupContextWindow("logpane_ctx",
                                           ImGuiPopupFlags_MouseButtonRight)) {
          if (ImGui::MenuItem("Clear log")) {
            st.log_lines.clear();
            st.log_scroll.clear();
          }
          ImGui::EndPopup();
        }
        ImGui::EndChild();
      }
      ImGui::EndChild();

      ImGui::TableSetColumnIndex(1);
      ImGui::BeginChild("shell_content", ImVec2(0.0F, 0.0F),
                        ImGuiChildFlags_Borders);

      {
        const char *page_title = "Script";
        switch (page) {
        case shell_page::Llm:
          page_title = "LLM";
          break;
        case shell_page::Console:
          page_title = "Console";
          break;
        case shell_page::Settings:
          page_title = "Settings";
          break;
        case shell_page::Script:
        default:
          page_title = "Script";
          break;
        }
        ImGui::PushFont(font_title);
        ImGui::TextUnformatted(page_title);
        ImGui::PopFont();
        ImGui::PushFont(font_bold);
        {
          const float status_w = ImGui::CalcTextSize(status_label).x;
          ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - status_w);
        }
        if (busy_now) {
          ImGui::TextColored(ImVec4(0.45F, 0.85F, 0.55F, 1.0F), "%s",
                             status_label);
        } else {
          ImGui::TextDisabled("%s", status_label);
        }
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
      }

      if (page == shell_page::Script) {
        script_panel_draw(st, cfg_view, ccat_ui, lock_manual_single, font_bold);
      } else if (page == shell_page::Llm) {
        llm_panel_draw_panel(st, cfg_view, lock_manual_single);
      } else if (page == shell_page::Console) {
        console_panel_draw(st, cfg_view, ccat_ui, busy_now, font_bold);
      } else if (page == shell_page::Settings) {
        settings_panel_draw(st, cfg_view);
      }

      ImGui::EndChild();
      ImGui::EndTable();
    }

    ImGui::End();

    // Top level, not inside the shell window: the user may switch pages while
    // correcting a box, and the window must survive that.
    llm_panel_draw_overlay();

    ImGui::Render();

    int display_w = 0;
    int display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);

    st.push_job_snapshot(&cfg_view, ccat_ui);
  }

  st.stop_workers();

  st.close_launched_mumu();

  campcat::automation_log::clear_sink();

  template_capture_panel_shutdown_gl();
  llm_panel_shutdown_gl();
  script_editor_panel_shutdown_gl(); // preview textures, before the context goes
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();
  // After the window is gone: this waits for the last memory write, and a wait
  // nobody can see is the whole point of splitting shutdown in two.
  llm_panel_shutdown_finish();
  return 0;
}
