#include <core/adb_client.h>
#include <core/app_config.h>
#include <core/script/ccat_interpreter.h>
#include <core/script/ccat_parser.h>
#include <core/smoke/smoke_run.h>
#include <core/template_matcher.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace campcat;
using nlohmann::json;

class ReplayDevice final : public adb_api {
public:
    cv::Mat frame{32, 32, CV_8UC3, cv::Scalar(20, 20, 20)};
    int taps = 0;
    bool change = false;
    bool fail_after_tap = false;
    bool stop = false;
    bool stop_on_tap = false;
    std::vector<cv::Mat> tap_frames;
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
    bool tap(int, int) override {
        ++taps;
        if (taps <= static_cast<int>(tap_frames.size()))
            frame = tap_frames[taps - 1].clone();
        if (change)
            frame.setTo(cv::Scalar(180, 180, 180));
        if (stop_on_tap)
            stop = true;
        return true;
    }
    bool swipe(int, int, int, int, int) override { return true; }
    bool keyevent(int) override { return true; }
    bool home_and_kill_all(int) override { return true; }
    bool screencap_bgr(cv::Mat *out, int, std::string *) override {
        if (fail_after_tap && taps)
            return false;
        *out = frame.clone();
        return true;
    }
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }
};

class CcatReplay : public testing::Test {
protected:
    std::filesystem::path dir;
    app_config cfg;
    ReplayDevice adb;
    void SetUp() override {
        dir =
            std::filesystem::temp_directory_path() /
            ("campcat_replay_" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir / "library");
        dir = std::filesystem::canonical(dir);
        cfg.config_home = (dir / "library").string();
        cfg.input.tap_delay_ms = cfg.input.action_gap_ms = 0;
    }
    void TearDown() override { std::filesystem::remove_all(dir); }
    automation_cycle_result run(const std::string &source) {
        const auto program = ccat_lang::parse_program(source);
        ccat_lang::CcatInterpreter interp(
            &adb, &cfg, template_matcher(.82, .96, false), dir / "library",
            dir / "library/main.ccat");
        return interp.run(*program, [&] { return adb.stop; });
    }
    std::vector<json> records() {
        std::vector<json> rows;
        const auto root = dir / "logs/replay";
        if (!std::filesystem::exists(root))
            return rows;
        for (const auto &file :
             std::filesystem::recursive_directory_iterator(root)) {
            if (file.path().filename() != "actions.jsonl")
                continue;
            std::ifstream in(file.path());
            std::string line;
            while (std::getline(in, line))
                rows.push_back(json::parse(line));
        }
        return rows;
    }
    json step(const std::string &action) {
        for (const auto &row : records())
            if (row.value("event", "") == "step" &&
                row.value("action", "") == action)
                return row;
        ADD_FAILURE() << "missing audit step: " << action;
        return json::object();
    }
};

class GoldenClaim : public CcatReplay {
protected:
    cv::Mat ready, done, missing, popup;
    void SetUp() override {
        CcatReplay::SetUp();
        const auto library = dir / "library";
        const auto bundle = library / "app.feed.claim";
        std::filesystem::create_directories(bundle);
        const auto write = [](const auto &file, const auto &text) { std::ofstream(file) << text; };
        auto reference = std::filesystem::path(__FILE__);
        for (int n = 0; n < 4; ++n) reference = reference.parent_path();
        std::ifstream in(reference / "gold.ccat");
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        const auto heading = text.find("### Action `app.feed.claim`");
        ASSERT_NE(heading, std::string::npos);
        const auto start = text.find("```ccat\n", heading) + 8;
        const auto end = text.find("```", start);
        ASSERT_NE(end, std::string::npos);
        write(bundle / "main.ccat", text.substr(start, end - start));
        missing = cv::Mat(64, 64, CV_8UC3, cv::Scalar(20, 20, 20));
        const auto mark = [&](const char *name, unsigned seed) {
            cv::Mat image(10, 10, CV_8UC3);
            cv::RNG rng(seed);
            rng.fill(image, cv::RNG::UNIFORM, 0, 255);
            cv::imwrite((bundle / name).string(), image);
            return image;
        };
        const cv::Mat feed = mark("feed.png", 11);
        feed.copyTo(missing(cv::Rect(4, 4, 10, 10)));
        ready = missing.clone();
        mark("claim.png", 12).copyTo(ready(cv::Rect(32, 32, 10, 10)));
        done = missing.clone();
        mark("claimed.png", 13).copyTo(done(cv::Rect(32, 32, 10, 10)));
        popup = missing.clone();
        mark("reward_popup.png", 14).copyTo(popup(cv::Rect(8, 32, 10, 10)));
        mark("popup_close.png", 15).copyTo(popup(cv::Rect(40, 32, 10, 10)));
        for (auto id : {"entry", "feed"}) {
            const auto anchor = library / (std::string("app.") + id);
            std::filesystem::create_directories(anchor);
            write(anchor / "golden.md", "---\nkind: page\n---\npage\n");
            cv::imwrite((anchor / "feed.png").string(), feed);
            write(anchor / "main.ccat", std::string(id) == "feed" ?
                "if (has(\"feed.png\")) { return true }\nreturn false\n" : "return false\n");
        }
        std::filesystem::create_directories(library / "app.entry-feed");
        write(library / "app.entry-feed/main.ccat", "home\n");
    }
    automation_cycle_result claim() {
        const std::string source = "run(\"app.feed.claim/main.ccat\")";
        std::ofstream(dir / "library/main.ccat") << source;
        return run(source);
    }
    std::vector<std::string> verify() {
        const auto fixture = dir / "expected.json";
        std::ofstream(fixture) << R"({"app":{"cases":{"feed.claim":{"ready":"claim.png","done":"claimed.png"}}}})";
        return campcat::smoke_problems(dir / "library", dir / "logs/replay", fixture);
    }
    bool has_problem(const std::string &needle) {
        for (const auto &problem : verify()) if (problem.find(needle) != std::string::npos) return true;
        return false;
    }
};

TEST_F(GoldenClaim, ReadyExecutesInputsAndAssertsTheClaimedState) {
    adb.frame = ready;
    adb.tap_frames = {popup, done};
    const auto result = claim();
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_EQ(adb.taps, 2);
    EXPECT_EQ(step("tap").at("diff").at("status"), "changed");
    EXPECT_FALSE(has_problem("unverified action app.feed.claim"));
    EXPECT_FALSE(has_problem("unverified case app.feed.claim/ready"));
}

TEST_F(GoldenClaim, AlreadyClaimedSucceedsWithoutInput) {
    adb.frame = done;
    const auto result = claim();
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_EQ(adb.taps, 0);
    EXPECT_FALSE(has_problem("unverified case app.feed.claim/done"));
    for (const auto &row : records())
        EXPECT_NE(row.value("error_code", ""), "script_failed");
}

TEST_F(GoldenClaim, MissingButtonWithoutDoneEvidenceFailsWithoutInput) {
    adb.frame = missing;
    const auto result = claim();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, "script_failed");
    EXPECT_EQ(adb.taps, 0);
    EXPECT_FALSE(has_problem("unverified case app.feed.claim/missing"));
}

TEST_F(CcatReplay, UnchangedTapIsRecordedWithoutRetryOrFailure) {
    ASSERT_TRUE(run("\n// source position\ntap_at(0.5, 0.5)\n").ok);
    EXPECT_EQ(adb.taps, 1);
    const auto rows = records();
    ASSERT_FALSE(rows.empty());
    const auto row = step("tap_at");
    EXPECT_EQ(row.at("line"), 3);
    EXPECT_EQ(row.at("column"), 1);
    EXPECT_EQ(row.at("coordinates"), json::array({.5, .5}));
    EXPECT_EQ(row.at("input_pixels"), json::array({16, 16}));
    EXPECT_EQ(row.at("diff").at("status"), "no_change");
    EXPECT_EQ(row.at("diff").at("changed_ratio"), 0.0);
    EXPECT_EQ(row.at("error_code"), "");
    EXPECT_EQ(row.at("script"), (dir / "library/main.ccat").string());
    EXPECT_FALSE(row.at("before_shot").is_null());
    EXPECT_FALSE(row.at("after_shot").is_null());
}

TEST_F(CcatReplay, ChangedTapStoresBothRealFrames) {
    adb.change = true;
    ASSERT_TRUE(run("tap_at(0.5, 0.5)").ok);
    ASSERT_FALSE(records().empty());
    const auto row = step("tap_at");
    EXPECT_EQ(row.at("diff").at("status"), "changed");
    EXPECT_EQ(row.at("diff").at("changed_ratio"), 1.0);
    const auto before = cv::imread(row.at("before_shot").get<std::string>());
    const auto after = cv::imread(row.at("after_shot").get<std::string>());
    ASSERT_FALSE(before.empty());
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(before.at<cv::Vec3b>(0, 0)[0], 20);
    EXPECT_EQ(after.at<cv::Vec3b>(0, 0)[0], 180);
}

TEST_F(CcatReplay, PostActionCaptureFailureStopsEvenInsideACondition) {
    adb.fail_after_tap = true;
    const auto result =
        run("if (tap_at(0.5, 0.5)) {} else { tap_at(0.1, 0.1) }");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, "device_error");
    EXPECT_EQ(adb.taps, 1);
}

TEST_F(CcatReplay, BoundsFailureHasAStableCodeAndDoesNotTap) {
    const auto result = run("tap_at(2, 0.5)");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, "out_of_bounds");
    EXPECT_EQ(adb.taps, 0);
}

TEST_F(CcatReplay, NestedFailurePreservesCodeAndCalleeLocation) {
    std::ofstream(dir / "library/child.ccat") << "\n\ntap_at(2, 0.5)\n";
    const auto result = run("run(\"child.ccat\")");
    EXPECT_EQ(result.error_code, "out_of_bounds");
    ASSERT_FALSE(records().empty());
    const auto row = step("tap_at");
    EXPECT_EQ(row.at("line"), 3);
    EXPECT_EQ(row.at("script"), (dir / "library/child.ccat").string());
    EXPECT_EQ(row.at("error_code"), "out_of_bounds");
}

TEST_F(CcatReplay, StopAfterTapIsNotReportedAsNoChangeOrRetried) {
    adb.stop_on_tap = true;
    const auto result = run("tap_at(0.5, 0.5)");
    EXPECT_EQ(result.error_code, "cancelled");
    EXPECT_EQ(adb.taps, 1);
    ASSERT_FALSE(records().empty());
    const auto row = step("tap_at");
    EXPECT_EQ(row.at("diff").at("status"), "unavailable");
    EXPECT_EQ(row.at("error_code"), "cancelled");
}

TEST_F(CcatReplay, FoldedBranchKeepsTheExecutedStatementLine) {
    ASSERT_TRUE(run("if (true)\n  tap_at(0.5, 0.5)").ok);
    ASSERT_FALSE(records().empty());
    EXPECT_EQ(step("tap_at").at("line"), 2);
}

TEST_F(CcatReplay, MatchingTapRecordsTheActualScoreThresholdAndOperand) {
    cv::RNG rng(71);
    rng.fill(adb.frame, cv::RNG::UNIFORM, 0, 256);
    ASSERT_TRUE(cv::imwrite((dir / "library/button.png").string(),
                            adb.frame(cv::Rect(8, 8, 12, 12))));
    ASSERT_TRUE(run("tap(\"button.png\")").ok);
    const auto row = step("tap");
    ASSERT_EQ(row.at("matches").size(), 1);
    EXPECT_GT(row.at("matches")[0].at("score").get<double>(), .99);
    EXPECT_DOUBLE_EQ(row.at("matches")[0].at("threshold").get<double>(), .82);
    ASSERT_TRUE(row.at("matches")[0].contains("shot"));
    ASSERT_TRUE(row.at("matches")[0].contains("fingerprint"));
    EXPECT_EQ(row.at("matches")[0].at("shot"), row.at("before_shot"));
    EXPECT_EQ(row.at("operands"), json::array({"button.png"}));
}

TEST_F(CcatReplay, MissingAndUnreadableTemplatesHaveDifferentCodes) {
    EXPECT_EQ(run("tap(\"missing.png\")").error_code, "template_missing");
    std::ofstream(dir / "library/bad.png") << "invalid png";
    EXPECT_EQ(run("tap(\"bad.png\")").error_code, "template_error");
    EXPECT_EQ(adb.taps, 0);
}

TEST_F(CcatReplay, FailedMatchAndTimeoutAreDistinguishable) {
    cv::Mat image(12, 12, CV_8UC3);
    cv::RNG rng(71);
    rng.fill(image, cv::RNG::UNIFORM, 0, 256);
    ASSERT_TRUE(cv::imwrite((dir / "library/miss.png").string(), image));
    EXPECT_EQ(run("tap(\"miss.png\")").error_code, "not_found");
    EXPECT_EQ(run("wait_until(\"miss.png\", 1)").error_code, "deadline");
    EXPECT_EQ(adb.taps, 0);
}

TEST_F(CcatReplay, AuditOpenFailureStopsBeforeAnyInput) {
    std::ofstream(dir / "logs") << "blocked directory";
    const auto result = run("tap_at(0.5, 0.5)");
    EXPECT_EQ(result.error_code, "storage_error");
    EXPECT_EQ(adb.taps, 0);
}

TEST_F(CcatReplay, DisablingAuditKeepsTheExistingInputPath) {
    cfg.replay.enabled = false;
    adb.fail_after_tap = true;
    EXPECT_TRUE(run("tap_at(0.5, 0.5)").ok);
    EXPECT_TRUE(records().empty());
}

TEST_F(CcatReplay, RetentionOnlyRemovesCompletedRunDirectories) {
    cfg.replay.keep_runs = 2;
    const auto incomplete = dir / "logs/replay/run-000-incomplete";
    std::filesystem::create_directories(incomplete);
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(run("tap_at(0.5, 0.5)").ok);
    int completed = 0;
    for (const auto &entry :
         std::filesystem::directory_iterator(dir / "logs/replay"))
        if (std::filesystem::exists(entry.path() / "complete"))
            ++completed;
    EXPECT_EQ(completed, 2);
    EXPECT_TRUE(std::filesystem::exists(incomplete));
}

TEST_F(CcatReplay, SizeBudgetStopsBeforeInputWhenEvenTheStartCannotBeRecorded) {
    cfg.replay.max_run_mb = 0;
    EXPECT_EQ(run("tap_at(0.5, 0.5)").error_code, "storage_error");
    EXPECT_EQ(adb.taps, 0);
}

TEST_F(CcatReplay, AuditRetainsTheExactExecutedSource) {
    const std::string source =
        "// retained even if the file changes\ntap_at(0.5, 0.5)\n";
    ASSERT_TRUE(run(source).ok);
    bool found = false;
    for (const auto &row : records()) {
        if (row.value("event", "") == "source") {
            EXPECT_EQ(row.at("source"), source);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(CcatReplay, BudgetFailureRetainsTheFailingStepAndDoesNotRetryInput) {
    cfg.replay.max_run_mb = 1;
    adb.frame = cv::Mat(512, 512, CV_8UC3);
    cv::RNG rng(71);
    rng.fill(adb.frame, cv::RNG::UNIFORM, 0, 256);
    const auto result =
        run("if (tap_at(0.5, 0.5)) {} else { tap_at(0.1, 0.1) }");
    EXPECT_EQ(result.error_code, "storage_error");
    EXPECT_EQ(adb.taps, 1);
    const auto row = step("tap_at");
    ASSERT_TRUE(row.contains("error_code"));
    EXPECT_EQ(row.at("error_code"), "storage_error");
    EXPECT_EQ(row.at("line"), 1);
    const auto rows = records();
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.back().at("event"), "run_end");
    EXPECT_EQ(rows.back().at("error_code"), "storage_error");
}

TEST_F(CcatReplay, AnchorAuditFailureCannotBecomeAnElseBranch) {
    cfg.replay.max_run_mb = 1;
    std::filesystem::create_directories(dir / "library/g.entry");
    std::ofstream(dir / "library/g.entry/golden.md")
        << "---\nkind: page\n---\nEntry\n";
    std::ofstream(dir / "library/g.entry/main.ccat")
        << "//" << std::string(1024 * 1024, 'x') << "\nreturn true\n";
    for (const std::string action : {"at", "goto"}) {
        const auto result = run("if (" + action +
                                "(\"g:entry\")) {} else { tap_at(0.5, 0.5) }");
        EXPECT_EQ(result.error_code, "storage_error") << action;
        EXPECT_EQ(adb.taps, 0) << action;
    }
}

TEST_F(CcatReplay, FailedStartCannotDeleteExistingEvidence) {
    cfg.replay.keep_runs = 1;
    const auto first = run("tap_at(0.5, 0.5)");
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(std::filesystem::exists(
        std::filesystem::path(first.audit_path) / "complete"));
    cfg.replay.max_run_mb = 0;
    EXPECT_EQ(run("tap_at(0.5, 0.5)").error_code, "storage_error");
    EXPECT_TRUE(std::filesystem::exists(
        std::filesystem::path(first.audit_path) / "actions.jsonl"));
}
} // namespace
