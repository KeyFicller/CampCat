#include "app/ui/settings_panel.h"

#include "app/shell_state.h"
#include "core/mumu_launcher.h"

#include <imgui.h>

#include <mutex>
#include <string>

namespace {

const char *mumu_state_label(campcat::mumu_state _state) {
  switch (_state) {
  case campcat::mumu_state::stopped:
    return "stopped";
  case campcat::mumu_state::starting:
    return "starting";
  case campcat::mumu_state::running:
    return "running";
  case campcat::mumu_state::service_down:
    return "service down";
  case campcat::mumu_state::failed:
    return "failed";
  case campcat::mumu_state::unknown:
    break;
  }
  return "unknown";
}

} // namespace

void settings_panel_draw(campcat::shell_state &_st,
                        campcat::app_config &_cfg_view) {
  static double min_th = 0.35;
  static double max_th = 1.0;

  ImGui::TextDisabled("%s", _st.cfg_path.string().c_str());
  ImGui::Spacing();

  if (ImGui::CollapsingHeader("Timing & match###cfg_timing",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SliderInt("Tap delay (ms)", &_cfg_view.input.tap_delay_ms, 40, 800);
    ImGui::SliderInt("Action gap (ms)", &_cfg_view.input.action_gap_ms, 1000,
                     5000);
    ImGui::SliderInt("Swipe duration (ms)", &_cfg_view.input.swipe_duration_ms,
                     50, 2000);
    ImGui::SliderScalar("Match threshold", ImGuiDataType_Double,
                        &_cfg_view.match.threshold, &min_th, &max_th, "%.3f");
    ImGui::Checkbox("Multi-scale match", &_cfg_view.match.multiscale);
  }

  ImGui::Spacing();
  if (ImGui::CollapsingHeader("ADB###cfg_adb",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::InputText("ADB path", _st.adb_path_buf,
                     IM_ARRAYSIZE(_st.adb_path_buf));
    ImGui::InputText("Device serial", _st.serial_buf,
                     IM_ARRAYSIZE(_st.serial_buf));
    ImGui::InputText("Connect address", _st.adb_connect_buf,
                     IM_ARRAYSIZE(_st.adb_connect_buf));
    ImGui::InputText("MuMu tool", _st.mumutool_buf,
                     IM_ARRAYSIZE(_st.mumutool_buf));
    ImGui::InputInt("Instance index", &_st.mumu_index_buf);
    // The button starts a VM, which takes a while; the wait runs on the
    // worker so the frame keeps drawing while it happens.
    const bool launching = _st.mumu_launching.load();
    if (launching) {
      ImGui::BeginDisabled();
    }
    const bool start_clicked = ImGui::Button("Start emulator");
    if (launching) {
      ImGui::EndDisabled();
    }
    if (start_clicked) {
      _st.start_mumu_job();
    }
    ImGui::SameLine();
    {
      std::lock_guard<std::mutex> lk(_st.mumu_mu);
      std::string status_line = mumu_state_label(_st.mumu_report.state);
      if (!_st.mumu_report.message.empty() &&
          _st.mumu_report.message != status_line) {
        status_line += " - " + _st.mumu_report.message;
      }
      ImGui::TextDisabled("%s", status_line.c_str());
    }
  }

  ImGui::Spacing();
  if (ImGui::CollapsingHeader("Scheduler###cfg_sched",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Checkbox("Enable scheduler", &_cfg_view.scheduler.enabled);
    ImGui::SliderInt("Interval (s)", &_cfg_view.scheduler.interval_seconds, 300,
                     6 * 3600);
    ImGui::SliderInt("Jitter (s)", &_cfg_view.scheduler.jitter_seconds, 0, 900);
    ImGui::Checkbox("Skip if busy", &_cfg_view.scheduler.skip_if_busy);
    ImGui::TextDisabled(
        "Arm the scheduler from the Script page with Start schedule.");
  }
}
