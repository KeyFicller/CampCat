#pragma once

#include <string>
#include <string_view>

namespace campcat::ccat_lang {

/**
 * @brief Lexical token category produced while scanning .ccat sources.
 */
enum class TokKind {
  Eof,
  KwIf,
  KwElse,
  KwTap,
  KwWait,
  KwLog,
  KwSwipe,
  KwTapAt,
  KwSwipeAt,
  KwWaitUntil,
  KwRetry,
  KwDo,
  KwWhile,
  KwLoop,
  KwBreak,
  KwDefs,
  KwTrue,
  KwFalse,
  LParen,
  RParen,
  LBrace,
  RBrace,
  Semi,
  Comma,
  Eq,
  Str,
  Ident,
};

/**
 * @brief Single lexeme with source coordinate hints for diagnostics.
 */
struct Token {
  TokKind kind = TokKind::Eof;
  /** @brief Spelling slice for identifiers/strings/punctuation payloads where relevant. */
  std::string text;
  /** @brief 1-based source line when token begins. */
  int line = 1;
  /** @brief 1-based column when token begins. */
  int col = 1;
};

/**
 * @brief Minimal lexer over `.ccat` text (`//` line comments, string escapes).
 */
class Lexer {
public:
  /**
   * @brief Prime internal cursor at start of `_source`.
   * @param[in] _source Full script UTF-8 (caller-owned backing storage lifetime).
   */
  explicit Lexer(std::string_view _source);

  /**
   * @brief Peek current token without advancing.
   * @return Copy of buffered lookahead identical until next() consumes stream progress.
   */
  Token peek() const { return m_cur; }

  /**
   * @brief Advance to next token after consuming whitespace/comments.
   * @return Token advancing grammar machinery one slot forward.
   */
  Token next();

private:
  void advance_cursor();
  char peek_char(size_t _off = 0) const;
  void skip_ws_and_comments();
  Token lex_string();
  Token lex_ident_or_kw();

  std::string_view m_src;
  size_t m_pos = 0;
  int m_line = 1;
  int m_col = 1;
  Token m_cur{};
};

} // namespace campcat::ccat_lang
