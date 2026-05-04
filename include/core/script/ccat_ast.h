#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace campcat::ccat_lang {

class CcatInterpreter;

/**
 * @brief Base node for one executable statement in a .ccat program.
 */
struct Stmt {
  virtual ~Stmt() = default;

  virtual std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const = 0;
};

/**
 * @brief Sequence of statements inside `{ ... }`.
 */
struct BlockStmt final : Stmt {
  std::vector<std::unique_ptr<Stmt>> body;

  std::optional<std::string>
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

  std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Tap the screen at best template match center (`tap(image)`).
 */
struct TapStmt final : Stmt {
  /** @brief Relative or absolute template path from DSL semantics. */
  std::string image_path;

  std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Cooperative delay (`wait(ms)`).
 */
struct WaitStmt final : Stmt {
  /** @brief Sleep duration in milliseconds (non-negative). */
  int milliseconds = 0;

  std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Emit one line to the automation log callback (`log(msg)`).
 */
struct LogStmt final : Stmt {
  /** @brief User-visible message payload (already decoded from DSL syntax). */
  std::string message;

  std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Swipe from first template match center to second on one screenshot.
 */
struct SwipeTemplatesStmt final : Stmt {
  std::string from_image_path;
  std::string to_image_path;

  std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Tap at normalized coordinates `[0,1]×[0,1]` mapping to current screenshot size.
 */
struct TapAtStmt final : Stmt {
  double nx = 0;
  double ny = 0;

  std::optional<std::string>
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

  std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Poll until `image_path` matches or `timeout_ms` elapses.
 */
struct WaitUntilStmt final : Stmt {
  std::string image_path;
  int timeout_ms = 0;

  std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Execute `body` up to `attempts` times until it succeeds (no error).
 */
struct RetryStmt final : Stmt {
  int attempts = 0;
  std::unique_ptr<Stmt> body;

  std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Run `body` once then repeat while `condition_image_path` is visible.
 */
struct DoWhileStmt final : Stmt {
  std::unique_ptr<BlockStmt> body;
  std::string condition_image_path;

  std::optional<std::string>
  exec(CcatInterpreter &_interp,
       const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Execute `body` exactly `repetitions` times (`loop(n) { ... }`).
 */
struct LoopStmt final : Stmt {
  int repetitions = 0;
  std::unique_ptr<BlockStmt> body;

  std::optional<std::string>
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
