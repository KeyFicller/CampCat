#include "core/script/ccat_repl.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace campcat::ccat_lang {
namespace {

bool starts_with_kw(std::string_view s, std::string_view kw) {
  if (s.size() < kw.size()) {
    return false;
  }
  if (s.compare(0, kw.size(), kw) != 0) {
    return false;
  }
  if (s.size() == kw.size()) {
    return true;
  }
  const unsigned char next = static_cast<unsigned char>(s[kw.size()]);
  return !std::isalnum(next) && next != '_';
}

} // namespace

bool is_source_complete(std::string_view source) {
  int paren = 0;
  int brace = 0;
  bool awaiting_brace = false;
  bool after_do_body = false;

  size_t i = 0;
  const size_t n = source.size();

  auto skip_ws = [&]() {
    while (i < n) {
      const unsigned char c = static_cast<unsigned char>(source[i]);
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++i;
        continue;
      }
      if (i + 1 < n && source[i] == '/' && source[i + 1] == '/') {
        i += 2;
        while (i < n && source[i] != '\n') {
          ++i;
        }
        continue;
      }
      break;
    }
  };

  auto scan_string = [&]() -> bool {
    // Opening " already consumed.
    while (i < n) {
      const char c = source[i++];
      if (c == '\\') {
        if (i < n) {
          ++i;
        }
        continue;
      }
      if (c == '"') {
        return true;
      }
    }
    return false; // unclosed
  };

  auto read_ident = [&]() -> std::string_view {
    const size_t start = i;
    while (i < n) {
      const unsigned char c = static_cast<unsigned char>(source[i]);
      if (std::isalnum(c) || c == '_') {
        ++i;
        continue;
      }
      break;
    }
    return source.substr(start, i - start);
  };

  while (i < n) {
    skip_ws();
    if (i >= n) {
      break;
    }

    const char c = source[i];

    if (c == '"') {
      ++i;
      if (!scan_string()) {
        return false;
      }
      awaiting_brace = false;
      after_do_body = false;
      continue;
    }

    if (c == '(') {
      ++i;
      ++paren;
      continue;
    }
    if (c == ')') {
      ++i;
      --paren;
      if (paren < 0) {
        return true; // let parser report; treat as "complete enough"
      }
      continue;
    }
    if (c == '{') {
      ++i;
      ++brace;
      awaiting_brace = false;
      after_do_body = false;
      continue;
    }
    if (c == '}') {
      ++i;
      --brace;
      if (brace < 0) {
        return true;
      }
      if (brace == 0 && paren == 0) {
        // Possible end of `do { ... }` — next may be `while (...)`.
        after_do_body = true;
      }
      continue;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const std::string_view id = read_ident();

      if (after_do_body && starts_with_kw(id, "while")) {
        after_do_body = false;
        skip_ws();
        if (i < n && source[i] == '(') {
          // Consume balanced while (...); statement then complete.
          ++i;
          int depth = 1;
          while (i < n && depth > 0) {
            if (source[i] == '"') {
              ++i;
              if (!scan_string()) {
                return false;
              }
              continue;
            }
            if (source[i] == '(') {
              ++depth;
            } else if (source[i] == ')') {
              --depth;
            } else if (i + 1 < n && source[i] == '/' && source[i + 1] == '/') {
              i += 2;
              while (i < n && source[i] != '\n') {
                ++i;
              }
              continue;
            }
            ++i;
          }
          if (depth != 0) {
            return false;
          }
          awaiting_brace = false;
          continue;
        }
        // `while` without `(` after do-body — incomplete / parse later
        awaiting_brace = false;
        continue;
      }

      after_do_body = false;

      if (starts_with_kw(id, "if") || starts_with_kw(id, "retry") ||
          starts_with_kw(id, "loop")) {
        skip_ws();
        if (i >= n || source[i] != '(') {
          awaiting_brace = true;
          continue;
        }
        ++i;
        int depth = 1;
        while (i < n && depth > 0) {
          if (source[i] == '"') {
            ++i;
            if (!scan_string()) {
              return false;
            }
            continue;
          }
          if (source[i] == '(') {
            ++depth;
          } else if (source[i] == ')') {
            --depth;
          } else if (i + 1 < n && source[i] == '/' && source[i + 1] == '/') {
            i += 2;
            while (i < n && source[i] != '\n') {
              ++i;
            }
            continue;
          }
          ++i;
        }
        if (depth != 0) {
          return false;
        }
        skip_ws();
        if (i < n && source[i] == '{') {
          // Will be handled by `{` branch next iteration — but we already
          // know brace follows, so clear awaiting and let `{` consume.
          awaiting_brace = false;
        } else {
          awaiting_brace = true;
        }
        continue;
      }

      if (starts_with_kw(id, "else") || starts_with_kw(id, "do")) {
        skip_ws();
        if (i < n && source[i] == '{') {
          awaiting_brace = false;
        } else {
          awaiting_brace = true;
        }
        continue;
      }

      awaiting_brace = false;
      continue;
    }

    // punctuation / numbers / $Debug etc.
    ++i;
    after_do_body = false;
    if (c != ';') {
      // keep awaiting_brace for `;` after head? rare — clear on other tokens
      awaiting_brace = false;
    }
  }

  if (paren != 0 || brace != 0) {
    return false;
  }
  if (awaiting_brace) {
    return false;
  }
  return true;
}

bool CcatRepl::awaiting_continuation() const { return !pending.empty(); }

const char *CcatRepl::prompt() const {
  return awaiting_continuation() ? "..." : ">>>";
}

void CcatRepl::clear() { pending.clear(); }

repl_feed_result CcatRepl::feed_line(std::string_view line) {
  auto is_ws = [](unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
  };
  const bool blank = std::all_of(
      line.begin(), line.end(),
      [&](char ch) { return is_ws(static_cast<unsigned char>(ch)); });
  if (pending.empty() && blank) {
    return repl_feed_result::ignored;
  }

  history.push_back(std::string(prompt()) + " " + std::string(line));
  if (!pending.empty()) {
    pending.push_back('\n');
  }
  pending.append(line.begin(), line.end());
  if (!is_source_complete(pending)) {
    return repl_feed_result::continue_input;
  }
  return repl_feed_result::ready;
}

} // namespace campcat::ccat_lang
