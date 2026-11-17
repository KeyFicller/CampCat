#include "app/ui/console_panel.h"

#include "app/shell_state.h"
#include "app/ui/fonts.h"
#include "app/ui/icons.h"
#include "app/ui/prompt_input.h"
#include "app/ui/theme.h"
#include "app/ui/widgets.h"
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
    // Last frame's: the terminal's border is drawn before the input exists.
    static bool input_active = false;
    using namespace campcat::app;

    WITH_FONT(small, {
        ImGui::TextColored(theme::k_text_3, "Interactive .ccat console. "
                                            "Multiline continues until braces balance.");
    });

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::k_surface_inset);
    ImGui::PushStyleColor(ImGuiCol_Border, input_active ? theme::k_accent_fg
                                                        : theme::k_border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::k_radius_card);
    ImGui::BeginChild("console_term", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    // Room for exactly the input row under it, so the prompt sits at the
    // bottom the way the LLM page's input does. The rule between them is
    // drawn in the gap, not as an item.
    const float row_h = fonts_get(font_role::mono)->FontSize +
                        ImGui::GetStyle().FramePadding.y * 2.0F;
    const float below = row_h + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("console_history", ImVec2(0.0F, -below));
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
    if (console_repl.history.empty()) {
        empty_state(icon::k_console, "No output yet",
                    "Send some ccat script code for execution.");
    }
    WITH_FONT(mono, {
        for (const auto &line : console_repl.history) {
            ImGui::TextUnformatted(line.c_str());
        }
    });
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();

    {
        // Edge to edge across the terminal, through its padding.
        const ImVec2 p = ImGui::GetWindowPos();
        const float x1 = p.x + ImGui::GetWindowWidth();
        const float y = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5F;
        ImGui::PushClipRect(p, ImVec2(x1, p.y + ImGui::GetWindowHeight()), false);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, y), ImVec2(x1, y),
                                            ImGui::GetColorU32(theme::k_border));
        ImGui::PopClipRect();
    }

    bool submit = false;
    WITH_FONT(mono, {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::k_accent_fg, "%s", console_repl.prompt());
        ImGui::SameLine();
        if (_busy_now) {
            tag("Cycle running \xE2\x80\x94 console locked", theme::k_warning);
            ImGui::SameLine();
            ImGui::BeginDisabled();
        }
        // The whole width now: the Clear button that used to sit here is the
        // context menu on the output above.
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4());
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4());
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4());
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
        submit = prompt_input_text("##console_in", "cwd → print the console directory",
                                   &console_input);
        input_active = ImGui::IsItemActive();
        // Must run immediately after InputText; later widgets would steal -1.
        if (submit && !_busy_now) {
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        // Drops the half-typed multiline buffer, not the output: `clear()` is
        // named for that buffer alone. The menu item above is what clears the
        // output.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            console_repl.clear();
        }

        if (_busy_now) {
            ImGui::EndDisabled();
        }
    });
    ImGui::EndChild();

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
