#include "app/ui/shell_dock.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace {

/// ~224px of the 1320px default window: the sidebar, full height.
constexpr float k_nav_ratio = 0.17F;
/// Log under the pages, where its lines get the page's width instead of being
/// folded into the sidebar's narrow column.
constexpr float k_log_ratio = 0.26F;
/// 440px tool column over the old content width (~970px).
constexpr float k_tools_ratio = 0.45F;

void build_default_layout(ImGuiID _dockspace_id, ImVec2 _size) {
    ImGui::DockBuilderAddNode(_dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(_dockspace_id, _size);

    ImGuiID nav = 0;
    ImGuiID rest = 0;
    ImGui::DockBuilderSplitNode(_dockspace_id, ImGuiDir_Left, k_nav_ratio, &nav,
                                &rest);
    ImGuiID tools = 0;
    ImGuiID work = 0;
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, k_tools_ratio, &tools,
                                &work);
    ImGuiID log = 0;
    ImGuiID center = 0;
    ImGui::DockBuilderSplitNode(work, ImGuiDir_Down, k_log_ratio, &log, &center);

    ImGui::DockBuilderDockWindow(k_dock_campcat, nav);
    ImGui::DockBuilderDockWindow(k_dock_log, log);
    ImGui::DockBuilderDockWindow(k_dock_script, center);
    ImGui::DockBuilderDockWindow(k_dock_llm, center);
    ImGui::DockBuilderDockWindow(k_dock_console, center);
    ImGui::DockBuilderDockWindow(k_dock_graph, center);
    ImGui::DockBuilderDockWindow(k_dock_settings, center);
    // Settings only. The windows are begun later (and only while they have
    // something to show); until then the right split contributes no pixels.
    ImGui::DockBuilderDockWindow(k_dock_editor, tools);
    ImGui::DockBuilderDockWindow(k_dock_details, tools);
    ImGui::DockBuilderFinish(_dockspace_id);
}

} // namespace

bool shell_dock_submit(bool _reset) {
#ifndef IMGUI_HAS_DOCK
#error "CampCat's shell needs the imgui docking branch (v1.91.8-docking)."
#endif
    const ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    // Ini loading has already run (first NewFrame). A missing node is a first
    // launch; DockSpace() itself would create an empty one, so test before it.
    const bool build = _reset || ImGui::DockBuilderGetNode(dockspace_id) == nullptr;
    if (build) {
        if (_reset) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
        }
        ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x < 1.0F || size.y < 1.0F) {
            size = ImGui::GetMainViewport()->WorkSize;
        }
        build_default_layout(dockspace_id, size);
    }
    ImGui::DockSpace(dockspace_id, ImVec2(0.0F, 0.0F));
    return build;
}

void shell_dock_hide_next_tab_bar() {
    ImGuiWindowClass window_class;
    window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&window_class);
}
