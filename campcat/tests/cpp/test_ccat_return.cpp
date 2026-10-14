// A script's result: `return false` makes it false, anything else true, and a
// `run` hands the callee's answer to whoever asked. The integration cases run
// real files through the real interpreter, which needs no device: `return`,
// `run`, `log` and an action `if` touch neither adb nor the matcher.

#include <core/adb_client.h>
#include <core/app_config.h>
#include <core/automation_log.h>
#include <core/script/ccat_interpreter.h>
#include <core/script/ccat_parser.h>
#include <core/template_matcher.h>

#include <opencv2/imgcodecs.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using campcat::automation_cycle_result;
using campcat::automation_log;
using campcat::ccat_lang::parse_error;
using campcat::ccat_lang::parse_program;
using campcat::ccat_lang::ReturnStmt;

/// A device the tests can control: every screenshot is the same deterministic
/// noise, which matches no template, or nothing at all. `screencap_bgr` is the
/// only member a condition can reach -- the rest are honest no-ops rather than
/// a mock framework. It lives up here because `run_source` needs one: the
/// interpreter's constructor throws on a null adb, so every case needs a device
/// object even when nothing reaches it.
class StubAdb final : public campcat::adb_api {
public:
    explicit StubAdb(bool _screencap_ok) : m_screencap_ok(_screencap_ok) {}

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
    bool screencap_bgr(cv::Mat *_bgr_out, int, std::string *) override {
        if (!m_screencap_ok) {
            return false;
        }
        cv::Mat screen(64, 64, CV_8UC3);
        cv::RNG rng(7);
        rng.fill(screen, cv::RNG::UNIFORM, 0, 256);
        *_bgr_out = screen;
        return true;
    }
    /// No ccat path reads this -- `tap_image` paces with `cooperative_sleep_ms`
    /// -- so it is a default, not a claim.
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }

private:
    bool m_screencap_ok;
};

// The log registry is process-wide, so a leaked sink changes every later case.
class CcatReturn : public testing::Test {
protected:
    void SetUp() override { automation_log::clear_sink(); }
    void TearDown() override { automation_log::clear_sink(); }
};

/// Runs `_check` against the single `return` `_src` parses to. The `Program`
/// stays alive in here, so the pointer handed out cannot dangle.
template <typename F> void with_return(const std::string &_src, F _check) {
    const auto prog = parse_program(_src);
    const auto *r = dynamic_cast<const ReturnStmt *>(prog->stmts.at(0).get());
    ASSERT_NE(r, nullptr);
    _check(*r);
}

TEST_F(CcatReturn, BareReturnAnswersTrue) {
    // The default is what keeps every script written before `return` took a
    // value meaning exactly what it did.
    with_return("return", [](const ReturnStmt &_r) { EXPECT_TRUE(_r.value); });
}

TEST_F(CcatReturn, TheTwoLiterals) {
    with_return("return true",
                [](const ReturnStmt &_r) { EXPECT_TRUE(_r.value); });
    with_return("return false",
                [](const ReturnStmt &_r) { EXPECT_FALSE(_r.value); });
}

TEST_F(CcatReturn, ABooleanDefIsAcceptedAndANumberDefIsNot) {
    with_return("defs { stop_now = false }\nreturn stop_now",
                [](const ReturnStmt &_r) { EXPECT_FALSE(_r.value); });

    EXPECT_THROW(parse_program("defs { n = 1 }\nreturn n"), parse_error);
}

// --- the integration half: real files, no device -------------------------

/// Writes each `name`/`source` pair into a fresh temporary directory and
/// returns that directory, which is the script root `run()` resolves against.
std::filesystem::path
write_scripts(const std::vector<std::pair<std::string, std::string>> &_files) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "campcat_ccat_return_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (const auto &[name, source] : _files) {
        std::ofstream out(dir / name);
        out << source;
    }
    return dir;
}

/// Runs `_source` with `_dir` as the script root. No script path, so nothing is
/// pushed on the run stack and a `run` of a file in `_dir` cannot read as a
/// cycle against the entry script. The default device answers with noise: the
/// device-free cases never reach it, and the ctor needs *some* adb.
automation_cycle_result run_source(const std::filesystem::path &_dir,
                                   const std::string &_source,
                                   campcat::adb_api *_adb = nullptr) {
    static StubAdb s_idle(/*_screencap_ok=*/true);
    campcat::app_config cfg{};
    const auto prog = parse_program(_source);
    campcat::ccat_lang::CcatInterpreter interp(
        _adb != nullptr ? _adb : &s_idle, &cfg,
        campcat::template_matcher(cfg.match.box_threshold,
                                  cfg.match.circle_threshold,
                                  cfg.match.multiscale),
        _dir, std::filesystem::path{});
    return interp.run(*prog, [] { return false; });
}

bool logged_marker(const std::vector<std::string> &_lines,
                   const std::string &_marker) {
    for (const std::string &line : _lines) {
        if (line.find("[script] " + _marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

TEST_F(CcatReturn, RunHandsTheCalleeAnswerToTheCondition) {
    const std::filesystem::path dir =
        write_scripts({{"inner.ccat", "return false\n"}});

    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        dir, "if (run(\"inner.ccat\")) { log(\"T\") } else { log(\"F\") }\n");

    EXPECT_TRUE(r.ok);
    // This script has no `return false` of its own, so its own answer is true
    // even though the callee's was false -- the answer belongs to whoever wrote
    // the `return`, which is why a `run` does not fail its caller.
    EXPECT_TRUE(logged_marker(lines, "F")) << "the else branch did not run";
    EXPECT_FALSE(logged_marker(lines, "T"));
    (void)sink;

    std::filesystem::remove_all(dir);
}

TEST_F(CcatReturn, ACalleeThatAnswersTrueTakesTheThenBranch) {
    const std::filesystem::path dir =
        write_scripts({{"inner.ccat", "return\n"}});

    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        dir, "if (run(\"inner.ccat\")) { log(\"T\") } else { log(\"F\") }\n");

    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(logged_marker(lines, "T"));
    EXPECT_FALSE(logged_marker(lines, "F"));
    (void)sink;

    std::filesystem::remove_all(dir);
}

TEST_F(CcatReturn, AFailingConditionLeavesNoLogLineOfItsOwn) {
    // A `run` of a file that is not there fails with no device involved, which
    // is the only device-free way to watch a condition come out false.
    const std::filesystem::path dir = write_scripts({});

    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        dir, "if (run(\"nope.ccat\")) { log(\"T\") } else { log(\"F\") }\n");
    (void)sink;

    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(logged_marker(lines, "F"));
    ASSERT_FALSE(lines.empty());
    // Every line is the script's own `log`. No `[ccat] if:` narration, and no
    // other interpreter line either: the interpreter logs nothing of its own.
    for (const std::string &line : lines) {
        EXPECT_NE(line.find(automation_log::k_script_log_tag),
                  std::string::npos)
            << line;
    }

    std::filesystem::remove_all(dir);
}

TEST_F(CcatReturn, ATopLevelReturnFalseIsNotOk) {
    const automation_cycle_result r =
        run_source(std::filesystem::temp_directory_path(), "return false\n");

    // The answer lands on `ok`, so a false-answering script reads as not ok and
    // the message is the whole story.
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.message, "the script returned false");
}

// --- a broken device is not a screen state --------------------------------
// (`StubAdb` is defined at the top of the file, next to the other helpers.)

/// A template that is a real, readable PNG and looks like nothing the stub's
/// screen does. A flat colour would not do: `TM_CCOEFF_NORMED` subtracts the
/// mean, so a flat template's correlation is meaningless.
void write_pattern_png(const std::filesystem::path &_path) {
    cv::Mat templ(16, 16, CV_8UC3);
    cv::RNG rng(1);
    rng.fill(templ, cv::RNG::UNIFORM, 0, 256);
    ASSERT_TRUE(cv::imwrite(_path.string(), templ));
}

TEST_F(CcatReturn, ADeviceFailureInAHasConditionStopsTheScript) {
    // A regular file, so this gets past the missing-template `false` and
    // reaches the screencap that fails. (The contents never matter: the device
    // is dead.)
    const std::filesystem::path dir = write_scripts({{"x.png", ""}});
    StubAdb adb(/*_screencap_ok=*/false);

    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        dir, "if (has(\"x.png\")) { log(\"T\") } else { log(\"F\") }\n", &adb);
    (void)sink;

    // The screenshot never happened, so nobody knows whether the template was
    // there. Reading that as `false` would run `else` on a dead device.
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.message, "has: screencap failed");
    EXPECT_FALSE(logged_marker(lines, "F"));
    EXPECT_FALSE(logged_marker(lines, "T"));

    std::filesystem::remove_all(dir);
}

TEST_F(CcatReturn, ADeviceFailureInATapConditionStopsTheScript) {
    // The other tagging path: a real action, whose screencap failure is marked
    // inside `tap_image` rather than in `HasStmt::exec`.
    const std::filesystem::path dir = write_scripts({{"x.png", ""}});
    StubAdb adb(/*_screencap_ok=*/false);

    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        dir, "if (tap(\"x.png\")) { log(\"T\") } else { log(\"F\") }\n", &adb);
    (void)sink;

    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(logged_marker(lines, "F"));
    EXPECT_FALSE(logged_marker(lines, "T"));

    std::filesystem::remove_all(dir);
}

TEST_F(CcatReturn, ADeviceFailureInADoWhileConditionStopsTheLoop) {
    // The loop has its own copy of `if`'s device check, so it gets its own
    // case. Reading the failure as "the screen is not like that" would end the
    // loop quietly and let the script run on against a dead device.
    const std::filesystem::path dir = write_scripts({{"x.png", ""}});
    StubAdb adb(/*_screencap_ok=*/false);

    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r =
        run_source(dir, "do { log(\"B\") } while (has(\"x.png\"))\n", &adb);
    (void)sink;

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.message, "has: screencap failed");
    // The body ran once -- the condition is tested after it -- and then the
    // loop stopped on the device rather than on the screen.
    EXPECT_TRUE(logged_marker(lines, "B"));

    std::filesystem::remove_all(dir);
}

TEST_F(CcatReturn, ADoWhileRunsOnceWhenTheCalleeAnswersFalse) {
    // `run` as a loop condition, the `do`/`while` half of what the `if` cases
    // above pin: the callee's answer is what decides, and the body ran once.
    const std::filesystem::path dir =
        write_scripts({{"inner.ccat", "return false\n"}});

    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r =
        run_source(dir, "do { log(\"B\") } while (run(\"inner.ccat\"))\n");
    (void)sink;

    EXPECT_TRUE(r.ok);
    int body_runs = 0;
    for (const std::string &line : lines) {
        if (line.find("[script] B") != std::string::npos) {
            ++body_runs;
        }
    }
    EXPECT_EQ(body_runs, 1);

    std::filesystem::remove_all(dir);
}

TEST_F(CcatReturn, AWorkingScreencapThatMissesIsStillJustFalse) {
    // The contrast that gives the device check its meaning: same condition,
    // same script, a device that answers -- and now the miss is a `false`, so
    // `else` runs and the script is fine.
    const std::filesystem::path dir = write_scripts({});
    write_pattern_png(dir / "x.png");
    StubAdb adb(/*_screencap_ok=*/true);

    std::vector<std::string> lines;
    const auto sink = automation_log::add_sink(
        [&lines](const std::string &_line) { lines.push_back(_line); });
    const automation_cycle_result r = run_source(
        dir, "if (has(\"x.png\")) { log(\"T\") } else { log(\"F\") }\n", &adb);
    (void)sink;

    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(logged_marker(lines, "F")) << "the else branch did not run";
    EXPECT_FALSE(logged_marker(lines, "T"));

    std::filesystem::remove_all(dir);
}

} // namespace
