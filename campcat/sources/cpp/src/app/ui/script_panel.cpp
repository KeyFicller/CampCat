#include "app/ui/script_panel.h"

#include "app/shell_state.h"
#include "app/ui/activity_status.h"
#include "app/ui/fonts.h"
#include "app/ui/icon_texture.h"
#include "app/ui/script_editor_panel.h"
#include "app/ui/template_capture_panel.h"
#include "core/script/ccat_bundle.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

/// The pencil on Edit, the same size as the icons around it. Drawn rather
/// than designed: a barrel at 45 degrees with a cone sharpened off its lower
/// left, so the pair cannot be confused with the folder. 8x supersampled and
/// INTER_AREA'd down. White on an alpha channel is the shape `load_icon`
/// uploads.
constexpr char k_edit_path[] = "campcat/sources/cpp/assets/icons/edit.png";

/// How wide an icon is drawn inside its square button, in pixels. The same box
/// the gear and the plane use.
constexpr float k_icon_side = 20.0F;

/// A square icon button: the icon carries the action, so no label is drawn and
/// the width a label would reserve stays with the box beside it. The tooltip is
/// shown even while the button is disabled, which is the only way to say why it
/// is greyed.
bool icon_button(const char *_id, const char *_icon_path,
                 const char *_tooltip) {
    const float side = ImGui::GetFrameHeight();
    const bool clicked = ImGui::Button(_id, ImVec2(side, 0.0F));
    const bool hovered =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const GLuint tex = campcat::app::load_icon(_icon_path);
    if (tex != 0) {
        const ImVec2 mn = ImGui::GetItemRectMin();
        const float pad = (side - k_icon_side) * 0.5F;
        ImGui::GetWindowDrawList()->AddImage(
            static_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
            ImVec2(mn.x + pad, mn.y + pad),
            ImVec2(mn.x + side - pad, mn.y + side - pad), ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F), ImGui::GetColorU32(ImGuiCol_Text));
    }
    if (hovered) {
        ImGui::SetTooltip("%s", _tooltip);
    }
    return clicked;
}

} // namespace

void script_panel_draw(campcat::shell_state &_st,
                       campcat::app_config &_cfg_view,
                       const campcat::ccat_script_profile &_ccat_ui,
                       bool _lock_manual_single) {

    WITH_FONT(bold, { ImGui::SeparatorText("Bundle"); });

    // A list, not a path box: every script is a bundle now, and the list is the
    // only source of names the editor can save back to.
    const std::vector<std::string> bundles =
        campcat::ccat_lang::bundle_names(_cfg_view.config_home);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float icon_w = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - icon_w - gap);
    if (ImGui::BeginCombo("##script_bundle",
                          _cfg_view.script_bundle.empty()
                              ? "(no bundle)"
                              : _cfg_view.script_bundle.c_str())) {
        for (const std::string &name : bundles) {
            if (ImGui::Selectable(name.c_str(),
                                  name == _cfg_view.script_bundle)) {
                // Two writes on purpose: the view is what Save persists, the
                // setter is what this frame's buttons and the automation tick
                // read.
                _cfg_view.script_bundle = name;
                _st.set_script_bundle(name);
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
        icon_button("##script_bundle_edit", k_edit_path,
                    have_bundle ? "Edit the script"
                                : "Editing needs a bundle: <root>/<name>/"
                                  "main.ccat");
    if (!have_bundle) {
        ImGui::EndDisabled();
    }
    if (edit_clicked) {
        script_editor_open(_cfg_view.config_home, _cfg_view.script_bundle);
    }

    WITH_FONT(bold, { ImGui::SeparatorText("Action"); })

    // Both start buttons need something to run and nobody else on the device.
    const bool start_locked =
        _lock_manual_single || !_ccat_ui.has_script_source();

    if (!_ccat_ui.has_script_source()) {
        ImGui::TextColored(
            campcat::activity_color(campcat::activity_level::waiting),
            "Pick a bundle before running.");
        ImGui::Spacing();
    }

    if (start_locked) {
        ImGui::BeginDisabled();
    }
    WITH_FONT(bold, {
        if (ImGui::Button("Run Once")) {
            _st.push_job_snapshot(&_cfg_view);
            _st.run_single_job();
        }
        if (start_locked) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        // The one button live while the other two grey out. An idle Stop only
        // writes a "[ui] stop cycle" that stopped nothing.
        if (!_lock_manual_single) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Stop")) {
            _st.stop_requested.store(true);
            _st.sched.stop();
            _st.append_log("[ui] stop cycle");
        }
        if (!_lock_manual_single) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        // Shares Run Once's gate: re-arming while armed fires an extra
        // immediate cycle and restarts the countdown, and arming mid-cycle
        // spends its first tick on the busy skip.
        if (start_locked) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Run Scheduled")) {
            // Arm from what the sliders currently hold: push first, because
            // refresh reads the config back out of shell_state.
            _st.push_job_snapshot(&_cfg_view);
            _st.refresh_scheduler_locked(true);
        }
        if (start_locked) {
            ImGui::EndDisabled();
        }
    });

    // Outside the header: this is the at-a-glance line, and it should stay
    // readable with the controls folded away.
    if (_st.sched.running()) {
        const auto wp = _st.sched.tick_wait_progress();
        if (wp.in_wait_phase && wp.duration_seconds > 0) {
            const double frac =
                std::clamp(wp.elapsed_seconds / wp.duration_seconds, 0.0, 1.0);
            ImGui::ProgressBar(static_cast<float>(frac), ImVec2(-1.0F, 0.0F));
            // The wait left, not the wait spent: `duration_seconds` is this
            // tick's interval plus its jitter, and what the user wants to know
            // is when the next cycle fires.
            const int left = static_cast<int>(
                std::max(0.0, wp.duration_seconds - wp.elapsed_seconds));
            ImGui::Text("Next run in %d:%02d", left / 60, left % 60);
        } else {
            ImGui::TextUnformatted("Running a cycle...");
        }
    }

    ImGui::Spacing();

    // The scheduler's controls used to live on the Settings page; they belong
    // next to the status and the arm button they drive.
    bool sched_open = false;
    WITH_FONT(bold, {
        ImGui::Indent(2.0f);
        sched_open = ImGui::CollapsingHeader("Scheduler###cfg_sched",
                                             ImGuiTreeNodeFlags_DefaultOpen);
    });
    if (sched_open) {
        // Both cadences on one row, each slider's footprint half the row with
        // its label counted in, so the pair lands on the same line.
        const ImGuiStyle &style = ImGui::GetStyle();
        const float half =
            (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5F;
        // Clamped, because ImGui reads a negative width as "fill the row": in a
        // window narrow enough to go negative the two would overlap instead of
        // shrinking.
        const auto slider_width = [&](const char *_label) {
            return std::max(60.0F, half - ImGui::CalcTextSize(_label).x -
                                       style.ItemInnerSpacing.x);
        };
        ImGui::SetNextItemWidth(slider_width("Interval (s)"));
        ImGui::SliderInt("Interval (s)", &_cfg_view.scheduler.interval_seconds,
                         300, 6 * 3600);
        bool reschedule = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(slider_width("Jitter (s)"));
        ImGui::SliderInt("Jitter (s)", &_cfg_view.scheduler.jitter_seconds, 0,
                         900);
        reschedule = ImGui::IsItemDeactivatedAfterEdit() || reschedule;
        // Only a running scheduler is re-armed: otherwise dragging a slider
        // would arm one the user has not armed on purpose.
        if (reschedule && _st.sched.running()) {
            _st.push_job_snapshot(&_cfg_view);
            _st.refresh_scheduler_locked();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Spacing();
    // Only the header is in bold: the panel it opens was outside the pair too.
    bool cap_open = false;
    WITH_FONT(bold, {
        cap_open = ImGui::CollapsingHeader(
            "Template Maker (Crop Screenshot)###script_ccat_cap",
            ImGuiTreeNodeFlags_DefaultOpen);
    });
    if (cap_open) {
        template_capture_panel_draw(_cfg_view, &_ccat_ui, _lock_manual_single);
    }
    // Also on the pages that have no script list: a window opened here is a
    // window the user expects to see, whichever page they are on.
    script_editor_draw_windows();
    ImGui::Unindent();
}
