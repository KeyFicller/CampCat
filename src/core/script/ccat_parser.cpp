#include "core/script/ccat_parser.h"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace campcat::ccat_lang {

namespace {

class Parser {
public:
  explicit Parser(std::string_view _src) : m_lex(_src) {}

  std::unique_ptr<Program> parse_program() {
    auto prog = std::make_unique<Program>();
    while (m_lex.peek().kind != TokKind::Eof) {
      prog->stmts.push_back(parse_stmt());
      if (m_lex.peek().kind == TokKind::Semi) {
        (void)m_lex.next();
      }
    }
    return prog;
  }

private:
  Lexer m_lex;

  [[noreturn]] void fail(const std::string &_what) {
    const Token t = m_lex.peek();
    throw parse_error(_what, t.line, t.col);
  }

  void expect(TokKind _k, const char *_ctx) {
    if (m_lex.peek().kind != _k) {
      std::ostringstream oss;
      oss << _ctx << " (unexpected token near line " << m_lex.peek().line
          << ")";
      fail(oss.str());
    }
    (void)m_lex.next();
  }

  std::string parse_image_ref() {
    Token t = m_lex.peek();
    if (t.kind == TokKind::Str) {
      (void)m_lex.next();
      return t.text;
    }
    if (t.kind == TokKind::Ident) {
      (void)m_lex.next();
      return t.text;
    }
    fail("expected image path (string or identifier)");
  }

  bool decimal_uint_literal(const std::string &_text, int *_out) {
    if (_text.empty()) {
      return false;
    }
    for (char ch : _text) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        return false;
      }
    }
    char *end = nullptr;
    long v = std::strtol(_text.c_str(), &end, 10);
    if (end != _text.c_str() + _text.size() || v < 0 ||
        v > static_cast<long>(std::numeric_limits<int>::max())) {
      return false;
    }
    *_out = static_cast<int>(v);
    return true;
  }

  int parse_non_negative_int(const char *_ctx) {
    Token t = m_lex.peek();
    if (t.kind != TokKind::Ident) {
      std::ostringstream oss;
      oss << _ctx << " (expected milliseconds literal near line " << t.line
          << ")";
      fail(oss.str());
    }
    (void)m_lex.next();
    int value = 0;
    if (!decimal_uint_literal(t.text, &value)) {
      fail("wait(ms): expected non-negative decimal integer");
    }
    return value;
  }

  std::string parse_log_operand() { return parse_image_ref(); }

  std::unique_ptr<Stmt> parse_stmt() {
    Token t = m_lex.peek();
    switch (t.kind) {
    case TokKind::KwIf:
      return parse_if();
    case TokKind::KwTap:
      return parse_tap();
    case TokKind::KwWait:
      return parse_wait();
    case TokKind::KwLog:
      return parse_log();
    case TokKind::LBrace:
      return parse_block();
    default:
      fail("expected if, tap, wait, log, or block");
    }
  }

  std::unique_ptr<BlockStmt> parse_block() {
    expect(TokKind::LBrace, "expected '{'");
    auto blk = std::make_unique<BlockStmt>();
    while (m_lex.peek().kind != TokKind::RBrace &&
           m_lex.peek().kind != TokKind::Eof) {
      blk->body.push_back(parse_stmt());
      if (m_lex.peek().kind == TokKind::Semi) {
        (void)m_lex.next();
      }
    }
    expect(TokKind::RBrace, "expected '}'");
    return blk;
  }

  std::unique_ptr<IfStmt> parse_if() {
    expect(TokKind::KwIf, "expected if");
    expect(TokKind::LParen, "expected '(' after if");
    std::string img = parse_image_ref();
    expect(TokKind::RParen, "expected ')' after condition image");
    auto node = std::make_unique<IfStmt>();
    node->image_path = std::move(img);
    node->then_branch = parse_stmt();
    if (m_lex.peek().kind == TokKind::KwElse) {
      (void)m_lex.next();
      node->else_branch = parse_stmt();
    }
    return node;
  }

  std::unique_ptr<TapStmt> parse_tap() {
    expect(TokKind::KwTap, "expected tap");
    expect(TokKind::LParen, "expected '(' after tap");
    std::string img = parse_image_ref();
    expect(TokKind::RParen, "expected ')' after tap image");
    auto node = std::make_unique<TapStmt>();
    node->image_path = std::move(img);
    return node;
  }

  std::unique_ptr<WaitStmt> parse_wait() {
    expect(TokKind::KwWait, "expected wait");
    expect(TokKind::LParen, "expected '(' after wait");
    const int ms = parse_non_negative_int("wait(ms)");
    expect(TokKind::RParen, "expected ')' after wait duration");
    auto node = std::make_unique<WaitStmt>();
    node->milliseconds = ms;
    return node;
  }

  std::unique_ptr<LogStmt> parse_log() {
    expect(TokKind::KwLog, "expected log");
    expect(TokKind::LParen, "expected '(' after log");
    std::string msg = parse_log_operand();
    expect(TokKind::RParen, "expected ')' after log message");
    auto node = std::make_unique<LogStmt>();
    node->message = std::move(msg);
    return node;
  }
};

} // namespace

std::unique_ptr<Program> parse_program(std::string_view _source) {
  Parser p(_source);
  return p.parse_program();
}

} // namespace campcat::ccat_lang
