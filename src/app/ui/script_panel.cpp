#include "app/ui/script_panel.h"

#include "app/shell_state.h"
#include "app/ui/fonts.h"
#include "app/ui/native_file_dialog.h"
#include "app/ui/template_capture_panel.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

void script_panel_draw(campcat::shell_state &_st,
                       campcat::app_config &_cfg_view,
                       campcat::ccat_script_profile &_ccat_ui,
                       bool _lock_manual_single) {

    ImGui::PushFont(campcat::app::fonts_get(campcat::app::font_role::bold));

    ImGui::SeparatorText("Path");

    ImGui::PopFont();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 88.0F);
    ImGui::InputText("##script_path_ccat", &_ccat_ui.source_rel);
    ImGui::SameLine();
    if (ImGui::Button("Browse##script_path_ccat", ImVec2(80.0F, 0.0F))) {
        std::filesystem::path start =
            std::filesystem::path(_cfg_view.config_home) / "scripts";
        if (_ccat_ui.has_script_source()) {
            const auto resolved = std::filesystem::path(_cfg_view.config_home) /
                                  _ccat_ui.source_rel;
            const auto parent = resolved.parent_path();
            if (!parent.empty()) {
                start = parent;
            }
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(start, ec)) {
            start = _cfg_view.config_home;
        }
        auto picked =
            campcat::pick_open_file(start, "ccat", "Select CampCat script");
        if (picked) {
            std::error_code rel_ec;
            const auto home_canon = std::filesystem::weakly_canonical(
                _cfg_view.config_home, rel_ec);
            const auto file_canon =
                std::filesystem::weakly_canonical(*picked, rel_ec);
            const auto rel =
                std::filesystem::relative(file_canon, home_canon, rel_ec);
            if (rel_ec || rel.empty() ||
                (!rel.begin()->empty() && *rel.begin() == "..")) {
                _st.append_log(
                    "[ui] pick a .ccat file under the config directory: " +
                    _cfg_view.config_home);
            } else {
                _ccat_ui.source_rel = rel.generic_string();
                _st.append_log("[ui] script path: " + _ccat_ui.source_rel);
            }
        }
    }

    ImGui::TextWrapped(
        "Run a single automation cycle, stop the current cycle, or arm "
        "the scheduler.");
    ImGui::Spacing();

    if (_lock_manual_single) {
        ImGui::BeginDisabled();
    }
    ImGui::PushFont(campcat::app::fonts_get(campcat::app::font_role::bold));
    if (ImGui::Button("Run once")) {
        _st.push_job_snapshot(&_cfg_view, _ccat_ui);
        _st.run_single_job();
    }
    if (_lock_manual_single) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        _st.stop_requested.store(true);
        _st.sched.stop();
        _st.append_log("[ui] stop cycle");
    }

    ImGui::SameLine();
    if (ImGui::Button("Start schedule")) {
        _cfg_view.scheduler.enabled = true;
        _st.push_job_snapshot(&_cfg_view, _ccat_ui);
        std::string err;
        bool ok_writ = _st.persist(&err);
        if (ok_writ) {
            _st.append_log("[ui] autosaved scheduler flag to " +
                           _st.cfg_path.string());
        } else {
            _st.append_log("[ui] autosave warning: " + err);
        }
        _st.refresh_scheduler_locked(true);
    }
    ImGui::PopFont();

    ImGui::SeparatorText("Scheduler");
    if (!_cfg_view.scheduler.enabled) {
        ImGui::TextDisabled(
            "Scheduler is off. Use Start schedule to enable and arm it.");
    } else if (!_st.sched.running()) {
        ImGui::TextDisabled(
            "Scheduler is enabled but not armed. Use Start schedule.");
    } else {
        const auto wp = _st.sched.tick_wait_progress();
        if (wp.in_wait_phase && wp.duration_seconds > 0) {
            const double frac =
                std::clamp(wp.elapsed_seconds / wp.duration_seconds, 0.0, 1.0);
            ImGui::ProgressBar(static_cast<float>(frac), ImVec2(-1.0F, 0.0F));
            ImGui::Text("Waiting - %.1f s / %.1f s", wp.elapsed_seconds,
                        wp.duration_seconds);
        } else {
            ImGui::TextUnformatted("Running a cycle...");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Spacing();
    ImGui::PushFont(campcat::app::fonts_get(campcat::app::font_role::bold));
    const bool cap_open =
        ImGui::CollapsingHeader("Screenshot crop (ADB)###script_ccat_cap",
                                ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopFont();
    if (cap_open) {
        template_capture_panel_draw(_cfg_view, &_ccat_ui, _lock_manual_single);
    }
    ImGui::Unindent();
}
