#pragma once

struct GLFWwindow; // only ever passed through here, so glfw3.h stays out

/**
 * @brief Dark appearance and a transparent title bar over `_bg_rgb`.
 *
 * The title bar, the menu bar and the content then read as one surface instead
 * of a light system strip over a dark app. The content view is not extended
 * under the title bar, so dragging and double-click zoom stay native.
 *
 * @param _window The shell window. Null, or a window without a Cocoa window,
 * is ignored.
 * @param _bg_rgb sRGB components in 0..1.
 *
 * Apple-only, like `macos_ime_attach`: the call site is guarded.
 */
void macos_window_style(GLFWwindow *_window, const float _bg_rgb[3]);
