#pragma once

#include <imgui.h>

namespace campcat::app::theme {

/// @brief A colour from `0xRRGGBB`.
constexpr ImVec4 rgb(unsigned _hex, float _alpha = 1.0F) {
    return ImVec4(static_cast<float>((_hex >> 16U) & 0xFFU) / 255.0F,
                  static_cast<float>((_hex >> 8U) & 0xFFU) / 255.0F,
                  static_cast<float>(_hex & 0xFFU) / 255.0F, _alpha);
}

/// @brief Per-channel linear blend, `_t` = 0 gives `_a`.
constexpr ImVec4 mix(const ImVec4 &_a, const ImVec4 &_b, float _t) {
    return ImVec4(_a.x + (_b.x - _a.x) * _t, _a.y + (_b.y - _a.y) * _t,
                  _a.z + (_b.z - _a.z) * _t, _a.w + (_b.w - _a.w) * _t);
}

inline constexpr ImVec4 k_bg_app = rgb(0x0F1115);
inline constexpr ImVec4 k_surface_1 = rgb(0x15181D);
inline constexpr ImVec4 k_surface_2 = rgb(0x1B1F25);
inline constexpr ImVec4 k_surface_3 = rgb(0x232830);
inline constexpr ImVec4 k_surface_4 = rgb(0x2B313B);
inline constexpr ImVec4 k_surface_inset = rgb(0x101317);
inline constexpr ImVec4 k_border = rgb(0x262C35);
inline constexpr ImVec4 k_border_strong = rgb(0x353D49);

inline constexpr ImVec4 k_text = rgb(0xE8EBF0);
inline constexpr ImVec4 k_text_2 = rgb(0xA3ABB8);
/// Hints and disabled text only: 4:1 on `k_surface_1` is too faint for body copy.
inline constexpr ImVec4 k_text_3 = rgb(0x6E7787);

inline constexpr ImVec4 k_accent = rgb(0x3D6BE0);
inline constexpr ImVec4 k_accent_hover = rgb(0x4C7AEC);
inline constexpr ImVec4 k_accent_press = rgb(0x3359C2);
/// The accent as text or line on a dark surface; `k_accent` is a fill.
inline constexpr ImVec4 k_accent_fg = rgb(0x7EA6FF);
inline constexpr ImVec4 k_accent_wash = rgb(0x7EA6FF, 0.14F);
inline constexpr ImVec4 k_accent_wash_hi = rgb(0x7EA6FF, 0.22F);

inline constexpr ImVec4 k_success = rgb(0x4CC38A);
inline constexpr ImVec4 k_warning = rgb(0xE8A94B);
inline constexpr ImVec4 k_danger = rgb(0xF0686A);
inline constexpr ImVec4 k_success_wash = rgb(0x4CC38A, 0.14F);
inline constexpr ImVec4 k_warning_wash = rgb(0xE8A94B, 0.14F);
inline constexpr ImVec4 k_danger_wash = rgb(0xF0686A, 0.14F);
/// A third region colour where warning and accent are already taken.
inline constexpr ImVec4 k_violet = rgb(0xC9A2F5);

inline constexpr ImVec4 k_syntax_keyword = rgb(0x8AB4FF);
inline constexpr ImVec4 k_syntax_string = rgb(0x8FD19E);
inline constexpr ImVec4 k_syntax_number = rgb(0xF2B36F);
inline constexpr ImVec4 k_syntax_decl = rgb(0xC9A2F5);
inline constexpr ImVec4 k_syntax_builtin = rgb(0xF0D58C);

inline constexpr float k_radius_control = 6.0F;
inline constexpr float k_radius_card = 10.0F;
inline constexpr float k_radius_popup = 8.0F;
inline constexpr float k_radius_bubble = 12.0F;
inline constexpr float k_status_bar_h = 26.0F;
inline constexpr float k_sidebar_item_h = 36.0F;
inline constexpr float k_script_max_w = 880.0F;
inline constexpr float k_settings_max_w = 760.0F;
inline constexpr float k_chat_max_w = 820.0F;
inline constexpr float k_index_w = 240.0F;

} // namespace campcat::app::theme

namespace campcat::app {

/// @brief Sets every style variable and every `ImGuiCol_` slot from the tokens.
void theme_apply();

} // namespace campcat::app
