#include <core/llm/frame_store.h>
#include <gtest/gtest.h>

TEST(FrameStore, EvictionPreservesOldShotAndBoundsMemory) {
    campcat::llm::frame_store frames(6);
    const int first = frames.put({10, 20, "aaaa"});
    const int second = frames.put({30, 40, "bbbb"});
    EXPECT_LE(frames.memory_bytes(), 6U);
    ASSERT_TRUE(frames.get(first).has_value());
    EXPECT_EQ(frames.get(first)->png, "aaaa");
    EXPECT_EQ(frames.get(second)->w, 30);
    frames.clear();
    EXPECT_FALSE(frames.get(first).has_value());
    EXPECT_EQ(frames.put({1, 1, "x"}), 1);
}

TEST(FrameStore, OversizedFrameIsAvailableWithoutBeingRetainedInMemory) {
    campcat::llm::frame_store frames(2);
    const int shot = frames.put({2, 3, "large image"});
    EXPECT_EQ(frames.memory_bytes(), 0U);
    EXPECT_EQ(frames.get(shot)->png, "large image");
    EXPECT_FALSE(frames.get(shot + 1).has_value());
}
