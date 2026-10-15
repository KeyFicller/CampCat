#include <core/script/ccat_graph.h>
#include <core/script/ccat_parser.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using campcat::ccat_lang::anchor_kind;
using campcat::ccat_lang::anchor_script_problems;
using campcat::ccat_lang::graph_part_name;
using campcat::ccat_lang::parse_golden_md;
using campcat::ccat_lang::parse_program;

std::vector<std::string> problems_of(const std::string &_source) {
    return anchor_script_problems(*parse_program(_source));
}

bool mentions(const std::vector<std::string> &_problems,
              const std::string &_word) {
    for (const std::string &p : _problems) {
        if (p.find(_word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

TEST(CcatAnchor, GoldenParsesKindAndBody) {
    std::string err;
    const auto doc = parse_golden_md(
        "---\nkind: popup\nclose: g.notice_close\n---\nnotice popup\n", &err);
    ASSERT_TRUE(doc.has_value()) << err;
    EXPECT_EQ(doc->kind, anchor_kind::popup);
    EXPECT_EQ(doc->close, "g.notice_close");
    EXPECT_EQ(doc->description, "notice popup");
}

TEST(CcatAnchor, GoldenDefaultsToPage) {
    std::string err;
    const auto doc = parse_golden_md("---\n---\nmain city", &err);
    ASSERT_TRUE(doc.has_value()) << err;
    EXPECT_EQ(doc->kind, anchor_kind::page);
    EXPECT_EQ(doc->description, "main city");
}

TEST(CcatAnchor, GoldenRejects) {
    for (const char *text :
         {"main city, no front matter\n", "---\nkind: foo\n---\nx\n",
          "---\nkind: popup\n---\nx\n", "---\nx: 1\n---\nx\n",
          "---\nkind: page\nno closing line\n"}) {
        std::string err;
        EXPECT_FALSE(parse_golden_md(text, &err).has_value()) << text;
        EXPECT_FALSE(err.empty()) << text;
    }
}

TEST(CcatAnchor, GoldenAcceptsCrlfAndBom) {
    std::string err;
    const auto doc = parse_golden_md(
        "\xEF\xBB\xBF---\r\nkind: transient\r\n---\r\nloading\r\n", &err);
    ASSERT_TRUE(doc.has_value()) << err;
    EXPECT_EQ(doc->kind, anchor_kind::transient);
    EXPECT_EQ(doc->description, "loading");
}

TEST(CcatAnchor, AnchorScriptAcceptsJudgement) {
    EXPECT_TRUE(problems_of("defs { a = \"a.png\" }\n"
                            "if (has(a)) { return true } elif (has(\"b.png\")) "
                            "{ log(\"b\") return true }\n"
                            "log(\"x\")\nreturn false\n")
                    .empty());
}

TEST(CcatAnchor, AnchorScriptRejectsActions) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"tap(\"a.png\")\nreturn true", "tap"},
        {"wait(10)\nreturn true", "wait"},
        {"wait_until(\"a.png\", 100)\nreturn true", "wait_until"},
        {"run(\"x/main.ccat\")\nreturn true", "run"},
        {"home\nreturn true", "home"},
        {"loop(2) { log(\"x\") }\nreturn true", "loop"},
        {"if (at(\"g:a\")) { return true }\nreturn false", "at"},
        {"goto(\"g:a\")\nreturn true", "goto"},
    };
    for (const auto &[source, word] : cases) {
        const auto problems = problems_of(source);
        EXPECT_TRUE(mentions(problems, word)) << source;
    }
}

TEST(CcatAnchor, AnchorScriptRejectsActionInCondition) {
    EXPECT_TRUE(mentions(
        problems_of("if (tap(\"a.png\")) { return true }\nreturn false"),
        "tap"));
}

TEST(CcatAnchor, AnchorScriptNeedsReturn) {
    EXPECT_TRUE(mentions(problems_of("log(\"x\")"), "no return"));
}

TEST(CcatAnchor, GraphPartName) {
    EXPECT_TRUE(graph_part_name("main_city2"));
    EXPECT_FALSE(graph_part_name(""));
    EXPECT_FALSE(graph_part_name("a-b"));
    EXPECT_FALSE(graph_part_name("a.b"));
    EXPECT_FALSE(graph_part_name("Main"));
}

} // namespace
