#include <core/script/bundle_store.h>
#include <core/file_io.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <core/llm/llm_memory.h>
#include <future>
#include <chrono>

namespace {
std::string read(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
}

TEST(BundleStore, FailedStagingPreservesEveryOldFile) {
    const auto root = std::filesystem::temp_directory_path() / "campcat_bundle_store_failure";
    std::filesystem::remove_all(root);
    campcat::write_file_atomic(root / "skill/main.ccat", "old");
    campcat::write_file_atomic(root / "skill/a.png", "old image");
    EXPECT_THROW(campcat::ccat_lang::publish_bundle(root, "skill", [](const auto &stage) {
        campcat::write_file_atomic(stage / "main.ccat", "new");
        throw std::runtime_error("image encoding failed");
    }), std::runtime_error);
    EXPECT_EQ(read(root / "skill/main.ccat"), "old");
    EXPECT_EQ(read(root / "skill/a.png"), "old image");
    std::filesystem::remove_all(root);
}

TEST(BundleStore, PublishesCompleteGenerationAndRecoversInterruptedSwap) {
    const auto root = std::filesystem::temp_directory_path() / "campcat_bundle_store_recovery";
    std::filesystem::remove_all(root);
    campcat::write_file_atomic(root / "skill/main.ccat", "old");
    campcat::write_file_atomic(root / "skill/golden.md", "old description");
    campcat::ccat_lang::publish_bundle(root, "skill", [](const auto &stage) {
        campcat::write_file_atomic(stage / "main.ccat", "new");
        campcat::write_file_atomic(stage / "golden.md", "new description");
    });
    EXPECT_EQ(read(root / "skill/main.ccat"), "new");
    EXPECT_EQ(read(root / "skill/golden.md"), "new description");
    std::filesystem::rename(root / "skill", root / ".campcat-transactions/skill.previous");
    campcat::ccat_lang::recover_bundle(root, "skill");
    EXPECT_EQ(read(root / "skill/main.ccat"), "new");
    std::filesystem::remove_all(root);
}

TEST(BundleStore, PublishingCannotLoseConcurrentUserMemoryEdit) {
    using namespace std::chrono_literals;
    namespace memory = campcat::llm;
    const auto root = std::filesystem::temp_directory_path() / "campcat_bundle_memory_concurrency";
    std::filesystem::remove_all(root);
    campcat::write_file_atomic(root / "skill/main.ccat", "old");
    memory::memory_op note;
    note.section = "叫法"; note.text = "old fact";
    ASSERT_EQ(memory::apply_ops(root, memory::memory_kind::operator_notes, "skill", {note}, {}, true, nullptr), 1);
    std::promise<void> staged, resume;
    auto resume_signal = resume.get_future();
    auto publication = std::async(std::launch::async, [&] {
        campcat::ccat_lang::publish_bundle(root, "skill", [&](const auto &stage) {
            campcat::write_file_atomic(stage / "main.ccat", "new");
            staged.set_value(); resume_signal.wait();
        });
    });
    staged.get_future().wait();
    auto edit = std::async(std::launch::async, [&] {
        note.text = "user fact";
        return memory::apply_ops(root, memory::memory_kind::operator_notes, "skill", {note}, {}, true, nullptr);
    });
    edit.wait_for(200ms);
    resume.set_value(); publication.get();
    EXPECT_EQ(edit.get(), 1);
    std::vector<memory::memory_entry> entries;
    ASSERT_TRUE(memory::read_ledger(root, memory::memory_kind::operator_notes, "skill", &entries));
    EXPECT_EQ(entries.size(), 2U);
    std::filesystem::remove_all(root);
}
