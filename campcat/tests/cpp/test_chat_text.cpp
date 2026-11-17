#include "app/ui/chat_text.h"

#include <gtest/gtest.h>

using campcat::app::chat_block;
using campcat::app::chat_blocks;
using campcat::app::first_line;
using campcat::app::split_tool_note;

TEST(chat_blocks, blank_lines_only_separate) {
    const std::vector<chat_block> want{{false, "one\ntwo"}, {false, "three"}};
    EXPECT_EQ(chat_blocks("\n\none\ntwo\n\n\n  \nthree\n\n"), want);
    EXPECT_TRUE(chat_blocks("").empty());
    EXPECT_TRUE(chat_blocks("\n \n").empty());
}

TEST(chat_blocks, crlf_is_a_newline) {
    const std::vector<chat_block> want{{false, "a"}, {false, "b"}};
    EXPECT_EQ(chat_blocks("a\r\n\r\nb\r\n"), want);
}

TEST(chat_blocks, fence_keeps_its_lines_verbatim) {
    const std::vector<chat_block> want{
        {false, "before"}, {true, "x = `a`\n\n  y"}, {false, "after"}};
    EXPECT_EQ(chat_blocks("before\n```ccat\nx = `a`\n\n  y\n```\nafter"), want);
}

TEST(chat_blocks, open_fence_closes_at_the_end) {
    const std::vector<chat_block> want{{false, "text"}, {true, "partial"}};
    EXPECT_EQ(chat_blocks("text\n```\npartial\n"), want);
}

TEST(chat_blocks, inline_code_becomes_an_empty_link) {
    const std::vector<chat_block> want{{false, "run [demo.home]() then [b]()."}};
    EXPECT_EQ(chat_blocks("run `demo.home` then `b`."), want);
}

TEST(chat_blocks, bold_around_inline_code_is_dropped) {
    const std::vector<chat_block> want{{false, "use [x]() now **bold**"}};
    EXPECT_EQ(chat_blocks("use **`x`** now **bold**"), want);
}

TEST(chat_blocks, unsafe_inline_code_stays_raw) {
    const std::vector<chat_block> want{{false, "`a]b` and `` and `open"}};
    EXPECT_EQ(chat_blocks("`a]b` and `` and `open"), want);
}

TEST(chat_blocks, heading_drops_markers_it_cannot_style) {
    const std::vector<chat_block> want{{false, "## the title"}};
    EXPECT_EQ(chat_blocks("## **the** `title`"), want);
}

TEST(chat_blocks, bold_after_a_cjk_character_gets_its_space) {
    const std::vector<chat_block> want{
        {false, "图标： **游戏中心**、 **鹿语云**\n**行首** ok"}};
    EXPECT_EQ(chat_blocks("图标：**游戏中心**、**鹿语云**\n**行首** ok"), want);
}

TEST(split_tool_note, host_note_with_body) {
    const auto parts = split_tool_note("**screenshot** error\n\nscreencap failed\nmore\n");
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->name, "screenshot");
    EXPECT_EQ(parts->status, "error");
    EXPECT_EQ(parts->body, "screencap failed\nmore");
}

TEST(split_tool_note, note_without_body) {
    const auto parts = split_tool_note("**tap** ok");
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->name, "tap");
    EXPECT_EQ(parts->status, "ok");
    EXPECT_TRUE(parts->body.empty());
}

TEST(split_tool_note, subagent_event_status_is_its_args) {
    const auto parts = split_tool_note(R"(**read_file** {"path": "a.ccat"})");
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->name, "read_file");
    EXPECT_EQ(parts->status, R"({"path": "a.ccat"})");
}

TEST(split_tool_note, other_text_is_not_a_note) {
    EXPECT_FALSE(split_tool_note("plain reply").has_value());
    EXPECT_FALSE(split_tool_note("****").has_value());
    EXPECT_FALSE(split_tool_note("**open\nclose** x").has_value());
}

TEST(first_line, skips_blank_lines_and_trims) {
    EXPECT_EQ(first_line("\n\n  first  \nsecond"), "first");
    EXPECT_EQ(first_line(""), "");
}
