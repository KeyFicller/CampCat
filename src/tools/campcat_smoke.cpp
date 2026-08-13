#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "core/script/ccat_interpreter.h"
#include "core/script/ccat_parser.h"
#include "core/template_matcher.h"

namespace {

namespace fs = std::filesystem;

constexpr std::string_view k_marker_pass = "GATE_PASS";
constexpr std::string_view k_marker_fail = "GATE_FAIL";

struct options {
  fs::path config_path = "config/default.json";
  fs::path cases_dir = "tests/smoke/cases";
  std::string only_case;
  bool require_device = false;
};

void print_usage(const char *_argv0) {
  std::cerr
      << "Usage: " << _argv0
      << " [--config path] [--cases-dir path] [--case name] [--require-device]\n"
      << "Env: CAMPCAT_SMOKE_REQUIRE_DEVICE=1 forces failure when no ADB "
         "device.\n";
}

bool parse_args(int _argc, char **_argv, options *_out) {
  for (int i = 1; i < _argc; ++i) {
    const std::string arg = _argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(_argv[0]);
      return false;
    }
    if (arg == "--require-device") {
      _out->require_device = true;
      continue;
    }
    if (arg == "--config" && i + 1 < _argc) {
      _out->config_path = _argv[++i];
      continue;
    }
    if (arg == "--cases-dir" && i + 1 < _argc) {
      _out->cases_dir = _argv[++i];
      continue;
    }
    if (arg == "--case" && i + 1 < _argc) {
      _out->only_case = _argv[++i];
      continue;
    }
    std::cerr << "unknown argument: " << arg << '\n';
    print_usage(_argv[0]);
    return false;
  }
  if (const char *env = std::getenv("CAMPCAT_SMOKE_REQUIRE_DEVICE")) {
    if (env[0] == '1' || env[0] == 't' || env[0] == 'T' || env[0] == 'y' ||
        env[0] == 'Y') {
      _out->require_device = true;
    }
  }
  return true;
}

std::string read_file(const fs::path &_path) {
  std::ifstream in(_path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

std::vector<fs::path> discover_cases(const fs::path &_cases_dir,
                                     const std::string &_only) {
  std::vector<fs::path> cases;
  if (!fs::is_directory(_cases_dir)) {
    return cases;
  }
  for (const auto &entry : fs::directory_iterator(_cases_dir)) {
    if (!entry.is_directory()) {
      continue;
    }
    const fs::path script = entry.path() / "script.ccat";
    if (!fs::is_regular_file(script)) {
      continue;
    }
    if (!_only.empty() && entry.path().filename().string() != _only) {
      continue;
    }
    cases.push_back(entry.path());
  }
  std::sort(cases.begin(), cases.end());
  return cases;
}

enum class case_verdict { pass, fail, error };

case_verdict judge_logs(const std::vector<std::string> &_lines) {
  bool saw_pass = false;
  bool saw_fail = false;
  for (const auto &line : _lines) {
    if (line.find(k_marker_fail) != std::string::npos) {
      saw_fail = true;
    }
    if (line.find(k_marker_pass) != std::string::npos) {
      saw_pass = true;
    }
  }
  if (saw_fail) {
    return case_verdict::fail;
  }
  if (saw_pass) {
    return case_verdict::pass;
  }
  return case_verdict::fail;
}

const char *verdict_name(case_verdict _v) {
  switch (_v) {
  case case_verdict::pass:
    return "PASS";
  case case_verdict::fail:
    return "FAIL";
  case case_verdict::error:
    return "ERROR";
  }
  return "?";
}

} // namespace

int main(int argc, char **argv) {
  options opt;
  if (!parse_args(argc, argv, &opt)) {
    return 2;
  }

  const campcat::app_config cfg =
      campcat::app_config::load_from_file(opt.config_path);

  std::mutex log_mu;
  std::vector<std::string> case_logs;
  campcat::automation_log::set_sink([&](const std::string &line) {
    std::lock_guard<std::mutex> lk(log_mu);
    case_logs.push_back(line);
    std::cout << "  " << line << '\n';
  });

  campcat::adb_client adb(cfg.adb_path, cfg.adb_serial);
  {
    std::string co;
    std::string ce;
    (void)adb.connect_remote(cfg.adb_connect_address, 15000, &co, &ce);
  }

  const auto devices = adb.list_devices();
  if (devices.empty()) {
    campcat::automation_log::clear_sink();
    if (opt.require_device) {
      std::cerr << "[smoke] no ADB device (require-device)\n";
      return 1;
    }
    std::cout << "[smoke] no ADB device — skipping cases\n";
    return 0;
  }

  std::cout << "[smoke] devices:";
  for (const auto &d : devices) {
    std::cout << ' ' << d;
  }
  std::cout << '\n';

  const auto cases = discover_cases(opt.cases_dir, opt.only_case);
  if (cases.empty()) {
    campcat::automation_log::clear_sink();
    std::cerr << "[smoke] no cases under " << opt.cases_dir.string() << '\n';
    return 1;
  }

  int failed = 0;
  for (const fs::path &case_dir : cases) {
    const std::string name = case_dir.filename().string();
    const fs::path script_path = case_dir / "script.ccat";
    std::cout << "[smoke] case " << name << '\n';

    {
      std::lock_guard<std::mutex> lk(log_mu);
      case_logs.clear();
    }

    const std::string source = read_file(script_path);
    if (source.empty()) {
      std::cout << "[smoke] " << name << " ERROR cannot read script\n";
      ++failed;
      continue;
    }

    case_verdict verdict = case_verdict::error;
    try {
      auto prog = campcat::ccat_lang::parse_program(source);
      campcat::template_matcher matcher(cfg.match_threshold,
                                        cfg.match_multiscale);
      // PNGs resolve beside script.ccat (case directory).
      campcat::ccat_lang::CcatInterpreter interp(&adb, &cfg, std::move(matcher),
                                                 case_dir);
      const auto res = interp.run(*prog, [] { return false; });
      std::vector<std::string> lines;
      {
        std::lock_guard<std::mutex> lk(log_mu);
        lines = case_logs;
      }
      verdict = judge_logs(lines);
      if (verdict == case_verdict::pass && !res.ok) {
        // Markers win; still note soft interpreter failure in message.
        std::cout << "  [smoke] note: interpreter ok=false msg=" << res.message
                  << '\n';
      }
      if (verdict != case_verdict::pass && !res.ok) {
        std::cout << "  [smoke] interpreter: " << res.message << '\n';
      }
    } catch (const campcat::ccat_lang::parse_error &ex) {
      std::cout << "  parse error: " << ex.what() << " (line " << ex.line
                << ", col " << ex.col << ")\n";
      verdict = case_verdict::error;
    } catch (const std::exception &ex) {
      std::cout << "  exception: " << ex.what() << '\n';
      verdict = case_verdict::error;
    }

    std::cout << "[smoke] " << name << ' ' << verdict_name(verdict) << '\n';
    if (verdict != case_verdict::pass) {
      ++failed;
    }
  }

  campcat::automation_log::clear_sink();
  if (failed == 0) {
    std::cout << "[smoke] all " << cases.size() << " case(s) passed\n";
    return 0;
  }
  std::cout << "[smoke] " << failed << '/' << cases.size() << " failed\n";
  return 1;
}
