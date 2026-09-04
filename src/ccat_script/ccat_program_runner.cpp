#include "ccat_script/ccat_program_runner.h"

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/script/ccat_interpreter.h"
#include "core/template_matcher.h"

namespace campcat {

automation_cycle_result run_ccat_program(
    adb_client *_adb, const app_config *_cfg,
    const std::filesystem::path &_images_base,
    const std::filesystem::path &_script_path,
    const ccat_lang::Program &_program,
    const std::function<bool()> &_should_stop) {
  template_matcher matcher(_cfg->match_threshold, _cfg->match_multiscale);
  ccat_lang::CcatInterpreter interp(_adb, _cfg, std::move(matcher),
                                    _images_base, _script_path);
  return interp.run(_program, _should_stop);
}

} // namespace campcat
