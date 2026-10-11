#pragma once

struct ImFont; // only ever held as a pointer here, so imgui.h stays out

namespace campcat::app {

/// The faces the UI draws with.
enum class font_role { body, bold, title, mono };

/**
 * @brief Build the four faces into ImGui's font atlas.
 *
 * Call once, after the ImGui context exists and **before the graphics backend
 * is initialized**: `ImGui_ImplOpenGL3_Init` bakes the atlas into a texture on
 * the spot, so a face added after it never reaches the screen.
 *
 * The font files are probed, so a missing one degrades instead of asserting:
 * `body` falls back to ImGui's built-in font, `bold` to `body`, `title` to
 * `bold`, and `mono` to `body`.
 */
void fonts_init();

/// @brief The face for a role. Never null.
ImFont *fonts_get(font_role _role);

} // namespace campcat::app
