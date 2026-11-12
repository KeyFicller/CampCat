#include "app/llm_prompt_queue.h"

#include <gtest/gtest.h>

namespace {

TEST(LlmPromptQueue, LatestCorrectionReplacesThePendingMessage) {
    campcat::llm_prompt_queue queue;
    EXPECT_TRUE(queue.replace("open the city"));
    EXPECT_TRUE(queue.replace("open recruitment instead"));
    ASSERT_EQ(queue.take(), "open recruitment instead");
    EXPECT_FALSE(queue.take().has_value());
}

TEST(LlmPromptQueue, StoppingDropsQueuedWorkAndRefusesNewMessages) {
    campcat::llm_prompt_queue queue;
    ASSERT_TRUE(queue.replace("run recruitment"));
    EXPECT_TRUE(queue.stop());
    EXPECT_FALSE(queue.stop());
    EXPECT_TRUE(queue.stopping());
    EXPECT_FALSE(queue.take().has_value());
    EXPECT_FALSE(queue.replace("this must not run while stopping"));
    EXPECT_FALSE(queue.take().has_value());
}

TEST(LlmPromptQueue, ANewTaskDoesNotReviveTheCancelledMessage) {
    campcat::llm_prompt_queue queue;
    ASSERT_TRUE(queue.replace("cancelled task"));
    queue.stop();
    queue.restart();
    EXPECT_FALSE(queue.stopping());
    EXPECT_FALSE(queue.take().has_value());
    ASSERT_TRUE(queue.replace("new task"));
    EXPECT_EQ(queue.take(), "new task");
}

} // namespace
