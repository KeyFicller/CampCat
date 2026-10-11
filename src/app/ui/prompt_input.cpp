#include "app/ui/prompt_input.h"

#if defined(__APPLE__)
#include "app/ui/macos_ime.h"
#endif

#include <imgui.h>
#include <imgui_internal.h> // GetInputTextState: the IME pre-edit hangs off the caret
#include <misc/cpp/imgui_stdlib.h>

#include <string>

namespace {

/// IME helper. Off Apple it is constant, so the input path needs no guard.
std::string ime_preedit() {
#if defined(__APPLE__)
  return macos_ime_preedit();
#else
  return {};
#endif
}

/// Paints the IME composition over the prompt box. While composing, ImGui's own
/// buffer is still empty, so nothing else in the box shows what is being typed.
///
/// Call it right after the prompt's InputText, before SameLine/Button: the caret
/// comes from the last item and the origin from its rect.
///
/// ponytail: no selection highlight, and the pre-edit is clipped to the box
/// instead of pushing the text after the caret along; upgrade if a mid-text
/// composition ever needs either.
void draw_ime_preedit(const ImVec2 &_box_min, const ImVec2 &_box_max,
                      const std::string &_buf, const std::string &_preedit) {
  ImGuiInputTextState *state = ImGui::GetInputTextState(ImGui::GetItemID());
  if (state == nullptr) {
    return; // not the live input right now
  }
  const int len = static_cast<int>(_buf.size());
  // Stb is only forward-declared outside imgui_widgets.cpp, so read the caret
  // through the accessor ImGui itself exposes.
  const int cursor = ImClamp(state->GetCursorPos(), 0, len); // byte index
  const ImGuiStyle &style = ImGui::GetStyle();
  const char *const buf = _buf.c_str();
  const float prefix = ImGui::CalcTextSize(buf, buf + cursor).x;
  const ImVec2 pos(_box_min.x + style.FramePadding.x + prefix - state->Scroll.x,
                   _box_min.y + style.FramePadding.y);
  const ImVec2 size = ImGui::CalcTextSize(_preedit.c_str());
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(_box_min, _box_max, true);
  dl->AddRectFilled(ImVec2(pos.x, _box_min.y + 3.0F),
                    ImVec2(pos.x + size.x + 2.0F, _box_min.y + size.y + 3.0F),
                    IM_COL32(70, 70, 85, 255), 3.0F);
  dl->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), _preedit.c_str());
  dl->AddLine(ImVec2(pos.x, pos.y + size.y),
              ImVec2(pos.x + size.x, pos.y + size.y),
              ImGui::GetColorU32(ImGuiCol_Text), 1.0F);
  dl->PopClipRect();
}

} // namespace

bool prompt_input_text(const char *_id, const char *_hint, std::string *_buf) {
  // ImGui is kept out of the keys while a composition is live (see macos_ime.mm),
  // so a submitted Enter is a real Enter, not one that picked a candidate.
  const std::string preedit = ime_preedit();
  // While composing the hint is blanked: the composition is painted over the box
  // and the two would collide on the same line.
  const bool submitted = ImGui::InputTextWithHint(
      _id, preedit.empty() ? _hint : "", _buf,
      ImGuiInputTextFlags_EnterReturnsTrue);
  // Both need the box to still be the last item.
  if (!preedit.empty()) {
    draw_ime_preedit(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), *_buf,
                     preedit);
  }
  return submitted;
}
