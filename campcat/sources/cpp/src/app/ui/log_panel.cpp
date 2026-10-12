#include "app/ui/log_panel.h"

#include "app/shell_state.h"
#include "app/ui/fonts.h"
#include "app/ui/icon_texture.h"

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

/// The on-screen lines outlive any one page, so the buffer is capped here
/// rather than by whoever emitted.
constexpr size_t k_max_scroll_lines = 2000;

/// Whether the log is expanded. Read by `log_panel_open` before the nav block
/// is drawn, flipped by the header's icon button.
bool g_open = true;

/// How wide the chevron is drawn inside its square button, in pixels. Smaller
/// than the button so the frame stays visible around it.
constexpr float k_icon_side = 14.0F;

/// The chevrons, relative to the working directory the app is started from (the
/// same base `fonts.cpp` resolves `campcat/sources/cpp/assets/fonts/` against). Drawn rather than
/// designed: a white isosceles triangle at a base span of 0.70 of the canvas
/// and an apex 0.55 out, 8x supersampled and INTER_AREA'd down, the right one
/// being the down one rotated a quarter turn. White on an alpha channel is the
/// shape `load_icon` uploads.
constexpr char k_chevron_down_path[] = "campcat/sources/cpp/assets/icons/chevron-down.png";
constexpr char k_chevron_right_path[] = "campcat/sources/cpp/assets/icons/chevron-right.png";

} // namespace

float log_panel_collapsed_height() {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0F;
}

bool log_panel_open() { return g_open; }

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

    // This frame's state, taken once: the click below flips `g_open` for the
    // next frame, so the box, the icon and the nav across the column all change
    // together instead of splitting across a frame.
    const bool open = g_open;

    // Both states want a height of 0: expanded, 0 fills the cell's remaining
    // height (the same amount the collapsed box leaves below it); collapsed,
    // AutoResizeY shrinks the box to its header. Sizing the expanded one
    // `-ItemSpacing.y` instead leaves an 8px strip the collapsed state does not
    // have, and the two should end at the same place.
    ImGui::BeginChild("shell_log", ImVec2(0.0F, 0.0F),
                      ImGuiChildFlags_Borders |
                          (open ? 0 : ImGuiChildFlags_AutoResizeY));

    const float side = ImGui::GetFrameHeight();
    if (ImGui::Button("##log_toggle", ImVec2(side, side))) {
        g_open = !g_open;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(open ? "Collapse" : "Expand");
    }
    // Drawn after the button, into the same window list, so the icon lands on
    // top of the frame. Tinted with the text colour so it follows the theme.
    const GLuint ic = campcat::app::load_icon(open ? k_chevron_down_path
                                                   : k_chevron_right_path);
    if (ic != 0) {
        const ImVec2 mn = ImGui::GetItemRectMin();
        const float pad = (side - k_icon_side) * 0.5F;
        ImGui::GetWindowDrawList()->AddImage(
            static_cast<ImTextureID>(static_cast<uintptr_t>(ic)),
            ImVec2(mn.x + pad, mn.y + pad),
            ImVec2(mn.x + side - pad, mn.y + side - pad), ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F), ImGui::GetColorU32(ImGuiCol_Text));
    }
    ImGui::SameLine();
    WITH_FONT(bold, { ImGui::TextUnformatted("Log"); });

    if (open) {
        ImGui::BeginChild("logpane", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None);
        for (const auto &line : _st.log_scroll) {
            ImGui::TextWrapped("%s", line.c_str());
        }
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
    ImGui::EndChild();
}
