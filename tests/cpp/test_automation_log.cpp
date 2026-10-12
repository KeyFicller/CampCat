// Skeleton for the C++ gate. Two cases, enough to prove the harness links and
// runs; the rest of the matrix is listed at the bottom for later.

#include <core/automation_log.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using campcat::automation_log;
using campcat::script_log_text;

// The registry is process-wide, so a case that leaks a sink changes what every
// later case observes.
class AutomationLog : public testing::Test {
protected:
    void SetUp() override { automation_log::clear_sink(); }
    void TearDown() override { automation_log::clear_sink(); }
};

TEST_F(AutomationLog, HandleLifetime) {
    std::vector<std::string> got;
    {
        auto live = automation_log::add_sink(
            [&](const std::string &line) { got.push_back(line); });
        automation_log::emit("while alive");
    }
    automation_log::emit("after death");

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "while alive");
}

TEST_F(AutomationLog, ScriptLogTag) {
    EXPECT_EQ(script_log_text("[script] hello"),
              std::optional<std::string>("hello"));
    // The interpreter's own diagnostics are untagged, and so is anything else.
    EXPECT_EQ(script_log_text("[ccat] hello"), std::nullopt);
    EXPECT_EQ(script_log_text("plain"), std::nullopt);
    EXPECT_EQ(script_log_text(""), std::nullopt);
}

// Still to write, all on this same fixture:
//   - a moved-from handle must not unregister the sink it moved into
//   - set_sink must drop the sinks add_sink registered earlier
//   - a sink that emits from inside emit must not deadlock
//   - a sink that drops its own handle from inside emit must not deadlock
//   - script_log_suffix's 40-line and 200-char caps, and its "and N more" tail

} // namespace
