// The smoke run's list file is the whole definition of "the graph is done", so
// what counts as an instruction is worth pinning: a CR sneaking through would
// ride on every prompt the model sees.

#include <core/smoke/smoke_run.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using campcat::smoke_prompts;

TEST(Smoke, PromptsSkipCommentsBlanksAndFileWhitespace) {
    EXPECT_EQ(smoke_prompts(""), (std::vector<std::string>{}));
    EXPECT_EQ(smoke_prompts(" \n\t\r\n# note\n"),
              (std::vector<std::string>{}));
    EXPECT_EQ(smoke_prompts(" a b \r\n#x\n\nc\n"),
              (std::vector<std::string>{"a b", "c"}));
    // The last line without a newline is an instruction too.
    EXPECT_EQ(smoke_prompts("# head\nd"), (std::vector<std::string>{"d"}));
}

class SmokeVerification : public testing::Test {
protected:
    using json = nlohmann::json;
    std::filesystem::path root, library, replays;
    const std::string body = "tap_at(0.5, 0.5)\nwait_until(\"done.png\", 1000)\nreturn true\n";
    void write(const std::filesystem::path &path, const std::string &text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }
    void SetUp() override {
        root = std::filesystem::temp_directory_path() / ("campcat_smoke_verify_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        library = root / "library";
        replays = root / "replay";
        for (auto id : {"entry", "a"}) {
            const auto dir = library / (std::string("g.") + id);
            write(dir / "golden.md", "---\nkind: page\n---\npage\n");
            write(dir / "main.ccat", "return true\n");
        }
        write(library / "g.entry-a/main.ccat", "home\n");
        write(library / "g.entry.claim/main.ccat", body);
        write(library / "g.entry.claim/done.png", "template presence for graph scan");
        write(library / "g.entry.claim/ready.png", "ready template presence");
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    json source(std::string bundle, std::string text, int id) {
        return {{"event", "source"}, {"source_id", id},
                {"script", (library / bundle / "main.ccat").string()}, {"source", text}};
    }
    json begin(std::string bundle, int id) {
        return {{"event", "step_start"}, {"step_id", id}, {"action", "run"},
                {"script", "<inline>"}, {"operands", {bundle + "/main.ccat"}}};
    }
    json end(std::string bundle, int id, bool ok = true) {
        auto row = begin(bundle, id);
        row["event"] = "step";
        row["ok"] = row["value"] = ok;
        return row;
    }
    json action(std::string kind, bool ok = true) {
        return {{"event", "step"}, {"action", kind}, {"source_id", 2},
            {"script", (library / "g.entry.claim/main.ccat").string()},
            {"ok", ok}, {"value", ok}, {"input_pixels", kind == "tap_at" ? json{5, 5} : json::array()},
            {"matches", kind == "wait_until" ? json::array({{{"found", ok},
                {"template", (library / "g.entry.claim/done.png").string()}}}) : json::array()},
            {"diff", {{"status", "no_change"}}}};
    }
    std::vector<json> prefix() {
        auto edge = action("return");
        edge["source_id"] = 1;
        edge["script"] = (library / "g.entry-a/main.ccat").string();
        return {{{"event", "run_start"}}, begin("g.entry-a", 1),
                source("g.entry-a", "home\n", 1), edge, end("g.entry-a", 1),
                begin("g.entry.claim", 2), source("g.entry.claim", body, 2)};
    }
    void audit(std::vector<json> rows, int run = 1) {
        std::string text;
        for (const auto &row : rows) text += row.dump() + "\n";
        write(replays / ("run-" + std::to_string(run)) / "actions.jsonl", text);
    }
    std::vector<std::string> problems() { return campcat::smoke_problems(library, replays); }
    static bool contains(const std::vector<std::string> &items, std::string needle) {
        for (const auto &item : items) if (item.find(needle) != std::string::npos) return true;
        return false;
    }
};

TEST_F(SmokeVerification, ModelSuccessAndASkipDoNotCoverTheInputBranch) {
    auto rows = prefix();
    rows.push_back(action("return"));
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    EXPECT_TRUE(contains(problems(), "unverified action g.entry.claim"));
}

TEST_F(SmokeVerification, InputNeedsAPostconditionAndASuccessfulCallee) {
    auto rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    EXPECT_TRUE(contains(problems(), "unverified action g.entry.claim"));
    rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(action("wait_until"));
    rows.push_back(end("g.entry.claim", 2, false));
    rows.push_back({{"event", "run_end"}, {"ok", true}}); // outer if caught false
    audit(rows);
    EXPECT_TRUE(contains(problems(), "unverified action g.entry.claim"));
}

TEST_F(SmokeVerification, NoChangeIsNotFailureWhenThePostconditionWasObserved) {
    auto rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(action("wait_until"));
    rows.back()["matches"][0]["template"] = (library / "g.entry.claim/done.png").string();
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    EXPECT_TRUE(problems().empty());
}

TEST_F(SmokeVerification, TwoInvocationsCannotCombineIntoOnePassingBranch) {
    auto rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back(begin("g.entry.claim", 3));
    rows.push_back(action("wait_until"));
    rows.push_back(end("g.entry.claim", 3));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    EXPECT_TRUE(contains(problems(), "unverified action g.entry.claim"));
}

TEST_F(SmokeVerification, OldSourceAndTruncatedAuditsAreNotAccepted) {
    auto rows = prefix();
    rows.back()["source"] = "old source";
    rows.push_back(action("tap_at"));
    rows.push_back(action("wait_until"));
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    EXPECT_TRUE(contains(problems(), "unverified action g.entry.claim"));
    rows.pop_back();
    audit(rows);
    EXPECT_TRUE(contains(problems(), "incomplete audit"));
}

TEST_F(SmokeVerification, GraphProblemsAndFixtureExpectationsFailAcceptance) {
    write(library / "g.entry/main.ccat", "if (has(\"logo.png\")) { return true }\nreturn false\n");
    EXPECT_TRUE(contains(problems(), "missing template logo.png"));
    const auto expected = root / "expected.json";
    write(expected, R"({"g":{"anchors":["entry","missing"],"edges":["a-entry"],"actions":["a.claim"]}})");
    const auto errors = campcat::smoke_problems(library, replays, expected);
    EXPECT_TRUE(contains(errors, "missing expected anchor g.missing"));
    EXPECT_TRUE(contains(errors, "missing expected edge g.a-entry"));
    EXPECT_TRUE(contains(errors, "missing expected action g.a.claim"));
}

TEST_F(SmokeVerification, RequiredDoneAndMissingCasesNeedTheirOwnEvidence) {
    const auto fixture = root / "expected.json";
    write(fixture, R"({"g":{"cases":{"entry.claim":{"ready":"ready.png","done":"done.png"}}}})");
    auto rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(action("wait_until"));
    rows.back()["matches"][0]["template"] = (library / "g.entry.claim/done.png").string();
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    auto errors = campcat::smoke_problems(library, replays, fixture);
    EXPECT_TRUE(contains(errors, "unverified case g.entry.claim/done"));
    EXPECT_TRUE(contains(errors, "unverified case g.entry.claim/missing"));

    const auto has = [&](const char *name, bool found) {
        auto row = action("has");
        row["value"] = found;
        row["matches"] = json::array({{{"found", found},
            {"template", (library / "g.entry.claim" / name).string()}}});
        return row;
    };
    rows = prefix();
    rows.push_back(has("ready.png", false));
    rows.push_back(has("done.png", true));
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows, 2);
    rows = prefix();
    rows.push_back(has("ready.png", false));
    rows.push_back(has("done.png", false));
    // An unconditional success on the missing-control branch is not evidence.
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows, 3);
    errors = campcat::smoke_problems(library, replays, fixture);
    EXPECT_TRUE(contains(errors, "unverified case g.entry.claim/missing"));
    rows[rows.size() - 2] = end("g.entry.claim", 2, false);
    rows[rows.size() - 2]["error_code"] = "script_failed";
    audit(rows, 3);
    EXPECT_TRUE(campcat::smoke_problems(library, replays, fixture).empty());
}

TEST_F(SmokeVerification, TrailingCorruptionInvalidatesEarlierPassingReceipts) {
    auto rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(action("wait_until"));
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    rows.push_back({{"event", "step"}});
    audit(rows);
    const auto errors = problems();
    EXPECT_TRUE(contains(errors, "invalid audit"));
    EXPECT_TRUE(contains(errors, "unverified action g.entry.claim"));
}

TEST_F(SmokeVerification, ReturningToThePageDoesNotProveATransientWasObserved) {
    const auto fixture = root / "expected.json";
    write(fixture, R"({"g":{"observations":{"entry.claim":["loading.png"]}}})");
    write(library / "g.entry.claim/loading.png", "loading template presence");
    auto rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(action("wait_until"));
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    auto errors = campcat::smoke_problems(library, replays, fixture);
    EXPECT_TRUE(contains(errors, "unobserved g.entry.claim/loading.png"));
    rows[rows.size() - 3]["matches"][0]["template"] = (library / "g.entry.claim/loading.png").string();
    audit(rows);
    EXPECT_TRUE(campcat::smoke_problems(library, replays, fixture).empty());
}

TEST_F(SmokeVerification, CaughtGotoFailureDoesNotValidateAnInternalEdge) {
    auto edge = action("home", false);
    edge["source_id"] = 2;
    edge["script"] = (library / "g.entry-a/main.ccat").string();
    audit({{{"event", "run_start"}},
           {{"event", "source"}, {"source_id", 1}, {"script", "<inline>"},
            {"source", "if (goto(\"g:a\")) {}"}},
           source("g.entry-a", "home\n", 2), edge,
           {{"event", "step"}, {"action", "goto"}, {"step_id", 1},
            {"script", "<inline>"}, {"ok", false}, {"value", false}},
           {{"event", "run_end"}, {"ok", true}}});
    EXPECT_TRUE(contains(problems(), "unverified edge g.entry-a"));
}

TEST_F(SmokeVerification, ABodyWithoutAGraphRunReceiptDoesNotValidateEndpoints) {
    auto edge = action("home");
    edge["source_id"] = 1;
    edge["script"] = (library / "g.entry-a/main.ccat").string();
    audit({{{"event", "run_start"}}, source("g.entry-a", "home\n", 1), edge,
           {{"event", "run_end"}, {"ok", true}}});
    EXPECT_TRUE(contains(problems(), "unverified edge g.entry-a"));
}

TEST_F(SmokeVerification, DeletingAMatchedTemplateInvalidatesItsReceipt) {
    auto rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(action("wait_until"));
    rows.back()["matches"][0]["template"] = (library / "g.entry.claim/done.png").string();
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    ASSERT_FALSE(contains(problems(), "unverified action g.entry.claim"));
    std::filesystem::remove(library / "g.entry.claim/done.png");
    EXPECT_TRUE(contains(problems(), "unverified action g.entry.claim"));
}

TEST_F(SmokeVerification, ContentIdentityDetectsAChangeWithPreservedTimestamps) {
    const auto png = library / "g.entry.claim/done.png";
    write(png, "a");
    const auto original_time = std::filesystem::last_write_time(png);
    auto rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(action("wait_until"));
    rows.back()["matches"][0]["template"] = png.string();
    // FNV-1a 64 of the single byte 'a', independently specified.
    rows.back()["matches"][0]["fingerprint"] = "fnv1a64:12638187200555641996";
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    ASSERT_FALSE(contains(problems(), "unverified action g.entry.claim"));
    write(png, "b");
    std::filesystem::last_write_time(png, original_time);
    EXPECT_TRUE(contains(problems(), "unverified action g.entry.claim"));
}

TEST_F(SmokeVerification, AMatchWithoutItsTemplatePathIsInvalidEvidence) {
    auto rows = prefix();
    rows.push_back(action("tap_at"));
    rows.push_back(action("wait_until"));
    rows.back()["matches"][0].erase("template");
    rows.push_back(end("g.entry.claim", 2));
    rows.push_back({{"event", "run_end"}, {"ok", true}});
    audit(rows);
    const auto errors = problems();
    EXPECT_TRUE(contains(errors, "invalid audit"));
    EXPECT_TRUE(contains(errors, "unverified action g.entry.claim"));
}

} // namespace
