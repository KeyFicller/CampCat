#include "app/ui/settings_panel.h"

#include "app/shell_state.h"
#include "app/ui/fonts.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

void settings_panel_draw(campcat::shell_state &_st,
                         campcat::app_config &_cfg_view) {
    static double min_th = 0.35;
    static double max_th = 1.0;

    ImGui::TextDisabled("%s", _st.cfg_path.string().c_str());
    ImGui::Spacing();

    bool input_open = false;
    WITH_FONT(bold, {
        input_open = ImGui::CollapsingHeader("Input###cfg_input",
                                             ImGuiTreeNodeFlags_DefaultOpen);
    });
    if (input_open) {
        ImGui::SliderInt("Tap delay (ms)", &_cfg_view.input.tap_delay_ms, 40,
                         800);
        ImGui::SliderInt("Action gap (ms)", &_cfg_view.input.action_gap_ms,
                         1000, 5000);
        ImGui::SliderInt("Swipe duration (ms)",
                         &_cfg_view.input.swipe_duration_ms, 50, 2000);
    }

    ImGui::Spacing();
    bool match_open = false;
    WITH_FONT(bold, {
        match_open = ImGui::CollapsingHeader("Template matching###cfg_match",
                                             ImGuiTreeNodeFlags_DefaultOpen);
    });
    if (match_open) {
        ImGui::SliderScalar("Match threshold", ImGuiDataType_Double,
                            &_cfg_view.match.box_threshold, &min_th, &max_th,
                            "%.3f");
        // Its own slider: a disc carries less texture than the square around
        // it, so it needs a higher bar, and the two are worth setting
        // separately -- this is where the circle bar gets calibrated.
        ImGui::SliderScalar("Circle threshold", ImGuiDataType_Double,
                            &_cfg_view.match.circle_threshold, &min_th, &max_th,
                            "%.3f");
        ImGui::Checkbox("Multi-scale match", &_cfg_view.match.multiscale);
    }

    ImGui::Spacing();
    bool adb_open = false;
    WITH_FONT(bold, {
        adb_open = ImGui::CollapsingHeader("ADB###cfg_adb",
                                           ImGuiTreeNodeFlags_DefaultOpen);
    });
    if (adb_open) {
        ImGui::InputText("ADB path", &_st.adb_path_buf);
        ImGui::InputText("Device serial", &_st.serial_buf);
        ImGui::InputText("MuMu tool", &_st.mumutool_buf);
        ImGui::InputInt("Instance index", &_st.mumu_index_buf);
    }
}
