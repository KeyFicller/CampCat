#pragma once

#include "app/ui/fonts.h"

#include <imgui.h>

namespace campcat::app {

inline bool button(const char *_label, const ImVec2 &_size = ImVec2()) {
    ImGui::PushFont(fonts_get(font_role::bold));
    const bool clicked = ImGui::Button(_label, _size);
    ImGui::PopFont();
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

} // namespace campcat::app
