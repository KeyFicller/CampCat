#include "core/script/ccat_repl.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

int g_fails = 0;

void expect_bool(const char *name, bool got, bool want) {
  if (got != want) {
    std::fprintf(stderr, "FAIL %s: got %s want %s\n", name,
                 got ? "true" : "false", want ? "true" : "false");
    ++g_fails;
  }
}

} // namespace

int main() {
  using campcat::ccat_lang::is_source_complete;

  expect_bool("single_log", is_source_complete("log(\"hi\")"), true);
  expect_bool("open_paren", is_source_complete("log(\"hi\""), false);
  expect_bool("open_brace", is_source_complete("if (\"a.png\") {"), false);
  expect_bool("if_head_no_brace", is_source_complete("if (\"a.png\")"), false);
  expect_bool("if_complete",
              is_source_complete("if (\"a.png\") {\n  tap(\"ok.png\")\n}"),
              true);
  expect_bool("unclosed_string", is_source_complete("log(\"hi"), false);
  expect_bool("comment_brace", is_source_complete("log(\"x\") // {\n"), true);
  expect_bool("retry_head", is_source_complete("retry(3)"), false);
  expect_bool("else_head",
              is_source_complete("if (\"a.png\") {\n}\nelse"), false);
  expect_bool("do_while_complete",
              is_source_complete("do {\n  wait(1)\n} while (\"x.png\")"), true);

  {
    campcat::ccat_lang::CcatRepl r;
    expect_bool("prompt_primary", std::string_view(r.prompt()) == ">>>", true);
    auto a = r.feed_line("if (\"a.png\")");
    expect_bool("feed_if_continue",
                a == campcat::ccat_lang::repl_feed_result::continue_input, true);
    expect_bool("prompt_cont", std::string_view(r.prompt()) == "...", true);
    auto b = r.feed_line("{");
    expect_bool("feed_brace_cont",
                b == campcat::ccat_lang::repl_feed_result::continue_input, true);
    auto c = r.feed_line("  log(\"x\")");
    expect_bool("feed_body_cont",
                c == campcat::ccat_lang::repl_feed_result::continue_input, true);
    auto d = r.feed_line("}");
    expect_bool("feed_ready", d == campcat::ccat_lang::repl_feed_result::ready,
                true);
    expect_bool("pending_has_if",
                r.pending.find("if (\"a.png\")") != std::string::npos, true);

    campcat::ccat_lang::CcatRepl r2;
    expect_bool("empty_ignored",
                r2.feed_line("   ") ==
                    campcat::ccat_lang::repl_feed_result::ignored,
                true);
    auto e = r2.feed_line("log(\"ok\")");
    expect_bool("single_ready", e == campcat::ccat_lang::repl_feed_result::ready,
                true);
    r2.clear();
    expect_bool("cleared_prompt", std::string_view(r2.prompt()) == ">>>", true);
  }

  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::puts("ccat_repl_test OK");
  return 0;
}
