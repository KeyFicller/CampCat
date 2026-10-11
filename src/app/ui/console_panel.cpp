#include "app/ui/console_panel.h"

#include "app/shell_state.h"
#include "app/ui/fonts.h"
#include "app/ui/prompt_input.h"
#include "core/script/ccat_parser.h"
#include "core/script/ccat_repl.h"

#include <imgui.h>

#include <algorithm>
#include <string>

void console_panel_draw(campcat::shell_state &_st,
                        campcat::app_config &_cfg_view,
                        const campcat::ccat_script_profile &_ccat_ui,
                        bool _busy_now) {
    // Only this page uses them, so they live here rather than in shell_state.
    static campcat::ccat_lang::CcatRepl console_repl;
    static std::string console_input;

    ImGui::TextWrapped(
        "Interactive .ccat console. Multiline uses ... until braces/parens "
        "balance. Disabled while a cycle is running.");
    ImGui::Spacing();

    ImGui::BeginChild("console_history",
                      ImVec2(0.0F, -ImGui::GetFrameHeightWithSpacing() * 2.5F),
                      ImGuiChildFlags_Borders);
    // An empty bordered box reads as a drawing glitch rather than as "no
    // output yet". Offset and centred the way the LLM log places its own
    // empty state, so the two panes read the same at rest.
    if (console_repl.history.empty()) {
        const char *title = "No output yet";
        const char *hint = "Send some ccat script code for execution.";
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.4F);
        ImGui::PushFont(campcat::app::fonts_get(campcat::app::font_role::bold));
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(0.0F, (avail.x - ImGui::CalcTextSize(title).x) * 0.5F));
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(0.0F, (avail.x - ImGui::CalcTextSize(hint).x) * 0.5F));
        ImGui::TextDisabled("%s", hint);
    }
    for (const auto &line : console_repl.history) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();

    if (_busy_now) {
        ImGui::TextColored(ImVec4(0.9F, 0.6F, 0.2F, 1.0F),
                           "cycle running - console locked");
        ImGui::BeginDisabled();
    }

    ImGui::TextUnformatted(console_repl.prompt());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-80.0F);
    const bool submit =
        prompt_input_text("##console_in", nullptr, &console_input);
    // Must run immediately after InputText; later widgets would steal -1.
    if (submit && !_busy_now) {
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        console_repl.clear();
        console_repl.history.push_back("(cleared)");
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        console_repl.clear();
    }

    if (_busy_now) {
        ImGui::EndDisabled();
    }

    if (submit && !_busy_now) {
        const auto fr = console_repl.feed_line(console_input);
        console_input.clear();
        if (fr == campcat::ccat_lang::repl_feed_result::ready) {
            if (!_ccat_ui.has_script_source()) {
                console_repl.history.push_back(
                    "need .ccat path for images_base");
                _st.append_log("[console] need .ccat path for images_base");
                console_repl.clear();
            } else {
                try {
                    (void)campcat::ccat_lang::parse_program(
                        console_repl.pending);
                    _st.push_job_snapshot(&_cfg_view, _ccat_ui);
                    _st.run_console_source_job(console_repl.pending);
                    console_repl.clear();
                } catch (const campcat::ccat_lang::parse_error &ex) {
                    const std::string msg = std::string("parse error: ") +
                                            ex.what() + " (line " +
                                            std::to_string(ex.line) + ", col " +
                                            std::to_string(ex.col) + ")";
                    console_repl.history.push_back(msg);
                    _st.append_log(std::string("[console] ") + msg);
                    console_repl.clear();
                }
            }
        }
    }
}
