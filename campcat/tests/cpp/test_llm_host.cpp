#include <core/llm/llm_host.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using campcat::llm_event;
using campcat::llm_events_append_text;
using campcat::llm_events_append_thinking;
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

TEST(LlmEvents, TextDeltasGrowOneStep) {
    std::vector<llm_event> events;
    llm_events_append_text(events, "tap ");
    llm_events_append_text(events, "512,1024");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].type, llm_event::kind::text);
    EXPECT_EQ(events[0].text, "tap 512,1024");
}

TEST(LlmEvents, AnEmptyDeltaAddsNothing) {
    // A chunk carries `thinking` and no text; without this the timeline would
    // grow an empty step per heartbeat.
    std::vector<llm_event> events;
    llm_events_append_text(events, "");
    EXPECT_TRUE(events.empty());
}

TEST(LlmEvents, AToolStepStartsANewTextStep) {
    // The boundary is the point: text before a tool call and text after it are
    // two steps, so the tool lands between them where it happened.
    std::vector<llm_event> events;
    llm_events_append_text(events, "let me look");
    events.push_back(llm_event{llm_event::kind::tool, "**screenshot** ok"});
    llm_events_append_text(events, "I see the button");

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].type, llm_event::kind::text);
    EXPECT_EQ(events[1].type, llm_event::kind::tool);
    EXPECT_EQ(events[2].type, llm_event::kind::text);
    EXPECT_EQ(events[2].text, "I see the button");
}

TEST(LlmEvents, AnImageStepStartsANewTextStep) {
    std::vector<llm_event> events;
    llm_events_append_text(events, "look");
    llm_event image;
    image.type = llm_event::kind::image;
    image.image_id = 7;
    events.push_back(image);
    llm_events_append_text(events, "now");

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[1].type, llm_event::kind::image);
    EXPECT_EQ(events[1].image_id, 7);
    EXPECT_EQ(events[2].type, llm_event::kind::text);
}

TEST(LlmEvents, ReasoningGetsAStepOfItsOwn) {
    // It must not fold into the answer: the UI draws the two differently and
    // collapses the reasoning once the step ends.
    std::vector<llm_event> events;
    llm_events_append_thinking(events, "the button is ");
    llm_events_append_thinking(events, "bottom right");
    llm_events_append_text(events, "Tapping it.");

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].type, llm_event::kind::thinking);
    EXPECT_EQ(events[0].text, "the button is bottom right");
    EXPECT_EQ(events[1].type, llm_event::kind::text);
    EXPECT_EQ(events[1].text, "Tapping it.");
}

TEST(LlmEvents, ReasoningAfterAToolStartsANewStep) {
    std::vector<llm_event> events;
    llm_events_append_thinking(events, "first");
    events.push_back(llm_event{llm_event::kind::tool, "**tap** ok"});
    llm_events_append_thinking(events, "second");

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[2].type, llm_event::kind::thinking);
    EXPECT_EQ(events[2].text, "second");
}

} // namespace
