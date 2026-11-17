#include "app/ui/script_panel.h"

#include "app/shell_state.h"
#include "app/ui/buttons.h"
#include "app/ui/icons.h"
#include "app/ui/script_editor_panel.h"
#include "app/ui/template_capture_panel.h"
#include "app/ui/theme.h"
#include "app/ui/widgets.h"
#include "core/script/ccat_bundle.h"
#include "core/script/ccat_graph.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <string>

void script_panel_draw(campcat::shell_state &_st,
                       campcat::app_config &_cfg_view,
                       const campcat::ccat_script_profile &_ccat_ui,
                       bool _lock_manual_single) {
    using namespace campcat::app;
    const float w = content_width(theme::k_script_max_w);

    if (card_begin("script_target", "Run target",
                   {.hint = "Choose the script for the next run. Browsing above "
                            "does not change this target.",
                    .width = w})) {
        // A list, not a path box: every script is a bundle now, and the list
        // is the only source of names the editor can save back to.
        const std::vector<std::string> bundles =
            campcat::ccat_lang::bundle_names(_cfg_view.config_home);
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float icon_w = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - icon_w -
                                gap);
        if (ImGui::BeginCombo("##script_bundle",
                              _cfg_view.script_bundle.empty()
                                  ? "(no bundle)"
                                  : _cfg_view.script_bundle.c_str())) {
            for (const std::string &name : bundles) {
                if (ImGui::Selectable(name.c_str(),
                                      name == _cfg_view.script_bundle)) {
                    // Two writes on purpose: the view is what Save persists,
                    // the setter is what this frame's buttons and the
                    // automation tick read.
                    _cfg_view.script_bundle = name;
                    _st.set_script_bundle(name);
                    const auto tag = campcat::ccat_lang::bundle_tags(
                        _cfg_view.config_home, name);
                    _st.workspace.select_bundle(
                        !tag.anchor_of.empty() ? tag.anchor_of
                        : !tag.edge_of.empty() ? tag.edge_of : tag.action_of,
                        name);
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        const bool have_bundle = !_cfg_view.script_bundle.empty();
        if (!have_bundle) {
            ImGui::BeginDisabled();
        }
        const bool edit_clicked =
            icon_button("##script_bundle_edit", icon::k_edit,
                        have_bundle ? "Edit the script"
                                    : "Editing needs a bundle: <root>/<name>/"
                                      "main.ccat");
        if (!have_bundle) {
            ImGui::EndDisabled();
        }
        if (edit_clicked) {
            script_editor_open(_cfg_view.config_home,
                               _cfg_view.script_bundle);
        }
    }
    card_end();

    card_begin("script_run", "Run", {.collapsible = false, .width = w});
    {
        // Both start buttons need something to run and nobody else on the
        // device.
        const bool start_locked =
            _lock_manual_single || !_ccat_ui.has_script_source();
        // The one button live while the other two grey out. An idle Stop
        // only writes a "[ui] stop cycle" that stopped nothing.
        const bool can_stop_cycle = _st.cycle_running.load() || _st.sched.running();
        const std::string stop_label = std::string(icon::k_stop) + "  Stop";
        auto stop_button = [&]() {
            ImGui::BeginDisabled(!can_stop_cycle);
            if (button(stop_label.c_str(), ImVec2(), button_kind::danger)) {
                _st.stop_requested.store(true);
                _st.sched.stop();
                _st.append_log("[ui] stop cycle");
            }
            ImGui::EndDisabled();
        };

        if (!_ccat_ui.has_script_source()) {
            ImGui::TextColored(theme::k_text_2, "%s", icon::k_script);
            ImGui::SameLine();
            WITH_FONT(bold, { ImGui::TextUnformatted("No bundle selected"); });
            ImGui::PushStyleColor(ImGuiCol_Text, theme::k_text_2);
            ImGui::TextWrapped("Choose a Run target above to run or edit it.");
            ImGui::PopStyleColor();
            // A scheduler armed before its bundle vanished still needs a way
            // out, so Stop stays while there is something to stop.
            if (can_stop_cycle) {
                stop_button();
            }
        } else {
            ImGui::BeginDisabled(start_locked);
            if (button((std::string(icon::k_play) + "  Run once").c_str(), ImVec2(),
                       button_kind::primary)) {
                _st.push_job_snapshot(&_cfg_view);
                _st.run_single_job();
            }
            ImGui::SameLine();
            // Shares Run once's gate: re-arming while armed fires an extra
            // immediate cycle and restarts the countdown, and arming mid-cycle
            // spends its first tick on the busy skip.
            if (button((std::string(icon::k_clock) + "  Run scheduled").c_str())) {
                // Arm from what the sliders currently hold: push first, because
                // refresh reads the config back out of shell_state.
                _st.push_job_snapshot(&_cfg_view);
                _st.refresh_scheduler_locked(true);
            }
            ImGui::EndDisabled();
            // Right-aligned, apart from the two that start something; a row too
            // narrow for all three puts it on the next line instead.
            const float right = ImGui::GetWindowContentRegionMax().x;
            const float stop_x = right - button_width(stop_label.c_str());
            if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x +
                    ImGui::GetStyle().ItemSpacing.x <=
                stop_x) {
                ImGui::SameLine(stop_x);
            }
            stop_button();
        }

        if (_st.sched.running()) {
            const auto wp = _st.sched.tick_wait_progress();
            char line[48];
            float frac = -1.0F;
            if (wp.in_wait_phase && wp.duration_seconds > 0) {
                frac = static_cast<float>(
                    std::clamp(wp.elapsed_seconds / wp.duration_seconds, 0.0, 1.0));
                // The wait left, not the wait spent: `duration_seconds` is this
                // tick's interval plus its jitter, and what the user wants to
                // know is when the next cycle fires.
                const int left = static_cast<int>(
                    std::max(0.0, wp.duration_seconds - wp.elapsed_seconds));
                std::snprintf(line, sizeof(line), "Next run in %d:%02d", left / 60,
                              left % 60);
            } else {
                std::snprintf(line, sizeof(line), "Running a cycle...");
            }
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                          ImGui::GetWindowContentRegionMax().x -
                                              ImGui::CalcTextSize(line).x));
            ImGui::TextColored(theme::k_text_2, "%s", line);
            if (frac >= 0.0F) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0F);
                ImGui::ProgressBar(frac, ImVec2(-1.0F, 6.0F), "");
                ImGui::PopStyleVar();
            }
        }
    }
    card_end();

    // The scheduler's controls used to live on the Settings page; they belong
    // next to the status and the arm button they drive.
    if (card_begin("cfg_sched", "Scheduler", {.width = w})) {
        form_label("Interval");
        ImGui::SliderInt("##interval", &_cfg_view.scheduler.interval_seconds,
                         300, 6 * 3600, "%d s");
        bool reschedule = ImGui::IsItemDeactivatedAfterEdit();
        form_label("Jitter");
        ImGui::SliderInt("##jitter", &_cfg_view.scheduler.jitter_seconds, 0,
                         900, "%d s");
        reschedule = ImGui::IsItemDeactivatedAfterEdit() || reschedule;
        // Only a running scheduler is re-armed: otherwise dragging a slider
        // would arm one the user has not armed on purpose.
        if (reschedule && _st.sched.running()) {
            _st.push_job_snapshot(&_cfg_view);
            _st.refresh_scheduler_locked();
        }
    }
    card_end();

    if (card_begin("script_ccat_cap", "Template Maker",
                   {.hint = "Crop a template from a device screenshot", .width = w})) {
        template_capture_panel_draw(_cfg_view, &_ccat_ui, _lock_manual_single,
                                    _st.device_tasks.busy() || _st.sched.running());
    }
    card_end();
}
