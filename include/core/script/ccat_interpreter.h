#pragma once

#include "core/automation/game_automation.h"
#include "core/script/ccat_ast.h"

#include "core/template_matcher.h"

#include <opencv2/core.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace campcat {

class adb_client;
struct app_config;

namespace ccat_lang {

/**
 * @brief Executes an already parsed Program against adb screencap/matcher knobs.
 */
class CcatInterpreter {
public:
  /**
   * @brief Bind automation primitives reused across DSL ops (threshold lives inside `_matcher`).
   * @param[in] _adb Live adb façade polling screenshots/taps.
   * @param[in] _cfg Shell knobs controlling matcher thresholds plus pacing hints.
   * @param[in] _matcher Initialized matcher honoring `_cfg` multiscale/threshold intent.
   * @param[in] _images_base Directory resolving relative PNG operands unless overridden elsewhere.
   * @param[in] _log Channel surfaced alongside worker-thread automation diagnostics.
   */
  CcatInterpreter(adb_client *_adb, const app_config *_cfg,
                  template_matcher _matcher,
                  std::filesystem::path _images_base,
                  game_automation::log_fn _log);

  /**
   * @brief Interpret successive statements until completion or first abnormal outcome.
   * @param[in] _program Parsed `.ccat` AST executing sequential semantics each iteration.
   * @param[in] _should_stop Cooperative cancellation hook polled inside waits/screenshots.
   * @return Aggregate automation_cycle_result mirroring classic shell worker telemetry.
   */
  automation_cycle_result run(const Program &_program,
                              const std::function<bool()> &_should_stop);

private:
  /**
   * @brief Dispatch polymorphic Stmt variants recursively honoring `_should_stop`.
   * @param[in] _stmt Nullable subtree pointer (ignored when nullptr).
   * @param[in] _should_stop Cooperative cancel predicate mirrored from outer worker thread.
   * @return std::nullopt when subtree succeeds; otherwise textual abort reason (also `"stopped"`).
   */
  std::optional<std::string>
  exec_stmt(const Stmt *_stmt, const std::function<bool()> &_should_stop);

  /**
   * @brief Acquire newest framebuffer PNG honoring cooperative cancellation edges.
   * @param[out] _out Destination BGR Mat populated via adb_client pipeline.
   * @param[in] _should_stop Checked before/after subprocess-heavy screenshot plumbing.
   * @return False when screenshot machinery fails or cancellation asserted mid-flight.
   */
  bool capture_screen(cv::Mat *_out, const std::function<bool()> &_should_stop);

  /**
   * @brief Match `_rel_path` template against freshly captured frame (threshold inside matcher).
   * @param[in] _rel_path Relative path merged against bundled images_base unless absolute.
   * @param[in] _should_stop Cooperative cancellation piped through capture_screen internals.
   * @param[out] _found Populated true when correlation surpasses configured matcher threshold.
   * @return False only when screencap plumbing aborts; unreadable PNG yields *_found=false yet true return.
   */
  bool image_matches(const std::string &_rel_path,
                     const std::function<bool()> &_should_stop, bool *_found);

  /**
   * @brief Locate `_rel_path`, tap correlation peak, then honor tap pacing hints from `_cfg`.
   * @param[in] _rel_path Template operand identical semantics as image_matches.
   * @param[in] _should_stop Cooperative cancellation mirrored across capture/tap phases.
   * @return std::nullopt when tap succeeds; descriptive failure otherwise (including adb tap errors).
   */
  std::optional<std::string>
  tap_image(const std::string &_rel_path,
            const std::function<bool()> &_should_stop);

  /**
   * @brief Normalize operand paths combining caller-provided images_base vs absolute overrides.
   * @param[in] _rel Possibly dotted relative filename or platform-specific absolute path token.
   * @return Fully qualified filesystem path forwarded into OpenCV/template matcher layers.
   */
  std::filesystem::path resolve_image_path(const std::string &_rel) const;

  std::optional<std::string>
  swipe_templates_impl(const std::string &_from_rel,
                       const std::string &_to_rel,
                       const std::function<bool()> &_should_stop);

  std::optional<std::string>
  tap_at_impl(double _nx, double _ny,
              const std::function<bool()> &_should_stop);

  std::optional<std::string>
  swipe_at_impl(double _x1, double _y1, double _x2, double _y2,
                const std::function<bool()> &_should_stop);

  std::optional<std::string>
  wait_until_impl(const std::string &_rel_path, int _timeout_ms,
                  const std::function<bool()> &_should_stop);

  adb_client *m_adb;
  const app_config *m_cfg;
  template_matcher m_matcher;
  std::filesystem::path m_images_base;
  game_automation::log_fn m_log;
};

} // namespace ccat_lang
} // namespace campcat
