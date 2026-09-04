#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace campcat::ccat_lang {

/**
 * @brief True when `_source` is delimiter-complete for REPL submit
 *        (parens/braces balanced, strings closed, no control head awaiting `{`).
 */
bool is_source_complete(std::string_view source);

enum class repl_feed_result { ignored, continue_input, ready };

/**
 * @brief Multiline REPL session buffer (Python-style >>> / ...).
 */
struct CcatRepl {
  std::string pending;
  std::vector<std::string> history;

  bool awaiting_continuation() const;
  const char *prompt() const;
  void clear();
  repl_feed_result feed_line(std::string_view line);
};

} // namespace campcat::ccat_lang
