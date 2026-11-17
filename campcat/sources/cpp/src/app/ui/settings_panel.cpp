#include "app/ui/settings_panel.h"

#include "app/shell_state.h"
#include "app/ui/fonts.h"
#include "app/ui/icons.h"
#include "app/ui/theme.h"
#include "app/ui/widgets.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

void settings_panel_draw(campcat::shell_state &_st,
                         campcat::app_config &_cfg_view) {
    using namespace campcat::app;
    static double min_th = 0.35;
    static double max_th = 1.0;
    const float w = content_width(theme::k_settings_max_w);
    const auto ring = [] {
        focus_ring(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                   ImGui::GetStyle().FrameRounding, ImGui::IsItemActive());
    };

    WITH_FONT(small, {
        ImGui::TextColored(theme::k_text_3, "%s  %s", icon::k_file,
                           _st.cfg_path.string().c_str());
    });
    ImGui::Spacing();

    if (card_begin("cfg_input", "Input", {.width = w})) {
        form_label("Tap delay");
        ImGui::SliderInt("##tap_delay", &_cfg_view.input.tap_delay_ms, 40, 800,
                         "%d ms");
        form_label("Action gap");
        ImGui::SliderInt("##action_gap", &_cfg_view.input.action_gap_ms, 1000,
                         5000, "%d ms");
        form_label("Swipe duration");
        ImGui::SliderInt("##swipe_duration", &_cfg_view.input.swipe_duration_ms,
                         50, 2000, "%d ms");
    }
    card_end();

    if (card_begin("cfg_match", "Template matching", {.width = w})) {
        form_label("Match threshold");
        ImGui::SliderScalar("##match_threshold", ImGuiDataType_Double,
                            &_cfg_view.match.box_threshold, &min_th, &max_th,
                            "%.3f");
        // Its own slider: a disc carries less texture than the square around
        // it, so it needs a higher bar, and the two are worth setting
        // separately -- this is where the circle bar gets calibrated.
        form_label("Circle threshold");
        ImGui::SliderScalar("##circle_threshold", ImGuiDataType_Double,
                            &_cfg_view.match.circle_threshold, &min_th, &max_th,
                            "%.3f");
        form_label("Multi-scale match");
        ImGui::Checkbox("##multiscale", &_cfg_view.match.multiscale);
    }
    card_end();

    if (card_begin("cfg_adb", "ADB", {.width = w})) {
        form_label("ADB path");
        ImGui::InputText("##adb_path", &_st.adb_path_buf);
        ring();
        form_label("Device serial");
        ImGui::InputText("##serial", &_st.serial_buf);
        ring();
        form_label("MuMu tool");
        ImGui::InputText("##mumutool", &_st.mumutool_buf);
        ring();
        form_label("Instance index");
        ImGui::InputInt("##mumu_index", &_st.mumu_index_buf);
        ring();
    }
    card_end();
}
