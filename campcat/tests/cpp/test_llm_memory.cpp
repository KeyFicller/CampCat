#include <core/llm/llm_memory.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

TEST(LlmMemory, EmptyIdlePassPreservesCommittedCursor) {
    const auto root = std::filesystem::temp_directory_path() / "campcat_empty_memory_cursor";
    std::filesystem::remove_all(root);
    const auto kind = campcat::llm::memory_kind::operator_notes;
    ASSERT_TRUE(campcat::llm::append_pass(root, kind, "", "turn:1", "turn:2"));
    ASSERT_TRUE(campcat::llm::commit_pass(root, kind, "", {}, {}, "", "", nullptr).storage_ok);
    EXPECT_EQ(campcat::llm::last_pass_to(root, kind, ""), "turn:2");
    std::filesystem::remove_all(root);
}

TEST(LlmMemory, ModelProposalRejectsMalformedTypesWithoutPartialAcceptance) {
    campcat::llm::memory_proposal proposal;
    std::string error;
    EXPECT_TRUE(campcat::llm::parse_memory_proposal(
        R"({"ops":[{"op":"add","section":"偏好","text":"中文"}],"from":"turn:1","to":"turn:2"})",
        &proposal, &error));
    ASSERT_EQ(proposal.ops.size(), 1U);
    EXPECT_EQ(proposal.to, "turn:2");
    for (auto raw : {R"({"ops":[{"op":12}]})", R"({"ops":[{"op":"add","text":false}]})",
                    R"({"ops":[{"op":"invented"}]})", R"({"ops":[],"to":[]})",
                    R"({"ops":[{"op":"add"},12]})"}) {
        EXPECT_FALSE(campcat::llm::parse_memory_proposal(raw, &proposal, &error));
        EXPECT_TRUE(proposal.ops.empty());
        EXPECT_FALSE(error.empty());
    }
}
#include <iterator>

namespace {

namespace {

/// A clean config home per test, named so a crash leaves an obvious corpse.
std::filesystem::path ledger_dir(const char *_name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / _name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<campcat::llm::memory_entry>
ledger_entries(const std::filesystem::path &_dir,
               const std::string &_bundle = std::string()) {
    std::vector<campcat::llm::memory_entry> out;
    EXPECT_TRUE(campcat::llm::read_ledger(
        _dir, campcat::llm::memory_kind::operator_notes, _bundle, &out));
    return out;
}

campcat::llm::memory_op op_of(campcat::llm::memory_op::kind _k,
                              const std::string &_id = std::string(),
                              const std::string &_section = std::string(),
                              const std::string &_text = std::string()) {
    campcat::llm::memory_op op;
    op.k = _k;
    op.id = _id;
    op.section = _section;
    op.text = _text;
    return op;
}

int apply(const std::filesystem::path &_dir, const std::string &_bundle,
          const std::vector<campcat::llm::memory_op> &_ops, bool _by_user,
          std::vector<std::string> *_notes = nullptr) {
    return campcat::llm::apply_ops(_dir,
                                   campcat::llm::memory_kind::operator_notes,
                                   _bundle, _ops, {"turn:1"}, _by_user, _notes);
}

} // namespace

TEST(LlmMemory, AddEditPinRetireRoundTrip) {
    const auto dir = ledger_dir("campcat_ledger_roundtrip");
    using kind = campcat::llm::memory_op::kind;
    EXPECT_EQ(
        apply(dir, "", {op_of(kind::add, "", "偏好", "用中文回答")}, false), 1);

    auto entries = ledger_entries(dir);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].section, "偏好");
    EXPECT_EQ(entries[0].text, "用中文回答");
    ASSERT_EQ(entries[0].src.size(), 1U);
    EXPECT_EQ(entries[0].src[0], "turn:1");
    const std::string id = entries[0].id;

    EXPECT_EQ(apply(dir, "", {op_of(kind::edit, id, "", "用中文思考")}, false),
              1);
    EXPECT_EQ(ledger_entries(dir)[0].text, "用中文思考");

    // Pinning is the user's verb; a pass asking for it is refused.
    EXPECT_EQ(apply(dir, "", {op_of(kind::pin, id)}, false), 0);
    EXPECT_EQ(apply(dir, "", {op_of(kind::pin, id)}, true), 1);
    EXPECT_TRUE(ledger_entries(dir)[0].pinned);
    // And a pinned fact is closed to the pass from then on.
    EXPECT_EQ(apply(dir, "", {op_of(kind::edit, id, "", "别的")}, false), 0);

    EXPECT_EQ(apply(dir, "", {op_of(kind::retire, id)}, true), 1);
    EXPECT_FALSE(ledger_entries(dir)[0].visible);
    EXPECT_EQ(apply(dir, "", {op_of(kind::restore, id)}, true), 1);
    EXPECT_TRUE(ledger_entries(dir)[0].visible);

    EXPECT_EQ(apply(dir, "", {op_of(kind::unpin, id)}, true), 1);
    EXPECT_FALSE(ledger_entries(dir)[0].pinned);

    EXPECT_EQ(apply(dir, "", {op_of(kind::remove, id)}, true), 1);
    EXPECT_TRUE(ledger_entries(dir).empty());
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, PassCannotTouchWhatTheUserWrote) {
    const auto dir = ledger_dir("campcat_ledger_user");
    using kind = campcat::llm::memory_op::kind;
    ASSERT_EQ(
        apply(dir, "", {op_of(kind::add, "", "偏好", "用中文回答")}, true), 1);
    const std::string id = ledger_entries(dir)[0].id;

    std::vector<std::string> notes;
    EXPECT_EQ(
        apply(dir, "", {op_of(kind::edit, id, "", "改成英文")}, false, &notes),
        0);
    EXPECT_FALSE(notes.empty());
    EXPECT_EQ(ledger_entries(dir)[0].text, "用中文回答");

    notes.clear();
    EXPECT_EQ(apply(dir, "", {op_of(kind::retire, id)}, false, &notes), 0);
    EXPECT_FALSE(notes.empty());

    // Adding is still the pass's job: it is the only way facts accumulate.
    EXPECT_EQ(
        apply(dir, "", {op_of(kind::add, "", "叫法", "收菜 -> x")}, false), 1);
    EXPECT_EQ(ledger_entries(dir).size(), 2U);
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, BadOpsAreRefusedWhole) {
    const auto dir = ledger_dir("campcat_ledger_refused");
    using kind = campcat::llm::memory_op::kind;
    std::vector<std::string> notes;

    EXPECT_EQ(apply(dir, "", {op_of(kind::add, "", "读不出的分区", "x")}, false,
                    &notes),
              0);
    EXPECT_FALSE(notes.empty());

    notes.clear();
    EXPECT_EQ(
        apply(dir, "",
              {op_of(kind::add, "", "偏好",
                     std::string(campcat::llm::k_entry_max_chars + 1, 'x'))},
              false, &notes),
        0);
    EXPECT_FALSE(notes.empty());

    notes.clear();
    EXPECT_EQ(
        apply(dir, "", {op_of(kind::edit, "ffff", "", "x")}, false, &notes), 0);
    EXPECT_FALSE(notes.empty());

    EXPECT_TRUE(ledger_entries(dir).empty());
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, AWholePassIsBounded) {
    // One bad pass can be wrong; it must not also be unbounded.
    const auto dir = ledger_dir("campcat_ledger_bound");
    using kind = campcat::llm::memory_op::kind;
    std::vector<campcat::llm::memory_op> ops;
    for (int i = 0; i < campcat::llm::k_max_ops_per_pass + 5; ++i) {
        ops.push_back(
            op_of(kind::add, "", "偏好", "fact " + std::to_string(i)));
    }
    std::vector<std::string> notes;
    EXPECT_EQ(apply(dir, "", ops, false, &notes),
              campcat::llm::k_max_ops_per_pass);
    EXPECT_FALSE(notes.empty());
    EXPECT_EQ(static_cast<int>(ledger_entries(dir).size()),
              campcat::llm::k_max_ops_per_pass);
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, IdsAreUniqueAndNeverReused) {
    const auto dir = ledger_dir("campcat_ledger_ids");
    using kind = campcat::llm::memory_op::kind;
    ASSERT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", "a")}, false), 1);
    ASSERT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", "b")}, false), 1);
    auto entries = ledger_entries(dir);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_NE(entries[0].id, entries[1].id);
    EXPECT_EQ(entries[0].id.size(), 4U);

    // Removing the newest must not let the next add take its id: an old note
    // may still cite it.
    const std::string newest = entries[1].id;
    ASSERT_EQ(apply(dir, "", {op_of(kind::remove, newest)}, true), 1);
    ASSERT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", "c")}, false), 1);
    entries = ledger_entries(dir);
    EXPECT_NE(entries.back().id, newest);
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, BudgetTrimsUnpinnedOnly) {
    const auto dir = ledger_dir("campcat_ledger_budget");
    using kind = campcat::llm::memory_op::kind;
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(apply(dir, "",
                        {op_of(kind::add, "", "偏好", std::string(60, 'p'))},
                        false),
                  1);
    }
    ASSERT_EQ(
        apply(dir, "", {op_of(kind::add, "", "偏好", "pinned fact")}, true), 1);
    auto entries = ledger_entries(dir);
    const std::string pinned_id = entries.back().id;
    ASSERT_EQ(apply(dir, "", {op_of(kind::pin, pinned_id)}, true), 1);

    entries = ledger_entries(dir);
    int dropped = 0;
    const std::string rendered =
        campcat::llm::render_memory(entries, 200, &dropped);
    EXPECT_GT(dropped, 0);
    // The pinned row survives a budget it alone almost exhausts.
    EXPECT_NE(rendered.find("pinned fact"), std::string::npos);
    // Retired entries never reach the prompt at all.
    ASSERT_EQ(apply(dir, "", {op_of(kind::retire, pinned_id)}, true), 1);
    const std::string after =
        campcat::llm::render_memory(ledger_entries(dir), 8000, nullptr);
    EXPECT_EQ(after.find("pinned fact"), std::string::npos);

    // A budget of nothing, or a notebook all retired, renders empty rather than
    // a bare heading: the prompt must be what it was before notes existed.
    EXPECT_TRUE(
        campcat::llm::render_memory(ledger_entries(dir), 0, nullptr).empty());
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, ABrokenLineCostsOnlyThatLine) {
    const auto dir = ledger_dir("campcat_ledger_broken");
    using kind = campcat::llm::memory_op::kind;
    ASSERT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", "kept")}, false), 1);
    {
        std::ofstream out(
            campcat::llm::ledger_path(
                dir, campcat::llm::memory_kind::operator_notes, ""),
            std::ios::app);
        out << R"({"v":1,"kind":"add","id":"00ff","section":"偏好","te)";
        out << "\n";
    }
    const auto entries = ledger_entries(dir);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].text, "kept");
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, GraphScopeKeepsNamesApart) {
    const auto dir = ledger_dir("campcat_ledger_scope");
    using kind = campcat::llm::memory_op::kind;
    ASSERT_EQ(apply(dir, "campcat.home",
                    {op_of(kind::add, "", "叫法", "收菜 -> home.harvest")},
                    false),
              1);
    ASSERT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", "用中文")}, false),
              1);

    EXPECT_EQ(ledger_entries(dir, "campcat.home").size(), 2U); // both scopes
    EXPECT_EQ(ledger_entries(dir).size(), 1U); // the library alone
    EXPECT_EQ(ledger_entries(dir, "campcat.other").size(), 1U);

    // The name is written into the graph's own ledger, not the library's.
    EXPECT_TRUE(std::filesystem::exists(campcat::llm::ledger_path(
        dir, campcat::llm::memory_kind::operator_notes, "campcat.home")));
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, LastPassToTracksTheNewestReceipt) {
    const auto dir = ledger_dir("campcat_ledger_pass");
    using mo = campcat::llm::memory_kind;
    EXPECT_EQ(campcat::llm::last_pass_to(dir, mo::operator_notes, ""),
              std::string());
    ASSERT_TRUE(campcat::llm::append_pass(dir, mo::operator_notes, "", "turn:1",
                                          "turn:4"));
    EXPECT_EQ(campcat::llm::last_pass_to(dir, mo::operator_notes, ""),
              "turn:4");
    ASSERT_TRUE(campcat::llm::append_pass(dir, mo::operator_notes, "", "turn:5",
                                          "turn:9"));
    EXPECT_EQ(campcat::llm::last_pass_to(dir, mo::operator_notes, ""),
              "turn:9");
    // The scripter's receipt is its own: one agent's pass is not the other's.
    EXPECT_EQ(campcat::llm::last_pass_to(dir, mo::scripter_notes, ""),
              std::string());
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, TheTwoLedgersStayApart) {
    const auto dir = ledger_dir("campcat_ledger_apart");
    using mo = campcat::llm::memory_kind;
    std::string missing;
    ASSERT_TRUE(
        campcat::llm::read_memory(dir, mo::scripter_notes, "", &missing));
    EXPECT_TRUE(missing.empty());

    std::vector<std::string> notes;
    EXPECT_EQ(campcat::llm::apply_ops(dir, mo::operator_notes, "",
                                      {op_of(campcat::llm::memory_op::kind::add,
                                             "", "偏好", "keep")},
                                      {}, false, &notes),
              1);
    EXPECT_EQ(campcat::llm::apply_ops(
                  dir, mo::scripter_notes, "",
                  {op_of(campcat::llm::memory_op::kind::add, "", "坑", "pit")},
                  {}, false, &notes),
              1);

    std::string op;
    ASSERT_TRUE(campcat::llm::read_memory(dir, mo::operator_notes, "", &op));
    EXPECT_NE(op.find("keep"), std::string::npos);
    EXPECT_EQ(op.find("pit"), std::string::npos);

    EXPECT_TRUE(campcat::llm::clear_memory(dir, mo::scripter_notes, ""));
    ASSERT_TRUE(campcat::llm::read_memory(dir, mo::operator_notes, "", &op));
    EXPECT_NE(op.find("keep"), std::string::npos);
    std::string pit;
    ASSERT_TRUE(campcat::llm::read_memory(dir, mo::scripter_notes, "", &pit));
    EXPECT_TRUE(pit.empty());
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, ClearTranscriptIsIdempotent) {
    // A clear has to delete the file even with no sidecar running, or the
    // conversation comes back on the next launch.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "campcat_transcript_clear";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    EXPECT_TRUE(campcat::llm::clear_transcript(dir)); // never existed
    {
        std::ofstream out(campcat::llm::transcript_path(dir));
        out << "{}\n";
    }
    ASSERT_TRUE(std::filesystem::exists(campcat::llm::transcript_path(dir)));
    EXPECT_TRUE(campcat::llm::clear_transcript(dir));
    EXPECT_FALSE(std::filesystem::exists(campcat::llm::transcript_path(dir)));
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, AnUnreadableLedgerIsNeverWrittenTo) {
    // A ledger we could not read must not be appended to: the record would land
    // somewhere we cannot see, or overwrite what we failed to look at.
    const auto dir = ledger_dir("campcat_ledger_unread");
    {
        std::ofstream out(dir / "memory");
        out << "not a directory";
    }
    using kind = campcat::llm::memory_op::kind;
    std::vector<std::string> notes;
    EXPECT_EQ(
        apply(dir, "", {op_of(kind::add, "", "偏好", "new")}, false, &notes),
        0);
    EXPECT_FALSE(notes.empty());

    std::vector<campcat::llm::memory_entry> entries;
    EXPECT_FALSE(campcat::llm::read_ledger(
        dir, campcat::llm::memory_kind::operator_notes, "", &entries));
    std::ifstream in(dir / "memory");
    const std::string body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "not a directory");
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, DeletedIdsInOtherGraphsStillReserveTheirNumbers) {
    const auto dir = ledger_dir("campcat_ledger_historical_ids");
    using kind = campcat::llm::memory_op::kind;
    ASSERT_EQ(apply(dir, "other", {op_of(kind::add, "", "叫法", "old")}, false),
              1);
    const auto old = ledger_entries(dir, "other")[0].id;
    ASSERT_EQ(apply(dir, "other", {op_of(kind::remove, old)}, true), 1);
    ASSERT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", "new")}, false), 1);
    EXPECT_NE(ledger_entries(dir)[0].id, old);
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, IdsGrowPastFourHexDigitsWithoutReusingDeletedIds) {
    const auto dir = ledger_dir("campcat_ledger_wide_ids");
    const auto path = campcat::llm::ledger_path(
        dir, campcat::llm::memory_kind::operator_notes, "");
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path);
        out << R"({"v":1,"kind":"add","id":"ffff","section":"偏好","text":"old"})"
            << '\n';
        out << R"({"v":1,"kind":"delete","id":"ffff"})" << '\n';
    }
    using kind = campcat::llm::memory_op::kind;
    ASSERT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", "new")}, false), 1);
    const auto entries = ledger_entries(dir);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].id, "10000");
    EXPECT_EQ(apply(dir, "", {op_of(kind::edit, "10000", "", "edited")}, false),
              1);
    EXPECT_EQ(ledger_entries(dir)[0].text, "edited");
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, AnIncompleteTailCannotSwallowTheNextAppend) {
    const auto dir = ledger_dir("campcat_ledger_partial_tail");
    using kind = campcat::llm::memory_op::kind;
    ASSERT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", "kept")}, false), 1);
    {
        std::ofstream out(
            campcat::llm::ledger_path(
                dir, campcat::llm::memory_kind::operator_notes, ""),
            std::ios::app);
        out << R"({"v":1,"kind":"add","id":"00ff","text":)";
    }
    ASSERT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", "new")}, false), 1);
    const auto entries = ledger_entries(dir);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].text, "kept");
    EXPECT_EQ(entries[1].text, "new");
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, WrongFieldTypesAreSkippedWithoutChangingValidEntries) {
    const auto dir = ledger_dir("campcat_ledger_field_types");
    const auto path = campcat::llm::ledger_path(
        dir, campcat::llm::memory_kind::operator_notes, "");
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path);
        out << R"({"v":1,"kind":"add","id":"0001","section":"偏好","text":"kept"})"
            << '\n';
        out << R"({"v":"1","kind":"delete","id":"0001"})" << '\n';
        out << R"({"v":1,"kind":7,"id":"0001"})" << '\n';
        out << R"({"v":1,"kind":"delete","id":1})" << '\n';
        out << R"({"v":1,"kind":"edit","id":"0001","text":"bad edit","by":true})"
            << '\n';
        out << R"({"v":1,"kind":"add","id":"0002","section":false,"text":"bad section"})"
            << '\n';
        out << R"({"v":1,"kind":"add","id":"0003","section":"偏好","text":{}})"
            << '\n';
        out << R"({"v":1,"kind":"add","id":"0004","section":"偏好","text":"bad timestamp","at":[]})"
            << '\n';
        out << R"({"v":1,"kind":"pass","at":{},"to":"bad"})" << '\n';
        out << R"({"v":1,"kind":"pass","to":[]})" << '\n';
        out << R"({"v":1,"kind":"pass","to":"turn:4"})" << '\n';
    }
    std::vector<campcat::llm::memory_entry> entries;
    EXPECT_NO_THROW(EXPECT_TRUE(campcat::llm::read_ledger(
        dir, campcat::llm::memory_kind::operator_notes, "", &entries)));
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].text, "kept");
    EXPECT_EQ(campcat::llm::last_pass_to(
                  dir, campcat::llm::memory_kind::operator_notes, ""),
              "turn:4");
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, EntryLimitCountsUnicodeCodepoints) {
    const auto dir = ledger_dir("campcat_ledger_utf8");
    using kind = campcat::llm::memory_op::kind;
    std::string text;
    for (int i = 0; i < 100; ++i)
        text += "中😀";
    EXPECT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", text)}, false), 1);
    EXPECT_EQ(apply(dir, "", {op_of(kind::add, "", "偏好", text + "a")}, false),
              0);
    const auto entries = ledger_entries(dir);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].text, text);
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, PromptBudgetCountsUnicodeCodepointsIncludingFormatting) {
    campcat::llm::memory_entry entry;
    entry.section = "偏好";
    entry.text = "中😀";
    int dropped = -1;
    EXPECT_EQ(campcat::llm::render_memory({entry}, 11, &dropped),
              "## 偏好\n- 中😀\n");
    EXPECT_EQ(dropped, 0);
    EXPECT_TRUE(campcat::llm::render_memory({entry}, 10, &dropped).empty());
    EXPECT_EQ(dropped, 1);
}

TEST(LlmMemory, StorageFailuresAreSeparateFromRejectedOperations) {
    const auto dir = ledger_dir("campcat_ledger_storage_result");
    using namespace campcat::llm;
    bool storage_ok = false;
    EXPECT_EQ(
        apply_ops(dir, memory_kind::operator_notes, "",
                  {op_of(memory_op::kind::add, "", "unknown", "rejected")}, {},
                  false, nullptr, &storage_ok),
        0);
    EXPECT_TRUE(storage_ok);
    {
        std::ofstream out(dir / "memory");
        out << "blocked";
    }
    EXPECT_EQ(apply_ops(dir, memory_kind::operator_notes, "",
                        {op_of(memory_op::kind::add, "", "偏好", "valid")}, {},
                        false, nullptr, &storage_ok),
              0);
    EXPECT_FALSE(storage_ok);
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, FailedPassDoesNotAdvanceTheCommittedCursor) {
    const auto dir = ledger_dir("campcat_ledger_commit_failure");
    using namespace campcat::llm;
    ASSERT_TRUE(
        append_pass(dir, memory_kind::operator_notes, "", "turn:1", "turn:4"));
    std::filesystem::create_directories(dir / "graph");
    {
        std::ofstream out(dir / "graph" / "memory");
        out << "blocked";
    }
    const auto result =
        commit_pass(dir, memory_kind::operator_notes, "graph",
                    {op_of(memory_op::kind::add, "", "偏好", "valid")},
                    {"turn:5"}, "turn:5", "turn:9", nullptr);
    EXPECT_FALSE(result.storage_ok);
    EXPECT_EQ(result.written, 0);
    EXPECT_EQ(last_pass_to(dir, memory_kind::operator_notes, ""), "turn:4");
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, CommittedPassIsIdempotentAcrossLibraryAndGraph) {
    const auto dir = ledger_dir("campcat_ledger_commit_idempotent");
    using namespace campcat::llm;
    const std::vector<memory_op> ops{
        op_of(memory_op::kind::add, "", "偏好", "library"),
        op_of(memory_op::kind::add, "", "叫法", "graph"),
        op_of(memory_op::kind::add, "", "unknown", "rejected")};
    const auto first =
        commit_pass(dir, memory_kind::operator_notes, "graph", ops, {"turn:1"},
                    "turn:1", "turn:4", nullptr);
    EXPECT_TRUE(first.storage_ok);
    EXPECT_EQ(first.written, 2);
    EXPECT_EQ(first.rejected, 1);
    const auto second =
        commit_pass(dir, memory_kind::operator_notes, "graph", ops, {"turn:1"},
                    "turn:1", "turn:4", nullptr);
    EXPECT_TRUE(second.storage_ok);
    EXPECT_EQ(second.written, 0);
    EXPECT_EQ(ledger_entries(dir, "graph").size(), 2U);
    EXPECT_EQ(ledger_entries(dir).size(), 1U);
    EXPECT_EQ(last_pass_to(dir, memory_kind::operator_notes, "graph"),
              "turn:4");
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, InterruptedPassRecoversWithoutDuplicatingItsLibraryWrite) {
    const auto dir = ledger_dir("campcat_ledger_commit_recover");
    using namespace campcat::llm;
    ASSERT_TRUE(
        append_pass(dir, memory_kind::operator_notes, "", "turn:1", "turn:4"));
    const auto graph_dir = dir / "graph" / "memory";
    std::filesystem::create_directories(graph_dir);
    std::filesystem::permissions(graph_dir,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_exec);
    const auto failed =
        commit_pass(dir, memory_kind::operator_notes, "graph",
                    {op_of(memory_op::kind::add, "", "偏好", "library"),
                     op_of(memory_op::kind::add, "", "叫法", "graph")},
                    {"turn:5", "turn:9"}, "turn:5", "turn:9", nullptr);
    EXPECT_FALSE(failed.storage_ok);
    EXPECT_EQ(last_pass_to(dir, memory_kind::operator_notes, ""), "turn:4");
    std::filesystem::permissions(graph_dir, std::filesystem::perms::owner_all);
    const auto recovered = ledger_entries(dir, "graph");
    ASSERT_EQ(recovered.size(), 2U);
    EXPECT_EQ(last_pass_to(dir, memory_kind::operator_notes, "graph"),
              "turn:9");
    const auto retried =
        commit_pass(dir, memory_kind::operator_notes, "graph",
                    {op_of(memory_op::kind::add, "", "偏好", "library"),
                     op_of(memory_op::kind::add, "", "叫法", "graph")},
                    {"turn:5", "turn:9"}, "turn:5", "turn:9", nullptr);
    EXPECT_TRUE(retried.storage_ok);
    EXPECT_EQ(retried.written, 0);
    EXPECT_EQ(ledger_entries(dir, "graph").size(), 2U);
    std::filesystem::remove_all(dir);
}

} // namespace
