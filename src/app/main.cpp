#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <tuple>

#ifndef _WIN32
#include <unistd.h>
#endif

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
#include "core/mumu_launcher.h"
#include "core/scheduler.h"
#include "app/template_capture_ui.h"
#include "app/llm_ui.h"
#include "app/script_editor_ui.h"
#include "app/macos_ime.h"
#include "app/native_file_dialog.h"
#include "app/prompt_input.h"
#include "ccat_script/ccat_script_profile.h"
#include "ccat_script/ccat_program_runner.h"
#include "core/script/ccat_parser.h"
#include "core/script/ccat_repl.h"

namespace {

namespace shell_sid = campcat::shell_script_id;

constexpr char k_default_config_relative[] = "config/default.json";

/// How long the start button waits for the instance to leave its not-ready
/// states before it gives up. Booting the VM is slow; this only bounds it.
constexpr int k_mumu_ready_timeout_ms = 180000;

/// How long the start button waits for MuMuPlayer's service to answer after the
/// app is launched. Shorter than a VM boot: this is just an app coming up.
constexpr int k_mumu_service_timeout_ms = 60000;

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
                              const char *_mumutool_buf, int _mumu_index) {
  *_dst = _ui;
  _dst->adb_path = _adb_path_buf;
  _dst->adb_serial = _serial_buf;
  _dst->adb_connect_address = _adb_connect_buf;
  _dst->mumutool_path = _mumutool_buf;
  _dst->mumu_device_index = _mumu_index;
}

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
  // The LLM sidecar can die between our readiness check and a write to its
  // stdin; that write would raise SIGPIPE and kill the whole app. Ignoring it
  // turns the case into an ordinary EPIPE that `llm_host` already reports.
  // The sidecar itself re-enables the default (see `process::spawn`).
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif

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

  // The MuMu instance we started, if any. `mumu_launched_*` is written when the
  // instance comes up, never re-read from `cfg`: the user can edit the path or
  // index after starting one, and closing the wrong instance is worse than
  // closing none. Recording it does not wait on the watchdog, so an instance we
  // started is closed on a clean exit even when none could be armed.
  std::mutex mumu_mu;
  campcat::mumu_status mumu_report;
  std::atomic<bool> mumu_launching{false};
  std::optional<campcat::process::child> mumu_watchdog;
  std::string mumu_launched_path;
  int mumu_launched_index = -1;
  /// Bundle this run started, empty when it started none. Same rule as above:
  /// only what we launched gets quit, and the path is frozen at launch time.
  std::string mumu_app_started;

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

    try {
      auto snap = capture_bundle_snapshot();
      worker_thread = std::thread(
          [&run_automation_thread_body, shell = std::move(std::get<0>(snap)),
           ccat_bundle = std::move(std::get<1>(snap))]() mutable {
            run_automation_thread_body(std::move(shell),
                                       std::move(ccat_bundle));
          });
    } catch (const std::exception &ex) {
      release_cycle(cycle_running);
      append_log(std::string("[job] failed to start worker: ") + ex.what());
    }
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
    try {
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
    } catch (const std::exception &ex) {
      release_cycle(cycle_running);
      append_log(std::string("[console] failed to start worker: ") + ex.what());
    }
  };

  /// Start the MuMu instance, then arm the watchdog that outlives us.
  auto start_mumu_job = [&]() {
    if (!acquire_cycle(cycle_running)) {
      append_log("[mumu] busy");
      return;
    }
    stop_requested.store(false);
    if (worker_thread.joinable()) {
      worker_thread.join();
    }
    const campcat::app_config shell = snapshot_cfg();
    mumu_launching.store(true);
    try {
      worker_thread = std::thread([&, shell]() {
        const std::string tool = shell.mumutool_path;
        const std::string app = campcat::mumu_app_path(tool);
        const int index = shell.mumu_device_index;
        // The app this run launched, if any; feeds the watchdog and is what
        // the shutdown path quits. Empty keeps somebody else's app alive.
        std::string app_owned;
        auto finish = [&](const campcat::mumu_status &_st) {
          {
            std::lock_guard<std::mutex> lk(mumu_mu);
            mumu_report = _st;
          }
          mumu_launching.store(false);
          release_cycle(cycle_running);
        };

        campcat::mumu_status st = campcat::mumu_query_state(tool, index);
        if (st.state == campcat::mumu_state::service_down) {
          // mumutool is only a client of MuMuPlayer's service: with the app down
          // it cannot answer `info` at all, so the app has to come first.
          st = campcat::mumu_launch_app(app);
          if (st.state != campcat::mumu_state::failed) {
            app_owned = app;
            {
              std::lock_guard<std::mutex> lk(mumu_mu);
              mumu_app_started = app;
            }
            st = campcat::mumu_wait_service(tool, index,
                                            k_mumu_service_timeout_ms);
          }
        }
        if (st.state == campcat::mumu_state::failed) {
          finish(st);
          return;
        }
        if (st.state != campcat::mumu_state::stopped) {
          // Somebody already had it up. Leave it alone and arm no watchdog:
          // that would close an instance we did not start. An app we just
          // launched is still ours to quit; `mumu_app_started` holds that.
          st.message = "already up";
          finish(st);
          return;
        }

        st = campcat::mumu_open(tool, index);
        if (st.state == campcat::mumu_state::failed) {
          finish(st);
          return;
        }
        st = campcat::mumu_wait_ready(tool, index, k_mumu_ready_timeout_ms);
        if (st.state == campcat::mumu_state::failed) {
          finish(st);
          return;
        }

        // Spawn first: the watchdog then covers the gap before anything is
        // recorded. Record either way, so an instance we started is still closed
        // on a clean exit when no watchdog could be armed.
        auto wd = campcat::mumu_spawn_watchdog(
            tool, index, static_cast<long>(::getpid()), app_owned);
        const bool armed = wd.has_value();
        {
          std::lock_guard<std::mutex> lk(mumu_mu);
          mumu_launched_path = tool;
          mumu_launched_index = index;
          mumu_watchdog = std::move(wd);
        }
        if (!armed) {
          st.message = "started, but no watchdog could be armed";
        }
        finish(st);
      });
    } catch (const std::exception &ex) {
      mumu_launching.store(false);
      release_cycle(cycle_running);
      append_log(std::string("[mumu] failed to start worker: ") + ex.what());
    }
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
    style.ChildRounding = 6.0F;
    style.FrameRounding = 5.0F;
    style.GrabRounding = 4.0F;
    style.ScrollbarRounding = 6.0F;
    style.PopupRounding = 6.0F;
    style.WindowPadding = ImVec2(10.0F, 10.0F);
    style.FramePadding = ImVec2(10.0F, 6.0F);
    style.ItemSpacing = ImVec2(10.0F, 8.0F);
    style.ItemInnerSpacing = ImVec2(8.0F, 5.0F);
    style.ScrollbarSize = 12.0F;
    style.GrabMinSize = 12.0F;
    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.PopupBorderSize = 1.0F;
    style.DisabledAlpha = 0.50F;

    // One palette, every slot filled. A partly themed style leaves the rest on
    // ImGui's defaults -- bright scrollbars and checkmarks, a saturated blue
    // on every header -- and that mix is what reads as unfinished. Values are
    // low-contrast on purpose: the accent marks what is active, nothing shouts.
    const ImVec4 bg{0.086F, 0.094F, 0.106F, 1.00F};
    const ImVec4 panel{0.106F, 0.116F, 0.133F, 1.00F};
    const ImVec4 field{0.145F, 0.157F, 0.176F, 1.00F};
    const ImVec4 field_hi{0.176F, 0.192F, 0.216F, 1.00F};
    const ImVec4 line{0.196F, 0.212F, 0.239F, 1.00F};
    const ImVec4 line_hi{0.278F, 0.302F, 0.337F, 1.00F};
    const ImVec4 ink{0.847F, 0.871F, 0.898F, 1.00F};
    const ImVec4 ink_dim{0.478F, 0.510F, 0.549F, 1.00F};
    const ImVec4 accent{0.243F, 0.400F, 0.529F, 1.00F};
    const ImVec4 accent_hi{0.302F, 0.478F, 0.616F, 1.00F};
    const ImVec4 accent_lo{0.200F, 0.341F, 0.463F, 1.00F};
    // Headers and rows get a wash of the accent, not a solid fill, so an
    // expanded section reads as a group instead of a second toolbar.
    const ImVec4 wash{0.243F, 0.400F, 0.529F, 0.28F};
    const ImVec4 wash_hi{0.243F, 0.400F, 0.529F, 0.46F};

    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = ink;
    c[ImGuiCol_TextDisabled] = ink_dim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = ImVec4(0.118F, 0.129F, 0.149F, 0.98F);
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_FrameBg] = field;
    c[ImGuiCol_FrameBgHovered] = field_hi;
    c[ImGuiCol_FrameBgActive] = accent_lo;
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.129F, 0.145F, 0.169F, 1.00F);
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = ImVec4(0.098F, 0.106F, 0.122F, 1.00F);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.235F, 0.255F, 0.286F, 1.00F);
    c[ImGuiCol_ScrollbarGrabHovered] = line_hi;
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent_hi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    // A plain button is a surface, not a primary. Filled with the accent, every
    // button on a page looked equally important -- Run once, Stop and Send all
    // read as the one thing to press. The accent is spent on the single primary
    // per page instead; see the Send button in llm_ui.cpp.
    c[ImGuiCol_Button] = field_hi;
    c[ImGuiCol_ButtonHovered] = line_hi;
    c[ImGuiCol_ButtonActive] = accent_lo;
    c[ImGuiCol_Header] = wash;
    c[ImGuiCol_HeaderHovered] = wash_hi;
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = line_hi;
    c[ImGuiCol_SeparatorActive] = accent_hi;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_ResizeGripActive] = accent_hi;
    c[ImGuiCol_TabHovered] = wash_hi;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabSelected] = accent_lo;
    c[ImGuiCol_TabSelectedOverline] = accent_hi;
    c[ImGuiCol_TabDimmed] = panel;
    c[ImGuiCol_TabDimmedSelected] = field_hi;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_PlotLines] = accent_hi;
    c[ImGuiCol_PlotLinesHovered] = accent_hi;
    c[ImGuiCol_PlotHistogram] = accent; // ProgressBar fill
    c[ImGuiCol_PlotHistogramHovered] = accent_hi;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.129F, 0.141F, 0.165F, 1.00F);
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.153F, 0.165F, 0.188F, 1.00F);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0F, 1.0F, 1.0F, 0.015F);
    c[ImGuiCol_TextLink] = accent_hi;
    c[ImGuiCol_TextSelectedBg] = wash_hi;
    c[ImGuiCol_DragDropTarget] = accent_hi;
    c[ImGuiCol_NavCursor] = accent_hi;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0F, 1.0F, 1.0F, 0.70F);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.45F);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.55F);
  }

  // Every UI font is one family: Arial for Latin, with the same CJK face merged
  // in so Chinese never lands on a font that lacks it. Sizes and weights stay
  // per-use (16 body, 16 bold, 20 titles).
  static const struct {
    const char *path;
    int face;
  } cjk_fonts[] = {
      // PingFang is the current macOS Chinese face, but here it is a
      // downloadable asset: it lives under an AssetsV2 hash rather than the
      // stable Fonts directory. Stable path first, hash second, and Hiragino
      // (older, always present) as the fallback. ponytail: an OS update that
      // rehashes the asset silently falls back to Hiragino -- cosmetic only.
      {"/System/Library/Fonts/PingFang.ttc", 3}, // PingFang SC / Regular
      {"/System/Library/AssetsV2/com_apple_MobileAsset_Font7/"
       "3419f2a427639ad8c8e139149a287865a90fa17e.asset/AssetData/PingFang.ttc",
       3},
      {"/System/Library/Fonts/Hiragino Sans GB.ttc", 0},
      {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 0},
  };

  // ImGui's ChineseFull set stops at the CJK blocks, so Unicode's Arrows block
  // (→, ←, ⇒) is never asked for and draws as nothing, even though both CJK
  // faces above have the glyph. Appended to ImGui's own list rather than copied,
  // so it cannot drift from it. `static` because ImGui keeps the pointer until
  // the atlas is built, not just for the call.
  static const std::vector<ImWchar> cjk_ranges = [&io] {
    std::vector<ImWchar> r;
    for (const ImWchar *p = io.Fonts->GetGlyphRangesChineseFull();
         p[0] != 0 && p[1] != 0; p += 2) {
      r.push_back(p[0]);
      r.push_back(p[1]);
    }
    r.push_back(0x2190); // Arrows
    r.push_back(0x21FF);
    r.push_back(0); // zero-terminated, as ImGui requires
    r.push_back(0);
    return r;
  }();

  // MergeMode appends to the font added just before it, so each merge is issued
  // right after its own base font. AddFontFromFileTTF asserts on a missing file,
  // so the CJK candidate is probed first. The full set (~21k hanzi) costs a few
  // tens of MB of atlas, but the 2500 common set left replies drawing "?" for
  // everyday words ("screen" is U+5C4F, outside it).
  //
  // `_cjk` is false for the 20px title only: the full set at 20px doubles the
  // atlas to 4096x16384 (+320 MB RSS, +128 MB VRAM) for strings that are all
  // ASCII today. A Chinese title would draw "?"; add the merge then if it
  // becomes worth that.
  auto load_font = [&io](const char *_path, float _size, bool _cjk,
                         int _face) -> ImFont * {
    ImFontConfig base;
    base.FontNo = _face; // a .ttc holds several faces
    ImFont *font = io.Fonts->AddFontFromFileTTF(_path, _size, &base);
    if (font == nullptr || !_cjk) {
      return font;
    }
    for (const auto &cjk : cjk_fonts) {
      std::error_code ec;
      if (!std::filesystem::exists(cjk.path, ec)) {
        continue;
      }
      ImFontConfig merge;
      merge.MergeMode = true; // add to that font instead of replacing it
      merge.FontNo = cjk.face; // a .ttc holds several faces
      io.Fonts->AddFontFromFileTTF(cjk.path, _size, &merge, cjk_ranges.data());
      break;
    }
    return font;
  };

  // Arial for Latin. San Francisco is unusable: it is a variable font and
  // ImGui's rasterizer ignores variation axes, so every weight -- titles
  // included -- would draw Regular. (Helvetica Neue has real faces if that
  // trade is ever worth making.)
  ImFont *font_body = load_font("/System/Library/Fonts/Supplemental/Arial.ttf",
                                16.0F, true, 0);
  if (font_body == nullptr) {
    font_body = io.Fonts->AddFontDefault();
  }
  ImFont *font_bold = load_font(
      "/System/Library/Fonts/Supplemental/Arial Bold.ttf", 16.0F, true, 0);
  if (font_bold == nullptr) {
    font_bold = font_body;
  }
  ImFont *font_title = load_font(
      "/System/Library/Fonts/Supplemental/Arial Bold.ttf", 20.0F, false, 0);
  if (font_title == nullptr) {
    font_title = font_bold;
  }
  llm_ui_set_bold_font(font_bold);

  // The script editor is a character grid, so it needs one width for every
  // glyph: with a proportional face the caret and the selection drift off the
  // text. CJK is merged in for the same reason as the other faces -- comments
  // in a script are written in whatever language its author thinks in.
  // Probed like the CJK faces below: AddFontFromFileTTF asserts on a missing
  // file, and this one is not guaranteed on every machine. The full merge
  // doubles the atlas to 4096x16384 (measured); the 2500 common set would keep
  // it at 4096x8192 but draw "?" for hanzi outside it, which the editor hits
  // while typing, not only while drawing.
  ImFont *font_mono = nullptr;
  const char *mono_path = "/System/Library/Fonts/Menlo.ttc";
  std::error_code mono_ec;
  if (std::filesystem::exists(mono_path, mono_ec)) {
    font_mono = load_font(mono_path, 16.0F, true, 0);
  }
  script_ui_set_mono_font(font_mono);

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);
#if defined(__APPLE__)
  // Points the IME candidate window at the caret ImGui publishes, and keeps a
  // live composition out of ImGui's hands.
  macos_ime_attach(window);
#endif

  append_log("[ui] cfg=" + cfg_path.string());
  append_log("[ui] script combo: switching logs that script snapshot to the "
             "log pane.");

  static char adb_path_buf[512]{};
  static char serial_buf[256]{};
  static char adb_connect_buf[256]{};
  static char mumutool_buf[512]{};
  static int mumu_index_buf = 0;
  static bool buffers_init = false;

  campcat::ccat_script_profile ccat_ui = snapshot_ccat_live();

  // Tracks the last active script id so the combo handler can detect real
  // switches; seeded from the loaded config on the first frame.
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
      std::snprintf(mumutool_buf, sizeof(mumutool_buf), "%s",
                    cfg_view.mumutool_path.c_str());
      mumu_index_buf = cfg_view.mumu_device_index;
      last_script_seen = cfg_view.active_script_id;
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
                                   serial_buf, adb_connect_buf, mumutool_buf,
                                   mumu_index_buf);
          // Persist first (outside cfg_mu); commit to memory only on success so
          // a failed write cannot desync the automation config from disk.
          const bool ok =
              persist_bundle_to_disk(to_write, ccat_ui, cfg_path, &err);
          if (ok) {
            {
              std::lock_guard<std::mutex> lk(cfg_mu);
              cfg = to_write;
              ccat_live = ccat_ui;
            }
            append_log("[ui] saved to shell: " + cfg_path.string());
            const auto pj_vis = to_write.resolve_script_json(
                std::string{shell_sid::k_ccat_script});
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

    enum class shell_page : int { Script = 0, Llm, Console, Settings };
    static shell_page page = shell_page::Script;
    static double min_th = 0.35;
    static double max_th = 1.0;

    auto push_job_snapshot = [&]() {
      shell_merge_buffer_paths(&cfg_view, cfg_view, adb_path_buf, serial_buf,
                               adb_connect_buf, mumutool_buf, mumu_index_buf);
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

    constexpr float k_left_w = 320.0F;
    static bool log_open = true;
    const float body_h = ImGui::GetContentRegionAvail().y;

    if (ImGui::BeginTable(
            "shell_layout", 2,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoPadOuterX |
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_SizingStretchProp,
            ImVec2(-1.0F, body_h))) {
      ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed,
                              k_left_w);
      ImGui::TableSetupColumn("content", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableNextRow(ImGuiTableRowFlags_None, body_h);
      ImGui::TableSetColumnIndex(0);

      ImGui::BeginChild("shell_nav", ImVec2(0.0F, 0.0F),
                        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
      ImGui::Spacing();
      ImGui::PushFont(font_title);
      ImGui::TextUnformatted("CampCat");
      ImGui::PopFont();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      nav_item("Script", shell_page::Script);
      nav_item("LLM", shell_page::Llm);
      nav_item("Console", shell_page::Console);
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      nav_item("Settings", shell_page::Settings);
      ImGui::EndChild();

      // Negative height consumes whatever the nav block left in the column;
      // collapsed, the box shrinks to its header instead.
      ImGui::BeginChild(
          "shell_log",
          ImVec2(0.0F,
                 log_open ? -ImGui::GetStyle().ItemSpacing.y : 0.0F),
          ImGuiChildFlags_Borders |
              (log_open ? 0 : ImGuiChildFlags_AutoResizeY));
      ImGui::PushFont(font_bold);
      if (ImGui::Button(log_open ? "[-] Log" : "[+] Log")) {
        log_open = !log_open;
      }
      ImGui::PopFont();
      ImGui::Separator();
      if (log_open) {
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
      }
      ImGui::EndChild();

      ImGui::TableSetColumnIndex(1);
      ImGui::BeginChild("shell_content", ImVec2(0.0F, 0.0F),
                        ImGuiChildFlags_Borders);

      {
        const char *page_title = "Script";
        switch (page) {
        case shell_page::Llm:
          page_title = "LLM";
          break;
        case shell_page::Console:
          page_title = "Console";
          break;
        case shell_page::Settings:
          page_title = "Settings";
          break;
        case shell_page::Script:
        default:
          page_title = "Script";
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

      if (page == shell_page::Script) {
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

        ImGui::SeparatorText("Scheduler");
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

        std::vector<std::string> script_ids;
        script_ids.reserve(cfg_view.script_config_paths.size() + 1);
        script_ids.push_back("none");
        for (const auto &kv : cfg_view.script_config_paths) {
          script_ids.push_back(kv.first);
        }
        std::sort(script_ids.begin() + 1, script_ids.end());

        const std::string preview_str =
            script_display_label(cfg_view.active_script_id);
        ImGui::SeparatorText("Active script");
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
          // Both panels configure the script picked above, so they sit one
          // level in instead of flushing left with the page title. Indented
          // before the Spacing they each draw, so the gap stays outside.
          ImGui::Indent();

          ImGui::PushFont(font_bold);
          const bool paths_open = ImGui::CollapsingHeader(
              "Paths###script_ccat_paths", ImGuiTreeNodeFlags_DefaultOpen);
          ImGui::PopFont();
          if (paths_open) {
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

          ImGui::PushFont(font_bold);
          const bool cap_open = ImGui::CollapsingHeader(
              "Screenshot crop (ADB)###script_ccat_cap",
              ImGuiTreeNodeFlags_DefaultOpen);
          ImGui::PopFont();
          if (cap_open) {
            template_capture_draw_panel(cfg_view, &ccat_ui,
                                        lock_manual_single);
          }
          ImGui::Unindent();
        } else {
          ImGui::TextDisabled("Choose CampCat to edit script paths.");
        }
      } else if (page == shell_page::Llm) {
        llm_ui_draw_panel(cfg_view, lock_manual_single);
      } else if (page == shell_page::Console) {
        ImGui::TextWrapped(
            "Interactive .ccat console. Multiline uses ... until braces/parens "
            "balance. Disabled while a cycle is running.");
        ImGui::Spacing();

        ImGui::BeginChild(
            "console_history",
            ImVec2(0.0F, -ImGui::GetFrameHeightWithSpacing() * 2.5F),
            ImGuiChildFlags_Borders);
        // An empty bordered box reads as a drawing glitch rather than as "no
        // output yet". Offset and centred the way the LLM log places its own
        // empty state, so the two panes read the same at rest.
        if (console_repl.history.empty()) {
          const char *title = "No output yet";
          const char *hint = "Send some ccat script code for execution.";
          const ImVec2 avail = ImGui::GetContentRegionAvail();
          ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.4F);
          ImGui::PushFont(font_bold);
          ImGui::SetCursorPosX(
              ImGui::GetCursorPosX() +
              std::max(0.0F, (avail.x - ImGui::CalcTextSize(title).x) * 0.5F));
          ImGui::TextUnformatted(title);
          ImGui::PopFont();
          ImGui::SetCursorPosX(
              ImGui::GetCursorPosX() +
              std::max(0.0F, (avail.x - ImGui::CalcTextSize(hint).x) * 0.5F));
          ImGui::TextDisabled("%s", hint);
        }
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
        const bool submit = prompt_input_text(
            "##console_in", nullptr, console_input, sizeof(console_input));
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

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("ADB###cfg_adb",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::InputText("ADB path", adb_path_buf,
                           IM_ARRAYSIZE(adb_path_buf));
          ImGui::InputText("Device serial", serial_buf,
                           IM_ARRAYSIZE(serial_buf));
          ImGui::InputText("Connect address", adb_connect_buf,
                           IM_ARRAYSIZE(adb_connect_buf));
          ImGui::InputText("MuMu tool", mumutool_buf,
                           IM_ARRAYSIZE(mumutool_buf));
          ImGui::InputInt("Instance index", &mumu_index_buf);
          // The button starts a VM, which takes a while; the wait runs on the
          // worker so the frame keeps drawing while it happens.
          const bool launching = mumu_launching.load();
          if (launching) {
            ImGui::BeginDisabled();
          }
          const bool start_clicked = ImGui::Button("Start emulator");
          if (launching) {
            ImGui::EndDisabled();
          }
          if (start_clicked) {
            start_mumu_job();
          }
          ImGui::SameLine();
          {
            std::lock_guard<std::mutex> lk(mumu_mu);
            std::string status_line = mumu_state_label(mumu_report.state);
            if (!mumu_report.message.empty() &&
                mumu_report.message != status_line) {
              status_line += " - " + mumu_report.message;
            }
            ImGui::TextDisabled("%s", status_line.c_str());
          }
        }

        ImGui::Spacing();
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
              "Arm the scheduler from the Script page with Start schedule.");
        }
      }

      ImGui::EndChild();
      ImGui::EndTable();
    }

    ImGui::End();

    // Top level, not inside the shell window: the user may switch pages while
    // correcting a box, and the window must survive that.
    llm_ui_draw_overlay();

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
                               adb_connect_buf, mumutool_buf, mumu_index_buf);
      ccat_live = ccat_ui;
    }
  }

  stop_requested.store(true);
  scheduler.stop();

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  // Close the instance this run started, quit the app it started, then drop the
  // watchdog so it cannot race us. Order is load-bearing: with the app gone,
  // mumutool cannot close anything. Dropping the watchdog last keeps it
  // covering a death mid-close, and both of its steps are idempotent, so a
  // concurrent fire is harmless. Both names stay empty when we started nothing,
  // which is what keeps somebody else's instance and app alive.
  {
    std::lock_guard<std::mutex> lk(mumu_mu);
    if (mumu_launched_index >= 0) {
      campcat::mumu_close(mumu_launched_path, mumu_launched_index);
    }
    if (!mumu_app_started.empty()) {
      campcat::mumu_quit_app(mumu_app_started);
    }
    mumu_watchdog.reset();
  }

  campcat::automation_log::clear_sink();

  template_capture_shutdown_gl();
  llm_ui_shutdown_gl();
  script_ui_shutdown_gl(); // preview textures, before the context goes
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();
  // After the window is gone: this waits for the last memory write, and a wait
  // nobody can see is the whole point of splitting shutdown in two.
  llm_ui_shutdown_finish();
  return 0;
}
