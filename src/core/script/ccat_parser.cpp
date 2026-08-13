#include "core/script/ccat_parser.h"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace campcat::ccat_lang {

namespace {

enum class DefKind { number, string, boolean };

struct DefValue {
  DefKind kind = DefKind::number;
  double number = 0;
  std::string str;
  bool boolean = false;
};

bool is_simple_def_name(const std::string &_name) {
  if (_name.empty()) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(_name[0]);
  if (!(std::isalpha(first) || first == '_')) {
    return false;
  }
  for (char ch : _name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (!(std::isalnum(c) || c == '_')) {
      return false;
    }
  }
  return true;
}

class Parser {
public:
  explicit Parser(std::string_view _src) : m_lex(_src) {}

  std::unique_ptr<Program> parse_program() {
    if (m_lex.peek().kind == TokKind::KwDefs) {
      parse_defs_block();
      if (m_lex.peek().kind == TokKind::Semi) {
        (void)m_lex.next();
      }
    }

    auto prog = std::make_unique<Program>();
    while (m_lex.peek().kind != TokKind::Eof) {
      if (m_lex.peek().kind == TokKind::KwDefs) {
        fail("defs must be the first statement (only one defs block allowed)");
      }
      prog->stmts.push_back(parse_stmt());
      if (m_lex.peek().kind == TokKind::Semi) {
        (void)m_lex.next();
      }
    }
    return prog;
  }

private:
  Lexer m_lex;
  std::unordered_map<std::string, DefValue> m_defs;

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

  const DefValue *find_def(const std::string &_name) const {
    if (!is_simple_def_name(_name)) {
      return nullptr;
    }
    const auto it = m_defs.find(_name);
    if (it == m_defs.end()) {
      return nullptr;
    }
    return &it->second;
  }

  DefValue parse_def_literal() {
    Token t = m_lex.peek();
    if (t.kind == TokKind::KwTrue) {
      (void)m_lex.next();
      DefValue v;
      v.kind = DefKind::boolean;
      v.boolean = true;
      return v;
    }
    if (t.kind == TokKind::KwFalse) {
      (void)m_lex.next();
      DefValue v;
      v.kind = DefKind::boolean;
      v.boolean = false;
      return v;
    }
    if (t.kind == TokKind::Str) {
      (void)m_lex.next();
      DefValue v;
      v.kind = DefKind::string;
      v.str = std::move(t.text);
      return v;
    }
    if (t.kind == TokKind::Ident) {
      (void)m_lex.next();
      try {
        size_t consumed = 0;
        const double num = std::stod(t.text, &consumed);
        if (consumed != t.text.size()) {
          fail("defs value: expected number, string, true, or false");
        }
        DefValue v;
        v.kind = DefKind::number;
        v.number = num;
        return v;
      } catch (const std::exception &) {
        fail("defs value: expected number, string, true, or false");
      }
    }
    fail("defs value: expected number, string, true, or false");
  }

  void parse_defs_block() {
    expect(TokKind::KwDefs, "expected defs");
    expect(TokKind::LBrace, "expected '{' after defs");
    while (m_lex.peek().kind != TokKind::RBrace &&
           m_lex.peek().kind != TokKind::Eof) {
      Token name_tok = m_lex.peek();
      if (name_tok.kind != TokKind::Ident ||
          !is_simple_def_name(name_tok.text)) {
        fail("defs: expected simple name [A-Za-z_][A-Za-z0-9_]*");
      }
      (void)m_lex.next();
      if (m_defs.contains(name_tok.text)) {
        fail("duplicate def `" + name_tok.text + "`");
      }
      expect(TokKind::Eq, "expected '=' after def name");
      DefValue value = parse_def_literal();
      m_defs.emplace(std::move(name_tok.text), std::move(value));
      if (m_lex.peek().kind == TokKind::Semi) {
        (void)m_lex.next();
      }
    }
    expect(TokKind::RBrace, "expected '}' after defs");
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

  int number_def_as_nonneg_int(const DefValue &_v, const char *_ctx) {
    if (_v.kind != DefKind::number) {
      fail(std::string(_ctx) + ": def has wrong type (expected number)");
    }
    const double n = _v.number;
    if (n < 0 || n > static_cast<double>(std::numeric_limits<int>::max())) {
      fail(std::string(_ctx) + ": number out of int range");
    }
    const int as_int = static_cast<int>(n);
    if (static_cast<double>(as_int) != n) {
      fail(std::string(_ctx) + ": expected non-negative integer");
    }
    return as_int;
  }

  std::string parse_image_ref() {
    Token t = m_lex.peek();
    if (t.kind == TokKind::Str) {
      (void)m_lex.next();
      return t.text;
    }
    if (t.kind == TokKind::Ident) {
      if (const DefValue *def = find_def(t.text)) {
        if (def->kind == DefKind::string) {
          (void)m_lex.next();
          return def->str;
        }
        fail("expected image path, but def `" + t.text + "` is not a string");
      }
      (void)m_lex.next();
      return t.text;
    }
    fail("expected image path (string or identifier)");
  }

  int parse_non_negative_int(const char *_ctx) {
    Token t = m_lex.peek();
    if (t.kind == TokKind::Ident) {
      if (const DefValue *def = find_def(t.text)) {
        (void)m_lex.next();
        return number_def_as_nonneg_int(*def, _ctx);
      }
      (void)m_lex.next();
      int value = 0;
      if (!decimal_uint_literal(t.text, &value)) {
        fail(std::string(_ctx) + ": expected non-negative decimal integer");
      }
      return value;
    }
    std::ostringstream oss;
    oss << _ctx << " (expected integer literal near line " << t.line << ")";
    fail(oss.str());
  }

  double parse_double_coord(const char *_ctx) {
    Token t = m_lex.peek();
    if (t.kind != TokKind::Ident) {
      std::ostringstream oss;
      oss << _ctx << " (expected numeric literal near line " << t.line << ")";
      fail(oss.str());
    }
    if (const DefValue *def = find_def(t.text)) {
      if (def->kind != DefKind::number) {
        fail(std::string(_ctx) + ": def `" + t.text + "` is not a number");
      }
      (void)m_lex.next();
      return def->number;
    }
    (void)m_lex.next();
    try {
      size_t consumed = 0;
      const double v = std::stod(t.text, &consumed);
      if (consumed != t.text.size()) {
        fail(std::string(_ctx) + ": malformed number");
      }
      return v;
    } catch (const std::exception &) {
      fail(std::string(_ctx) + ": invalid number");
    }
  }

  std::string parse_log_operand() {
    Token t = m_lex.peek();
    if (t.kind == TokKind::Ident) {
      if (const DefValue *def = find_def(t.text)) {
        if (def->kind != DefKind::string) {
          fail("log(...): def `" + t.text + "` is not a string");
        }
        (void)m_lex.next();
        return def->str;
      }
    }
    return parse_image_ref();
  }

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
    case TokKind::KwSwipe:
      return parse_swipe_templates();
    case TokKind::KwTapAt:
      return parse_tap_at();
    case TokKind::KwSwipeAt:
      return parse_swipe_at();
    case TokKind::KwWaitUntil:
      return parse_wait_until();
    case TokKind::KwRetry:
      return parse_retry();
    case TokKind::KwDo:
      return parse_do_while();
    case TokKind::KwLoop:
      return parse_loop();
    case TokKind::KwBreak:
      return parse_break();
    case TokKind::KwReturn:
      return parse_return();
    case TokKind::LBrace:
      return parse_block();
    default:
      fail("expected statement (if, tap, wait, log, swipe, tap_at, swipe_at, "
           "wait_until, retry, do, loop, break, return, or block)");
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

  std::unique_ptr<Stmt> parse_if() {
    expect(TokKind::KwIf, "expected if");
    expect(TokKind::LParen, "expected '(' after if");

    Token cond = m_lex.peek();
    if (cond.kind == TokKind::Ident) {
      if (const DefValue *def = find_def(cond.text)) {
        if (def->kind == DefKind::boolean) {
          const bool take_then = def->boolean;
          (void)m_lex.next();
          expect(TokKind::RParen, "expected ')' after if condition");
          auto then_branch = parse_stmt();
          std::unique_ptr<Stmt> else_branch;
          if (m_lex.peek().kind == TokKind::KwElse) {
            (void)m_lex.next();
            else_branch = parse_stmt();
          }
          if (take_then) {
            return then_branch;
          }
          if (else_branch) {
            return else_branch;
          }
          return std::make_unique<BlockStmt>();
        }
        if (def->kind == DefKind::number) {
          fail("if condition cannot be a number def");
        }
        // string def → image path via parse_image_ref
      }
    }

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

  std::unique_ptr<SwipeTemplatesStmt> parse_swipe_templates() {
    expect(TokKind::KwSwipe, "expected swipe");
    expect(TokKind::LParen, "expected '(' after swipe");
    std::string from = parse_image_ref();
    expect(TokKind::Comma, "expected ',' between swipe templates");
    std::string to = parse_image_ref();
    expect(TokKind::RParen, "expected ')' after swipe");
    auto node = std::make_unique<SwipeTemplatesStmt>();
    node->from_image_path = std::move(from);
    node->to_image_path = std::move(to);
    return node;
  }

  std::unique_ptr<TapAtStmt> parse_tap_at() {
    expect(TokKind::KwTapAt, "expected tap_at");
    expect(TokKind::LParen, "expected '(' after tap_at");
    const double x = parse_double_coord("tap_at x");
    expect(TokKind::Comma, "expected ',' after tap_at x");
    const double y = parse_double_coord("tap_at y");
    expect(TokKind::RParen, "expected ')' after tap_at");
    auto node = std::make_unique<TapAtStmt>();
    node->nx = x;
    node->ny = y;
    return node;
  }

  std::unique_ptr<SwipeAtStmt> parse_swipe_at() {
    expect(TokKind::KwSwipeAt, "expected swipe_at");
    expect(TokKind::LParen, "expected '(' after swipe_at");
    const double x1 = parse_double_coord("swipe_at x1");
    expect(TokKind::Comma, "expected ','");
    const double y1 = parse_double_coord("swipe_at y1");
    expect(TokKind::Comma, "expected ','");
    const double x2 = parse_double_coord("swipe_at x2");
    expect(TokKind::Comma, "expected ','");
    const double y2 = parse_double_coord("swipe_at y2");
    expect(TokKind::RParen, "expected ')' after swipe_at");
    auto node = std::make_unique<SwipeAtStmt>();
    node->x1 = x1;
    node->y1 = y1;
    node->x2 = x2;
    node->y2 = y2;
    return node;
  }

  std::unique_ptr<WaitUntilStmt> parse_wait_until() {
    expect(TokKind::KwWaitUntil, "expected wait_until");
    expect(TokKind::LParen, "expected '(' after wait_until");
    std::string img = parse_image_ref();
    expect(TokKind::Comma, "expected ',' before wait_until timeout");
    const int ms = parse_non_negative_int("wait_until timeout");
    if (ms <= 0) {
      fail("wait_until: timeout must be positive");
    }
    expect(TokKind::RParen, "expected ')' after wait_until");
    auto node = std::make_unique<WaitUntilStmt>();
    node->image_path = std::move(img);
    node->timeout_ms = ms;
    return node;
  }

  std::unique_ptr<RetryStmt> parse_retry() {
    expect(TokKind::KwRetry, "expected retry");
    expect(TokKind::LParen, "expected '(' after retry");
    const int n = parse_non_negative_int("retry count");
    if (n <= 0) {
      fail("retry(n): n must be positive");
    }
    expect(TokKind::RParen, "expected ')' after retry count");
    auto node = std::make_unique<RetryStmt>();
    node->attempts = n;
    node->body = parse_stmt();
    return node;
  }

  std::unique_ptr<DoWhileStmt> parse_do_while() {
    expect(TokKind::KwDo, "expected do");
    auto node = std::make_unique<DoWhileStmt>();
    node->body = parse_block();
    expect(TokKind::KwWhile, "expected while after do block");
    expect(TokKind::LParen, "expected '(' after while");
    node->condition_image_path = parse_image_ref();
    expect(TokKind::RParen, "expected ')' after while condition");
    return node;
  }

  std::unique_ptr<LoopStmt> parse_loop() {
    expect(TokKind::KwLoop, "expected loop");
    expect(TokKind::LParen, "expected '(' after loop");
    const int n = parse_non_negative_int("loop count");
    expect(TokKind::RParen, "expected ')' after loop count");
    auto node = std::make_unique<LoopStmt>();
    node->repetitions = n;
    node->body = parse_block();
    return node;
  }

  std::unique_ptr<BreakStmt> parse_break() {
    expect(TokKind::KwBreak, "expected break");
    return std::make_unique<BreakStmt>();
  }

  std::unique_ptr<ReturnStmt> parse_return() {
    expect(TokKind::KwReturn, "expected return");
    return std::make_unique<ReturnStmt>();
  }
};

} // namespace

std::unique_ptr<Program> parse_program(std::string_view _source) {
  Parser p(_source);
  return p.parse_program();
}

} // namespace campcat::ccat_lang
