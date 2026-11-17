#include "app/ui/theme.h"

namespace campcat::app {

void theme_apply() {
    using namespace theme;
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(16.0F, 14.0F);
    style.FramePadding = ImVec2(10.0F, 7.0F);
    style.ItemSpacing = ImVec2(10.0F, 10.0F);
    style.ItemInnerSpacing = ImVec2(8.0F, 6.0F);
    style.CellPadding = ImVec2(8.0F, 6.0F);
    style.ScrollbarSize = 10.0F;
    style.GrabMinSize = 10.0F;

    style.WindowRounding = 0.0F;
    style.FrameRounding = k_radius_control;
    style.ChildRounding = k_radius_card;
    style.PopupRounding = k_radius_popup;
    style.TabRounding = k_radius_control;
    style.GrabRounding = 4.0F;
    style.ScrollbarRounding = 8.0F;

    // Lines, not fills, separate regions: every control, card and popup gets
    // a 1px edge, the dock windows themselves none.
    style.WindowBorderSize = 0.0F;
    style.FrameBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.TabBarOverlineSize = 2.0F;
    style.DockingSeparatorSize = 1.0F;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.DisabledAlpha = 0.45F;

    // One palette, every slot filled. A partly themed style leaves the rest
    // on ImGui's defaults -- bright scrollbars and checkmarks, a saturated
    // blue on every header -- and that mix is what reads as unfinished.
    constexpr ImVec4 none{};
    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = k_text;
    c[ImGuiCol_TextDisabled] = k_text_3;
    c[ImGuiCol_WindowBg] = k_surface_1;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = k_surface_3;
    c[ImGuiCol_Border] = k_border_strong;
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = k_surface_inset;
    c[ImGuiCol_FrameBgHovered] = k_surface_2;
    c[ImGuiCol_FrameBgActive] = k_surface_2;
    c[ImGuiCol_TitleBg] = k_bg_app;
    c[ImGuiCol_TitleBgActive] = k_surface_1;
    c[ImGuiCol_TitleBgCollapsed] = k_bg_app;
    c[ImGuiCol_MenuBarBg] = k_bg_app;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = k_surface_4;
    c[ImGuiCol_ScrollbarGrabHovered] = k_border_strong;
    c[ImGuiCol_ScrollbarGrabActive] = k_text_3;
    c[ImGuiCol_CheckMark] = k_accent_fg;
    c[ImGuiCol_SliderGrab] = k_accent;
    c[ImGuiCol_SliderGrabActive] = k_accent_hover;
    // A plain button is a surface, not a primary. Filled with the accent,
    // every button on a page looked equally important -- Run once, Stop and
    // Send all read as the one thing to press. The accent is spent on the
    // single primary per page instead; see `button_kind` in ui/buttons.h.
    c[ImGuiCol_Button] = k_surface_3;
    c[ImGuiCol_ButtonHovered] = k_surface_4;
    c[ImGuiCol_ButtonActive] = k_surface_4;
    c[ImGuiCol_Header] = k_accent_wash;
    c[ImGuiCol_HeaderHovered] = k_surface_3;
    c[ImGuiCol_HeaderActive] = k_accent_wash_hi;
    c[ImGuiCol_Separator] = k_border;
    c[ImGuiCol_SeparatorHovered] = k_border_strong;
    c[ImGuiCol_SeparatorActive] = k_accent_fg;
    c[ImGuiCol_ResizeGrip] = none;
    c[ImGuiCol_ResizeGripHovered] = k_border_strong;
    c[ImGuiCol_ResizeGripActive] = k_accent_fg;
    c[ImGuiCol_TabHovered] = k_surface_3;
    c[ImGuiCol_Tab] = none;
    c[ImGuiCol_TabSelected] = k_surface_1;
    c[ImGuiCol_TabSelectedOverline] = k_accent_fg;
    c[ImGuiCol_TabDimmed] = none;
    c[ImGuiCol_TabDimmedSelected] = k_surface_1;
    c[ImGuiCol_TabDimmedSelectedOverline] = k_border_strong;
    c[ImGuiCol_DockingPreview] = rgb(0x7EA6FF, 0.40F);
    c[ImGuiCol_DockingEmptyBg] = k_bg_app;
    c[ImGuiCol_PlotLines] = k_accent_fg;
    c[ImGuiCol_PlotLinesHovered] = k_accent_hover;
    c[ImGuiCol_PlotHistogram] = k_accent; // ProgressBar fill
    c[ImGuiCol_PlotHistogramHovered] = k_accent_hover;
    c[ImGuiCol_TableHeaderBg] = k_surface_2;
    c[ImGuiCol_TableBorderStrong] = k_border;
    c[ImGuiCol_TableBorderLight] = k_border;
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = rgb(0xFFFFFF, 0.02F);
    c[ImGuiCol_TextLink] = k_accent_fg;
    c[ImGuiCol_TextSelectedBg] = rgb(0x7EA6FF, 0.30F);
    c[ImGuiCol_DragDropTarget] = k_accent_fg;
    c[ImGuiCol_NavCursor] = k_accent_fg;
    c[ImGuiCol_NavWindowingHighlight] = rgb(0xFFFFFF, 0.70F);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0x000000, 0.45F);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.55F);
}

} // namespace campcat::app
