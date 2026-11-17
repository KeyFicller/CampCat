#pragma once

#include "app/ui/fonts.h"
#include "app/ui/theme.h"

#include <imgui.h>

namespace campcat::app {

/// How loud a button is. One primary per page at most; secondary is the plain
/// themed button.
enum class button_kind { primary, secondary, ghost, danger };

/// @brief Pushes the colours of `_kind`. @return how many to pop.
inline int push_button_kind(button_kind _kind) {
    using namespace theme;
    switch (_kind) {
    case button_kind::primary:
        ImGui::PushStyleColor(ImGuiCol_Button, k_accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k_accent_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, k_accent_press);
        ImGui::PushStyleColor(ImGuiCol_Border, k_accent);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(0xFFFFFF));
        return 5;
    case button_kind::ghost:
        // A transparent border rather than a zero border size, so a ghost
        // button stays the same box as its neighbours.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k_surface_3);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, k_surface_4);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4());
        ImGui::PushStyleColor(ImGuiCol_Text, k_text_2);
        return 5;
    case button_kind::danger:
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k_danger_wash);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(0xF0686A, 0.24F));
        ImGui::PushStyleColor(ImGuiCol_Border, rgb(0xF0686A, 0.6F));
        ImGui::PushStyleColor(ImGuiCol_Text, k_danger);
        return 5;
    case button_kind::secondary:
        break;
    }
    return 0;
}

inline bool button(const char *_label, const ImVec2 &_size = ImVec2(),
                   button_kind _kind = button_kind::secondary) {
    const int pushed = push_button_kind(_kind);
    ImGui::PushFont(fonts_get(font_role::bold));
    const bool clicked = ImGui::Button(_label, _size);
    ImGui::PopFont();
    ImGui::PopStyleColor(pushed);
    return clicked;
}

inline bool small_button(const char *_label) {
    ImGui::PushFont(fonts_get(font_role::bold));
    const bool clicked = ImGui::SmallButton(_label);
    ImGui::PopFont();
    return clicked;
}

inline float button_width(const char *_label) {
    ImGui::PushFont(fonts_get(font_role::bold));
    const float width = ImGui::CalcTextSize(_label, nullptr, true).x +
                        ImGui::GetStyle().FramePadding.x * 2.0F;
    ImGui::PopFont();
    return width;
}

/// @brief Where `AddText` must start so `_glyph`'s ink, not its line box, sits
/// in the middle of the box. A Lucide glyph fills neither the line nor the
/// advance, so centring those leaves it high and to one side.
inline ImVec2 icon_origin(ImFont *_font, const char *_glyph, const ImVec2 &_box_min,
                          const ImVec2 &_box_max) {
    // One codepoint. Every icon is a three-byte private-use character.
    const auto b0 = static_cast<unsigned char>(_glyph[0]);
    const auto b1 = static_cast<unsigned char>(_glyph[1]);
    const auto b2 = static_cast<unsigned char>(_glyph[2]);
    const unsigned int cp = ((b0 & 0x0FU) << 12) | ((b1 & 0x3FU) << 6) | (b2 & 0x3FU);
    const ImFontGlyph *g = _font->FindGlyph(static_cast<ImWchar>(cp));
    const ImVec2 centre((_box_min.x + _box_max.x) * 0.5F, (_box_min.y + _box_max.y) * 0.5F);
    if (g == nullptr || g->Codepoint != cp) {
        const ImVec2 sz = _font->CalcTextSizeA(_font->FontSize, FLT_MAX, 0.0F, _glyph);
        return ImVec2(centre.x - sz.x * 0.5F, centre.y - sz.y * 0.5F);
    }
    return ImVec2(centre.x - (g->X0 + g->X1) * 0.5F, centre.y - (g->Y0 + g->Y1) * 0.5F);
}

/// @brief A square, frame-height button showing one icon glyph.
///
/// The icon carries the action, so no label is drawn and the width a label
/// would reserve stays with the box beside it. The tooltip is shown even while
/// the button is disabled, which is the only way to say why it is greyed.
inline bool icon_button(const char *_id, const char *_glyph, const char *_tooltip,
                        button_kind _kind = button_kind::ghost) {
    const float side = ImGui::GetFrameHeight();
    ImGui::PushID(_id);
    const int pushed = push_button_kind(_kind);
    // The body face carries the icons; the caller's may be mono or chat.
    ImFont *icon_font = fonts_get(font_role::body);
    ImGui::PushFont(icon_font);
    const bool clicked = ImGui::Button("##", ImVec2(side, side));
    ImGui::GetWindowDrawList()->AddText(
        icon_font, icon_font->FontSize,
        icon_origin(icon_font, _glyph, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()),
        ImGui::GetColorU32(ImGuiCol_Text), _glyph);
    ImGui::PopFont();
    ImGui::PopStyleColor(pushed);
    ImGui::PopID();
    // Wrapped, because some of these answers are a paragraph, not a word.
    if (_tooltip != nullptr &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0F);
        ImGui::TextUnformatted(_tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    return clicked;
}

} // namespace campcat::app
