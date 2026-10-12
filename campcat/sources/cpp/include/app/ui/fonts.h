#pragma once

struct ImFont; // only ever held as a pointer here, so imgui.h stays out

namespace campcat::app {

/// The faces the UI draws with.
enum class font_role { body, bold, title, mono, small, chat };

/**
 * @brief Build the six faces into ImGui's font atlas.
 *
 * Call once, after the ImGui context exists and **before the graphics backend
 * is initialized**: `ImGui_ImplOpenGL3_Init` bakes the atlas into a texture on
 * the spot, so a face added after it never reaches the screen.
 *
 * The font files are probed, so a missing one degrades instead of asserting:
 * `body` falls back to ImGui's built-in font, `bold` to `body`, `title` to
 * `bold`, `mono` to `body`, `small` to `body`, and `chat` to `body`.
 */
void fonts_init();

/// @brief The face for a role. Never null.
ImFont *fonts_get(font_role _role);

} // namespace campcat::app

/// Draw `code` in `role`. `code` is a braced block:
///
///     WITH_FONT(bold, {
///         ImGui::SeparatorText("Path");
///     });
///
/// A `for` rather than an `if`, so a following `else` cannot bind to it. The
/// pair is expanded here rather than wrapped in a guard type because pushing a
/// font is an ImGui call and this header stays free of imgui.h; the `for` also
/// keeps the body a statement, so `continue` and `break` mean what they look
/// like.
///
/// A `return` inside the block would skip the pop. That is not silent: ImGui
/// checks the stack at the window boundary and reports `Missing PopFont()` --
/// an assert under this Debug build, an error and an auto-pop under NDEBUG.
#define WITH_FONT(role, ...)                                                   \
    for (bool _font_once = (::ImGui::PushFont(::campcat::app::fonts_get(       \
                                ::campcat::app::font_role::role)),             \
                            true);                                             \
         _font_once; _font_once = false, ::ImGui::PopFont())                   \
    __VA_ARGS__
