#include "app/macos_ime.h"

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <imgui.h>

#include <cstdio>

namespace {

/// Caret rect in the content view's own coordinates, published by ImGui each
/// frame. Cocoa screens are queried on the main thread, same as the renderer.
NSRect g_caret_rect = NSMakeRect(0.0, 0.0, 0.0, 0.0);
NSView *g_text_view = nil;

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

// GLFW is fetched, not vendored, so this can only be a runtime substitution. If
// a future GLFW drops or renames the selector the app keeps the old bottom-left
// behaviour instead of failing to start, which is why the lookup is checked
// rather than assumed.
void install_first_rect_override(NSView *_view) {
  SEL selector = @selector(firstRectForCharacterRange:actualRange:);
  Method method = class_getInstanceMethod([_view class], selector);
  if (method == nullptr) {
    return;
  }
#ifndef NDEBUG
  std::fprintf(stderr, "[ime] overriding %s\n", sel_getName(selector));
#endif
  method_setImplementation(method, reinterpret_cast<IMP>(
                                       first_rect_for_character_range));
}

} // namespace

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
  install_first_rect_override(view);
  ImGui::GetPlatformIO().Platform_SetImeDataFn = set_ime_data;
}
