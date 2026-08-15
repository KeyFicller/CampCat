#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace campcat::ccat_lang {

class CcatInterpreter;

/**
 * @brief Control-flow result from executing one Stmt subtree (success, loop break,
 * script return, cooperative stop, or error message).
 */
struct stmt_exec_outcome {
  enum class tag { ok, break_loop, returned, stopped, error };

  tag kind = tag::ok;
  std::string message;

  bool is_ok() const { return kind == tag::ok; }

  static stmt_exec_outcome make_ok() { return {}; }

  static stmt_exec_outcome make_break() {
    stmt_exec_outcome o;
    o.kind = tag::break_loop;
    return o;
  }

  static stmt_exec_outcome make_returned() {
    stmt_exec_outcome o;
    o.kind = tag::returned;
    return o;
  }

  static stmt_exec_outcome make_stopped() {
    stmt_exec_outcome o;
    o.kind = tag::stopped;
    o.message = "stopped";
    return o;
  }

  static stmt_exec_outcome make_error(std::string _msg) {
    stmt_exec_outcome o;
    o.kind = tag::error;
    o.message = std::move(_msg);
    return o;
  }
};

inline stmt_exec_outcome outcome_from_opt(std::optional<std::string> &&_maybe_err) {
  if (!_maybe_err) {
    return stmt_exec_outcome::make_ok();
  }
  return stmt_exec_outcome::make_error(std::move(*_maybe_err));
}

/**
 * @brief Base node for one executable statement in a .ccat program.
 */
struct Stmt {
  virtual ~Stmt() = default;

  virtual stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const = 0;
};

/**
 * @brief Sequence of statements inside `{ ... }`.
 */
struct BlockStmt final : Stmt {
  std::vector<std::unique_ptr<Stmt>> body;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Conditional branch on template visibility (`if (image)` … optional `else`).
 */
struct IfStmt final : Stmt {
  /** @brief Relative or absolute template path from DSL semantics. */
  std::string image_path;
  std::unique_ptr<Stmt> then_branch;
  std::unique_ptr<Stmt> else_branch;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Exit the innermost enclosing `loop`, `do`/`while`, or `retry`.
 */
struct BreakStmt final : Stmt {
  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief End the entire script successfully (`return`).
 */
struct ReturnStmt final : Stmt {
  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Return to launcher and force-stop recent apps (`home`).
 */
struct HomeStmt final : Stmt {
  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Run another `.ccat` relative to the current script directory (`run`).
 */
struct RunStmt final : Stmt {
  std::string script_rel;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Tap the screen at best template match center (`tap(image)`).
 */
struct TapStmt final : Stmt {
  /** @brief Relative or absolute template path from DSL semantics. */
  std::string image_path;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Cooperative delay (`wait(ms)`).
 */
struct WaitStmt final : Stmt {
  /** @brief Sleep duration in milliseconds (non-negative). */
  int milliseconds = 0;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Emit one line to the automation log callback (`log(msg)`).
 */
struct LogStmt final : Stmt {
  /** @brief User-visible message payload (already decoded from DSL syntax). */
  std::string message;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Swipe from first template match center to second on one screenshot.
 */
struct SwipeTemplatesStmt final : Stmt {
  std::string from_image_path;
  std::string to_image_path;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Tap at normalized coordinates `[0,1]×[0,1]` mapping to current screenshot size.
 */
struct TapAtStmt final : Stmt {
  double nx = 0;
  double ny = 0;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Tap template match center plus screen-normalized offset `(dx, dy)`.
 */
struct TapOffsetStmt final : Stmt {
  std::string image_path;
  double dx = 0;
  double dy = 0;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Swipe between normalized endpoints `(x1,y1)→(x2,y2)` on current resolution.
 */
struct SwipeAtStmt final : Stmt {
  double x1 = 0;
  double y1 = 0;
  double x2 = 0;
  double y2 = 0;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Poll until `image_path` matches or `timeout_ms` elapses.
 */
struct WaitUntilStmt final : Stmt {
  std::string image_path;
  int timeout_ms = 0;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Execute `body` up to `attempts` times until it succeeds (no error).
 */
struct RetryStmt final : Stmt {
  int attempts = 0;
  std::unique_ptr<Stmt> body;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Run `body` once then repeat while `condition_image_path` is visible.
 */
struct DoWhileStmt final : Stmt {
  std::unique_ptr<BlockStmt> body;
  std::string condition_image_path;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Execute `body` exactly `repetitions` times (`loop(n) { ... }`).
 */
struct LoopStmt final : Stmt {
  int repetitions = 0;
  std::unique_ptr<BlockStmt> body;

  stmt_exec_outcome
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Root AST container — ordered top-level statements of one parsed script.
 */
struct Program {
  std::vector<std::unique_ptr<Stmt>> stmts;
};

} // namespace campcat::ccat_lang
