#include "app/ui/log_panel.h"

#include "app/shell_state.h"
#include "app/ui/buttons.h"
#include "app/ui/icons.h"
#include "app/ui/shell_dock.h"
#include "app/ui/theme.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace {

/// The on-screen lines outlive any one page, so the buffer is capped here
/// rather than by whoever emitted.
constexpr size_t k_max_scroll_lines = 2000;

/// Whether the lines are shown. Starts open: the window is a dock node whose
/// height does not follow this flag, and a closed log would leave that node
/// empty. The chevron still hides the lines.
bool g_open = true;

} // namespace

void log_panel_draw(campcat::shell_state &_st) {
    // Drained every frame, expanded or not: a collapsed log that stopped
    // collecting would hand the whole backlog over on the next expand.
    {
        std::vector<std::string> drained;
        _st.log_lines.drain(&drained);
        for (auto &line : drained) {
            _st.log_scroll.push_back(std::move(line));
        }
        while (_st.log_scroll.size() > k_max_scroll_lines) {
            _st.log_scroll.pop_front();
        }
    }

    // Taken once: the click below flips `g_open` for the next frame, so the
    // icon and the lines agree.
    const bool open = g_open;

    if (!ImGui::Begin(k_dock_log, nullptr, k_dock_panel_flags)) {
        ImGui::End();
        return;
    }

    if (campcat::app::icon_button("##log_toggle",
                                  open ? campcat::app::icon::k_chevron_down
                                       : campcat::app::icon::k_chevron_right,
                                  open ? "Collapse" : "Expand")) {
        g_open = !g_open;
    }

    if (open) {
        using namespace campcat::app::theme;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, k_surface_inset);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_radius_popup);
        ImGui::BeginChild("logpane", ImVec2(0.0F, 0.0F),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        WITH_FONT(mono, {
            for (const auto &line : _st.log_scroll) {
                // The clock is context, the message is the content: only the
                // leading stamp is dimmed, the rest prints as emitted.
                const std::size_t stamp = log_stamp_len(line);
                if (stamp > 0) {
                    ImGui::TextColored(k_text_3, "%.*s", static_cast<int>(stamp),
                                       line.c_str());
                    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
                }
                const char *rest = line.c_str() + stamp;
                if (stamp > 0 && *rest == ' ') {
                    ++rest;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, k_text_2);
                ImGui::TextWrapped("%s", rest);
                ImGui::PopStyleColor();
            }
        });
        if (!_st.log_scroll.empty() &&
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0F) {
            ImGui::SetScrollHereY(1.0F);
        }
        if (ImGui::BeginPopupContextWindow("logpane_ctx",
                                           ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::MenuItem("Clear log")) {
                _st.log_lines.clear();
                _st.log_scroll.clear();
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
