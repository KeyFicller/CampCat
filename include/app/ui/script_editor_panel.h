#pragma once

#include <filesystem>

/// Width of the script list column, which sits beside the conversation rather
/// than taking height off it.
inline constexpr float k_script_list_w = 180.0F;

/**
 * @brief Draw the script list column, and every script window that is open.
 *
 * The caller has already moved the cursor to the column's top-left corner, so
 * it lands where it is placed in the row. Double-clicking a row opens that
 * script for viewing and editing; each script gets its own window, so opening a
 * second one never costs unsaved edits.
 *
 * @param[in] _script_dir The bundle root, the same directory the script tools
 * work in (`<config_home>/scripts/llm`).
 * @param[in] _list_w     Column width in pixels.
 * @param[in] _list_h     Column height, negative meaning "the parent's height
 * minus this" the way ImGui reads a size.
 */
void script_editor_panel_draw(const std::filesystem::path &_script_dir,
                              float _list_w, float _list_h);

/// @brief Release the preview textures and close the editors. Must run before
/// the GL context goes away.
void script_editor_panel_shutdown_gl();
