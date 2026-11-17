#include "app/ui/widgets.h"

#include "app/ui/buttons.h"
#include "app/ui/fonts.h"
#include "app/ui/icons.h"
#include "app/ui/theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>

namespace campcat::app {
namespace {

/// The dot diameter and the gap after it, in ems of the current font. The dot
/// sits on the text's midline, so it reads as a bullet rather than a stray dot.
constexpr float k_pill_dot_ems = 0.5F;
constexpr float k_pill_dot_gap = 6.0F;

constexpr float k_empty_tile = 56.0F;
constexpr float k_empty_tile_rounding = 14.0F;

/// `_text` in `_color`, cut with an ellipsis past `_room` and spelled out on
/// hover: the sidebar can be dragged narrower than a card's words.
void text_fit(const char *_text, float _room, const ImVec4 &_color) {
    const ImVec2 size = ImGui::CalcTextSize(_text);
    if (size.x <= _room) {
        ImGui::TextColored(_color, "%s", _text);
        return;
    }
    const float w = std::max(1.0F, _room);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float y = at.y + ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
    ImGui::PushStyleColor(ImGuiCol_Text, _color);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(at.x, y),
                              ImVec2(at.x + w, y + size.y), at.x + w, at.x + w, _text,
                              nullptr, &size);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(w, size.y));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _text);
    }
}

} // namespace

bool card_begin(const char *_id, const char *_title, const card_options &_opt) {
    using namespace theme;
    // Read before BeginChild: the flag belongs to the page, not to the card's
    // own window, so it is there on the frame the card is first created.
    ImGuiStorage *storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(_id);
    const bool open = !_opt.collapsible || storage->GetBool(key, true);

    // Background and border are painted inside BeginChild, so both colours can
    // go straight after it; the controls in the card keep the stronger edge.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, k_surface_2);
    ImGui::PushStyleColor(ImGuiCol_Border, k_border);
    ImGui::BeginChild(_id, ImVec2(_opt.width, 0.0F),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor(2);

    if (_opt.bar.w > 0.0F) {
        // The window clips half its padding in; the bar lives in that margin.
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 s = ImGui::GetWindowSize();
        ImGui::PushClipRect(p, ImVec2(p.x + s.x, p.y + s.y), false);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p.x + 1.0F, p.y + k_radius_card),
            ImVec2(p.x + 4.0F, p.y + s.y - k_radius_card),
            ImGui::GetColorU32(_opt.bar), 1.5F);
        ImGui::PopClipRect();
    }

    ImGui::AlignTextToFramePadding();
    if (_opt.glyph != nullptr) {
        ImGui::TextColored(_opt.bar.w > 0.0F ? _opt.bar : k_text_2, "%s", _opt.glyph);
        ImGui::SameLine();
    }
    const float right = ImGui::GetWindowContentRegionMax().x;
    const float toggle_w = _opt.collapsible ? ImGui::GetFrameHeight() : 0.0F;
    const float title_room = right - ImGui::GetCursorPosX() - toggle_w -
                             (_opt.collapsible ? ImGui::GetStyle().ItemSpacing.x : 0.0F);
    WITH_FONT(bold, { text_fit(_title, title_room, ImGui::GetStyleColorVec4(ImGuiCol_Text)); });

    const float title_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    // A narrow card (the sidebar's) moves the pill under the title rather than
    // dropping it: the state is the reason the card is on screen.
    bool pill_below = false;
    if (_opt.status != nullptr) {
        const float gap = _opt.collapsible ? ImGui::GetStyle().ItemSpacing.x : 0.0F;
        const float x = right - toggle_w - gap - pill_width(*_opt.status);
        if (x > title_end + ImGui::GetStyle().ItemSpacing.x) {
            ImGui::SameLine(x);
            pill(*_opt.status);
        } else {
            pill_below = true;
        }
    }
    if (_opt.collapsible) {
        ImGui::SameLine(right - toggle_w);
        if (icon_button("##card_toggle", open ? icon::k_chevron_down : icon::k_chevron_right,
                        open ? "Collapse" : "Expand")) {
            storage->SetBool(key, !open);
        }
    }
    if (pill_below) {
        pill(*_opt.status);
    }
    if (open && _opt.hint != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, k_text_2);
        ImGui::TextWrapped("%s", _opt.hint);
        ImGui::PopStyleColor();
    }
    return open;
}

void card_end() { ImGui::EndChild(); }

float pill_width(const activity_status &_s) {
    return ImGui::GetTextLineHeight() * k_pill_dot_ems + k_pill_dot_gap +
           ImGui::CalcTextSize(_s.label).x;
}

void pill(const activity_status &_s) {
    // ImGui has no bullet widget, so the dot goes on the draw list and a dummy
    // reserves its box. The baseline offset keeps the dot on the word's
    // midline when the row was aligned to a frame.
    const float dot = ImGui::GetTextLineHeight() * k_pill_dot_ems;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float mid = at.y + ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset +
                      ImGui::GetTextLineHeight() * 0.5F;
    const ImVec4 col = activity_color(_s.level);
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + dot * 0.5F, mid),
                                                dot * 0.5F, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(dot + k_pill_dot_gap, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0F, 0.0F);
    text_fit(_s.label, ImGui::GetContentRegionAvail().x, col);
}

void tag(const char *_text, const ImVec4 &_color) {
    const float frame_h = ImGui::GetFrameHeight();
    ImGui::PushFont(fonts_get(font_role::small));
    const ImVec2 text = ImGui::CalcTextSize(_text);
    const ImVec2 pad(8.0F, 2.0F);
    const ImVec2 size(text.x + pad.x * 2.0F, text.y + pad.y * 2.0F);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float top = at.y + std::max(0.0F, (frame_h - size.y) * 0.5F);
    ImVec4 wash = _color;
    wash.w *= 0.14F;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + size.x, top + size.y),
                      ImGui::GetColorU32(wash), size.y * 0.5F);
    dl->AddText(ImVec2(at.x + pad.x, top + pad.y), ImGui::GetColorU32(_color), _text);
    ImGui::Dummy(ImVec2(size.x, std::max(frame_h, size.y)));
    ImGui::PopFont();
}

bool segmented(const char *_id, const char *const *_labels, int _count, int *_index) {
    using namespace theme;
    const ImGuiStyle &style = ImGui::GetStyle();
    float seg_w = 0.0F;
    for (int i = 0; i < _count; ++i) {
        seg_w = std::max(seg_w, ImGui::CalcTextSize(_labels[i]).x);
    }
    seg_w += style.FramePadding.x * 2.0F;
    // Too wide beside its label (a docked tool column): it takes the next line
    // under the label rather than running off the edge.
    if (seg_w * static_cast<float>(_count) > ImGui::GetContentRegionAvail().x &&
        ImGui::GetCursorPosX() > ImGui::GetCursorStartPos().x + 0.5F) {
        ImGui::NewLine();
    }
    const float h = ImGui::GetFrameHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 end(at.x + seg_w * static_cast<float>(_count), at.y + h);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, end, ImGui::GetColorU32(k_surface_inset), k_radius_control);
    dl->AddRect(at, end, ImGui::GetColorU32(k_border_strong), k_radius_control);

    bool changed = false;
    ImGui::PushID(_id);
    for (int i = 0; i < _count; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0F, 0.0F);
        }
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##seg", ImVec2(seg_w, h)) && *_index != i) {
            *_index = i;
            changed = true;
        }
        const bool selected = *_index == i;
        const ImVec2 inset_min(p.x + 2.0F, p.y + 2.0F);
        const ImVec2 inset_max(p.x + seg_w - 2.0F, p.y + h - 2.0F);
        if (selected) {
            dl->AddRectFilled(inset_min, inset_max, ImGui::GetColorU32(k_surface_3),
                              k_radius_control - 2.0F);
        } else if (ImGui::IsItemHovered()) {
            dl->AddRectFilled(inset_min, inset_max, ImGui::GetColorU32(k_surface_2),
                              k_radius_control - 2.0F);
        }
        const float tw = ImGui::CalcTextSize(_labels[i]).x;
        dl->AddText(ImVec2(p.x + (seg_w - tw) * 0.5F, p.y + style.FramePadding.y),
                    ImGui::GetColorU32(selected ? k_text : k_text_2), _labels[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

void form_label(const char *_label, float _label_w) {
    // A narrow panel (a docked tool column) keeps most of its width for the
    // control: the column shrinks to about a third of the row.
    const float column = std::min(_label_w, ImGui::GetCursorPosX() +
                                                ImGui::GetContentRegionAvail().x * 0.35F);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::k_text_2, "%s", _label);
    // A label longer than the column pushes its control right instead of
    // running under it.
    ImGui::SameLine();
    if (ImGui::GetCursorPosX() < column) {
        ImGui::SameLine(column);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void focus_ring(const ImVec2 &_min, const ImVec2 &_max, float _rounding, bool _active) {
    if (_active) {
        ImGui::GetWindowDrawList()->AddRect(_min, _max, ImGui::GetColorU32(theme::k_accent_fg),
                                            _rounding);
    }
}

float content_width(float _max) { return std::min(ImGui::GetContentRegionAvail().x, _max); }

void same_line_if_fits(float _w) {
    if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x +
            _w <=
        ImGui::GetWindowContentRegionMax().x) {
        ImGui::SameLine();
    }
}

void empty_state(const char *_glyph, const char *_title, const char *_hint) {
    using namespace theme;
    ImFont *display = fonts_get(font_role::display);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = ImGui::GetStyle().ItemSpacing.y;
    const float block_h =
        k_empty_tile + gap * 2.0F + display->FontSize + gap + ImGui::GetTextLineHeight();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         std::max(0.0F, avail.y * 0.4F - block_h * 0.5F));
    const float x0 = ImGui::GetCursorPosX();
    auto centre = [&](float _w) {
        ImGui::SetCursorPosX(x0 + std::max(0.0F, (avail.x - _w) * 0.5F));
    };

    centre(k_empty_tile);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + k_empty_tile, at.y + k_empty_tile),
                      ImGui::GetColorU32(k_accent_wash), k_empty_tile_rounding);
    ImGui::PushFont(display);
    dl->AddText(icon_origin(display, _glyph, at, ImVec2(at.x + k_empty_tile, at.y + k_empty_tile)),
                ImGui::GetColorU32(k_accent_fg), _glyph);
    ImGui::Dummy(ImVec2(k_empty_tile, k_empty_tile + gap));
    centre(ImGui::CalcTextSize(_title).x);
    ImGui::TextUnformatted(_title);
    ImGui::PopFont();
    centre(ImGui::CalcTextSize(_hint).x);
    ImGui::TextColored(k_text_2, "%s", _hint);
}

} // namespace campcat::app
