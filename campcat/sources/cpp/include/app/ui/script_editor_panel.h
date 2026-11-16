#pragma once

#include <filesystem>
#include <string_view>

namespace campcat {
struct workspace_selection;
}

/// Width of the script list column, which sits beside the conversation rather
/// than taking height off it.
inline constexpr float k_script_list_w = 180.0F;

/// Payload type the script list drags a row under, and the LLM prompt box takes
/// it from. ImGui matches drag payloads by this string, so both sides must use
/// this one constant, and it must not start with '_' (ImGui reserves those).
inline constexpr const char *k_script_drag_payload = "campcat_script";

/**
 * @brief Draw the script list column for the shared workspace selection.
 *
 * The graph chosen in the workspace toolbar has its anchors listed as Nodes
 * and its edges as Edges. Double-clicking a row opens that
 * script for viewing and editing; each script keeps its own editor buffer.
 *
 * @param[in] _script_dir The bundle root, the same directory the script tools
 * work in (`<config_home>`).
 * @param[in] _list_w     Column width in pixels.
 * @param[in] _list_h     Column height, negative meaning "the parent's height
 * minus this" the way ImGui reads a size.
 */
void script_editor_panel_draw(const std::filesystem::path &_script_dir,
                              float _list_w, float _list_h,
                              campcat::workspace_selection &_selection, bool _save_locked = false);

/// Description and template evidence for the selected skill, without running it.
void script_editor_inspector_draw(const std::filesystem::path &_script_dir,
                                  std::string_view _name);

/**
 * @brief Open and select a bundle's editor, as a double-click in the list does.
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
 * @brief Draw open editors as floating windows when docking is unavailable.
 *
 * Called centrally once per frame so editors remain visible on every page.
 * Each editor carries its bundle root. Switching layout keeps its buffer.
 */
void script_editor_draw_windows(bool _docked, bool _save_locked = false);

/// Open editors and pending requests to select one in the workspace sidebar.
bool script_editor_has_open();
bool script_editor_focus_requested();

/// Draw the same buffers as tabs in the workspace sidebar.
void script_editor_draw_docked(bool _save_locked = false);

/// @brief Release the preview textures and close the editors. Must run before
/// the GL context goes away.
void script_editor_panel_shutdown_gl();
