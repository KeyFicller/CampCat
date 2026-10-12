#pragma once

#include "app/ui/gl_texture.h"

namespace campcat::app {

/**
 * @brief Upload the white icon stored at `_path`, once per path.
 *
 * The path is the key, which is what a caller inside the draw loop needs: two
 * calls for one path upload once, and a path whose file was missing is not
 * re-read on every later frame. The result is meant to be tinted with
 * `GetColorU32(ImGuiCol_Text)` at the draw call, so one white glyph follows
 * whatever colour the theme puts on the button.
 *
 * @return 0 when the file is missing or does not decode to BGRA; the caller
 * draws a bare frame rather than crashing.
 */
GLuint load_icon(const char *_path);

/// @brief Delete every texture `load_icon` handed out, so a panel that owns
/// none of them still gets them released with the context.
void release_icons();

} // namespace campcat::app
