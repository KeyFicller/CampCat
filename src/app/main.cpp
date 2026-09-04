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
#include <tuple>

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
#include "core/automation/automation_driver.h"
#include "core/automation/game_automation.h"
#include "core/automation_log.h"
#include "core/log_buffer.h"
#include "core/scheduler.h"
#include "app/template_capture_ui.h"
#include "app/native_file_dialog.h"
#include "ccat_script/ccat_script_profile.h"
#include "ccat_script/ccat_program_runner.h"
#include "core/script/ccat_parser.h"
#include "core/script/ccat_repl.h"

namespace {

namespace shell_sid = campcat::shell_script_id;

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
                              const char *_adb_connect_buf) {
  *_dst = _ui;
  _dst->adb_path = _adb_path_buf;
  _dst->adb_serial = _serial_buf;
  _dst->adb_connect_address = _adb_connect_buf;
}

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

bool persist_bundle_to_disk(
    const campcat::app_config &_shell_written,
    const campcat::ccat_script_profile &_ccat_snap,
    const std::filesystem::path &_shell_path,
    std::string *_error_out = nullptr) {
  if (!_ccat_snap.has_script_source()) {
    if (_error_out) {
      *_error_out =
          "ccat script source path is empty; set .ccat path before save";
    }
    return false;
  }
  try {
    std::filesystem::create_directories(_shell_path.parent_path());
    campcat::app_config tmp_shell = _shell_written;
    tmp_shell.save_to_file(_shell_path);
    const auto pj_vis =
        _shell_written.resolve_script_json(std::string{shell_sid::k_ccat_script});
    if (!pj_vis.empty()) {
      _ccat_snap.save_to_bundle_path(pj_vis, _shell_written.config_home);
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

  campcat::ccat_script_profile ccat_live{};
  {
    const auto pj =
        cfg.resolve_script_json(std::string{shell_sid::k_ccat_script});
    if (!pj.empty()) {
      ccat_live =
          campcat::ccat_script_profile::load_from_bundle_path(pj,
                                                                  cfg.config_home);
    } else {
      ccat_live = campcat::ccat_script_profile::defaults();
    }
  }

  campcat::log_buffer log_lines;
  campcat::scheduler scheduler;

  campcat::automation_log::set_sink([&](const std::string &s) {
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
    log_lines.push(std::string("[") + buf + "] " + s);
  });

  std::atomic<bool> cycle_running{false};
  std::atomic<bool> stop_requested{false};
  std::thread worker_thread;

  std::deque<std::string> log_scroll;

  auto append_log = [&](const std::string &s) {
    campcat::automation_log::emit(s);
  };

  auto snapshot_cfg = [&]() -> campcat::app_config {
    std::lock_guard<std::mutex> lk(cfg_mu);
    return cfg;
  };

  auto snapshot_ccat_live = [&]() -> campcat::ccat_script_profile {
    std::lock_guard<std::mutex> lk(cfg_mu);
    return ccat_live;
  };

  auto run_automation_thread_body =
      [&](const campcat::app_config _snap_shell,
          const campcat::ccat_script_profile &_snap_ccat) {
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
          std::string co, ce;
          const bool ok_c = adb.connect_remote(
              _snap_shell.adb_connect_address, 15000, &co, &ce);
          split_log("[adb connect] ", co);
          split_log("[adb connect err] ", ce);
          if (!_snap_shell.adb_connect_address.empty() && !ok_c) {
            append_log("[adb connect] failed or timeout");
          }
          const campcat::automation_profile *bundle = nullptr;
          if (_snap_shell.active_script_id == shell_sid::k_ccat_script) {
            bundle = &_snap_ccat;
          }
          auto automator = campcat::make_game_automation(&adb, &_snap_shell,
                                                         bundle);
          const auto should_stop = [&]() { return stop_requested.load(); };
          const auto res = automator->run_cycle(should_stop);
          append_log(std::string("[fsm] ok=") + (res.ok ? "true" : "false") +
                     " | " + res.message);
        } catch (const std::exception &ex) {
          append_log(std::string("[fsm] exception: ") + ex.what());
        }
        release_cycle(cycle_running);
      };

  auto capture_bundle_snapshot = [&]() {
    std::lock_guard<std::mutex> lk(cfg_mu);
    return std::tuple<campcat::app_config, campcat::ccat_script_profile>(
        cfg, ccat_live);
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
    worker_thread = std::thread(
        [&run_automation_thread_body, shell = std::move(std::get<0>(snap)),
         ccat_bundle = std::move(std::get<1>(snap))]() mutable {
          run_automation_thread_body(std::move(shell),
                                     std::move(ccat_bundle));
        });
  };

  campcat::ccat_lang::CcatRepl console_repl;
  char console_input[1024] = {};

  auto run_console_source_job = [&](std::string source) {
    if (!acquire_cycle(cycle_running)) {
      append_log("[console] busy");
      return;
    }
    stop_requested.store(false);
    if (worker_thread.joinable()) {
      worker_thread.join();
    }
    auto snap = capture_bundle_snapshot();
    worker_thread = std::thread(
        [&, source = std::move(source),
         shell = std::move(std::get<0>(snap)),
         ccat_bundle = std::move(std::get<1>(snap))]() mutable {
          try {
            campcat::adb_client adb(shell.adb_path, shell.adb_serial);
            std::string co, ce;
            adb.connect_remote(shell.adb_connect_address, 15000, &co, &ce);
            auto prog = campcat::ccat_lang::parse_program(source);
            const auto images = ccat_bundle.images_base(shell.config_home);
            std::filesystem::path script_abs = ccat_bundle.script_path_absolute;
            if (script_abs.empty()) {
              script_abs = shell.config_home / ccat_bundle.source_rel;
            }
            const auto should_stop = [&]() { return stop_requested.load(); };
            const auto res = campcat::run_ccat_program(
                &adb, &shell, images, script_abs, *prog, should_stop);
            append_log(std::string("[console] ok=") +
                       (res.ok ? "true" : "false") + " | " + res.message);
          } catch (const campcat::ccat_lang::parse_error &ex) {
            append_log(std::string("[console] parse error: ") + ex.what());
          } catch (const std::exception &ex) {
            append_log(std::string("[console] exception: ") + ex.what());
          }
          release_cycle(cycle_running);
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
      append_log("[sched] armed - immediate tick once, then "
                 "sleep(interval+jitter) and repeat until exit/disable");
    } else {
      append_log("[sched] armed - sleep(interval+jitter) then tick, repeat "
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
      glfwCreateWindow(1180, 780, "CampCat Shell", nullptr, nullptr);
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
  {
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0F;
    style.ChildRounding = 4.0F;
    style.FrameRounding = 4.0F;
    style.GrabRounding = 3.0F;
    style.ScrollbarRounding = 4.0F;
    style.WindowPadding = ImVec2(10.0F, 10.0F);
    style.FramePadding = ImVec2(10.0F, 6.0F);
    style.ItemSpacing = ImVec2(10.0F, 8.0F);
    style.ItemInnerSpacing = ImVec2(8.0F, 5.0F);
    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.09F, 0.10F, 0.11F, 1.0F);
    colors[ImGuiCol_ChildBg] = ImVec4(0.11F, 0.12F, 0.13F, 1.0F);
    colors[ImGuiCol_Border] = ImVec4(0.22F, 0.24F, 0.26F, 1.0F);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16F, 0.17F, 0.19F, 1.0F);
    colors[ImGuiCol_Button] = ImVec4(0.22F, 0.35F, 0.48F, 1.0F);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.28F, 0.44F, 0.58F, 1.0F);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.18F, 0.30F, 0.42F, 1.0F);
    colors[ImGuiCol_Header] = ImVec4(0.20F, 0.32F, 0.42F, 0.85F);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.26F, 0.40F, 0.52F, 0.95F);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.22F, 0.36F, 0.48F, 1.0F);
  }

  ImFont *font_body = io.Fonts->AddFontFromFileTTF(
      "/System/Library/Fonts/Supplemental/Arial.ttf", 16.0F);
  ImFont *font_bold = io.Fonts->AddFontFromFileTTF(
      "/System/Library/Fonts/Supplemental/Arial Bold.ttf", 16.0F);
  ImFont *font_title = io.Fonts->AddFontFromFileTTF(
      "/System/Library/Fonts/Supplemental/Arial Bold.ttf", 20.0F);
  if (font_body == nullptr) {
    font_body = io.Fonts->AddFontDefault();
  }
  if (font_bold == nullptr) {
    font_bold = font_body;
  }
  if (font_title == nullptr) {
    font_title = font_bold;
  }

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  append_log("[ui] cfg=" + cfg_path.string());
  append_log("[ui] script combo: switching logs that script snapshot to the "
             "log pane.");

  static char adb_path_buf[512]{};
  static char serial_buf[256]{};
  static char adb_connect_buf[256]{};
  static bool buffers_init = false;

  campcat::ccat_script_profile ccat_ui = snapshot_ccat_live();

  static bool script_switch_warmed = false;
  static std::string last_script_seen;

  auto log_active_script_bundle = [&](const campcat::app_config &_view_shell) {
    const std::string &sid = _view_shell.active_script_id;
    if (sid.empty() || sid == shell_sid::k_none) {
      append_log("[script cfg] (no script selected) automation disabled");
      return;
    }
    if (sid == shell_sid::k_ccat_script) {
      campcat::ccat_script_profile body;
      {
        std::lock_guard<std::mutex> lk(cfg_mu);
        body = ccat_live;
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

        if (_view_shell_before_combo.active_script_id ==
            shell_sid::k_ccat_script) {
          const auto pj =
              _view_shell_before_combo.resolve_script_json(
                  std::string{shell_sid::k_ccat_script});
          {
            std::lock_guard<std::mutex> lk(cfg_mu);
            ccat_live =
                pj.empty()
                    ? campcat::ccat_script_profile::defaults()
                    : campcat::ccat_script_profile::load_from_bundle_path(
                          pj, _view_shell_before_combo.config_home);
          }
          ccat_ui = snapshot_ccat_live();
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
      buffers_init = true;
    }

    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("File")) {
        const bool can_save = ccat_ui.has_script_source();
        if (!can_save) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Save config")) {
          std::string err;
          campcat::app_config to_write{};
          shell_merge_buffer_paths(&to_write, cfg_view, adb_path_buf,
                                   serial_buf, adb_connect_buf);
          bool ok = false;
          {
            std::lock_guard<std::mutex> lk(cfg_mu);
            cfg = to_write;
            ccat_live = ccat_ui;
            ok = persist_bundle_to_disk(cfg, ccat_live, cfg_path, &err);
          }
          if (ok) {
            append_log("[ui] saved to shell: " + cfg_path.string());
            const auto pj_vis =
                to_write.resolve_script_json(std::string{shell_sid::k_ccat_script});
            if (!pj_vis.empty()) {
              append_log("[ui] saved to ccat bundle: " + pj_vis.string());
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
        if (!can_save) {
          ImGui::EndDisabled();
          ImGui::TextDisabled("Set a .ccat script path before saving.");
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

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

    const bool busy_now = cycle_running.load();
    const bool lock_manual_single = busy_now || scheduler.running();
    const char *status_label = busy_now ? "Running" : "Idle";

    enum class shell_page : int { Run = 0, Script, Console, Settings };
    static shell_page page = shell_page::Run;
    static double min_th = 0.35;
    static double max_th = 1.0;

    auto push_job_snapshot = [&]() {
      shell_merge_buffer_paths(&cfg_view, cfg_view, adb_path_buf, serial_buf,
                               adb_connect_buf);
      std::lock_guard<std::mutex> lk(cfg_mu);
      cfg = cfg_view;
      ccat_live = ccat_ui;
    };

    auto nav_item = [&](const char *label, shell_page target) {
      const bool selected = (page == target);
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.28F, 0.45F, 0.58F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.32F, 0.50F, 0.64F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.24F, 0.40F, 0.52F, 1.0F));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.16F, 0.17F, 0.19F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.22F, 0.24F, 0.27F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.20F, 0.22F, 0.25F, 1.0F));
      }
      ImGui::PushFont(font_bold);
      if (ImGui::Button(label, ImVec2(-1.0F, 0.0F))) {
        page = target;
      }
      ImGui::PopFont();
      ImGui::PopStyleColor(3);
    };

    constexpr float k_nav_w = 176.0F;
    constexpr float k_log_h = 220.0F;
    const float body_h = ImGui::GetContentRegionAvail().y;
    const float top_h =
        std::max(120.0F, body_h - k_log_h - ImGui::GetStyle().ItemSpacing.y);

    if (ImGui::BeginTable(
            "shell_layout", 2,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoPadOuterX |
                ImGuiTableFlags_SizingStretchProp,
            ImVec2(-1.0F, top_h))) {
      ImGui::TableSetupColumn("nav", ImGuiTableColumnFlags_WidthFixed,
                              k_nav_w);
      ImGui::TableSetupColumn("content", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableNextRow(ImGuiTableRowFlags_None, top_h);
      ImGui::TableSetColumnIndex(0);

      ImGui::BeginChild("shell_nav", ImVec2(k_nav_w - 8.0F, top_h - 4.0F),
                        ImGuiChildFlags_Borders);
      ImGui::Spacing();
      ImGui::PushFont(font_title);
      ImGui::TextUnformatted("CampCat");
      ImGui::PopFont();
      ImGui::TextDisabled("Shell");
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      nav_item("Run", shell_page::Run);
      nav_item("Script", shell_page::Script);
      nav_item("Console", shell_page::Console);
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      nav_item("Settings", shell_page::Settings);
      ImGui::EndChild();

      ImGui::TableSetColumnIndex(1);
      ImGui::BeginChild("shell_content", ImVec2(0.0F, top_h - 4.0F),
                        ImGuiChildFlags_Borders);

      {
        const char *page_title = "Run";
        switch (page) {
        case shell_page::Script:
          page_title = "Script";
          break;
        case shell_page::Console:
          page_title = "Console";
          break;
        case shell_page::Settings:
          page_title = "Settings";
          break;
        case shell_page::Run:
        default:
          page_title = "Run";
          break;
        }
        ImGui::PushFont(font_title);
        ImGui::TextUnformatted(page_title);
        ImGui::PopFont();
        ImGui::PushFont(font_bold);
        {
          const float status_w = ImGui::CalcTextSize(status_label).x;
          ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - status_w);
        }
        if (busy_now) {
          ImGui::TextColored(ImVec4(0.45F, 0.85F, 0.55F, 1.0F), "%s",
                             status_label);
        } else {
          ImGui::TextDisabled("%s", status_label);
        }
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
      }

      if (page == shell_page::Run) {
        ImGui::TextWrapped(
            "Run a single automation cycle, stop the current cycle, or arm "
            "the scheduler.");
        ImGui::Spacing();

        if (lock_manual_single) {
          ImGui::BeginDisabled();
        }
        ImGui::PushFont(font_bold);
        if (ImGui::Button("Run once")) {
          push_job_snapshot();
          run_single_job();
        }
        if (lock_manual_single) {
          ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
          stop_requested.store(true);
          scheduler.stop();
          append_log("[ui] stop cycle");
        }

        ImGui::SameLine();
        if (ImGui::Button("Start schedule")) {
          cfg_view.scheduler.enabled = true;
          push_job_snapshot();
          std::string err;
          bool ok_writ = persist_bundle_to_disk(
              snapshot_cfg(), snapshot_ccat_live(), cfg_path, &err);
          if (ok_writ) {
            append_log("[ui] autosaved scheduler flag to " +
                       cfg_path.string());
          } else {
            append_log("[ui] autosave warning: " + err);
          }
          refresh_scheduler_locked(true);
        }
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushFont(font_bold);
        ImGui::TextUnformatted("Scheduler");
        ImGui::PopFont();
        if (!cfg_view.scheduler.enabled) {
          ImGui::TextDisabled(
              "Scheduler is off. Use Start schedule to enable and arm it.");
        } else if (!scheduler.running()) {
          ImGui::TextDisabled(
              "Scheduler is enabled but not armed. Use Start schedule.");
        } else {
          const auto wp = scheduler.tick_wait_progress();
          if (wp.in_wait_phase && wp.duration_seconds > 0) {
            const double frac = std::clamp(
                wp.elapsed_seconds / wp.duration_seconds, 0.0, 1.0);
            ImGui::ProgressBar(static_cast<float>(frac), ImVec2(-1.0F, 0.0F));
            ImGui::Text("Waiting - %.1f s / %.1f s", wp.elapsed_seconds,
                        wp.duration_seconds);
          } else {
            ImGui::TextUnformatted("Running a cycle...");
          }
        }
      } else if (page == shell_page::Script) {
        std::vector<std::string> script_ids;
        script_ids.reserve(cfg_view.script_config_paths.size() + 1);
        script_ids.push_back("none");
        for (const auto &kv : cfg_view.script_config_paths) {
          script_ids.push_back(kv.first);
        }
        std::sort(script_ids.begin() + 1, script_ids.end());

        const std::string preview_str =
            script_display_label(cfg_view.active_script_id);
        ImGui::TextUnformatted("Active script");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::BeginCombo("##active_script", preview_str.c_str())) {
          for (const std::string &sid : script_ids) {
            const bool selected = (sid == cfg_view.active_script_id);
            const auto label_full =
                script_display_label(sid) + "###sid_" + sid;
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

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (cfg_view.active_script_id == shell_sid::k_ccat_script) {
          ImGui::TextWrapped(
              "CampCat uses a .ccat script and a bundle JSON under "
              "config/scripts/.");
          ImGui::Spacing();

          if (ImGui::CollapsingHeader("Paths###script_ccat_paths",
                                      ImGuiTreeNodeFlags_DefaultOpen)) {
            static char v_src[512]{};
            std::snprintf(v_src, sizeof(v_src), "%s",
                          ccat_ui.source_rel.c_str());
            ImGui::SetNextItemWidth(
                ImGui::GetContentRegionAvail().x - 88.0F);
            if (ImGui::InputText("##script_path_ccat", v_src,
                                 IM_ARRAYSIZE(v_src))) {
              ccat_ui.source_rel = v_src;
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse##script_path_ccat", ImVec2(80.0F, 0.0F))) {
              std::filesystem::path start = cfg_view.config_home / "scripts";
              if (ccat_ui.has_script_source()) {
                const auto resolved =
                    cfg_view.config_home / ccat_ui.source_rel;
                const auto parent = resolved.parent_path();
                if (!parent.empty()) {
                  start = parent;
                }
              }
              std::error_code ec;
              if (!std::filesystem::is_directory(start, ec)) {
                start = cfg_view.config_home;
              }
              auto picked = campcat::pick_open_file(
                  start, "ccat", "Select CampCat script");
              if (picked) {
                std::error_code rel_ec;
                const auto home_canon = std::filesystem::weakly_canonical(
                    cfg_view.config_home, rel_ec);
                const auto file_canon =
                    std::filesystem::weakly_canonical(*picked, rel_ec);
                const auto rel = std::filesystem::relative(
                    file_canon, home_canon, rel_ec);
                if (rel_ec || rel.empty() ||
                    (!rel.begin()->empty() && *rel.begin() == "..")) {
                  append_log(
                      "[ui] pick a .ccat file under the config directory: " +
                      cfg_view.config_home.string());
                } else {
                  ccat_ui.source_rel = rel.generic_string();
                  std::snprintf(v_src, sizeof(v_src), "%s",
                                ccat_ui.source_rel.c_str());
                  append_log("[ui] script path: " + ccat_ui.source_rel);
                }
              }
            }
            ImGui::TextDisabled(
                "Script path (.ccat, relative to config)");
            if (!ccat_ui.has_script_source()) {
              ImGui::TextColored(ImVec4(0.95F, 0.45F, 0.35F, 1.0F),
                                 "Set a .ccat script path before saving.");
            }
            ImGui::TextWrapped(
                "Resolved script: %s",
                (cfg_view.config_home / ccat_ui.source_rel).string().c_str());
            ImGui::TextWrapped(
                "Template folder: %s",
                ccat_ui.images_base(cfg_view.config_home).string().c_str());
          }

          if (ImGui::CollapsingHeader(
                  "Screenshot crop (ADB)###script_ccat_cap",
                  ImGuiTreeNodeFlags_DefaultOpen)) {
            template_capture_draw_panel(cfg_view, &ccat_ui,
                                        lock_manual_single);
          }
        } else {
          ImGui::TextDisabled("Choose CampCat to edit script paths.");
        }
      } else if (page == shell_page::Console) {
        ImGui::TextWrapped(
            "Interactive .ccat console. Multiline uses ... until braces/parens "
            "balance. Disabled while a cycle is running.");
        ImGui::Spacing();

        ImGui::BeginChild(
            "console_history",
            ImVec2(0.0F, -ImGui::GetFrameHeightWithSpacing() * 2.5F),
            ImGuiChildFlags_Borders);
        for (const auto &line : console_repl.history) {
          ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
          ImGui::SetScrollHereY(1.0F);
        }
        ImGui::EndChild();

        if (busy_now) {
          ImGui::TextColored(ImVec4(0.9F, 0.6F, 0.2F, 1.0F),
                             "cycle running - console locked");
          ImGui::BeginDisabled();
        }

        ImGui::TextUnformatted(console_repl.prompt());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-80.0F);
        const bool submit = ImGui::InputText(
            "##console_in", console_input, sizeof(console_input),
            ImGuiInputTextFlags_EnterReturnsTrue);
        // Must run immediately after InputText; later widgets would steal -1.
        if (submit && !busy_now) {
          ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
          console_repl.clear();
          console_repl.history.push_back("(cleared)");
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
          console_repl.clear();
        }

        if (busy_now) {
          ImGui::EndDisabled();
        }

        if (submit && !busy_now) {
          const auto fr = console_repl.feed_line(console_input);
          console_input[0] = '\0';
          if (fr == campcat::ccat_lang::repl_feed_result::ready) {
            if (!ccat_ui.has_script_source()) {
              console_repl.history.push_back(
                  "need .ccat path for images_base");
              append_log("[console] need .ccat path for images_base");
              console_repl.clear();
            } else {
              try {
                (void)campcat::ccat_lang::parse_program(console_repl.pending);
                push_job_snapshot();
                run_console_source_job(console_repl.pending);
                console_repl.clear();
              } catch (const campcat::ccat_lang::parse_error &ex) {
                const std::string msg =
                    std::string("parse error: ") + ex.what() + " (line " +
                    std::to_string(ex.line) + ", col " +
                    std::to_string(ex.col) + ")";
                console_repl.history.push_back(msg);
                append_log(std::string("[console] ") + msg);
                console_repl.clear();
              }
            }
          }
        }
      } else if (page == shell_page::Settings) {
        ImGui::TextDisabled("%s", cfg_path.string().c_str());
        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Timing & match###cfg_timing",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::SliderInt("Tap delay (ms)", &cfg_view.tap_delay_ms, 40, 800);
          ImGui::SliderInt("Action gap (ms)", &cfg_view.action_gap_ms, 1000,
                           5000);
          ImGui::SliderInt("Swipe duration (ms)", &cfg_view.swipe_duration_ms,
                           50, 2000);
          ImGui::SliderScalar("Match threshold", ImGuiDataType_Double,
                              &cfg_view.match_threshold, &min_th, &max_th,
                              "%.3f");
          ImGui::Checkbox("Multi-scale match", &cfg_view.match_multiscale);
        }

        if (ImGui::CollapsingHeader("ADB###cfg_adb",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::InputText("ADB path", adb_path_buf,
                           IM_ARRAYSIZE(adb_path_buf));
          ImGui::InputText("Device serial", serial_buf,
                           IM_ARRAYSIZE(serial_buf));
          ImGui::InputText("Connect address", adb_connect_buf,
                           IM_ARRAYSIZE(adb_connect_buf));
        }

        if (ImGui::CollapsingHeader("Scheduler###cfg_sched",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::Checkbox("Enable scheduler", &cfg_view.scheduler.enabled);
          ImGui::SliderInt("Interval (s)",
                           &cfg_view.scheduler.interval_seconds, 300,
                           6 * 3600);
          ImGui::SliderInt("Jitter (s)", &cfg_view.scheduler.jitter_seconds, 0,
                           900);
          ImGui::Checkbox("Skip if busy", &cfg_view.scheduler.skip_if_busy);
          ImGui::TextDisabled(
              "Arm the scheduler from the Run page with Start schedule.");
        }
      }

      ImGui::EndChild();
      ImGui::EndTable();
    }

    ImGui::BeginChild("shell_log", ImVec2(0.0F, k_log_h),
                      ImGuiChildFlags_Borders);
    ImGui::PushFont(font_bold);
    ImGui::TextUnformatted("Log");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("Right-click to clear");
    ImGui::Separator();
    ImGui::BeginChild("logpane", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None);
    for (const auto &line : log_scroll) {
      ImGui::TextWrapped("%s", line.c_str());
    }
    if (!log_scroll.empty() &&
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0F) {
      ImGui::SetScrollHereY(1.0F);
    }
    if (ImGui::BeginPopupContextWindow("logpane_ctx",
                                       ImGuiPopupFlags_MouseButtonRight)) {
      if (ImGui::MenuItem("Clear log")) {
        log_lines.clear();
        log_scroll.clear();
      }
      ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::EndChild();

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
                               adb_connect_buf);
      ccat_live = ccat_ui;
    }
  }

  stop_requested.store(true);
  scheduler.stop();

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  campcat::automation_log::clear_sink();

  template_capture_shutdown_gl();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
