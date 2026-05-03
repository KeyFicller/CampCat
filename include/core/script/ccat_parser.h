#pragma once

#include "core/script/ccat_ast.h"
#include "core/script/ccat_lexer.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace campcat::ccat_lang {

/**
 * @brief Recoverable parse failure carrying caret hints identical to lexer numbering.
 */
class parse_error : public std::runtime_error {
public:
  /**
   * @brief Attach human-readable reason plus originating lex offsets.
   * @param[in] _msg Diagnostic forwarded through std::runtime_error.
   * @param[in] _line Lexical line (1-based) nearest offending lookahead token.
   * @param[in] _col Lexical column (1-based) nearest offending lookahead token.
   */
  parse_error(std::string _msg, int _line, int _col)
      : std::runtime_error(std::move(_msg)), line(_line), col(_col) {}

  /** @brief Mirrors lexer-derived diagnostics surfaced via peek/next failures. */
  int line;
  /** @brief Mirrors lexer-derived diagnostics surfaced via peek/next failures. */
  int col;
};

/**
 * @brief Parse an entire `.ccat` script buffer into executable AST.
 * @param[in] _source Full UTF-8 source identical lifetime semantics as Lexer ctor.
 * @return owning AST rooted at Program — caller frees naturally via unique_ptr.
 * @throws parse_error on malformed grammar.
 */
std::unique_ptr<Program> parse_program(std::string_view _source);

} // namespace campcat::ccat_lang
