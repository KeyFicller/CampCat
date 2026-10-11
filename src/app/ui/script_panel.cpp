#include "app/ui/script_panel.h"

#include "app/ui/native_file_dialog.h"
#include "app/shell_state.h"
#include "app/ui/template_capture_panel.h"
#include "core/automation/automation_driver.h"

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace shell_sid = campcat::shell_script_id;

std::string script_display_label(const std::string &_id) {
  if (_id == shell_sid::k_none) {
    return "(none)";
  }
  if (_id == shell_sid::k_ccat_script) {
    return "CampCat script (.ccat)";
  }
  return _id;
}

void append_prefixed_multiline_json(
    const nlohmann::json &_json, const std::string &_prefix,
    const std::function<void(const std::string &)> &_emit) {
  std::ostringstream oss;
  oss << std::setw(2) << _json << '\n';
  std::istringstream iss(oss.str());
  std::string line;
  while (std::getline(iss, line)) {
    _emit(_prefix + line);
  }
}

void log_active_script_bundle(campcat::shell_state &_st,
                              const campcat::app_config &_view_shell) {
  const std::string &sid = _view_shell.active_script_id;
  if (sid.empty() || sid == shell_sid::k_none) {
    _st.append_log("[script cfg] (no script selected) automation disabled");
    return;
  }
  if (sid == shell_sid::k_ccat_script) {
    const campcat::ccat_script_profile body = _st.snapshot_ccat_live();
    append_prefixed_multiline_json(
        body.to_summary_json_for_log(), "[script cfg] ",
        [&_st](const std::string &_line) { _st.append_log(_line); });
    return;
  }
  _st.append_log("[script cfg] unknown script `" + sid + "` (no profile block)");
}

void maybe_handle_script_combo_change(campcat::shell_state &_st,
                                      campcat::app_config *_view,
                                      campcat::ccat_script_profile *_ccat_ui) {
  if (_view->active_script_id == _st.last_script_seen) {
    return;
  }
  _st.last_script_seen = _view->active_script_id;

  if (_view->active_script_id == shell_sid::k_ccat_script) {
    const auto pj =
        _view->resolve_script_json(std::string{shell_sid::k_ccat_script});
    _st.set_ccat_live(pj.empty() ? campcat::ccat_script_profile::defaults()
                                 : campcat::ccat_script_profile::
                                       load_from_bundle_path(pj,
                                                             _view->config_home));
    *_ccat_ui = _st.snapshot_ccat_live();
  }

  _st.append_log("[script] active_script='" + _st.last_script_seen + "'");
  log_active_script_bundle(_st, *_view);
}

} // namespace

void script_panel_draw(campcat::shell_state &_st, campcat::app_config &_cfg_view,
                      campcat::ccat_script_profile &_ccat_ui,
                      bool _lock_manual_single, ImFont *_font_bold) {
  ImGui::TextWrapped(
      "Run a single automation cycle, stop the current cycle, or arm "
      "the scheduler.");
  ImGui::Spacing();

  if (_lock_manual_single) {
    ImGui::BeginDisabled();
  }
  ImGui::PushFont(_font_bold);
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

  std::vector<std::string> script_ids;
  script_ids.reserve(_cfg_view.script_config_paths.size() + 1);
  script_ids.push_back("none");
  for (const auto &kv : _cfg_view.script_config_paths) {
    script_ids.push_back(kv.first);
  }
  std::sort(script_ids.begin() + 1, script_ids.end());

  const std::string preview_str =
      script_display_label(_cfg_view.active_script_id);
  ImGui::SeparatorText("Active script");
  ImGui::SetNextItemWidth(-1.0F);
  if (ImGui::BeginCombo("##active_script", preview_str.c_str())) {
    for (const std::string &sid : script_ids) {
      const bool selected = (sid == _cfg_view.active_script_id);
      const auto label_full = script_display_label(sid) + "###sid_" + sid;
      if (ImGui::Selectable(label_full.c_str(), selected)) {
        _cfg_view.active_script_id = sid;
        maybe_handle_script_combo_change(_st, &_cfg_view, &_ccat_ui);
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  if (_cfg_view.active_script_id == shell_sid::k_ccat_script) {
    ImGui::TextWrapped(
        "CampCat uses a .ccat script and a bundle JSON under "
        "config/scripts/.");
    ImGui::Spacing();
    // Both panels configure the script picked above, so they sit one
    // level in instead of flushing left with the page title. Indented
    // before the Spacing they each draw, so the gap stays outside.
    ImGui::Indent();

    ImGui::PushFont(_font_bold);
    const bool paths_open = ImGui::CollapsingHeader(
        "Paths###script_ccat_paths", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopFont();
    if (paths_open) {
      static char v_src[512]{};
      std::snprintf(v_src, sizeof(v_src), "%s", _ccat_ui.source_rel.c_str());
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 88.0F);
      if (ImGui::InputText("##script_path_ccat", v_src, IM_ARRAYSIZE(v_src))) {
        _ccat_ui.source_rel = v_src;
      }
      ImGui::SameLine();
      if (ImGui::Button("Browse##script_path_ccat", ImVec2(80.0F, 0.0F))) {
        std::filesystem::path start = _cfg_view.config_home / "scripts";
        if (_ccat_ui.has_script_source()) {
          const auto resolved = _cfg_view.config_home / _ccat_ui.source_rel;
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
          const auto home_canon =
              std::filesystem::weakly_canonical(_cfg_view.config_home, rel_ec);
          const auto file_canon =
              std::filesystem::weakly_canonical(*picked, rel_ec);
          const auto rel =
              std::filesystem::relative(file_canon, home_canon, rel_ec);
          if (rel_ec || rel.empty() ||
              (!rel.begin()->empty() && *rel.begin() == "..")) {
            _st.append_log(
                "[ui] pick a .ccat file under the config directory: " +
                _cfg_view.config_home.string());
          } else {
            _ccat_ui.source_rel = rel.generic_string();
            std::snprintf(v_src, sizeof(v_src), "%s",
                          _ccat_ui.source_rel.c_str());
            _st.append_log("[ui] script path: " + _ccat_ui.source_rel);
          }
        }
      }
      ImGui::TextDisabled("Script path (.ccat, relative to config)");
      if (!_ccat_ui.has_script_source()) {
        ImGui::TextColored(ImVec4(0.95F, 0.45F, 0.35F, 1.0F),
                           "Set a .ccat script path before saving.");
      }
      ImGui::TextWrapped(
          "Resolved script: %s",
          (_cfg_view.config_home / _ccat_ui.source_rel).string().c_str());
      ImGui::TextWrapped(
          "Template folder: %s",
          _ccat_ui.images_base(_cfg_view.config_home).string().c_str());
    }

    ImGui::PushFont(_font_bold);
    const bool cap_open = ImGui::CollapsingHeader(
        "Screenshot crop (ADB)###script_ccat_cap",
        ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopFont();
    if (cap_open) {
      template_capture_panel_draw(_cfg_view, &_ccat_ui, _lock_manual_single);
    }
    ImGui::Unindent();
  } else {
    ImGui::TextDisabled("Choose CampCat to edit script paths.");
  }
}
