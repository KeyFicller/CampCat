#pragma once

struct GLFWwindow; // only ever passed through here, so glfw3.h stays out

/**
 * @brief Point macOS IME candidate windows at the text caret.
 *
 * GLFW hardcodes the rect macOS asks for to the content view origin (the window
 * bottom-left corner) and exposes no API to set it; ImGui's GLFW backend never
 * registers `Platform_SetImeDataFn` either, so nothing reports the caret. This
 * replaces `firstRectForCharacterRange:actualRange:` on the window's content
 * view class and answers with the caret position ImGui publishes each frame.
 *
 * @param _window Window whose view receives text input. Null, a window without
 * a content view, or a GLFW without that selector is ignored.
 *
 * Apple-only: the definition is compiled only on Apple, so the call site is
 * guarded. The declaration stays unconditional to keep the guard in one place.
 */
void macos_ime_attach(GLFWwindow *_window);
