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
 * @brief Why an action failed. The `device` flag is the distinction that lets a
 * condition tell a broken device from a screen state: a screenshot that never
 * happened must not read as `false` and quietly take the `else` branch.
 */
struct action_failure {
    std::string message;
    bool device = false;

    /// One argument (a screen-state failure) converts implicitly, which is the
    /// common case, so the existing `return oss.str();` sites stay as they are.
    /// The `device` tag is for the screenshot that never happened.
    action_failure(std::string _message, bool _device = false)
        : message(std::move(_message)), device(_device) {}
};

/**
 * @brief Control-flow result from executing one Stmt subtree (success, loop
 * break, script return, cooperative stop, or error message).
 */
struct stmt_exec_outcome {
    enum class tag { ok, break_loop, returned, stopped, error };

    tag kind = tag::ok;
    std::string message;
    /// The script's own answer. Set on `returned`, and on the `ok` a `run`
    /// produces for a callee that answered false -- riding `ok` is what keeps
    /// that answer from being read as an error and stopping the caller. True
    /// everywhere else: an action reports "it worked" through `kind`.
    bool value = true;
    /// Set only alongside `tag::error`: the device itself failed, so a condition
    /// must not swallow it.
    bool device = false;

    bool is_ok() const { return kind == tag::ok; }

    /// `_value` is the script's result: a bare `return` ends it with true. A
    /// `has` that did not find its template is the other caller, with false.
    static stmt_exec_outcome make_ok(bool _value = true) {
        stmt_exec_outcome o;
        o.value = _value;
        return o;
    }

    static stmt_exec_outcome make_break() {
        stmt_exec_outcome o;
        o.kind = tag::break_loop;
        return o;
    }

    /// `_value` is the script's result: a bare `return` ends it with true.
    static stmt_exec_outcome make_returned(bool _value = true) {
        stmt_exec_outcome o;
        o.kind = tag::returned;
        o.value = _value;
        return o;
    }

    static stmt_exec_outcome make_stopped() {
        stmt_exec_outcome o;
        o.kind = tag::stopped;
        o.message = "stopped";
        return o;
    }

    /// `_device` marks a failure of the device rather than of the screen, which
    /// only a condition reads.
    static stmt_exec_outcome make_error(std::string _msg,
                                        bool _device = false) {
        stmt_exec_outcome o;
        o.kind = tag::error;
        o.message = std::move(_msg);
        o.device = _device;
        return o;
    }
};

inline stmt_exec_outcome
outcome_from_opt(std::optional<action_failure> &&_maybe_err) {
    if (!_maybe_err) {
        return stmt_exec_outcome::make_ok();
    }
    if (_maybe_err->message == "stopped") {
        return stmt_exec_outcome::make_stopped();
    }
    return stmt_exec_outcome::make_error(std::move(_maybe_err->message),
                                         _maybe_err->device);
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
 * @brief Conditional branch on a statement's own result (`if (...) ... else`)
 * or on `has(...)`.
 */
struct IfStmt final : Stmt {
    /** @brief The condition, as a statement whose own result decides the branch:
     * one of the seven result actions (`if (tap("a.png"))`) or `has("a.png")`.
     *
     * Never null. A literal or bool-def condition is folded away at parse time,
     * so an `IfStmt` that reaches the tree always has something to evaluate.
     *
     * A plain `Stmt` on purpose -- the node really is a `TapStmt` /
     * `TapOffsetStmt` / `SwipeTemplatesStmt` / `TapAtStmt` / `SwipeAtStmt` /
     * `WaitUntilStmt` / `RunStmt` / `HasStmt`, so the walks that collect
     * templates and run() targets still find it. */
    std::unique_ptr<Stmt> action;
    std::unique_ptr<Stmt> then_branch;
    std::unique_ptr<Stmt> else_branch;

    stmt_exec_outcome
    exec(CcatInterpreter &_interp,
         const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Exit the innermost enclosing `loop` or `do`/`while`.
 */
struct BreakStmt final : Stmt {
    stmt_exec_outcome
    exec(CcatInterpreter &_interp,
         const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief End the entire script with a result (`return` / `return false`).
 */
struct ReturnStmt final : Stmt {
    /** @brief The script's result: true unless it wrote `return false`. */
    bool value = true;

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
 * @brief Toggle match-debug screenshot dumps (`$Debug On` / `$Debug Off`).
 */
struct DebugStmt final : Stmt {
    bool enabled = false;

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
    /** @brief User-visible message payload (already decoded from DSL syntax).
     */
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
 * @brief Tap at normalized coordinates `[0,1]×[0,1]` mapping to current
 * screenshot size.
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
 * @brief Swipe between normalized endpoints `(x1,y1)→(x2,y2)` on current
 * resolution.
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
 * @brief Test whether `image_path` is on screen right now (`has("a.png")`).
 *
 * Never a statement of its own: the parser accepts it only where a condition
 * belongs. A template that is not there is `false`, not an error.
 */
struct HasStmt final : Stmt {
    std::string image_path;

    stmt_exec_outcome
    exec(CcatInterpreter &_interp,
         const std::function<bool()> &_should_stop) const override;
};

/**
 * @brief Run `body` once then repeat while the condition holds.
 */
struct DoWhileStmt final : Stmt {
    std::unique_ptr<BlockStmt> body;
    /** @brief The action whose own result decides whether to repeat
     * (`do { A } while (tap("next.png"))`), or null. The same eight statement
     * types `IfStmt::action` accepts (`has` plus the seven actions), and
     * `parse_do_while` is what guarantees it. */
    std::unique_ptr<Stmt> condition_action;
    /** @brief The answer when the condition is a literal, which the parser knows
     * without looking at the screen: `while (true)` / `while (false)` / a bool
     * def. Empty when the condition is an action.
     *
     * `std::optional` rather than a plain `bool` so that "not a literal" is
     * expressible at all: when `condition_action` is set, this must be empty.
     *
     * `while (false)` is *not* folded down to its body even though it runs it
     * once: a `break` in the body would then leave the enclosing `loop` instead
     * of this statement. Same reason `while (true)` is not rewritten as
     * `loop(50000)` -- `LoopStmt` ends quietly at its cap while this loop
     * errors. */
    std::optional<bool> condition_literal;

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
 * @brief Root AST container — ordered top-level statements of one parsed
 * script.
 */
struct Program {
    std::vector<std::unique_ptr<Stmt>> stmts;
};

} // namespace campcat::ccat_lang
