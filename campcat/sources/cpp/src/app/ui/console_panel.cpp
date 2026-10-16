#include "app/ui/console_panel.h"

#include "app/shell_state.h"
#include "app/ui/activity_status.h"
#include "app/ui/fonts.h"
#include "app/ui/prompt_input.h"
#include "ccat_driver/ccat_script_profile.h"
#include "core/script/ccat_parser.h"
#include "core/script/ccat_repl.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string>

void console_panel_draw(campcat::shell_state &_st,
                        campcat::app_config &_cfg_view, bool _busy_now) {
    // Only this page uses them, so they live here rather than in shell_state.
    static campcat::ccat_lang::CcatRepl console_repl;
    static std::string console_input;

    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        "Interactive .ccat console. Multiline uses ... until braces/parens "
        "balance. Disabled while a cycle is running.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Room for exactly the rows drawn below it, so the prompt sits at the
    // bottom the way the LLM page's input does.
    const float below =
        ImGui::GetFrameHeightWithSpacing() +
        (_busy_now ? ImGui::GetTextLineHeightWithSpacing() : 0.0F);
    ImGui::BeginChild("console_history", ImVec2(0.0F, -below),
                      ImGuiChildFlags_Borders);
    // The same affordance as the log and the LLM panes: right-click the output
    // rather than a button beside the prompt. Clearing brings back the empty
    // state below, so it is only offered when there is output to drop.
    if (ImGui::BeginPopupContextWindow("console_history_ctx",
                                       ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Clear output", nullptr, false,
                            !console_repl.history.empty())) {
            console_repl.history.clear();
        }
        ImGui::EndPopup();
    }
    // An empty bordered box reads as a drawing glitch rather than as "no
    // output yet". Offset and centred the way the LLM log places its own
    // empty state, so the two panes read the same at rest.
    if (console_repl.history.empty()) {
        const char *title = "No output yet";
        const char *hint = "Send some ccat script code for execution.";
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.4F);
        WITH_FONT(bold, {
            ImGui::SetCursorPosX(
                ImGui::GetCursorPosX() +
                std::max(0.0F,
                         (avail.x - ImGui::CalcTextSize(title).x) * 0.5F));
            ImGui::TextUnformatted(title);
        });
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
        ImGui::TextColored(
            campcat::activity_color(campcat::activity_level::waiting),
            "cycle running - console locked");
        ImGui::BeginDisabled();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(console_repl.prompt());
    ImGui::SameLine();
    // The whole width now: the Clear button that used to sit here is the
    // context menu on the output above.
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool submit = prompt_input_text(
        "##console_in", "cwd → print the console directory", &console_input);
    // Must run immediately after InputText; later widgets would steal -1.
    if (submit && !_busy_now) {
        ImGui::SetKeyboardFocusHere(-1);
    }

    // Drops the half-typed multiline buffer, not the output: `clear()` is named
    // for that buffer alone. The menu item above is what clears the output.
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
            const std::string &pending = console_repl.pending;
            const auto begin = std::find_if_not(
                pending.begin(), pending.end(), [](unsigned char c) {
                    return std::isspace(c);
                });
            const auto end = std::find_if_not(
                pending.rbegin(), pending.rend(), [](unsigned char c) {
                    return std::isspace(c);
                }).base();
            if (begin < end && std::string(begin, end) == "cwd") {
                const campcat::ccat_script_profile profile =
                    campcat::ccat_script_profile::load(_cfg_view.config_home,
                                                       _cfg_view.script_bundle);
                const std::string root =
                    profile.console_root(_cfg_view.config_home).string();
                console_repl.history.push_back(root);
                _st.append_log(std::string("[console] ") + root);
                console_repl.clear();
            } else {
                try {
                    (void)campcat::ccat_lang::parse_program(
                        console_repl.pending);
                    _st.push_job_snapshot(&_cfg_view);
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
