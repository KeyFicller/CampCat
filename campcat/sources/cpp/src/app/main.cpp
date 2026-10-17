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

#include "app/shell_state.h"
#include "app/ui/activity_status.h"
#include "app/ui/console_panel.h"
#include "app/ui/fonts.h"
#include "app/ui/graph_panel.h"
#include "app/ui/llm_panel.h"
#include "app/ui/log_panel.h"
#include "app/ui/macos_ime.h"
#include "app/ui/script_editor_panel.h"
#include "app/ui/script_panel.h"
#include "app/ui/settings_panel.h"
#include "app/ui/template_capture_panel.h"
#include "ccat_driver/ccat_script_profile.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "core/llm/llm_tools.h"

namespace {

/// The four UI faces come from one place (`app/ui/fonts.cpp`); the aliases keep
/// the call sites below to one line.
using campcat::app::font_role;
using campcat::app::fonts_get;

constexpr char k_default_config_relative[] = "config.json";

/// The dot diameter and the gap after it, in ems of the current font. The dot
/// sits on the text's midline, so it reads as a bullet rather than a stray dot.
constexpr float k_chip_dot_ems = 0.5F;
constexpr float k_chip_dot_gap = 6.0F;
/// Between the page chip and the device chip, wide enough that they read as two
/// answers instead of one long one.
constexpr float k_chip_gap = 16.0F;

/// Shared by the palette and the nav's selected item, so the shell has one blue.
const ImVec4 k_accent{0.243F, 0.400F, 0.529F, 1.00F};
const ImVec4 k_accent_hi{0.302F, 0.478F, 0.616F, 1.00F};
const ImVec4 k_accent_lo{0.200F, 0.341F, 0.463F, 1.00F};

/// One chip's width: the dot, its gap, then the word.
float chip_width(const campcat::activity_status &_status) {
    return ImGui::GetTextLineHeight() * k_chip_dot_ems + k_chip_dot_gap +
           ImGui::CalcTextSize(_status.label).x;
}

/// Draw one dot-and-word chip at the cursor. The dot goes on the window's draw
/// list because ImGui has no bullet widget; the dummy reserves its box so the
/// word starts where the next chip's measurement expects it to.
void draw_chip(const campcat::activity_status &_status) {
    const float dot = ImGui::GetTextLineHeight() * k_chip_dot_ems;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec4 col = activity_color(_status.level);
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(at.x + dot * 0.5F, at.y + ImGui::GetTextLineHeight() * 0.5F),
        dot * 0.5F, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(dot + k_chip_dot_gap, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0F, 0.0F);
    ImGui::TextColored(col, "%s", _status.label);
}

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

    // The shell's state and its jobs: what used to be this function's locals
    // and capturing lambdas, now shared with the pages.
    campcat::shell_state st(cfg_path);
    // Off the UI thread, so the first bad `adb` path cannot freeze the window.
    st.start_device_poll();

    glfwSetErrorCallback([](int code, const char *desc) {
        std::fprintf(stderr, "GLFW error %d: %s\n", code,
                     desc ? desc : "(null)");
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

        // One palette, every slot filled. A partly themed style leaves the rest
        // on ImGui's defaults -- bright scrollbars and checkmarks, a saturated
        // blue on every header -- and that mix is what reads as unfinished.
        // Values are low-contrast on purpose: the accent marks what is active,
        // nothing shouts.
        const ImVec4 bg{0.086F, 0.094F, 0.106F, 1.00F};
        const ImVec4 panel{0.106F, 0.116F, 0.133F, 1.00F};
        const ImVec4 field{0.145F, 0.157F, 0.176F, 1.00F};
        const ImVec4 field_hi{0.176F, 0.192F, 0.216F, 1.00F};
        const ImVec4 line{0.196F, 0.212F, 0.239F, 1.00F};
        const ImVec4 line_hi{0.278F, 0.302F, 0.337F, 1.00F};
        const ImVec4 ink{0.847F, 0.871F, 0.898F, 1.00F};
        const ImVec4 ink_dim{0.478F, 0.510F, 0.549F, 1.00F};
        const ImVec4 &accent = k_accent;
        const ImVec4 &accent_hi = k_accent_hi;
        const ImVec4 &accent_lo = k_accent_lo;
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
        // A plain button is a surface, not a primary. Filled with the accent,
        // every button on a page looked equally important -- Run once, Stop and
        // Send all read as the one thing to press. The accent is spent on the
        // single primary per page instead; see the Send button in
        // ui/llm_panel.cpp.
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

    campcat::app::fonts_init();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
#if defined(__APPLE__)
    // Points the IME candidate window at the caret ImGui publishes, and keeps a
    // live composition out of ImGui's hands.
    macos_ime_attach(window);
#endif

    st.append_log("[ui] cfg=" + cfg_path.string());

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
        // Re-read every frame: the Script page's bundle picker publishes
        // through `set_script_bundle`, so this view is the shell's, not ours.
        const campcat::ccat_script_profile ccat_ui = st.snapshot_ccat_live();

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save config")) {
                    st.save_config(cfg_view);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        const bool busy_now = st.cycle_running.load();
        const bool lock_manual_single = busy_now || st.sched.running();

        enum class shell_page : int { Script = 0, Llm, Console, Graph, Settings };
        static shell_page page = shell_page::Script;

        // What the title row reports. Per page, because the badge sits next to
        // the page's own title: one global flag there either claimed the LLM
        // was idle while it thought, or said "Running" on the LLM page because
        // an automation cycle happened to be running. The device chip is the
        // opposite -- an app level fact, so every page shows it.
        campcat::activity_status page_status{};
        switch (page) {
        case shell_page::Llm:
            page_status = llm_panel_activity();
            break;
        case shell_page::Script:
            if (st.mumu_launching.load()) {
                page_status = {campcat::activity_level::waiting, "Booting"};
            } else if (busy_now) {
                page_status = {campcat::activity_level::busy, "Running"};
            } else if (st.sched.running()) {
                page_status = {campcat::activity_level::waiting, "Scheduled"};
            }
            break;
        case shell_page::Console:
            if (busy_now) {
                page_status = {campcat::activity_level::busy, "Running"};
            }
            break;
        case shell_page::Graph:
            break; // no activity of its own; the device chip below still shows
        case shell_page::Settings:
            break; // no activity of its own; the device chip below still shows
        }
        const bool show_page_chip =
            page != shell_page::Settings && page != shell_page::Graph;

        campcat::activity_status device_status{};
        if (st.device_online.load()) {
            // The poll connects before it reads, so this is ground truth: a
            // launch still in flight must not hide a device that already
            // answers, which is what read as "should be connected" while the
            // instance was up.
            device_status = {campcat::activity_level::ok, "Connected"};
        } else if (st.mumu_launching.load() || campcat::llm::emulator_starting()) {
            // Only reached before the device answers: while the instance comes
            // up the poll can only report "not there yet", which would read as
            // a failure for minutes on end.
            device_status = {campcat::activity_level::waiting, "Booting"};
        } else {
            device_status = {campcat::activity_level::idle, "No device"};
        }

        auto nav_item = [&](const char *label, shell_page target) {
            const bool selected = (page == target);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, k_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k_accent_hi);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, k_accent_lo);
            }
            WITH_FONT(bold, {
                if (ImGui::Button(label, ImVec2(-1.0F, 0.0F))) {
                    page = target;
                }
            });
            if (selected) {
                ImGui::PopStyleColor(3);
            }
        };

        constexpr float k_left_w = 320.0F;
        const float body_h = ImGui::GetContentRegionAvail().y;

        if (ImGui::BeginTable("shell_layout", 2,
                              ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_NoPadOuterX |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp,
                              ImVec2(-1.0F, body_h))) {
            ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed,
                                    k_left_w);
            ImGui::TableSetupColumn("content",
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, body_h);
            ImGui::TableSetColumnIndex(0);

            // Collapsed, the log shrinks to its header, so the nav block above
            // takes the rest of the column instead of leaving it empty. It
            // cannot simply fill: the log is submitted after it, so by then the
            // nav's height is fixed and the header would be pushed off the
            // bottom. The nav therefore stops short of the header, whose height
            // `log_panel` owns and derives from the style.
            const bool log_open = log_panel_open();
            const bool nav_fills = !log_open;
            ImGui::BeginChild(
                "shell_nav",
                ImVec2(0.0F, nav_fills ? -(log_panel_collapsed_height() +
                                           ImGui::GetStyle().ItemSpacing.y)
                                       : 0.0F),
                ImGuiChildFlags_Borders |
                    (nav_fills ? 0 : ImGuiChildFlags_AutoResizeY));
            ImGui::Spacing();
            WITH_FONT(title, { ImGui::TextUnformatted("CampCat"); });
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            nav_item("Script", shell_page::Script);
            nav_item("LLM", shell_page::Llm);
            nav_item("Console", shell_page::Console);
            nav_item("Graph", shell_page::Graph);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            nav_item("Settings", shell_page::Settings);
            ImGui::EndChild();

            // The other half of the pair above: one fills, the other fits.
            log_panel_draw(st);

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
                case shell_page::Graph:
                    page_title = "Graph";
                    break;
                case shell_page::Settings:
                    page_title = "Settings";
                    break;
                case shell_page::Script:
                default:
                    page_title = "Script";
                    break;
                }
                WITH_FONT(title, { ImGui::TextUnformatted(page_title); });
                WITH_FONT(bold, {
                    const float chips_w =
                        (show_page_chip ? chip_width(page_status) + k_chip_gap
                                        : 0.0F) +
                        chip_width(device_status);
                    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x -
                                    chips_w);
                    if (show_page_chip) {
                        draw_chip(page_status);
                        ImGui::SameLine(0.0F, k_chip_gap);
                    }
                    draw_chip(device_status);
                });
                ImGui::Separator();
                ImGui::Spacing();
            }

            if (page == shell_page::Script) {
                script_panel_draw(st, cfg_view, ccat_ui, lock_manual_single);
            } else if (page == shell_page::Llm) {
                llm_panel_draw_panel(st, cfg_view, lock_manual_single);
            } else if (page == shell_page::Console) {
                console_panel_draw(st, cfg_view, busy_now);
            } else if (page == shell_page::Graph) {
                graph_panel_draw(cfg_view);
            } else if (page == shell_page::Settings) {
                settings_panel_draw(st, cfg_view);
            }

            ImGui::EndChild();
            ImGui::EndTable();
        }

        ImGui::End();

        // Top level, not inside the shell window: the user may switch pages
        // while correcting a box, and the window must survive that.
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

        st.push_job_snapshot(&cfg_view);
    }

    // The settings popup edits the live config and nothing else, so this is
    // where a change made there outlives the run. Before `stop_workers` and
    // before the log sink goes: the save reports through the log.
    st.save_config(st.snapshot_cfg());

    st.stop_workers();

    st.close_launched_mumu();

    campcat::automation_log::clear_sink();

    template_capture_panel_shutdown_gl();
    llm_panel_shutdown_gl();
    script_editor_panel_shutdown_gl(); // preview textures, before the context
                                       // goes
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    // After the window is gone: this waits for the last memory write, and a
    // wait nobody can see is the whole point of splitting shutdown in two.
    llm_panel_shutdown_finish();
    return 0;
}
