#pragma once

#include <filesystem>
#include <string_view>

/// Width of the script list column, which sits beside the conversation rather
/// than taking height off it.
inline constexpr float k_script_list_w = 180.0F;

/// Payload type the script list drags a row under, and the LLM prompt box takes
/// it from. ImGui matches drag payloads by this string, so both sides must use
/// this one constant, and it must not start with '_' (ImGui reserves those).
inline constexpr const char *k_script_drag_payload = "campcat_script";

/**
 * @brief Draw the script list column, and every script window that is open.
 *
 * The column starts with a graph combo. Under it, that graph's anchors are
 * listed as Nodes and its edges as Edges. Double-clicking a row opens that
 * script for viewing and editing; each script gets its own window, so opening a
 * second one never costs unsaved edits.
 *
 * @param[in] _script_dir The bundle root, the same directory the script tools
 * work in (`<config_home>`).
 * @param[in] _list_w     Column width in pixels.
 * @param[in] _list_h     Column height, negative meaning "the parent's height
 * minus this" the way ImGui reads a size.
 */
void script_editor_panel_draw(const std::filesystem::path &_script_dir,
                              float _list_w, float _list_h);

/**
 * @brief Open one bundle's editor window, the same window a double-click in the
 * list opens.
 *
 * For callers that know which bundle they want without showing the list, such
 * as the Script page's path box. A bundle already open keeps its unsaved text.
 *
 * @param[in] _script_dir The bundle root, as in script_editor_panel_draw.
 * @param[in] _name       A bundle directory name under `_script_dir`.
 */
void script_editor_open(const std::filesystem::path &_script_dir,
                        std::string_view _name);

/**
 * @brief Draw every open script window, and drop the ones closed since the last
 * call.
 *
 * Split out of script_editor_panel_draw so a page that has no list -- the
 * Script page -- can still show the windows it opens. Each editor carries the
 * bundle root it was opened from, so this needs no directory of its own.
 */
void script_editor_draw_windows();

/// @brief Release the preview textures and close the editors. Must run before
/// the GL context goes away.
void script_editor_panel_shutdown_gl();
