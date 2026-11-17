#pragma once

#include <imgui.h>

/// Dockable shell windows. `Begin` titles must stay equal to these: the default
/// layout and File > Reset layout dock by this string.
inline constexpr const char k_dock_campcat[] = "CampCat";
inline constexpr const char k_dock_log[] = "Log";
inline constexpr const char k_dock_script[] = "Script";
inline constexpr const char k_dock_llm[] = "LLM";
inline constexpr const char k_dock_console[] = "Console";
inline constexpr const char k_dock_graph[] = "Graph";
inline constexpr const char k_dock_settings[] = "Settings";
inline constexpr const char k_dock_details[] = "Details";
inline constexpr const char k_dock_editor[] = "Editor";

/// Pages, tools, and the side panels share this. No close button is a separate
/// choice (`p_open == nullptr`); these flags only stop a docked tab from
/// collapsing or stealing focus on the frame it first appears.
inline constexpr ImGuiWindowFlags k_dock_panel_flags =
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing;

/**
 * @brief Submit the shell DockSpace, building the default splits when needed.
 *
 * Call from inside the host window, after its menu bar. The first launch and a
 * reset dock CampCat full height on the left, the five pages as center tabs
 * with Log under them, and Details plus Editor on the right. The right node
 * stays hidden until one of those windows is actually begun, so a fresh
 * session is the side column plus the page and not an empty tool well.
 *
 * @param[in] _reset Discard the saved splits and dock the named windows again.
 * @return true when the default layout was applied this frame.
 */
bool shell_dock_submit(bool _reset);

/**
 * @brief Make the next `Begin` hide its dock node's tab bar.
 *
 * Applied through the window class every frame, so it also takes effect in a
 * layout saved before it existed.
 */
void shell_dock_hide_next_tab_bar();
