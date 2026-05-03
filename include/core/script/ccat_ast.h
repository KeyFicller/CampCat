#pragma once

#include <memory>
#include <string>
#include <vector>

namespace campcat::ccat_lang {

/**
 * @brief Base node for one executable statement in a .ccat program.
 */
struct Stmt {
  virtual ~Stmt() = default;
};

/**
 * @brief Sequence of statements inside `{ ... }`.
 */
struct BlockStmt final : Stmt {
  std::vector<std::unique_ptr<Stmt>> body;
};

/**
 * @brief Conditional branch on template visibility (`if (image)` … optional `else`).
 */
struct IfStmt final : Stmt {
  /** @brief Relative or absolute template path from DSL semantics. */
  std::string image_path;
  std::unique_ptr<Stmt> then_branch;
  std::unique_ptr<Stmt> else_branch;
};

/**
 * @brief Tap the screen at best template match center (`tap(image)`).
 */
struct TapStmt final : Stmt {
  /** @brief Relative or absolute template path from DSL semantics. */
  std::string image_path;
};

/**
 * @brief Cooperative delay (`wait(ms)`).
 */
struct WaitStmt final : Stmt {
  /** @brief Sleep duration in milliseconds (non-negative). */
  int milliseconds = 0;
};

/**
 * @brief Emit one line to the automation log callback (`log(msg)`).
 */
struct LogStmt final : Stmt {
  /** @brief User-visible message payload (already decoded from DSL syntax). */
  std::string message;
};

/**
 * @brief Root AST container — ordered top-level statements of one parsed script.
 */
struct Program {
  std::vector<std::unique_ptr<Stmt>> stmts;
};

} // namespace campcat::ccat_lang
