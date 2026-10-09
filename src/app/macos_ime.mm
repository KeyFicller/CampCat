#include "app/macos_ime.h"

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <imgui.h>

#include <cstdio>
#include <string>

namespace {

/// Caret rect in the content view's own coordinates, published by ImGui each
/// frame. Cocoa screens are queried on the main thread, same as the renderer.
NSRect g_caret_rect = NSMakeRect(0.0, 0.0, 0.0, 0.0);
NSView *g_text_view = nil;

/// The composition GLFW is holding. Its content view stores the marked text but
/// nothing ever draws it, so without publishing this the pinyin is invisible.
std::string g_preedit;
bool g_composing = false;
/// UTF-16 length of the composition, the unit `markedRange` deals in.
NSUInteger g_marked_len = 0;

IMP g_orig_set_marked = nullptr;
IMP g_orig_unmark = nullptr;
IMP g_orig_insert = nullptr;
IMP g_orig_key_down = nullptr;

/// Answers the IME's "where is the caret" query. Must return screen
/// coordinates, which is what the caller positions the candidate window from.
NSRect first_rect_for_character_range(id, SEL, NSRange, NSRangePointer) {
  if (g_text_view == nil) {
    return NSMakeRect(0.0, 0.0, 0.0, 0.0);
  }
  const NSRect in_window = [g_text_view convertRect:g_caret_rect toView:nil];
  return [g_text_view.window convertRectToScreen:in_window];
}

/// ImGui reports the caret as y-down from the content area's top-left, which is
/// not the view's axis when it is not flipped; ask the view instead of assuming.
void set_ime_data(ImGuiContext *, ImGuiViewport *,
                  ImGuiPlatformImeData *_data) {
  if (!_data->WantVisible || g_text_view == nil) {
    return;
  }
  const float view_h = static_cast<float>(g_text_view.bounds.size.height);
  const float y = g_text_view.isFlipped
                      ? _data->InputPos.y
                      : view_h - _data->InputPos.y - _data->InputLineHeight;
  g_caret_rect = NSMakeRect(_data->InputPos.x, y, 1.0, _data->InputLineHeight);
#ifndef NDEBUG
  std::fprintf(stderr, "[ime] pos %.1f,%.1f h=%.1f -> view %.1f,%.1f\n",
               _data->InputPos.x, _data->InputPos.y, _data->InputLineHeight,
               g_caret_rect.origin.x, g_caret_rect.origin.y);
#endif
}

/// Records what GLFW is holding as the composition. The original implementation
/// must still run, or GLFW's own markedText/hasMarkedText state goes stale.
void set_marked_text(id _self, SEL _cmd, id _string, NSRange _sel, NSRange _repl) {
  id text = [_string isKindOfClass:[NSAttributedString class]] ? [_string string]
                                                              : _string;
  const char *utf8 = [text UTF8String];
  g_preedit = (utf8 != nullptr) ? utf8 : "";
  g_composing = !g_preedit.empty();
  g_marked_len = [text length];
#ifndef NDEBUG
  // TEMPORARY diagnostics: pin down which call clears an emptied composition.
  std::fprintf(stderr, "[ime] setMarkedText len=%zu preedit='%s'\n",
               g_preedit.size(), g_preedit.c_str());
#endif
  reinterpret_cast<void (*)(id, SEL, id, NSRange, NSRange)>(g_orig_set_marked)(
      _self, _cmd, _string, _sel, _repl);
}

/// The IME dropped or cancelled the composition.
void unmark_text(id _self, SEL _cmd) {
#ifndef NDEBUG
  std::fprintf(stderr, "[ime] unmarkText\n");
#endif
  g_preedit.clear();
  g_composing = false;
  g_marked_len = 0;
  reinterpret_cast<void (*)(id, SEL)>(g_orig_unmark)(_self, _cmd);
}

/// The composition was committed. The characters themselves reach ImGui through
/// GLFW's char callback as usual.
///
/// GLFW's own `insertText:` leaves its marked text in place, so `hasMarkedText`
/// keeps answering YES after a commit — the IME then believes a composition is
/// still live. Clear it, which is what the protocol says a commit means.
void insert_text(id _self, SEL _cmd, id _string, NSRange _repl) {
#ifndef NDEBUG
  id text = [_string isKindOfClass:[NSAttributedString class]] ? [_string string]
                                                              : _string;
  const char *utf8 = [text UTF8String];
  std::fprintf(stderr, "[ime] insertText '%s'\n", utf8 != nullptr ? utf8 : "");
#endif
  g_preedit.clear();
  g_composing = false;
  g_marked_len = 0;
  reinterpret_cast<void (*)(id, SEL, id, NSRange)>(g_orig_insert)(_self, _cmd,
                                                                 _string, _repl);
  reinterpret_cast<void (*)(id, SEL)>(g_orig_unmark)(_self, _cmd);
}

/// GLFW reports the marked range one character short, so a single-letter
/// composition answers `(0, 0)` — an empty range — while `hasMarkedText` still
/// answers YES. The IME reads that as "no composition" and hands Backspace back
/// to the client instead of dropping the letter, which is why the last pinyin
/// needed a second press. Answer both methods from the same counter.
NSRange marked_range(id, SEL) {
  if (g_marked_len == 0) {
    return NSMakeRange(NSNotFound, 0);
  }
#ifndef NDEBUG
  // TEMPORARY diagnostics: how often does the IME ask, and what does it hear?
  std::fprintf(stderr, "[ime] markedRange -> %lu\n",
               static_cast<unsigned long>(g_marked_len));
#endif
  return NSMakeRange(0, g_marked_len);
}

/// GLFW implements this as an empty method, so a command the IME delegates to
/// the client dies here. The IME should drop the letter itself; when it asks us
/// instead, drop it from the drawing so the display does not outlive it.
void do_command_by_selector(id, SEL, SEL _selector) {
#ifndef NDEBUG
  std::fprintf(stderr, "[ime] doCommandBySelector %s\n", sel_getName(_selector));
#endif
  if (_selector != @selector(deleteBackward:) || g_preedit.empty()) {
    return;
  }
  std::size_t cut = g_preedit.size();
  while (cut > 0 && (static_cast<unsigned char>(g_preedit[cut - 1]) & 0xC0U) == 0x80U) {
    --cut; // back over the continuation bytes of a multi-byte character
  }
  g_preedit.erase(cut == 0 ? 0 : cut - 1); // composing stays true: the IME owns it
}

/// GLFW hands every key to ImGui before the IME sees it, so a Backspace that is
/// only meant to trim the composition also deletes a character ImGui already
/// holds, and the Enter that commits one sends. While a composition is live the
/// keys belong to the IME, so keep ImGui out of it; `insertText:`/`unmarkText`
/// are what end the composition and hand normal handling back.
void key_down(id _self, SEL _cmd, NSEvent *_event) {
#ifndef NDEBUG
  // TEMPORARY diagnostics: is the key we suppress the one the IME still holds?
  std::fprintf(stderr,
               "[ime] keyDown composing=%d hasMarkedText=%d keyCode=%d preedit='%s'\n",
               static_cast<int>(g_composing),
               static_cast<int>([_self hasMarkedText]), [_event keyCode],
               g_preedit.c_str());
#endif
  if (g_composing) {
    [_self interpretKeyEvents:@[_event]];
    return;
  }
  reinterpret_cast<void (*)(id, SEL, id)>(g_orig_key_down)(_self, _cmd, _event);
}

// GLFW is fetched, not vendored, so this can only be a runtime substitution. If
// a future GLFW drops or renames the selector the app keeps the old behaviour
// instead of failing to start, which is why the lookup is checked rather than
// assumed. The original implementation comes back so callers keep chaining it.
IMP override_view_method(NSView *_view, const char *_name, IMP _replacement) {
  SEL selector = sel_registerName(_name);
  Method method = class_getInstanceMethod([_view class], selector);
  if (method == nullptr) {
#ifndef NDEBUG
    std::fprintf(stderr, "[ime] NOT FOUND %s\n", _name);
#endif
    return nullptr;
  }
#ifndef NDEBUG
  std::fprintf(stderr, "[ime] overriding %s\n", sel_getName(selector));
#endif
  IMP original = method_getImplementation(method);
  method_setImplementation(method, _replacement);
  return original;
}

} // namespace

/// GLFW hardcodes the rect macOS asks for to the view origin, drawers the marked
/// text nowhere, and forwards keys to ImGui ahead of the IME. Everything here is
/// a runtime substitution of the fetched GLFW's view methods, because GLFW is
/// not vendored; a renamed selector leaves the old behaviour rather than a
/// startup failure. `Platform_SetImeDataFn` is the one thing that needs a live
/// ImGui context, which the caller has by this point.
void macos_ime_attach(GLFWwindow *_window) {
  if (_window == nullptr) {
    return;
  }
  NSWindow *window = glfwGetCocoaWindow(_window);
  NSView *view = window.contentView;
  if (view == nil) {
    return;
  }

  g_text_view = view;
  override_view_method(view, "firstRectForCharacterRange:actualRange:",
                       reinterpret_cast<IMP>(first_rect_for_character_range));
  g_orig_set_marked = override_view_method(
      view, "setMarkedText:selectedRange:replacementRange:",
      reinterpret_cast<IMP>(set_marked_text));
  g_orig_unmark = override_view_method(
      view, "unmarkText", reinterpret_cast<IMP>(unmark_text));
  g_orig_insert = override_view_method(
      view, "insertText:replacementRange:", reinterpret_cast<IMP>(insert_text));
  g_orig_key_down =
      override_view_method(view, "keyDown:", reinterpret_cast<IMP>(key_down));
  override_view_method(view, "markedRange", reinterpret_cast<IMP>(marked_range));
  override_view_method(view, "doCommandBySelector:",
                       reinterpret_cast<IMP>(do_command_by_selector));
  ImGui::GetPlatformIO().Platform_SetImeDataFn = set_ime_data;
}

std::string macos_ime_preedit() { return g_preedit; }
