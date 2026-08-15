#include "core/script/ccat_lexer.h"

#include <cctype>

namespace campcat::ccat_lang {

Lexer::Lexer(std::string_view _source) : m_src(_source) {
  m_cur = next();
}

void Lexer::advance_cursor() {
  if (m_pos >= m_src.size()) {
    return;
  }
  if (m_src[m_pos] == '\n') {
    ++m_line;
    m_col = 1;
  } else {
    ++m_col;
  }
  ++m_pos;
}

char Lexer::peek_char(size_t _off) const {
  const size_t j = m_pos + _off;
  if (j >= m_src.size()) {
    return '\0';
  }
  return m_src[j];
}

void Lexer::skip_ws_and_comments() {
  for (;;) {
    while (m_pos < m_src.size() &&
           std::isspace(static_cast<unsigned char>(m_src[m_pos]))) {
      advance_cursor();
    }
    if (peek_char() == '/' && peek_char(1) == '/') {
      while (m_pos < m_src.size() && peek_char() != '\n') {
        advance_cursor();
      }
      continue;
    }
    break;
  }
}

Token Lexer::lex_string() {
  const int line = m_line;
  const int col = m_col;
  advance_cursor(); // opening "
  std::string buf;
  while (m_pos < m_src.size()) {
    char c = peek_char();
    if (c == '"') {
      advance_cursor();
      return Token{TokKind::Str, std::move(buf), line, col};
    }
    if (c == '\\' && peek_char(1) != '\0') {
      advance_cursor();
      char esc = peek_char();
      advance_cursor();
      if (esc == 'n') {
        buf.push_back('\n');
      } else if (esc == 't') {
        buf.push_back('\t');
      } else if (esc == '\\' || esc == '"') {
        buf.push_back(esc);
      } else {
        buf.push_back(esc);
      }
      continue;
    }
    buf.push_back(c);
    advance_cursor();
  }
  return Token{TokKind::Ident, std::move(buf), line, col}; // malformed; parser will choke
}

Token Lexer::lex_ident_or_kw() {
  const int line = m_line;
  const int col = m_col;
  std::string buf;
  while (m_pos < m_src.size()) {
    char c = peek_char();
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
        c == '.' || c == '-') {
      buf.push_back(c);
      advance_cursor();
    } else {
      break;
    }
  }
  if (buf == "if") {
    return Token{TokKind::KwIf, std::move(buf), line, col};
  }
  if (buf == "else") {
    return Token{TokKind::KwElse, std::move(buf), line, col};
  }
  if (buf == "tap") {
    return Token{TokKind::KwTap, std::move(buf), line, col};
  }
  if (buf == "wait") {
    return Token{TokKind::KwWait, std::move(buf), line, col};
  }
  if (buf == "log") {
    return Token{TokKind::KwLog, std::move(buf), line, col};
  }
  if (buf == "swipe") {
    return Token{TokKind::KwSwipe, std::move(buf), line, col};
  }
  if (buf == "tap_at") {
    return Token{TokKind::KwTapAt, std::move(buf), line, col};
  }
  if (buf == "tap_offset") {
    return Token{TokKind::KwTapOffset, std::move(buf), line, col};
  }
  if (buf == "swipe_at") {
    return Token{TokKind::KwSwipeAt, std::move(buf), line, col};
  }
  if (buf == "wait_until") {
    return Token{TokKind::KwWaitUntil, std::move(buf), line, col};
  }
  if (buf == "retry") {
    return Token{TokKind::KwRetry, std::move(buf), line, col};
  }
  if (buf == "do") {
    return Token{TokKind::KwDo, std::move(buf), line, col};
  }
  if (buf == "while") {
    return Token{TokKind::KwWhile, std::move(buf), line, col};
  }
  if (buf == "loop") {
    return Token{TokKind::KwLoop, std::move(buf), line, col};
  }
  if (buf == "break") {
    return Token{TokKind::KwBreak, std::move(buf), line, col};
  }
  if (buf == "return") {
    return Token{TokKind::KwReturn, std::move(buf), line, col};
  }
  if (buf == "home") {
    return Token{TokKind::KwHome, std::move(buf), line, col};
  }
  if (buf == "run") {
    return Token{TokKind::KwRun, std::move(buf), line, col};
  }
  if (buf == "defs") {
    return Token{TokKind::KwDefs, std::move(buf), line, col};
  }
  if (buf == "true") {
    return Token{TokKind::KwTrue, std::move(buf), line, col};
  }
  if (buf == "false") {
    return Token{TokKind::KwFalse, std::move(buf), line, col};
  }
  return Token{TokKind::Ident, std::move(buf), line, col};
}

Token Lexer::next() {
  skip_ws_and_comments();
  const int line = m_line;
  const int col = m_col;
  if (m_pos >= m_src.size()) {
    m_cur = Token{TokKind::Eof, {}, line, col};
    return m_cur;
  }
  char c = peek_char();
  switch (c) {
  case '(':
    advance_cursor();
    m_cur = Token{TokKind::LParen, "(", line, col};
    return m_cur;
  case ')':
    advance_cursor();
    m_cur = Token{TokKind::RParen, ")", line, col};
    return m_cur;
  case '{':
    advance_cursor();
    m_cur = Token{TokKind::LBrace, "{", line, col};
    return m_cur;
  case '}':
    advance_cursor();
    m_cur = Token{TokKind::RBrace, "}", line, col};
    return m_cur;
  case ';':
    advance_cursor();
    m_cur = Token{TokKind::Semi, ";", line, col};
    return m_cur;
  case ',':
    advance_cursor();
    m_cur = Token{TokKind::Comma, ",", line, col};
    return m_cur;
  case '=':
    advance_cursor();
    m_cur = Token{TokKind::Eq, "=", line, col};
    return m_cur;
  case '$':
    advance_cursor();
    m_cur = Token{TokKind::Dollar, "$", line, col};
    return m_cur;
  case '"':
    m_cur = lex_string();
    return m_cur;
  default:
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' ||
        std::isdigit(static_cast<unsigned char>(c))) {
      m_cur = lex_ident_or_kw();
      return m_cur;
    }
    std::string junk(1, c);
    advance_cursor();
    m_cur = Token{TokKind::Ident, std::move(junk), line, col};
    return m_cur;
  }
}

} // namespace campcat::ccat_lang
