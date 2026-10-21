// The smoke run's list file is the whole definition of "the graph is done", so
// what counts as an instruction is worth pinning: a CR sneaking through would
// ride on every prompt the model sees.

#include <core/smoke/smoke_run.h>

#include <gtest/gtest.h>

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

} // namespace
