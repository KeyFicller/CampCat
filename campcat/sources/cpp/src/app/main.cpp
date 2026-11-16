#include <algorithm>
#include <cfloat>
#include <csignal>
#include <cstdio>
#include <cstdlib>
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
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app/shell_state.h"
#include "app/ui/activity_status.h"
#include "app/ui/console_panel.h"
#include "app/ui/buttons.h"
#include "app/ui/graph_panel.h"
#include "app/ui/llm_panel.h"
#include "app/ui/log_panel.h"
#include "app/ui/macos_ime.h"
#include "app/ui/script_editor_panel.h"
#include "app/ui/script_panel.h"
#include "app/ui/settings_panel.h"
#include "app/ui/shell_dock.h"
#include "app/ui/template_capture_panel.h"
#include "ccat_driver/ccat_script_profile.h"
#include "core/app_config.h"
#include "core/script/ccat_bundle.h"
#include "core/script/ccat_graph.h"
#include "core/automation_log.h"
#include "core/llm/llm_tools.h"
#include "core/mcp/script_server.h"
#include "core/smoke/smoke_run.h"

namespace {

/// The four UI faces come from one place (`app/ui/fonts.cpp`); the aliases keep
/// the call sites below to one line.
using campcat::app::font_role;
using campcat::app::fonts_get;

constexpr char k_default_config_relative[] = "config.json";

/// The port `--mcp-http` listens on when none is named.
constexpr int k_default_mcp_http_port = 8787;

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
                                campcat::app::button_width("More"));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
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
        if (campcat::app::button("More")) {
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

/// Title in the large face, chips on its midline, then the rule under both.
/// The chips used to sit in the one content header; each page window draws its
/// own now that more than one page can be on screen.
void draw_page_heading(const char *_title, const campcat::activity_status &_page,
                       bool _show_page, const campcat::activity_status &_device) {
    const ImVec2 title_at = ImGui::GetCursorScreenPos();
    float title_h = 0.0F;
    WITH_FONT(title, {
        title_h = ImGui::GetTextLineHeight();
        ImGui::TextUnformatted(_title);
    });
    WITH_FONT(bold, {
        const float chips_w =
            (_show_page ? chip_width(_page) + k_chip_gap : 0.0F) +
            chip_width(_device);
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - chips_w);
        const float dy = (title_h - ImGui::GetTextLineHeight()) * 0.5F;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + dy));
        if (_show_page) {
            draw_chip(_page);
            ImGui::SameLine(0.0F, k_chip_gap);
        }
        draw_chip(_device);
        ImGui::SetCursorScreenPos(ImVec2(title_at.x, title_at.y + title_h));
    });
    ImGui::Separator();
    ImGui::Spacing();
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

    GLFWwindow *window =
        glfwCreateWindow(1180, 780, "CampCat Shell", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 2;
    }
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
    io.ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
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
        c[ImGuiCol_DockingPreview] =
            ImVec4(accent.x, accent.y, accent.z, 0.55F);
        c[ImGuiCol_DockingEmptyBg] = bg;
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

        campcat::app_config cfg_view = st.snapshot_cfg();
        // Re-read every frame: the Script page's bundle picker publishes
        // through `set_script_bundle`, so this view is the shell's, not ours.

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

        const bool busy_now = st.device_tasks.busy();
        const bool lock_manual_single = busy_now || st.sched.running();

        static shell_page page = shell_page::Script;
        llm_panel_tick(st, cfg_view, page == shell_page::Llm);

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

        // The device chip is an app level fact, so every page shows it. The
        // page chip is computed inside that page: one shared flag either
        // claimed the LLM was idle while it thought, or said "Running" on the
        // LLM page because an automation cycle happened to be running.
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
            if (campcat::app::button(label, ImVec2(-1.0F, 0.0F))) {
                show_page(target);
            }
            if (selected) {
                ImGui::PopStyleColor(3);
            }
        };

        if (ImGui::Begin(k_dock_campcat, nullptr, k_dock_panel_flags)) {
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
            if (llm_panel_draw_sidebar_task(page == shell_page::Llm)) {
                show_page(shell_page::Llm);
            }
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
                const bool show_chip = which != shell_page::Settings &&
                                       which != shell_page::Graph;
                draw_page_heading(page_window_name(which),
                                  page_activity(which, st, busy_now), show_chip,
                                  device_status);
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
