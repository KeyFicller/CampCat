// The condition slot: `has(...)`, the seven statements that report a result, an
// `elif` chain, and the shapes that have to keep failing. Parsing only, so none
// of it needs a device.

#include <core/script/ccat_ast.h>
#include <core/script/ccat_parser.h>

#include <gtest/gtest.h>

#include <string>

namespace {

using campcat::ccat_lang::BlockStmt;
using campcat::ccat_lang::DoWhileStmt;
using campcat::ccat_lang::HasStmt;
using campcat::ccat_lang::IfStmt;
using campcat::ccat_lang::LogStmt;
using campcat::ccat_lang::LoopStmt;
using campcat::ccat_lang::parse_error;
using campcat::ccat_lang::parse_program;
using campcat::ccat_lang::RunStmt;
using campcat::ccat_lang::Stmt;
using campcat::ccat_lang::SwipeAtStmt;
using campcat::ccat_lang::SwipeTemplatesStmt;
using campcat::ccat_lang::TapAtStmt;
using campcat::ccat_lang::TapOffsetStmt;
using campcat::ccat_lang::TapStmt;
using campcat::ccat_lang::WaitStmt;
using campcat::ccat_lang::WaitUntilStmt;

/// The message `_src` fails with, or empty when it parses. One call instead of a
/// `try` around every negative case.
std::string parse_failure(const std::string &_src) {
    try {
        (void)parse_program(_src);
    } catch (const parse_error &ex) {
        return ex.what();
    }
    return {};
}

/// Runs `_check` against the condition of the single `if` `_src` parses to. The
/// `Program` stays alive in here, so the reference handed out cannot dangle.
template <typename F> void with_condition(const std::string &_src, F _check) {
    const auto prog = parse_program(_src);
    const auto *i = dynamic_cast<const IfStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(i, nullptr);
    ASSERT_NE(i->action, nullptr);
    _check(*i->action);
}

/// Same, for a `do { ... } while (...)` whose condition is an action.
template <typename F>
void with_while_action(const std::string &_src, F _check) {
    const auto prog = parse_program(_src);
    const auto *d = dynamic_cast<const DoWhileStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(d, nullptr);
    ASSERT_NE(d->condition_action, nullptr);
    _check(*d->condition_action);
}

/// Same, for the whole `do ... while` node -- used where the point is which of
/// its two condition slots got filled.
template <typename F> void with_do_while(const std::string &_src, F _check) {
    const auto prog = parse_program(_src);
    const auto *d = dynamic_cast<const DoWhileStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(d, nullptr);
    _check(*d);
}

TEST(CcatCondAction, EveryStatementThatReportsAResultIsACondition) {
    with_condition("if (has(\"a.png\")) { log(\"y\") }", [](const Stmt &_a) {
        const auto *h = dynamic_cast<const HasStmt *>(&_a);
        ASSERT_NE(h, nullptr);
        EXPECT_EQ(h->image_path, "a.png");
    });
    with_condition("if (tap(\"a.png\")) { log(\"y\") }", [](const Stmt &_a) {
        EXPECT_NE(dynamic_cast<const TapStmt *>(&_a), nullptr);
    });
    with_condition("if (tap_offset(\"a.png\", 0, 1)) { log(\"y\") }",
                   [](const Stmt &_a) {
                       EXPECT_NE(dynamic_cast<const TapOffsetStmt *>(&_a),
                                 nullptr);
                   });
    with_condition("if (swipe(\"a.png\", \"b.png\")) { log(\"y\") }",
                   [](const Stmt &_a) {
                       EXPECT_NE(
                           dynamic_cast<const SwipeTemplatesStmt *>(&_a),
                           nullptr);
                   });
    with_condition("if (tap_at(0.5, 0.5)) { log(\"y\") }", [](const Stmt &_a) {
        EXPECT_NE(dynamic_cast<const TapAtStmt *>(&_a), nullptr);
    });
    with_condition("if (swipe_at(1, 0.5, 0, 0.5)) { log(\"y\") }",
                   [](const Stmt &_a) {
                       EXPECT_NE(dynamic_cast<const SwipeAtStmt *>(&_a),
                                 nullptr);
                   });
    with_condition("if (wait_until(\"a.png\", 500)) { log(\"y\") }",
                   [](const Stmt &_a) {
                       EXPECT_NE(dynamic_cast<const WaitUntilStmt *>(&_a),
                                 nullptr);
                   });
    with_condition("if (run(\"other.ccat\")) { log(\"y\") }", [](const Stmt &_a) {
        EXPECT_NE(dynamic_cast<const RunStmt *>(&_a), nullptr);
    });
}

TEST(CcatCondAction, StatementsThatOnlyEverSucceedCannotBeConditions) {
    // `log` and `wait` always succeed; `home` only fails when the device is gone,
    // which is not a screen state; the rest are control flow, not answers.
    for (const char *src : {"if (log(\"x\")) { log(\"y\") }",
                            "if (wait(10)) { log(\"y\") }",
                            "if (home) { log(\"y\") }",
                            "if (loop(2) { log(\"y\") }) { log(\"z\") }",
                            "if (break) { log(\"y\") }"}) {
        const std::string msg = parse_failure(src);
        EXPECT_NE(msg.find("wait_until"), std::string::npos) << src << ": " << msg;
        EXPECT_NE(msg.find("has"), std::string::npos) << src << ": " << msg;
    }
}

TEST(CcatCondAction, HasCannotStandAloneAsAStatement) {
    const std::string msg = parse_failure("has(\"a.png\")");
    EXPECT_NE(msg.find("if (...) or while (...)"), std::string::npos) << msg;
}

TEST(CcatCondAction, RetryIsNoLongerAKeyword) {
    // Not merely rejected with a name: the word is an ordinary identifier now, so
    // each falls into the grammar's own error rather than a bespoke one.
    EXPECT_FALSE(parse_failure("retry(2) { tap(\"n.png\") }").empty());
    EXPECT_FALSE(parse_failure("if (retry) { log(\"y\") }").empty());
}

TEST(CcatCondAction, APollingLoopReplacesRetry) {
    // The idiom `retry` was written for, in the new spelling.
    const auto prog = parse_program(
        "loop(3) { if (tap_at(0.5, 0.5)) { break } wait(50) }\n");
    const auto *loop = dynamic_cast<const LoopStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(loop, nullptr);
    ASSERT_NE(loop->body, nullptr);
    ASSERT_EQ(loop->body->body.size(), 2u);
    const auto *guard =
        dynamic_cast<const IfStmt *>(loop->body->body.at(0).get());
    ASSERT_NE(guard, nullptr);
    EXPECT_NE(dynamic_cast<const TapAtStmt *>(guard->action.get()), nullptr);
    EXPECT_NE(dynamic_cast<const WaitStmt *>(loop->body->body.at(1).get()),
              nullptr);
}

TEST(CcatCondAction, ABareStringIsNotACondition) {
    EXPECT_FALSE(parse_failure("if (\"a.png\") { log(\"y\") }").empty());
}

TEST(CcatCondAction, AStringDefNeedsHas) {
    const std::string msg =
        parse_failure("defs { s = \"a.png\" }\nif (s) { log(\"y\") }");
    EXPECT_NE(msg.find("has(a)"), std::string::npos) << msg;
}

TEST(CcatCondAction, AnUnknownIdentifierIsNotACondition) {
    EXPECT_FALSE(parse_failure("if (nope) { log(\"y\") }").empty());
}

TEST(CcatCondAction, ElifChainsNest) {
    const auto prog = parse_program(
        "if (tap(\"t1.png\")) { } elif (tap(\"t2.png\")) { } "
        "elif (tap(\"t3.png\")) { } else { }\n");
    const auto *first = dynamic_cast<const IfStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(first, nullptr);
    ASSERT_NE(dynamic_cast<const TapStmt *>(first->action.get()), nullptr);

    const auto *second =
        dynamic_cast<const IfStmt *>(first->else_branch.get());
    ASSERT_NE(second, nullptr);
    ASSERT_NE(dynamic_cast<const TapStmt *>(second->action.get()), nullptr);

    const auto *third = dynamic_cast<const IfStmt *>(second->else_branch.get());
    ASSERT_NE(third, nullptr);
    ASSERT_NE(dynamic_cast<const TapStmt *>(third->action.get()), nullptr);

    // The `else` of the last link, and nothing after it.
    EXPECT_NE(dynamic_cast<const BlockStmt *>(third->else_branch.get()),
              nullptr);
}

TEST(CcatCondAction, ElseIfStillParses) {
    // Both spellings reach `parse_if`, so the older one must not regress.
    const auto prog =
        parse_program("if (tap(\"t1.png\")) { } else if (has(\"b.png\")) { }\n");
    const auto *first = dynamic_cast<const IfStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(first, nullptr);
    const auto *second =
        dynamic_cast<const IfStmt *>(first->else_branch.get());
    ASSERT_NE(second, nullptr);
    EXPECT_NE(dynamic_cast<const HasStmt *>(second->action.get()), nullptr);
}

TEST(CcatCondAction, ElifCannotFollowElse) {
    EXPECT_FALSE(
        parse_failure("if (tap(\"t1.png\")) { } else { } elif (tap(\"t2.png\")) { }")
            .empty());
}

TEST(CcatCondAction, ElifCannotStartAProgram) {
    EXPECT_FALSE(parse_failure("elif (tap(\"t1.png\")) { }\n").empty());
}

TEST(CcatCondAction, DoWhileTakesTheSameConditions) {
    with_while_action("do { log(\"x\") } while (tap(\"n.png\"))",
                      [](const Stmt &_a) {
                          EXPECT_NE(dynamic_cast<const TapStmt *>(&_a), nullptr);
                      });
    with_while_action("do { log(\"x\") } while (has(\"n.png\"))",
                      [](const Stmt &_a) {
                          EXPECT_NE(dynamic_cast<const HasStmt *>(&_a),
                                    nullptr);
                      });
    // An empty template reaches the node as an empty path; it is not the empty
    // `optional` that means "the condition is a literal".
    with_while_action("do { log(\"x\") } while (has(\"\"))",
                      [](const Stmt &_a) {
                          const auto *h = dynamic_cast<const HasStmt *>(&_a);
                          ASSERT_NE(h, nullptr);
                          EXPECT_TRUE(h->image_path.empty());
                      });
    // `run` is in the same set, and this is the one condition that speaks to
    // something other than the screen, so it gets its own assertion.
    with_while_action("do { log(\"x\") } while (run(\"other.ccat\"))",
                      [](const Stmt &_a) {
                          EXPECT_NE(dynamic_cast<const RunStmt *>(&_a),
                                    nullptr);
                      });
    EXPECT_FALSE(
        parse_failure("do { log(\"x\") } while (\"n.png\")").empty());
}

TEST(CcatCondAction, DoWhileTakesLiteralsAndBoolDefsWithoutFolding) {
    struct case_ {
        const char *src;
        bool want;
    } const cases[] = {
        {"do { log(\"x\") } while (true)", true},
        {"do { log(\"x\") } while (false)", false},
        {"defs { f = true }\ndo { log(\"x\") } while (f)", true},
        {"defs { f = false }\ndo { log(\"x\") } while (f)", false},
    };
    for (const auto &c : cases) {
        with_do_while(c.src, [&c](const DoWhileStmt &_d) {
            // Still a `DoWhileStmt`, never folded down to its body: a `break`
            // inside would otherwise leave the enclosing loop.
            ASSERT_TRUE(_d.condition_literal.has_value()) << c.src;
            EXPECT_EQ(*_d.condition_literal, c.want) << c.src;
            EXPECT_EQ(_d.condition_action, nullptr) << c.src;
        });
    }
}

TEST(CcatCondAction, DoWhileRejectsTheRest) {
    // The three that reach the same message as `if`, and the one that names its
    // own rule.
    for (const char *src : {"do { log(\"x\") } while (log(\"y\"))",
                            "do { log(\"x\") } while (home)"}) {
        const std::string msg = parse_failure(src);
        EXPECT_EQ(msg.find("while (...)"), 0u) << src << ": " << msg;
        EXPECT_NE(msg.find("wait_until"), std::string::npos) << src << ": " << msg;
    }
    EXPECT_FALSE(
        parse_failure("defs { n = 1 }\ndo { log(\"x\") } while (n)").empty());
    EXPECT_FALSE(parse_failure("do { log(\"x\") } while (nope)").empty());
}

/// The message of the `log` a folded branch hands back. A branch is a
/// `BlockStmt` -- `{ log("A") }` is a block, not a bare statement -- so this
/// unwraps the one statement it holds.
std::string folded_log(const Stmt &_s) {
    const Stmt *inner = &_s;
    if (const auto *b = dynamic_cast<const BlockStmt *>(&_s)) {
        if (b->body.size() != 1) {
            return {};
        }
        inner = b->body.at(0).get();
    }
    const auto *l = dynamic_cast<const LogStmt *>(inner);
    return l == nullptr ? std::string{} : l->message;
}

TEST(CcatCondAction, AFoldedElifIsKeptAsTheBranchTaken) {
    // A false bool def drops the `then` and hands back the `elif`, whose own
    // condition is an action -- so it must survive as a real node.
    const auto prog = parse_program(
        "defs { f = false }\nif (f) { log(\"y\") } elif (has(\"b.png\")) "
        "{ log(\"n\") }\n");
    ASSERT_EQ(prog->stmts.size(), 1u);
    const auto *i = dynamic_cast<const IfStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(i, nullptr);
    EXPECT_NE(dynamic_cast<const HasStmt *>(i->action.get()), nullptr);
}

TEST(CcatCondAction, AFoldedElifIsDroppedWhenThenWins) {
    const auto prog = parse_program(
        "defs { f = true }\nif (f) { log(\"y\") } elif (has(\"b.png\")) "
        "{ log(\"n\") }\n");
    ASSERT_EQ(prog->stmts.size(), 1u);
    EXPECT_EQ(folded_log(*prog->stmts.at(0)), "y");
}

TEST(CcatCondAction, LiteralConditionsFold) {
    const auto yes =
        parse_program("if (true) { log(\"A\") } else { log(\"B\") }\n");
    ASSERT_EQ(yes->stmts.size(), 1u);
    EXPECT_EQ(folded_log(*yes->stmts.at(0)), "A");

    const auto no =
        parse_program("if (false) { log(\"A\") } else { log(\"B\") }\n");
    ASSERT_EQ(no->stmts.size(), 1u);
    EXPECT_EQ(folded_log(*no->stmts.at(0)), "B");

    // No `else`: the chosen branch is nothing, which is an empty block rather
    // than the absence of a statement.
    const auto nothing = parse_program("if (false) { log(\"A\") }\n");
    ASSERT_EQ(nothing->stmts.size(), 1u);
    const auto *empty =
        dynamic_cast<const BlockStmt *>(nothing->stmts.at(0).get());
    ASSERT_NE(empty, nullptr);
    EXPECT_TRUE(empty->body.empty());
}

TEST(CcatCondAction, AFoldedElifIsDiscardedEntirely) {
    const auto prog = parse_program(
        "if (true) { log(\"A\") } elif (tap(\"b.png\")) { log(\"B\") }\n");
    ASSERT_EQ(prog->stmts.size(), 1u);
    EXPECT_EQ(folded_log(*prog->stmts.at(0)), "A");
}

TEST(CcatCondAction, FoldingAnElifDoesNotCarryTheActionConditionAway) {
    // The `then` is an action, so the outer `if` is a real node; the `elif` under
    // it folds to its `else`, and the action stays where it was.
    const auto prog = parse_program(
        "if (tap(\"x.png\")) { log(\"L\") } elif (false) { log(\"A\") } "
        "else { log(\"B\") }\n");
    const auto *i = dynamic_cast<const IfStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(i, nullptr);
    EXPECT_NE(dynamic_cast<const TapStmt *>(i->action.get()), nullptr);
    ASSERT_NE(i->else_branch, nullptr);
    EXPECT_EQ(folded_log(*i->else_branch), "B");
}

TEST(CcatCondAction, AFoldedIfStillChecksItsSyntax) {
    EXPECT_FALSE(parse_failure("if (true { log(\"A\") }").empty());
}

} // namespace
