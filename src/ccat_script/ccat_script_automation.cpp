#include "ccat_script/ccat_script_automation.h"

#include "ccat_script/ccat_script_profile.h"
#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "core/script/ccat_interpreter.h"
#include "core/script/ccat_parser.h"
#include "core/template_matcher.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace campcat {

namespace {

std::string load_ccat_source(const app_config &_cfg,
                             const ccat_script_profile &_prof) {
  std::error_code ec;
  const std::filesystem::path script_path = _cfg.config_home / _prof.source_rel;
  const std::filesystem::path canon =
      std::filesystem::weakly_canonical(script_path, ec);
  const std::filesystem::path resolved =
      ec ? script_path.lexically_normal() : canon;

  std::ifstream in(resolved, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

} // namespace

ccat_script_automation::ccat_script_automation(adb_client *_adb,
                                               const app_config *_cfg,
                                               const ccat_script_profile *_profile)
    : m_adb(_adb), m_cfg(_cfg), m_profile(_profile) {
  if (!m_adb || !m_cfg || !m_profile) {
    throw std::invalid_argument(
        "ccat_script_automation requires adb, cfg, ccat_script_profile");
  }
}

automation_cycle_result ccat_script_automation::run_cycle(
    const std::function<bool()> &_should_stop) {
  automation_cycle_result r{};
  const std::string source = load_ccat_source(*m_cfg, *m_profile);
  if (source.empty()) {
    std::ostringstream oss;
    oss << "cannot read script file under config_home: "
        << (m_cfg->config_home / m_profile->source_rel).string();
    r.ok = false;
    r.message = oss.str();
    automation_log::emit(std::string("[ccat] ") + r.message);
    return r;
  }

  std::unique_ptr<ccat_lang::Program> prog;
  try {
    prog = ccat_lang::parse_program(source);
  } catch (const ccat_lang::parse_error &ex) {
    std::ostringstream oss;
    oss << "parse error: " << ex.what() << " (line " << ex.line << ", col "
        << ex.col << ")";
    r.ok = false;
    r.message = oss.str();
    automation_log::emit(std::string("[ccat] ") + r.message);
    return r;
  }

  std::error_code ec;
  const std::filesystem::path script_path =
      m_cfg->config_home / m_profile->source_rel;
  const std::filesystem::path canon =
      std::filesystem::weakly_canonical(script_path, ec);
  const std::filesystem::path script_abs =
      ec ? script_path.lexically_normal() : canon;

  template_matcher matcher(m_cfg->match_threshold, m_cfg->match_multiscale);
  ccat_lang::CcatInterpreter interp(m_adb, m_cfg, std::move(matcher),
                                    m_profile->images_base(m_cfg->config_home),
                                    script_abs);
  return interp.run(*prog, _should_stop);
}

} // namespace campcat
