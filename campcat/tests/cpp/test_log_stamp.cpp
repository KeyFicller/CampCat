#include "app/ui/log_panel.h"

#include <gtest/gtest.h>

TEST(log_stamp, reads_a_leading_clock) {
    EXPECT_EQ(log_stamp_len("[12:31:56] [script] back"), 10u);
}

TEST(log_stamp, a_tag_is_not_a_clock) {
    EXPECT_EQ(log_stamp_len("[ui] cfg=config.json"), 0u);
}

TEST(log_stamp, short_or_empty_is_zero) {
    EXPECT_EQ(log_stamp_len(""), 0u);
    EXPECT_EQ(log_stamp_len("[12:31]"), 0u);
}

TEST(log_stamp, digits_must_be_digits) {
    EXPECT_EQ(log_stamp_len("[ab:cd:ef] x"), 0u);
}
