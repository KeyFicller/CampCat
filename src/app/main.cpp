#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <nlohmann/json.hpp>

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation/game_automation.h"
#include "core/log_buffer.h"
#include "core/scheduler.h"
#include "games/stzb_auto_assemble/stzb_auto_assemble_profile.h"

namespace {

constexpr char k_default_config_relative[] = "config/default.json";

inline bool acquire_cycle(std::atomic<bool> &_cycle_running) {
  bool expected = false;
  return _cycle_running.compare_exchange_strong(expected, true);
}

inline void release_cycle(std::atomic<bool> &_cycle_running) {
  _cycle_running.store(false);
}

void shell_merge_buffer_paths(campcat::app_config *_dst,
                              const campcat::app_config &_ui,
                              const char *_adb_path_buf,
                              const char *_serial_buf,
                              const char *_adb_connect_buf,
                              const char *_dbg_buf) {
  *_dst = _ui;
  _dst->adb_path = _adb_path_buf;
  _dst->adb_serial = _serial_buf;
  _dst->adb_connect_address = _adb_connect_buf;
  _dst->debug_dir = _dbg_buf;
}

std::string script_display_label(const std::string &_id) {
  if (_id == "none") {
    return "(none)";
  }
  if (_id == "stzb_auto_assemble") {
    return "STZB · main city / formation";
  }
  return _id;
}

void append_prefixed_multiline_json(
    const nlohmann::json &_json, const std::string &_prefix,
    const std::function<void(std::string)> &_emit) {
  std::ostringstream oss;
  oss << std::setw(2) << _json << '\n';
  std::istringstream iss(oss.str());
  std::string line;
  while (std::getline(iss, line)) {
    _emit(_prefix + line);
  }
}

constexpr const char k_stzb_id[] = "stzb_auto_assemble";

bool persist_bundle_to_disk(
    const campcat::app_config &_shell_written,
    const campcat::stzb_auto_assemble_profile &_stzb_snap,
    const std::filesystem::path &_shell_path,
    std::string *_error_out = nullptr) {
  try {
    std::filesystem::create_directories(_shell_path.parent_path());
    campcat::app_config tmp_shell = _shell_written;
    tmp_shell.save_to_file(_shell_path);
    const auto pj = _shell_written.resolve_script_json(k_stzb_id);
    if (!pj.empty()) {
      _stzb_snap.save_to_file(pj, _shell_written.project_root());
    }
    return true;
  } catch (const std::exception &ex) {
    if (_error_out) {
      *_error_out = ex.what();
    }
    return false;
  }
}

} // namespace

int main(int argc, char **argv) {
  const std::filesystem::path cfg_path =
      std::filesystem::path(argc >= 2 ? argv[1] : k_default_config_relative);

  std::mutex cfg_mu;
  campcat::app_config cfg = campcat::app_config::load_from_file(cfg_path);

  campcat::stzb_auto_assemble_profile stzb_live{};
  {
    const auto pj = cfg.resolve_script_json(k_stzb_id);
    if (!pj.empty()) {
      stzb_live = campcat::stzb_auto_assemble_profile::load_from_file(
          pj, cfg.project_root());
    } else {
      stzb_live = campcat::stzb_auto_assemble_profile::defaults();
    }
  }

  campcat::log_buffer log_lines;
  campcat::scheduler scheduler;

  std::atomic<bool> cycle_running{false};
  std::atomic<bool> stop_requested{false};
  std::thread worker_thread;

  std::deque<std::string> log_scroll;

  auto append_log = [&](std::string s) {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[64]{};
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    log_lines.push(std::string("[") + buf + "] " + std::move(s));
  };

  auto snapshot_cfg = [&]() -> campcat::app_config {
    std::lock_guard<std::mutex> lk(cfg_mu);
    return cfg;
  };

  auto snapshot_stzb_live = [&]() -> campcat::stzb_auto_assemble_profile {
    std::lock_guard<std::mutex> lk(cfg_mu);
    return stzb_live;
  };

  auto run_automation_thread_body =
      [&](const campcat::app_config _snap_shell,
          const campcat::stzb_auto_assemble_profile &_snap_stzb) {
        auto split_log = [&](const char *prefix, const std::string &block) {
          std::istringstream iss(block);
          std::string line;
          while (std::getline(iss, line)) {
            if (!line.empty()) {
              append_log(std::string(prefix) + line);
            }
          }
        };
        try {
          campcat::adb_client adb(_snap_shell.adb_path, _snap_shell.adb_serial);
          if (!_snap_shell.adb_connect_address.empty()) {
            std::string co, ce;
            const bool ok_c = adb.run(
                {"connect", _snap_shell.adb_connect_address}, &co, &ce, 15000);
            split_log("[adb connect] ", co);
            split_log("[adb connect err] ", ce);
            if (!ok_c) {
              append_log("[adb connect] failed or timeout");
            }
          }
          const auto log_fn = [&](std::string line) {
            append_log(std::move(line));
          };

          const campcat::stzb_auto_assemble_profile *stzb_arg =
              (_snap_shell.active_script_id == k_stzb_id) ? &_snap_stzb
                                                          : nullptr;
          auto automator = campcat::make_game_automation(&adb, &_snap_shell,
                                                         stzb_arg, log_fn);
          const auto should_stop = [&]() { return stop_requested.load(); };
          const auto res = automator->run_cycle(should_stop);
          append_log(std::string("[fsm] ok=") + (res.ok ? "true" : "false") +
                     " · " + res.message);
        } catch (const std::exception &ex) {
          append_log(std::string("[fsm] exception: ") + ex.what());
        }
        release_cycle(cycle_running);
      };

  auto capture_bundle_snapshot = [&]() {
    std::lock_guard<std::mutex> lk(cfg_mu);
    return std::pair<campcat::app_config, campcat::stzb_auto_assemble_profile>(
        cfg, stzb_live);
  };

  auto run_single_job = [&]() {
    if (!acquire_cycle(cycle_running)) {
      append_log("[job] busy");
      return;
    }
    stop_requested.store(false);

    if (worker_thread.joinable()) {
      worker_thread.join();
    }

    auto snap = capture_bundle_snapshot();
    worker_thread =
        std::thread([&run_automation_thread_body, shell = std::move(snap.first),
                     stzb = std::move(snap.second)]() mutable {
          run_automation_thread_body(std::move(shell), std::move(stzb));
        });
  };

  auto refresh_scheduler_locked = [&](bool immediate_first_tick = false) {
    scheduler.stop();
    const auto snap = snapshot_cfg();
    if (!snap.scheduler.enabled) {
      append_log("[sched] disabled");
      return;
    }

    scheduler.configure(std::chrono::seconds(snap.scheduler.interval_seconds),
                        std::chrono::seconds(snap.scheduler.jitter_seconds));

    scheduler.start(
        [&]() {
          const auto shell = snapshot_cfg();
          if (shell.scheduler.skip_if_busy && cycle_running.load()) {
            append_log("[sched] skip tick (busy)");
            return;
          }
          run_single_job();
        },
        immediate_first_tick);

    if (immediate_first_tick) {
      append_log("[sched] armed · immediate tick once, then "
                 "sleep(interval+jitter) and repeat until exit/disable");
    } else {
      append_log("[sched] armed · sleep(interval+jitter) then tick, repeat "
                 "until exit/disable");
    }
  };

  glfwSetErrorCallback([](int code, const char *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, desc ? desc : "(null)");
  });

  if (!glfwInit()) {
    return 1;
  }

#if defined(__APPLE__)
  const char *glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#else
  const char *glsl_version = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

  GLFWwindow *window =
      glfwCreateWindow(1180, 780, "CampCat · Shell", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 2;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  append_log("[ui] cfg=" + cfg_path.string());
  append_log("[ui] script combo: switching logs that script snapshot to the "
             "debug log.");

  static char adb_path_buf[512]{};
  static char serial_buf[256]{};
  static char adb_connect_buf[256]{};
  static char dbg_buf[512]{};
  static bool buffers_init = false;

  campcat::stzb_auto_assemble_profile stzb_ui = snapshot_stzb_live();

  static bool script_switch_warmed = false;
  static std::string last_script_seen;

  auto log_active_script_bundle = [&](const campcat::app_config &_view_shell) {
    const std::string &sid = _view_shell.active_script_id;
    if (sid.empty() || sid == "none") {
      append_log("[script cfg] (no script selected) automation disabled");
      return;
    }
    if (sid == k_stzb_id) {
      campcat::stzb_auto_assemble_profile body;
      {
        std::lock_guard<std::mutex> lk(cfg_mu);
        body = stzb_live;
      }
      append_prefixed_multiline_json(body.to_summary_json_for_log(),
                                     "[script cfg] ", append_log);
      return;
    }
    append_log("[script cfg] unknown script `" + sid + "` (no profile block)");
  };

  auto maybe_handle_script_combo_change =
      [&](const campcat::app_config &_view_shell_before_combo) {
        if (!script_switch_warmed) {
          script_switch_warmed = true;
          last_script_seen = _view_shell_before_combo.active_script_id;
          return;
        }
        if (_view_shell_before_combo.active_script_id == last_script_seen) {
          return;
        }
        last_script_seen = _view_shell_before_combo.active_script_id;

        if (_view_shell_before_combo.active_script_id == k_stzb_id) {
          const auto pj =
              _view_shell_before_combo.resolve_script_json(k_stzb_id);
          {
            std::lock_guard<std::mutex> lk(cfg_mu);
            stzb_live =
                pj.empty()
                    ? campcat::stzb_auto_assemble_profile::defaults()
                    : campcat::stzb_auto_assemble_profile::load_from_file(
                          pj, _view_shell_before_combo.project_root());
          }
          stzb_ui = snapshot_stzb_live();
        }

        append_log("[script] active_script='" + last_script_seen + "'");
        log_active_script_bundle(_view_shell_before_combo);
      };

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);

    ImGui::Begin("campcat-shell", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar);

    campcat::app_config cfg_view = snapshot_cfg();

    if (!buffers_init) {
      std::snprintf(adb_path_buf, sizeof(adb_path_buf), "%s",
                    cfg_view.adb_path.c_str());
      std::snprintf(serial_buf, sizeof(serial_buf), "%s",
                    cfg_view.adb_serial.c_str());
      std::snprintf(adb_connect_buf, sizeof(adb_connect_buf), "%s",
                    cfg_view.adb_connect_address.c_str());
      std::snprintf(dbg_buf, sizeof(dbg_buf), "%s",
                    cfg_view.debug_dir.string().c_str());
      buffers_init = true;
    }

    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save config")) {
          std::string err;
          campcat::app_config to_write{};
          shell_merge_buffer_paths(&to_write, cfg_view, adb_path_buf,
                                   serial_buf, adb_connect_buf, dbg_buf);
          bool ok = false;
          {
            std::lock_guard<std::mutex> lk(cfg_mu);
            cfg = to_write;
            stzb_live = stzb_ui;
            ok = persist_bundle_to_disk(cfg, stzb_live, cfg_path, &err);
          }
          if (ok) {
            append_log("[ui] saved → shell: " + cfg_path.string());
            const auto pj = to_write.resolve_script_json(k_stzb_id);
            if (!pj.empty()) {
              append_log("[ui] saved → script bundle: " + pj.string());
            }
            if (!snapshot_cfg().scheduler.enabled) {
              scheduler.stop();
              append_log(
                  "[sched] stopped (scheduler.disabled in saved config)");
            }
          } else {
            append_log(std::string("[ui] save failed: ") + err);
          }
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

    ImGui::TextDisabled("%s", cfg_path.string().c_str());

    ImGui::Separator();

    {
      std::vector<std::string> drained;
      log_lines.drain(&drained);
      for (auto &l : drained) {
        log_scroll.push_back(std::move(l));
      }
      while (log_scroll.size() > 2000) {
        log_scroll.pop_front();
      }
    }

    ImGui::SliderInt("tap_delay_ms", &cfg_view.tap_delay_ms, 40, 800);
    ImGui::SliderInt("action_gap_ms (min pause between ops)",
                     &cfg_view.action_gap_ms, 1000, 5000);

    ImGui::SliderInt("swipe_duration_ms", &cfg_view.swipe_duration_ms, 50,
                     2000);

    static double min_th = 0.35;
    static double max_th = 1.0;
    ImGui::SliderScalar("match_threshold", ImGuiDataType_Double,
                        &cfg_view.match_threshold, &min_th, &max_th, "%.3f");

    ImGui::Checkbox("match_multiscale", &cfg_view.match_multiscale);

    ImGui::InputText("adb_path", adb_path_buf, IM_ARRAYSIZE(adb_path_buf));
    ImGui::InputText("adb_serial", serial_buf, IM_ARRAYSIZE(serial_buf));
    ImGui::InputText("adb_connect_address", adb_connect_buf,
                     IM_ARRAYSIZE(adb_connect_buf));

    ImGui::Checkbox("debug_screenshots", &cfg_view.debug_screenshots);
    ImGui::InputText("debug_dir", dbg_buf, IM_ARRAYSIZE(dbg_buf));

    ImGui::Separator();
    ImGui::Checkbox("scheduler.enabled", &cfg_view.scheduler.enabled);
    ImGui::SliderInt("interval_s", &cfg_view.scheduler.interval_seconds, 300,
                     6 * 3600);
    ImGui::SliderInt("jitter_s", &cfg_view.scheduler.jitter_seconds, 0, 900);
    ImGui::Checkbox("scheduler.skip_if_busy", &cfg_view.scheduler.skip_if_busy);
    if (cfg_view.scheduler.enabled && !scheduler.running()) {
      ImGui::TextDisabled(
          "scheduler idle : click run_schedule to arm (not started at boot)");
    }

    ImGui::Separator();

    const bool busy_now = cycle_running.load();
    if (busy_now) {
      ImGui::BeginDisabled();
    }

    auto push_job_snapshot = [&]() {
      shell_merge_buffer_paths(&cfg_view, cfg_view, adb_path_buf, serial_buf,
                               adb_connect_buf, dbg_buf);
      std::lock_guard<std::mutex> lk(cfg_mu);
      cfg = cfg_view;
      stzb_live = stzb_ui;
    };

    if (ImGui::Button("run_single")) {
      push_job_snapshot();
      run_single_job();
    }
    if (busy_now) {
      ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("stop cycle")) {
      stop_requested.store(true);
    }

    ImGui::SameLine();
    if (ImGui::Button("run_schedule")) {
      cfg_view.scheduler.enabled = true;
      {
        push_job_snapshot();
      }
      std::string err;
      bool ok_writ = persist_bundle_to_disk(
          snapshot_cfg(), snapshot_stzb_live(), cfg_path, &err);
      if (ok_writ) {
        append_log("[ui] autosaved scheduler flag → " + cfg_path.string());
      } else {
        append_log("[ui] autosave warning: " + err);
      }
      refresh_scheduler_locked(true);
    }

    if (cfg_view.scheduler.enabled && scheduler.running()) {
      const auto wp = scheduler.tick_wait_progress();
      if (wp.in_wait_phase && wp.duration_seconds > 0) {
        const double frac =
            std::clamp(wp.elapsed_seconds / wp.duration_seconds, 0.0, 1.0);
        ImGui::ProgressBar(static_cast<float>(frac), ImVec2(-1.0F, 0.0F));
        ImGui::Text("scheduler · elapsed %.1f s / interval %.1f s",
                    wp.elapsed_seconds, wp.duration_seconds);
      } else {
        ImGui::TextUnformatted(
            "scheduler · not waiting (immediate tick or tick running) …");
      }
    }

    ImGui::Separator();

    ImGui::Columns(2, nullptr, true);

    ImGui::BeginChild("logpane", ImVec2(0, -52), true);
    ImGui::TextUnformatted("log");
    ImGui::Separator();
    for (const auto &line : log_scroll) {
      ImGui::TextUnformatted(line.c_str());
    }
    if (!log_scroll.empty() &&
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0F) {
      ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();

    ImGui::NextColumn();

    ImGui::BeginChild("scriptpane", ImVec2(0, -52), true);
    ImGui::TextUnformatted("Script bundle");

    std::vector<std::string> script_ids;
    script_ids.reserve(cfg_view.script_config_paths.size() + 1);
    script_ids.push_back("none");
    for (const auto &kv : cfg_view.script_config_paths) {
      script_ids.push_back(kv.first);
    }
    std::sort(script_ids.begin() + 1, script_ids.end());

    const std::string preview_str =
        script_display_label(cfg_view.active_script_id);
    if (ImGui::BeginCombo("script##script_combo_box", preview_str.c_str())) {
      for (const std::string &sid : script_ids) {
        const bool selected = (sid == cfg_view.active_script_id);
        const auto label_full = script_display_label(sid) + "###sid_" + sid;
        if (ImGui::Selectable(label_full.c_str(), selected)) {
          cfg_view.active_script_id = sid;
          maybe_handle_script_combo_change(cfg_view);
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    ImGui::Separator();

    if (cfg_view.active_script_id == k_stzb_id) {
      ImGui::TextUnformatted(
          "STZB profile (stored in separate script JSON file)");
      static char bundle_dir_buf[512]{};
      static char res_buf[128]{};
      std::snprintf(bundle_dir_buf, sizeof(bundle_dir_buf), "%s",
                    stzb_ui.bundle_dir.string().c_str());
      std::snprintf(res_buf, sizeof(res_buf), "%s",
                    stzb_ui.resolution_subdir.c_str());
      if (ImGui::InputText("bundle_dir (relative to project root)",
                           bundle_dir_buf, IM_ARRAYSIZE(bundle_dir_buf))) {
        stzb_ui.bundle_dir = std::filesystem::path(bundle_dir_buf);
      }
      if (ImGui::InputText("resolution_subdir", res_buf,
                           IM_ARRAYSIZE(res_buf))) {
        stzb_ui.resolution_subdir = res_buf;
      }
      ImGui::TextWrapped("PNG directory: %s",
                         stzb_ui.template_resolution_dir().string().c_str());

      ImGui::Separator();
      auto tt_input = [&](const char *label, std::string &dest) {
        std::vector<char> buf(260);
        std::snprintf(buf.data(), buf.size(), "%s", dest.c_str());
        if (ImGui::InputText(label, buf.data(), buf.size())) {
          dest = buf.data();
        }
      };
      tt_input("dismiss_notice", stzb_ui.templates["dismiss_notice"]);
      tt_input("game_main", stzb_ui.templates["game_main"]);
      tt_input("main_menu_anchor", stzb_ui.templates["main_menu_anchor"]);
      tt_input("enter_city_location", stzb_ui.templates["enter_city_location"]);
      tt_input("enter_city_city", stzb_ui.templates["enter_city_city"]);
      tt_input("enter_city_detail", stzb_ui.templates["enter_city_detail"]);
      tt_input("recruit_detail", stzb_ui.templates["recruit_detail"]);
      tt_input("recruit_back", stzb_ui.templates["recruit_back"]);
      tt_input("recruit_back_2", stzb_ui.templates["recruit_back_2"]);
      tt_input("assemble", stzb_ui.templates["assemble"]);

      ImGui::Separator();
      ImGui::SliderInt("team_count", &stzb_ui.team_count, 1,
                       static_cast<int>(stzb_ui.team_rois.size()));
      ImGui::TextUnformatted("team_rois (normalized xywh)");
      for (size_t i = 0; i < stzb_ui.team_rois.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("team %zu", i + 1);
        ImGui::InputScalarN("xywh", ImGuiDataType_Double,
                            &stzb_ui.team_rois[i].x, 4, nullptr, nullptr,
                            "%.3f");
        ImGui::PopID();
      }
    } else {
      ImGui::TextDisabled(
          "Pick a script in the combo to edit its bundle. Choose STZB to "
          "edit template keys and ROIs.");
    }

    ImGui::EndChild();

    ImGui::Columns(1);

    ImGui::Separator();

    ImGui::TextUnformatted(busy_now ? "status: working ~" : "status: idle");

    ImGui::End();

    ImGui::Render();

    int display_w = 0;
    int display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);

    {
      std::lock_guard<std::mutex> lk(cfg_mu);
      shell_merge_buffer_paths(&cfg, cfg_view, adb_path_buf, serial_buf,
                               adb_connect_buf, dbg_buf);
      stzb_live = stzb_ui;
    }
  }

  stop_requested.store(true);
  scheduler.stop();

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
