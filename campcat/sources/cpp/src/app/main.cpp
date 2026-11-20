#include <algorithm>
#include <cfloat>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app/shell_state.h"
#include "app/ui/activity_status.h"
#include "app/ui/console_panel.h"
#include "app/ui/buttons.h"
#include "app/ui/graph_panel.h"
#include "app/ui/icons.h"
#include "app/ui/llm_panel.h"
#include "app/ui/log_panel.h"
#include "app/ui/macos_ime.h"
#include "app/ui/macos_window.h"
#include "app/ui/script_editor_panel.h"
#include "app/ui/script_panel.h"
#include "app/ui/settings_panel.h"
#include "app/ui/shell_dock.h"
#include "app/ui/template_capture_panel.h"
#include "app/ui/theme.h"
#include "app/ui/widgets.h"
#include "ccat_driver/ccat_script_profile.h"
#include "core/app_config.h"
#include "core/script/ccat_bundle.h"
#include "core/script/ccat_graph.h"
#include "core/automation_log.h"
#include "core/llm/llm_tools.h"
#include "core/mcp/script_server.h"
#include "core/smoke/smoke_run.h"

namespace {

constexpr char k_default_config_relative[] = "config.json";

/// The port `--mcp-http` listens on when none is named.
constexpr int k_default_mcp_http_port = 8787;

/// The MCP faces take `--graph <name>` anywhere after the mode token. The other
/// tokens keep their old positions, so `--mcp-http 8787 config.json` and
/// `--mcp-http --graph stzb 8787 config.json` mean the same thing.
/// @return false when `--graph` is missing or has no value.
bool take_graph(int _argc, char **_argv, int _from, std::string *_graph,
                std::vector<std::string> *_rest) {
    for (int i = _from; i < _argc; ++i) {
        if (std::string(_argv[i]) == "--graph") {
            if (i + 1 >= _argc) {
                return false;
            }
            *_graph = _argv[++i];
            continue;
        }
        _rest->push_back(_argv[i]);
    }
    return !_graph->empty();
}

/// Two lines, printed when `--graph` is missing: the one thing a client author
/// has to add, and where it goes.
void print_mcp_usage() {
    std::fprintf(stderr,
                 "campcat: --graph <name> is required for --mcp and --mcp-http\n"
                 "usage: campcat --mcp --graph <name> [config.json]\n"
                 "       campcat --mcp-http [port] --graph <name> [config.json]\n");
}

enum class shell_page : int { Script = 0, Llm, Console, Graph, Settings };
enum class workspace_destination { current, graph, script };

workspace_destination draw_workspace_toolbar(campcat::shell_state &_st,
                                             campcat::app_config &_cfg,
                                             bool _locked,
                                             bool *_details_open,
                                             shell_page _page) {
    static std::filesystem::path cached_home;
    static bool loaded = false;
    static double next_scan = 0.0;
    static std::vector<std::string> graphs;
    static std::vector<std::string> bundles;
    auto &selection = _st.workspace;
    const std::filesystem::path home = _cfg.config_home;
    const bool changed_home = !loaded || cached_home != home;
    if (changed_home || ImGui::GetTime() >= next_scan) {
        cached_home = home;
        loaded = true;
        next_scan = ImGui::GetTime() + 1.0;
        selection.set_home(home);
        graphs = campcat::ccat_lang::graph_names(home);
        bundles = campcat::ccat_lang::bundle_names(home);
        selection.reconcile(graphs, bundles);
        if (changed_home) {
            if (std::find(bundles.begin(), bundles.end(), _cfg.script_bundle) !=
                bundles.end()) {
                const auto tag = campcat::ccat_lang::bundle_tags(home,
                                                                _cfg.script_bundle);
                selection.select_bundle(
                    !tag.anchor_of.empty() ? tag.anchor_of
                    : !tag.edge_of.empty() ? tag.edge_of : tag.action_of,
                    _cfg.script_bundle);
            } else if (!graphs.empty()) {
                selection.select_graph(graphs.front());
            }
        }
    }
    if (_page != shell_page::Graph && _page != shell_page::Llm) {
        return workspace_destination::current;
    }
    workspace_destination destination = workspace_destination::current;
    ImGui::PushID("workspace_toolbar");
    const float width = ImGui::GetContentRegionAvail().x;
    if (ImGui::BeginTable("context", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("graph", ImGuiTableColumnFlags_WidthFixed,
                                std::min(200.0F, width * 0.4F));
        ImGui::TableSetupColumn("selection", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFrameHeight());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (_page == shell_page::Llm) {
            llm_panel_draw_index_toggle();
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::BeginDisabled(_locked);
        if (ImGui::BeginCombo("##graph", selection.graph.empty()
                                          ? "(no graph)" : selection.graph.c_str())) {
            for (const std::string &name : graphs) {
                if (ImGui::Selectable(name.c_str(), name == selection.graph)) {
                    selection.select_graph(name);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Choose graph");
        }
        ImGui::EndDisabled();
        ImGui::TableSetColumnIndex(1);
        const ImVec2 label_at = ImGui::GetCursorScreenPos();
        const ImVec2 label_size(std::max(1.0F, ImGui::GetContentRegionAvail().x),
                                ImGui::GetFrameHeight());
        const ImVec4 label_clip(label_at.x, label_at.y,
                                label_at.x + label_size.x,
                                label_at.y + label_size.y);
        ImGui::GetWindowDrawList()->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(),
            ImVec2(label_at.x, label_at.y + ImGui::GetStyle().FramePadding.y),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            selection.bundle.empty() ? "Select a skill" : selection.bundle.c_str(),
            nullptr, 0.0F, &label_clip);
        ImGui::Dummy(label_size);
        if (ImGui::IsItemHovered() && !selection.bundle.empty()) {
            ImGui::SetTooltip("%s", selection.bundle.c_str());
        }
        ImGui::TableSetColumnIndex(2);
        if (campcat::app::icon_button("##more", campcat::app::icon::k_more, "More")) {
            ImGui::OpenPopup("skill_actions");
        }
        if (ImGui::BeginPopup("skill_actions")) {
            if (ImGui::MenuItem("Edit script", nullptr, false,
                                 !selection.bundle.empty())) {
                script_editor_open(home, selection.bundle);
            }
            if (ImGui::MenuItem("Details", nullptr, *_details_open,
                                 !selection.bundle.empty())) {
                *_details_open = !*_details_open;
            }
            if (_page != shell_page::Graph &&
                ImGui::MenuItem("Show graph", nullptr, false,
                                 !selection.graph.empty())) {
                graph_panel_focus_selection();
                destination = workspace_destination::graph;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Use for run", nullptr, false,
                                 !selection.bundle.empty() && !_locked)) {
                _cfg.script_bundle = selection.bundle;
                _st.set_script_bundle(selection.bundle);
                destination = workspace_destination::script;
            }
            ImGui::EndPopup();
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    return destination;
}

/// The words next to the page title. Graph and Settings have no activity of
/// their own; the caller skips the chip there. An untouched status is Idle,
/// which is what Script shows when nothing is running.
campcat::activity_status page_activity(shell_page _page, campcat::shell_state &_st,
                                      bool _busy) {
    switch (_page) {
    case shell_page::Llm:
        return llm_panel_activity();
    case shell_page::Script:
        if (_st.mumu_launching.load()) {
            return {campcat::activity_level::waiting, "Booting"};
        }
        if (_busy) {
            return {campcat::activity_level::busy, "Running"};
        }
        if (_st.sched.running()) {
            return {campcat::activity_level::waiting, "Scheduled"};
        }
        break;
    case shell_page::Console:
        if (_busy) {
            return {campcat::activity_level::busy, "Running"};
        }
        break;
    case shell_page::Graph:
    case shell_page::Settings:
        break;
    }
    return {};
}

const char *page_window_name(shell_page _page) {
    switch (_page) {
    case shell_page::Llm:
        return k_dock_llm;
    case shell_page::Console:
        return k_dock_console;
    case shell_page::Graph:
        return k_dock_graph;
    case shell_page::Settings:
        return k_dock_settings;
    case shell_page::Script:
        return k_dock_script;
    }
    return k_dock_script;
}

/// The strip under everything: the device, the current page's own activity
/// (null where the page has none), and the scheduler's next run. Called before
/// the host window, because it takes its height out of the viewport's work area.
void draw_status_bar(campcat::shell_state &_st,
                     const campcat::activity_status &_device,
                     const campcat::activity_status *_page) {
    using namespace campcat::app;
    ImGui::PushFont(fonts_get(font_role::small));
    // A menu bar is the one row ImGui centres vertically; its height is the
    // font plus twice this padding, so the padding is what fits it to the bar.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x,
                               (theme::k_status_bar_h - ImGui::GetFontSize()) * 0.5F));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, theme::k_bg_app);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::k_bg_app);
    if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down,
                                    theme::k_status_bar_h,
                                    ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_MenuBar)) {
        const ImVec2 p = ImGui::GetWindowPos();
        ImGui::PushClipRect(p, ImVec2(p.x + ImGui::GetWindowWidth(), p.y + 1.0F), false);
        ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetWindowWidth(), p.y),
                                            ImGui::GetColorU32(theme::k_border));
        ImGui::PopClipRect();
        if (ImGui::BeginMenuBar()) {
            pill(_device);
            // An emulator boot is both the device's state and the page's, and
            // the same word twice reads as two things happening.
            if (_page != nullptr && std::strcmp(_page->label, _device.label) != 0) {
                ImGui::SameLine(0.0F, 16.0F);
                pill(*_page);
            }
            if (_st.sched.running()) {
                // The wait left, not the wait spent: `duration_seconds` is this
                // tick's interval plus its jitter, and what the user wants to
                // know is when the next cycle fires.
                char text[64];
                const auto wp = _st.sched.tick_wait_progress();
                if (wp.in_wait_phase && wp.duration_seconds > 0) {
                    const int left = static_cast<int>(
                        std::max(0.0, wp.duration_seconds - wp.elapsed_seconds));
                    std::snprintf(text, sizeof(text), "%s  Next run %d:%02d", icon::k_clock,
                                  left / 60, left % 60);
                } else {
                    std::snprintf(text, sizeof(text), "%s  Running a cycle...", icon::k_clock);
                }
                const float w = ImGui::CalcTextSize(text).x;
                const float x = ImGui::GetWindowContentRegionMax().x - w;
                if (x > ImGui::GetCursorPosX()) {
                    ImGui::SetCursorPosX(x);
                }
                ImGui::TextColored(theme::k_text_2, "%s", text);
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

/// The sidebar's head: the accent tile with the cat, then the name.
void draw_brand() {
    using namespace campcat::app;
    constexpr float k_tile = 28.0F;
    ImFont *title = fonts_get(font_role::title);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + k_tile, at.y + k_tile),
                      ImGui::GetColorU32(theme::k_accent), 8.0F);
    dl->AddText(title, title->FontSize,
                icon_origin(title, icon::k_cat, at, ImVec2(at.x + k_tile, at.y + k_tile)),
                ImGui::GetColorU32(theme::rgb(0xFFFFFF)), icon::k_cat);
    dl->AddText(title, title->FontSize,
                ImVec2(at.x + k_tile + 10.0F, at.y + (k_tile - title->FontSize) * 0.5F),
                ImGui::GetColorU32(theme::k_text), "CampCat");
    ImGui::Dummy(ImVec2(k_tile, k_tile));
}

/// One sidebar row: icon then label, left-aligned, with a wash when selected.
bool nav_item(const char *_glyph, const char *_label, bool _selected) {
    using namespace campcat::app;
    constexpr float k_inset = 12.0F;
    constexpr float k_glyph_gap = 10.0F;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    // A split dragged onto the window padding leaves no content width, and
    // InvisibleButton asserts on a zero size.
    const float w = ImGui::GetContentRegionAvail().x;
    if (w < 1.0F) {
        return false;
    }
    const ImVec2 size(w, theme::k_sidebar_item_h);
    ImGui::PushID(_label);
    const bool clicked = ImGui::InvisibleButton("##nav", size);
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (_selected || ImGui::IsItemHovered()) {
        dl->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y),
                          ImGui::GetColorU32(_selected ? theme::k_accent_wash
                                                       : theme::k_surface_3),
                          8.0F);
    }
    const float y = at.y + (size.y - ImGui::GetTextLineHeight()) * 0.5F;
    dl->AddText(ImVec2(at.x + k_inset, y),
                ImGui::GetColorU32(_selected ? theme::k_accent_fg : theme::k_text_2), _glyph);
    ImFont *label_font = fonts_get(_selected ? font_role::bold : font_role::body);
    dl->AddText(label_font, label_font->FontSize,
                ImVec2(at.x + k_inset + ImGui::CalcTextSize(_glyph).x + k_glyph_gap, y),
                ImGui::GetColorU32(_selected ? theme::k_text : theme::k_text_2), _label);
    return clicked;
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

    // The MCP face: run saved bundles for another agent, over stdio. No window,
    // no device poll, no scheduler -- and nothing that writes to the library.
    if (argc >= 2 && std::string(argv[1]) == "--mcp") {
        std::string graph;
        std::vector<std::string> rest;
        if (!take_graph(argc, argv, 2, &graph, &rest)) {
            print_mcp_usage();
            return 2;
        }
        return campcat::serve_script_mcp(
            std::filesystem::path(rest.empty() ? k_default_config_relative
                                               : rest[0]),
            std::move(graph));
    }
    // The same face over Streamable HTTP, on the loopback address only: this
    // machine, or a reverse proxy in front of it that terminates TLS.
    if (argc >= 2 && std::string(argv[1]) == "--mcp-http") {
        std::string graph;
        std::vector<std::string> rest;
        if (!take_graph(argc, argv, 2, &graph, &rest)) {
            print_mcp_usage();
            return 2;
        }
        const int port = rest.size() >= 1 ? std::atoi(rest[0].c_str())
                                          : k_default_mcp_http_port;
        return campcat::serve_script_mcp(
            std::filesystem::path(rest.size() >= 2 ? rest[1]
                                                   : k_default_config_relative),
            std::move(graph), port);
    }

    if (argc >= 2 && std::string(argv[1]) == "--verify-smoke") {
        if (argc < 4 || argc > 5) {
            std::fprintf(stderr, "usage: campcat --verify-smoke <library> <replays> [expect.json]\n");
            return 2;
        }
        const auto problems = campcat::smoke_problems(argv[2], argv[3], argc == 5 ? argv[4] : "");
        for (const auto &problem : problems) std::printf("[smoke] %s\n", problem.c_str());
        std::printf("[smoke] acceptance %s\n", problems.empty() ? "passed" : "FAILED");
        return problems.empty() ? 0 : 1;
    }

    // The smoke face: build one graph on the demo app with no window and print
    // what it took. Same sidecar, prompts and tools as the conversation page,
    // so the number is what a person in the GUI would have paid.
    if (argc >= 2 && std::string(argv[1]) == "--smoke") {
        if (argc < 3) {
            std::fprintf(stderr,
                         "usage: campcat --smoke <prompts.txt> [config.json]\n");
            return 2;
        }
        return campcat::run_smoke(
            std::filesystem::path(argv[2]),
            std::filesystem::path(argc >= 4 ? argv[3]
                                            : k_default_config_relative));
    }

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

    // The design size, shrunk to the primary monitor's work area: a 13" laptop
    // is 1280x800 points, and macOS cuts a window taller than its screen.
    int window_w = 1320;
    int window_h = 860;
    if (GLFWmonitor *monitor = glfwGetPrimaryMonitor()) {
        int area_x = 0;
        int area_y = 0;
        int area_w = 0;
        int area_h = 0;
        glfwGetMonitorWorkarea(monitor, &area_x, &area_y, &area_w, &area_h);
        // ponytail: 28 is the macOS title bar; a taller one (other OS, or a
        // future macOS) clips a few points until the user resizes once.
        constexpr int k_title_bar_h = 28;
        if (area_w > 0 && area_h > k_title_bar_h) {
            window_w = std::min(window_w, area_w);
            window_h = std::min(window_h, area_h - k_title_bar_h);
        }
    }
    GLFWwindow *window =
        glfwCreateWindow(window_w, window_h, "CampCat", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 2;
    }
    glfwSetWindowSizeLimits(window, 1024, 680, GLFW_DONT_CARE, GLFW_DONT_CARE);
#if defined(__APPLE__)
    {
        const float bg_rgb[3] = {campcat::app::theme::k_bg_app.x,
                                 campcat::app::theme::k_bg_app.y,
                                 campcat::app::theme::k_bg_app.z};
        macos_window_style(window, bg_rgb);
    }
#endif
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Beside config.json, not inside ccat_library. That directory is the script
    // pack submodule; the ini is shell state, and the repo already ignores the
    // name `imgui.ini`. The pointer has to outlive the context.
    static std::string imgui_ini;
    {
        const std::filesystem::path library(st.snapshot_cfg().config_home);
        std::filesystem::path ini_dir =
            library.empty() ? cfg_path.parent_path() : library.parent_path();
        if (ini_dir.empty()) {
            ini_dir = ".";
        }
        std::error_code ini_ec;
        std::filesystem::create_directories(ini_dir, ini_ec);
        imgui_ini = (ini_dir / "imgui.ini").string();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = imgui_ini.c_str();
    llm_panel_init_settings();
    io.ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    campcat::app::theme_apply();

    float content_scale_x = 1.0F;
    float content_scale_y = 1.0F;
    glfwGetWindowContentScale(window, &content_scale_x, &content_scale_y);
    campcat::app::fonts_init(content_scale_x);

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

        campcat::app_config cfg_view = st.snapshot_cfg();
        // Re-read every frame: the Script page's bundle picker publishes
        // through `set_script_bundle`, so this view is the shell's, not ours.

        static shell_page page = shell_page::Script;
        const bool busy_now = st.device_tasks.busy();
        const bool lock_manual_single = busy_now || st.sched.running();
        llm_panel_tick(st, cfg_view, page == shell_page::Llm);

        // The device pill is an app level fact, so every page shows it. The
        // page pill is that page's own: one shared flag either claimed the LLM
        // was idle while it thought, or said "Running" on the LLM page because
        // an automation cycle happened to be running.
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
        const campcat::activity_status page_status = page_activity(page, st, busy_now);
        draw_status_bar(st, device_status,
                        page != shell_page::Settings && page != shell_page::Graph
                            ? &page_status
                            : nullptr);

        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        // The host is the frame around the dockspace. Zero padding so the
        // splits reach the window edge; popped immediately so the panels keep
        // the themed padding.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::Begin("campcat-shell", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoNavFocus |
                         ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_MenuBar |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();

        bool reset_layout = false;
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save config")) {
                    st.save_config(cfg_view);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Reset layout")) {
                    reset_layout = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        const bool layout_built = shell_dock_submit(reset_layout);
        ImGui::End();

        static bool focus_queued = false;
        static shell_page focus_page = shell_page::Script;
        if (layout_built) {
            page = shell_page::Script;
            focus_queued = true;
            focus_page = shell_page::Script;
        }
        auto show_page = [&](shell_page target) {
            page = target;
            focus_queued = true;
            focus_page = target;
        };

        auto nav = [&](const char *glyph, const char *label, shell_page target) {
            if (nav_item(glyph, label, page == target)) {
                show_page(target);
            }
        };

        shell_dock_hide_next_tab_bar();
        if (ImGui::Begin(k_dock_campcat, nullptr, k_dock_panel_flags)) {
            namespace icon = campcat::app::icon;
            draw_brand();
            ImGui::Spacing();
            nav(icon::k_script, "Script", shell_page::Script);
            nav(icon::k_llm, "LLM", shell_page::Llm);
            nav(icon::k_console, "Console", shell_page::Console);
            nav(icon::k_graph, "Graph", shell_page::Graph);
            if (llm_panel_draw_sidebar_task(page == shell_page::Llm)) {
                show_page(shell_page::Llm);
            }
            // Pinned to the bottom while there is room; in a short window it
            // just follows the rest, so it can never cover the agent card.
            // The extra point keeps rounding from summoning a scrollbar.
            const float bottom = ImGui::GetWindowHeight() -
                                 campcat::app::theme::k_sidebar_item_h -
                                 ImGui::GetStyle().WindowPadding.y - 1.0F;
            if (ImGui::GetCursorPosY() < bottom) {
                ImGui::SetCursorPosY(bottom);
            }
            nav(icon::k_settings, "Settings", shell_page::Settings);
        }
        ImGui::End();

        log_panel_draw(st);

        static bool details_open = false;
        const auto activity = llm_panel_activity();
        const bool run_locked = lock_manual_single || st.mumu_launching.load() ||
            activity.level == campcat::activity_level::busy ||
            activity.level == campcat::activity_level::waiting;
        const bool save_locked = st.device_tasks.busy() || st.sched.running();

        auto begin_page = [&](shell_page which) {
            // A nav click queues the target before this runs. An earlier page
            // that is still focused this frame must not write `page` back.
            const bool want_focus = focus_queued && focus_page == which;
            if (want_focus) {
                ImGui::SetNextWindowFocus();
            }
            const bool open =
                ImGui::Begin(page_window_name(which), nullptr, k_dock_panel_flags);
            if (want_focus) {
                focus_queued = false;
                page = which;
            } else if (open && !focus_queued &&
                       ImGui::IsWindowFocused(
                           ImGuiFocusedFlags_RootAndChildWindows)) {
                page = which;
            }
            if (open) {
                if (which == shell_page::Llm || which == shell_page::Graph) {
                    switch (draw_workspace_toolbar(st, cfg_view, run_locked,
                                                   &details_open, which)) {
                    case workspace_destination::graph:
                        show_page(shell_page::Graph);
                        break;
                    case workspace_destination::script:
                        show_page(shell_page::Script);
                        break;
                    case workspace_destination::current:
                        break;
                    }
                }
            }
            return open;
        };

        if (begin_page(shell_page::Script)) {
            script_panel_draw(st, cfg_view, st.snapshot_ccat_live(),
                              lock_manual_single);
        }
        ImGui::End();
        if (begin_page(shell_page::Llm)) {
            llm_panel_draw_panel(st, cfg_view);
        }
        ImGui::End();
        if (begin_page(shell_page::Console)) {
            console_panel_draw(st, cfg_view, lock_manual_single);
        }
        ImGui::End();
        if (begin_page(shell_page::Graph)) {
            graph_panel_draw(cfg_view, st.workspace);
        }
        ImGui::End();
        if (begin_page(shell_page::Settings)) {
            settings_panel_draw(st, cfg_view);
        }
        ImGui::End();

        // Same right-hand node as Details. Hidden until a script is open, so
        // the first frame does not reserve an empty column.
        if (script_editor_has_open()) {
            if (script_editor_focus_requested()) {
                ImGui::SetNextWindowFocus();
            }
            if (ImGui::Begin(k_dock_editor, nullptr, k_dock_panel_flags)) {
                script_editor_draw_docked(save_locked);
            }
            ImGui::End();
        }

        static bool details_was_open = false;
        const bool show_details =
            details_open && !st.workspace.bundle.empty();
        if (show_details) {
            if (!details_was_open) {
                ImGui::SetNextWindowFocus();
            }
            if (ImGui::Begin(k_dock_details, &details_open, k_dock_panel_flags)) {
                script_editor_inspector_draw(cfg_view.config_home,
                                             st.workspace.bundle);
            }
            ImGui::End();
        }
        details_was_open = show_details;

        // Top level, not inside the shell window: the user may switch pages
        // while correcting a box, and the window must survive that.
        llm_panel_draw_overlay();

        ImGui::Render();

        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(campcat::app::theme::k_bg_app.x, campcat::app::theme::k_bg_app.y,
                     campcat::app::theme::k_bg_app.z, 1.0F);
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

    template_capture_panel_shutdown_gl();
    llm_panel_shutdown_gl();
    // A nested emulator launch must finish before its watchdog is closed.
    st.close_launched_mumu();
    campcat::automation_log::clear_sink();
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
