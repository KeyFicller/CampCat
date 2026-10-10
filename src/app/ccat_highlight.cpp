#include "app/ccat_highlight.h"

#include <cctype>
#include <string>

namespace campcat::app {

namespace {

using PaletteIndex = TextEditor::PaletteIndex;

/// A word character, as `.ccat` spells one: the lexer folds `.` and `-` into
/// identifiers so a bare `ok.png` stays a single word.
bool word_char(char _c) {
  const unsigned char c = static_cast<unsigned char>(_c);
  return c == '_' || c == '.' || c == '-' || std::isalnum(c) != 0;
}

bool word_start(char _c) {
  const unsigned char c = static_cast<unsigned char>(_c);
  return c == '_' || std::isalpha(c) != 0;
}

/**
 * One token starting exactly at `in_begin`, or false to let the editor advance
 * one character (which is how whitespace and stray characters are skipped).
 *
 * Keywords are *not* matched here: returning `Identifier` lets the editor look
 * the word up in the definition's `mKeywords` itself, so the list lives in one
 * place.
 */
bool tokenize_line(const char *in_begin, const char *in_end, const char *&out_begin,
                   const char *&out_end, PaletteIndex &palette) {
  if (in_begin >= in_end) {
    return false;
  }
  const char c = *in_begin;

  // `//` to the end of the line: `.ccat` has no block comments.
  if (c == '/' && in_begin + 1 < in_end && in_begin[1] == '/') {
    out_begin = in_begin;
    out_end = in_end;
    palette = PaletteIndex::Comment;
    return true;
  }
  // A string, tolerating one that was never closed -- the caret can sit inside
  // an unfinished literal while it is being typed.
  if (c == '"') {
    const char *p = in_begin + 1;
    while (p < in_end) {
      if (*p == '\\' && p + 1 < in_end) {
        p += 2;
        continue;
      }
      if (*p == '"') {
        ++p;
        break;
      }
      ++p;
    }
    out_begin = in_begin;
    out_end = p;
    palette = PaletteIndex::String;
    return true;
  }
  // `$Debug`: the sigil and its word are one token, so they cannot be split.
  if (c == '$') {
    const char *p = in_begin + 1;
    while (p < in_end && word_char(*p)) {
      ++p;
    }
    out_begin = in_begin;
    out_end = p;
    palette = PaletteIndex::Preprocessor;
    return true;
  }
  if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
    const char *p = in_begin;
    while (p < in_end && (std::isdigit(static_cast<unsigned char>(*p)) != 0 || *p == '.')) {
      ++p;
    }
    out_begin = in_begin;
    out_end = p;
    palette = PaletteIndex::Number;
    return true;
  }
  if (word_start(c)) {
    const char *p = in_begin;
    while (p < in_end && word_char(*p)) {
      ++p;
    }
    out_begin = in_begin;
    out_end = p;
    palette = PaletteIndex::Identifier;
    return true;
  }
  switch (c) {
  case '(':
  case ')':
  case '{':
  case '}':
  case ',':
  case ';':
    out_begin = in_begin;
    out_end = in_begin + 1;
    palette = PaletteIndex::Punctuation;
    return true;
  default:
    return false;
  }
}

} // namespace

const TextEditor::LanguageDefinition &ccat_language() {
  static const TextEditor::LanguageDefinition lang = [] {
    TextEditor::LanguageDefinition def;
    def.mName = "CCAT";
    // Spelled as the lexer spells them (`ccat_lexer.cpp`, lex_ident_or_kw).
    // ponytail: this list and the lexer's are two copies by choice; a test
    // (`ccat_highlight_test`) fails if they drift apart.
    static const char *const keywords[] = {
        "if",   "else",       "tap",   "wait",   "log",    "swipe", "tap_at",
        "tap_offset", "swipe_at", "wait_until", "retry", "do",    "while",
        "loop", "break",      "return", "home",  "run",    "defs",  "true",
        "false"};
    for (const char *word : keywords) {
      def.mKeywords.insert(word);
    }
    // `.ccat` has line comments only, but the editor's comment scanner cannot be
    // handed an empty start/end pair: its equality checks compare a zero-length
    // range, which is equal to itself, so an empty pair matches on *every*
    // character. The scanner then flags every glyph as a multi-line comment and
    // `GetGlyphColor` returns the comment colour for all of them, erasing the
    // keyword, number and punctuation colours the tokenizer below asks for.
    // `//` and a newline are truthful here (a comment runs to the end of the
    // line), and neither can ever be matched as a block pair, so no block
    // comment is ever opened.
    def.mCommentStart = "//";
    def.mCommentEnd = "\n";
    def.mSingleLineComment = "//";
    def.mCaseSensitive = true;
    def.mAutoIndentation = true;
    def.mTokenize = tokenize_line;
    return def;
  }();
  return lang;
}

const TextEditor::Palette &ccat_palette() {
  static const TextEditor::Palette palette = [] {
    TextEditor::Palette p{};
    p[static_cast<int>(PaletteIndex::Default)] = IM_COL32(0xD4, 0xD8, 0xDC, 0xFF);
    p[static_cast<int>(PaletteIndex::Keyword)] = IM_COL32(0x7C, 0xB2, 0xE8, 0xFF);
    p[static_cast<int>(PaletteIndex::Number)] = IM_COL32(0xE0, 0xA0, 0x6A, 0xFF);
    p[static_cast<int>(PaletteIndex::String)] = IM_COL32(0x9E, 0xC9, 0x7A, 0xFF);
    p[static_cast<int>(PaletteIndex::CharLiteral)] = IM_COL32(0x9E, 0xC9, 0x7A, 0xFF);
    p[static_cast<int>(PaletteIndex::Punctuation)] = IM_COL32(0x9A, 0xA4, 0xAE, 0xFF);
    p[static_cast<int>(PaletteIndex::Preprocessor)] = IM_COL32(0xC5, 0x92, 0xD8, 0xFF);
    p[static_cast<int>(PaletteIndex::Identifier)] = IM_COL32(0xD4, 0xD8, 0xDC, 0xFF);
    p[static_cast<int>(PaletteIndex::KnownIdentifier)] = IM_COL32(0xE8, 0xD8, 0x9A, 0xFF);
    p[static_cast<int>(PaletteIndex::PreprocIdentifier)] = IM_COL32(0xC5, 0x92, 0xD8, 0xFF);
    p[static_cast<int>(PaletteIndex::Comment)] = IM_COL32(0x76, 0x80, 0x88, 0xFF);
    p[static_cast<int>(PaletteIndex::MultiLineComment)] = IM_COL32(0x76, 0x80, 0x88, 0xFF);
    // The child background of the surrounding panel, so the editor does not
    // read as a hole punched in the window.
    p[static_cast<int>(PaletteIndex::Background)] = IM_COL32(0x1C, 0x1F, 0x21, 0xFF);
    p[static_cast<int>(PaletteIndex::Cursor)] = IM_COL32(0xD4, 0xD8, 0xDC, 0xFF);
    p[static_cast<int>(PaletteIndex::Selection)] = IM_COL32(0x33, 0x4C, 0x5E, 0xFF);
    p[static_cast<int>(PaletteIndex::ErrorMarker)] = IM_COL32(0xE0, 0x5A, 0x5A, 0xFF);
    p[static_cast<int>(PaletteIndex::Breakpoint)] = IM_COL32(0xE0, 0x5A, 0x5A, 0xFF);
    p[static_cast<int>(PaletteIndex::LineNumber)] = IM_COL32(0x66, 0x70, 0x78, 0xFF);
    p[static_cast<int>(PaletteIndex::CurrentLineFill)] = IM_COL32(0x24, 0x28, 0x2C, 0xFF);
    p[static_cast<int>(PaletteIndex::CurrentLineFillInactive)] =
        IM_COL32(0x1F, 0x23, 0x26, 0xFF);
    p[static_cast<int>(PaletteIndex::CurrentLineEdge)] = IM_COL32(0x33, 0x4C, 0x5E, 0xFF);
    return p;
  }();
  return palette;
}

} // namespace campcat::app
