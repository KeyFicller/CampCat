#include <core/llm/llm_host.h>
#include <gtest/gtest.h>
#include <string>

namespace {

using campcat::tool_note;

TEST(ToolNote, StatusAndBodyAreSeparateParagraphs) {
    // The blank line matters: without it a multi-line payload is one soft
    // wrapped paragraph, so the status fuses onto the payload's first line.
    EXPECT_EQ(tool_note("tap", "ok", "tapped 512,1024"),
              "**tap** ok\n\ntapped 512,1024");
}

TEST(ToolNote, AnEmptyBodyLeavesTheStatusAlone) {
    // `screenshot` and friends return an image and no text; a trailing blank
    // line there would just be an empty row in the bubble.
    EXPECT_EQ(tool_note("screenshot", "ok", ""), "**screenshot** ok");
}

TEST(ToolNote, AnErrorReadsTheSameWay) {
    EXPECT_EQ(tool_note("run_script", "error", "no such bundle"),
              "**run_script** error\n\nno such bundle");
}

TEST(ToolNote, ARunOfBlankLinesCollapsesToOne) {
    EXPECT_EQ(tool_note("read_script", "ok", "a\n\n\n\nb"),
              "**read_script** ok\n\na\n\nb");
}

TEST(ToolNote, AWhitespaceOnlyLineCountsAsBlank) {
    EXPECT_EQ(tool_note("read_script", "ok", "a\n   \n \t \nb"),
              "**read_script** ok\n\na\n   \nb");
}

TEST(ToolNote, ABlankLineAtTheBodyStartDoesNotDoubleTheSeparator) {
    EXPECT_EQ(tool_note("read_script", "ok", "\n\nbundle \"gold\""),
              "**read_script** ok\n\nbundle \"gold\"");
}

TEST(ToolNote, ASingleBlankLineSurvives) {
    // The payload still separates its own blocks; only runs get folded.
    EXPECT_EQ(tool_note("read_script", "ok", "header\n\nsource"),
              "**read_script** ok\n\nheader\n\nsource");
}

TEST(ToolNote, NoBlankLineMeansNoChange) {
    EXPECT_EQ(tool_note("list_scripts", "ok", "a -- one\nb -- two"),
              "**list_scripts** ok\n\na -- one\nb -- two");
}

} // namespace
