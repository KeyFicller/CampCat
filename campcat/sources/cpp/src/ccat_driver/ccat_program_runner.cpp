#include "ccat_driver/ccat_program_runner.h"

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/script/ccat_interpreter.h"
#include "core/template_matcher.h"

namespace campcat {

automation_cycle_result
run_ccat_program(adb_api *_adb, const app_config *_cfg,
                 const std::filesystem::path &_images_base,
                 const std::filesystem::path &_script_path,
                 const ccat_lang::Program &_program,
                 const std::function<bool()> &_should_stop,
                 ccat_lang::ask_handler _ask,
                 std::string _graph_limit) {
    if (!_adb || !_cfg) {
        automation_cycle_result r{};
        r.ok = false;
        r.message = "run_ccat_program: adb/cfg must be non-null";
        return r;
    }
    template_matcher matcher(_cfg->match.box_threshold,
                             _cfg->match.circle_threshold,
                             _cfg->match.multiscale);
    ccat_lang::CcatInterpreter interp(_adb, _cfg, std::move(matcher),
                                      _images_base, _script_path,
                                      std::move(_ask), std::move(_graph_limit));
    return interp.run(_program, _should_stop);
}

ccat_lang::locate_hit locate_on_frame(adb_api *_adb, const app_config *_cfg,
                                      const std::string &_graph,
                                      const cv::Mat &_frame,
                                      std::string *_err) {
    template_matcher matcher(_cfg->match.box_threshold,
                             _cfg->match.circle_threshold,
                             _cfg->match.multiscale);
    ccat_lang::CcatInterpreter interp(_adb, _cfg, std::move(matcher),
                                      _cfg->config_home);
    return interp.locate_frame(_graph, _frame, [] { return false; }, _err);
}

} // namespace campcat
