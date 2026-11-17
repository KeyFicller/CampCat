#include "app/ui/macos_window.h"

#import <Cocoa/Cocoa.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

void macos_window_style(GLFWwindow *_window, const float _bg_rgb[3]) {
  if (_window == nullptr) {
    return;
  }
  NSWindow *window = glfwGetCocoaWindow(_window);
  if (window == nil) {
    return;
  }
  // Forced dark: the app has no light theme, and under a light system
  // appearance the title text would draw dark on the dark bar.
  window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  window.titlebarAppearsTransparent = YES;
  window.backgroundColor = [NSColor colorWithSRGBRed:_bg_rgb[0]
                                               green:_bg_rgb[1]
                                                blue:_bg_rgb[2]
                                               alpha:1.0];
}
