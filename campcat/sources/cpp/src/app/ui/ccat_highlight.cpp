#include "app/ui/ccat_highlight.h"
#include "app/ui/theme.h"

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
bool tokenize_line(const char *in_begin, const char *in_end,
                   const char *&out_begin, const char *&out_end,
                   PaletteIndex &palette) {
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
    // A string, tolerating one that was never closed -- the caret can sit
    // inside an unfinished literal while it is being typed.
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
        while (
            p < in_end &&
            (std::isdigit(static_cast<unsigned char>(*p)) != 0 || *p == '.')) {
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
            "if",    "else",   "elif",       "tap",      "wait",       "log",
            "swipe", "tap_at", "tap_offset", "swipe_at", "wait_until", "do",
            "while", "loop",   "break",      "return",   "home",       "run",
            "has",   "ask",    "goto",     "at",         "defs",     "true",
            "false"};
        for (const char *word : keywords) {
            def.mKeywords.insert(word);
        }
        // `.ccat` has line comments only, but the editor's comment scanner
        // cannot be handed an empty start/end pair: its equality checks compare
        // a zero-length range, which is equal to itself, so an empty pair
        // matches on *every* character. The scanner then flags every glyph as a
        // multi-line comment and `GetGlyphColor` returns the comment colour for
        // all of them, erasing the keyword, number and punctuation colours the
        // tokenizer below asks for.
        // `//` and a newline are truthful here (a comment runs to the end of
        // the line), and neither can ever be matched as a block pair, so no
        // block comment is ever opened.
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
        const auto set = [&p](PaletteIndex _i, const ImVec4 &_c) {
            p[static_cast<int>(_i)] = ImGui::ColorConvertFloat4ToU32(_c);
        };
        set(PaletteIndex::Default, theme::k_text);
        set(PaletteIndex::Keyword, theme::k_syntax_keyword);
        set(PaletteIndex::Number, theme::k_syntax_number);
        set(PaletteIndex::String, theme::k_syntax_string);
        set(PaletteIndex::CharLiteral, theme::k_syntax_string);
        set(PaletteIndex::Punctuation, theme::k_text_2);
        set(PaletteIndex::Preprocessor, theme::k_syntax_decl);
        set(PaletteIndex::Identifier, theme::k_text);
        set(PaletteIndex::KnownIdentifier, theme::k_syntax_builtin);
        set(PaletteIndex::PreprocIdentifier, theme::k_syntax_decl);
        set(PaletteIndex::Comment, theme::k_text_3);
        set(PaletteIndex::MultiLineComment, theme::k_text_3);
        set(PaletteIndex::Background, theme::k_surface_inset);
        set(PaletteIndex::Cursor, theme::k_text);
        set(PaletteIndex::Selection, theme::rgb(0x7EA6FF, 0.30F));
        set(PaletteIndex::ErrorMarker, theme::k_danger);
        set(PaletteIndex::Breakpoint, theme::k_danger);
        set(PaletteIndex::LineNumber, theme::k_text_3);
        set(PaletteIndex::CurrentLineFill, theme::k_surface_2);
        set(PaletteIndex::CurrentLineFillInactive, theme::k_surface_1);
        set(PaletteIndex::CurrentLineEdge, theme::k_border);
        return p;
    }();
    return palette;
}

} // namespace campcat::app
