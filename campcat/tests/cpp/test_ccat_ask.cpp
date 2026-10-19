// `ask(...)` as a condition: parsing it, and what the interpreter does with
// each kind of answer -- yes, no, and "nobody could answer". The last one must
// stop the script rather than fall into the else branch, which is the whole
// point of the statement.

#include <core/adb_client.h>
#include <core/app_config.h>
#include <core/automation_log.h>
#include <core/script/ccat_interpreter.h>
#include <core/script/ccat_graph.h>
#include <core/script/ccat_parser.h>
#include <core/template_matcher.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

using campcat::automation_cycle_result;
using campcat::automation_log;
using campcat::ccat_lang::ask_handler;
using campcat::ccat_lang::ask_result;
using campcat::ccat_lang::AskStmt;
using campcat::ccat_lang::DoWhileStmt;
using campcat::ccat_lang::IfStmt;
using campcat::ccat_lang::LogStmt;
using campcat::ccat_lang::parse_error;
using campcat::ccat_lang::parse_program;

/// A device object the interpreter's constructor accepts and nothing here
/// reaches: `ask` takes its own screenshot through the answerer, not the
/// interpreter.
class StubAdb final : public campcat::adb_api {
public:
    std::vector<std::string> list_devices(int) override { return {}; }
    void set_serial(const std::string &) override {}
    bool run(const std::vector<std::string> &, std::string *, std::string *,
             int) override {
        return false;
    }
    bool connect_remote(std::string_view, int, std::string *,
                        std::string *) override {
        return false;
    }
    bool tap(int, int) override { return false; }
    bool swipe(int, int, int, int, int) override { return false; }
    bool keyevent(int) override { return false; }
    bool home_and_kill_all(int) override { return false; }
    bool screencap_bgr(cv::Mat *, int, std::string *) override { return false; }
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }
};

std::string parse_failure(const std::string &_src) {
    try {
        (void)parse_program(_src);
    } catch (const parse_error &ex) {
        return ex.what();
    }
    return {};
}

template <typename F> void with_condition(const std::string &_src, F _check) {
    const auto prog = parse_program(_src);
    const auto *i = dynamic_cast<const IfStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(i, nullptr);
    ASSERT_NE(i->action, nullptr);
    _check(*i->action);
}

automation_cycle_result run_source(const std::string &_source,
                                   const ask_handler &_ask) {
    static StubAdb adb;
    static const campcat::app_config cfg{};
    const auto prog = parse_program(_source);
    campcat::ccat_lang::CcatInterpreter interp(
        &adb, &cfg,
        campcat::template_matcher(cfg.match.box_threshold,
                                  cfg.match.circle_threshold,
                                  cfg.match.multiscale),
        std::filesystem::temp_directory_path(), std::filesystem::path{}, _ask);
    return interp.run(*prog, [] { return false; });
}

bool logged(const std::vector<std::string> &_lines,
            const std::string &_marker) {
    for (const std::string &line : _lines) {
        if (line.find("[script] " + _marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The log registry is process-wide, so a leaked sink changes every later case.
class CcatAsk : public testing::Test {
protected:
    void SetUp() override { automation_log::clear_sink(); }
    void TearDown() override { automation_log::clear_sink(); }
};

TEST_F(CcatAsk, AQuestionIsAnIfCondition) {
    with_condition("if (ask(\"is it?\")) { log(\"y\") }",
                   [](const auto &_a) {
                       const auto *a = dynamic_cast<const AskStmt *>(&_a);
                       ASSERT_NE(a, nullptr);
                       EXPECT_EQ(a->message, "is it?");
                   });
}

TEST_F(CcatAsk, AQuestionIsADoWhileCondition) {
    const auto prog = parse_program("do { log(\"x\") } while (ask(\"again?\"))");
    const auto *d = dynamic_cast<const DoWhileStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(d, nullptr);
    const auto *a = dynamic_cast<const AskStmt *>(d->condition_action.get());
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->message, "again?");
}

TEST_F(CcatAsk, ABareAskIsRejectedWithTheConditionHint) {
    const std::string msg = parse_failure("ask(\"is it?\")");
    EXPECT_NE(msg.find("if (...) or while (...)"), std::string::npos) << msg;
}

TEST_F(CcatAsk, TheQuestionTakesTheSameFormsAsAMessage) {
    // A string literal with an escape, a string def, and a bare word.
    with_condition("if (ask(\"line\\nbreak\")) { log(\"y\") }",
                   [](const auto &_a) {
                       EXPECT_EQ(dynamic_cast<const AskStmt *>(&_a)->message,
                                 "line\nbreak");
                   });
    with_condition("defs { q = \"from def\" }\nif (ask(q)) { log(\"y\") }",
                   [](const auto &_a) {
                       EXPECT_EQ(dynamic_cast<const AskStmt *>(&_a)->message,
                                 "from def");
                   });
    with_condition("defs { q = \"from def\" }\nif (ask(bare)) { log(\"y\") }",
                   [](const auto &_a) {
                       EXPECT_EQ(dynamic_cast<const AskStmt *>(&_a)->message,
                                 "bare");
                   });
}

TEST_F(CcatAsk, ANonStringDefCannotBeAQuestion) {
    const std::string msg =
        parse_failure("defs { n = 1 }\nif (ask(n)) { log(\"y\") }");
    EXPECT_NE(msg.find("is not a string"), std::string::npos) << msg;
}

TEST_F(CcatAsk, YesTakesTheThenBranch) {
    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        "if (ask(\"is it?\")) { log(\"T\") } else { log(\"F\") }",
        [](std::string_view) { return ask_result{true, ""}; });
    (void)sink;

    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_TRUE(logged(lines, "T"));
    EXPECT_FALSE(logged(lines, "F"));
}

TEST_F(CcatAsk, NoTakesTheElseBranch) {
    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        "if (ask(\"is it?\")) { log(\"T\") } else { log(\"F\") }",
        [](std::string_view) { return ask_result{false, ""}; });
    (void)sink;

    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_TRUE(logged(lines, "F"));
    EXPECT_FALSE(logged(lines, "T"));
}

TEST_F(CcatAsk, AnUnansweredQuestionStopsTheScript) {
    // Not false: the else branch must not run on a question nobody settled.
    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        "if (ask(\"is it?\")) { log(\"T\") } else { log(\"F\") }\nlog(\"after\")",
        [](std::string_view) {
            return ask_result{std::nullopt, "ask: the model did not answer"};
        });
    (void)sink;

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.message, "ask: the model did not answer");
    EXPECT_FALSE(logged(lines, "T"));
    EXPECT_FALSE(logged(lines, "F"));
    EXPECT_FALSE(logged(lines, "after"));
}

TEST_F(CcatAsk, NoAnswererAtAllStopsTheScript) {
    const automation_cycle_result r = run_source(
        "if (ask(\"is it?\")) { log(\"T\") } else { log(\"F\") }", {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.message, "ask: nobody can answer here");
}

TEST_F(CcatAsk, AnAnchorCannotAsk) {
    // A graph is built from what the screen shows, not from an opinion about it.
    const auto prog = parse_program(
        "if (ask(\"is it?\")) { return true }\nreturn false");
    const std::vector<std::string> problems =
        campcat::ccat_lang::anchor_script_problems(*prog);
    bool named = false;
    for (const std::string &p : problems) {
        named = named || p.find("ask is not allowed") != std::string::npos;
    }
    EXPECT_TRUE(named);
}

TEST_F(CcatAsk, ASecondQuestionGetsItsOwnAnswer) {
    // The answerer is asked once per statement, in order; nothing is cached from
    // the first answer.
    std::vector<std::string> asked;
    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        "if (ask(\"first?\")) { log(\"one\") }\n"
        "if (ask(\"second?\")) { log(\"two\") } else { log(\"not two\") }",
        [&asked](std::string_view _q) {
            asked.push_back(std::string(_q));
            return asked.size() == 1 ? ask_result{true, ""}
                                     : ask_result{false, ""};
        });
    (void)sink;

    EXPECT_TRUE(r.ok) << r.message;
    ASSERT_EQ(asked.size(), 2u);
    EXPECT_EQ(asked.at(0), "first?");
    EXPECT_EQ(asked.at(1), "second?");
    EXPECT_TRUE(logged(lines, "one"));
    EXPECT_TRUE(logged(lines, "not two"));
}

TEST_F(CcatAsk, ADoWhileAsksOnEveryRound) {
    int answers = 0;
    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        "do { log(\"body\") } while (ask(\"again?\"))",
        [&answers](std::string_view) {
            ++answers; // yes once, then no
            return ask_result{answers == 1, ""};
        });
    (void)sink;

    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_EQ(answers, 2);
    int bodies = 0;
    for (const std::string &line : lines) {
        if (line.find("[script] body") != std::string::npos) {
            ++bodies;
        }
    }
    EXPECT_EQ(bodies, 2);
}

TEST_F(CcatAsk, AnUnansweredDoWhileQuestionStopsTheLoop) {
    const automation_cycle_result r = run_source(
        "do { log(\"body\") } while (ask(\"again?\"))",
        [](std::string_view) {
            return ask_result{std::nullopt, "ask: the client cannot answer"};
        });
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.message, "ask: the client cannot answer");
}

} // namespace
