#pragma once

/// @file chat_text.h
/// @brief Text shaping for the LLM chat: replies split into paragraphs and
///        fenced code, tool notes split into name, status and body.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace campcat::app {

/// @brief One piece of a reply: a paragraph for the Markdown renderer, or the
///        inside of a fenced block drawn verbatim.
struct chat_block {
    bool code = false;
    std::string text;

    bool operator==(const chat_block &) const = default;
};

/// @brief A tool note, `**name** status` and an optional body after a newline.
struct tool_note_parts {
    std::string_view name;
    std::string_view status;
    std::string_view body;
};

namespace detail {

inline bool chat_blank(std::string_view _s) {
    return _s.find_first_not_of(" \t\r") == std::string_view::npos;
}

inline bool chat_fence(std::string_view _line) {
    const size_t at = _line.find_first_not_of(' ');
    return at != std::string_view::npos && at < 4 && _line.substr(at).starts_with("```");
}

/// imgui_markdown only opens `**` after a space or at a line start, and CJK
/// prose puts bold straight after a character (`：**x**、**y**`), so an
/// opener gets the space it needs. `_bold` carries the pairing across a line.
inline void chat_append_prose(std::string &_out, std::string_view _s, bool &_bold) {
    for (size_t i = 0; i < _s.size(); ++i) {
        if (_s.substr(i, 2) != "**") {
            _out += _s[i];
            continue;
        }
        if (!_bold && !_out.empty() && _out.back() != ' ' && _out.back() != '\t' &&
            _out.back() != '\n') {
            _out += ' ';
        }
        _out += "**";
        _bold = !_bold;
        ++i;
    }
}

/// imgui_markdown has no inline code. An empty-url link is the one inline span
/// it hands to the format callback, so a code span becomes `[code]()`.
inline void chat_append_line(std::string &_out, std::string_view _line) {
    constexpr auto npos = std::string_view::npos;
    const size_t lead = _line.find_first_not_of(' ');
    // Its parser styles neither links nor emphasis on a heading line, which
    // is bold already, so the markers could only show.
    if (lead != npos && _line[lead] == '#') {
        for (size_t i = 0; i < _line.size(); ++i) {
            if (_line.substr(i, 2) == "**") {
                ++i;
            } else if (_line[i] != '`') {
                _out += _line[i];
            }
        }
        return;
    }
    bool bold = false;
    size_t i = 0;
    while (i < _line.size()) {
        const size_t open = _line.find('`', i);
        const size_t close = open == npos ? npos : _line.find('`', open + 1);
        if (close == npos) {
            break;
        }
        const std::string_view code = _line.substr(open + 1, close - open - 1);
        // ponytail: a `]` ends imgui_markdown's link text early and the parser
        // then hunts for any later `(`, so such spans stay raw. Lift this with
        // a real inline-code renderer.
        if (code.empty() || code.find(']') != npos) {
            chat_append_prose(_out, _line.substr(i, close + 1 - i), bold);
            i = close + 1;
            continue;
        }
        size_t text_end = open;
        size_t next = close + 1;
        // A link resets emphasis, which would leave the closing `**` as an
        // opener with nothing after it.
        if (open >= i + 2 && _line.substr(open - 2, 2) == "**" &&
            _line.substr(close + 1, 2) == "**") {
            text_end = open - 2;
            next = close + 3;
        }
        chat_append_prose(_out, _line.substr(i, text_end - i), bold);
        _out += '[';
        _out += code;
        _out += "]()";
        i = next;
    }
    chat_append_prose(_out, _line.substr(i), bold);
}

inline std::string_view chat_trim(std::string_view _s, const char *_chars) {
    const size_t a = _s.find_first_not_of(_chars);
    if (a == std::string_view::npos) {
        return {};
    }
    return _s.substr(a, _s.find_last_not_of(_chars) - a + 1);
}

} // namespace detail

/// @brief Splits a reply at blank lines and code fences. Blank lines only
///        separate, so leading, trailing and repeated ones leave no gap; a
///        fence still open (a reply mid-stream) closes at the end.
inline std::vector<chat_block> chat_blocks(std::string_view _text) {
    std::vector<chat_block> out;
    chat_block cur;
    auto flush = [&] {
        if (cur.code) {
            while (!cur.text.empty() && cur.text.back() == '\n') {
                cur.text.pop_back();
            }
        }
        if (!cur.text.empty()) {
            out.push_back(std::move(cur));
        }
        cur = chat_block{};
    };
    bool fenced = false;
    size_t pos = 0;
    while (pos <= _text.size()) {
        size_t end = _text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = _text.size();
        }
        std::string_view line = _text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (detail::chat_fence(line)) {
            flush();
            fenced = !fenced;
            cur.code = fenced;
        } else if (fenced) {
            cur.text += line;
            cur.text += '\n';
        } else if (detail::chat_blank(line)) {
            flush();
        } else {
            if (!cur.text.empty()) {
                cur.text += '\n';
            }
            detail::chat_append_line(cur.text, line);
        }
    }
    flush();
    return out;
}

/// @brief Parts of a note in the `**name** status` shape the host and the
///        subagent stream both write; nullopt when the text is not one.
inline std::optional<tool_note_parts> split_tool_note(std::string_view _text) {
    if (!_text.starts_with("**")) {
        return std::nullopt;
    }
    const size_t close = _text.find("**", 2);
    if (close == std::string_view::npos || close == 2 || close > _text.find('\n')) {
        return std::nullopt;
    }
    tool_note_parts parts;
    parts.name = _text.substr(2, close - 2);
    const std::string_view rest = _text.substr(close + 2);
    const size_t nl = rest.find('\n');
    parts.status = detail::chat_trim(rest.substr(0, nl), " \t\r");
    if (nl != std::string_view::npos) {
        parts.body = detail::chat_trim(rest.substr(nl + 1), " \t\r\n");
    }
    return parts;
}

/// @brief A subagent's call, `**name** {args}`, joined with what came back
///        into the host's note shape: `**name** ok|error`, then the result,
///        then the args. nullopt when `_call` is not a call still waiting.
inline std::optional<std::string> tool_call_settled(std::string_view _call,
                                                    std::string_view _result, bool _ok) {
    const std::optional<tool_note_parts> note = split_tool_note(_call);
    if (!note || note->status == "ok" || note->status == "error") {
        return std::nullopt;
    }
    std::string out = "**";
    out += note->name;
    out += _ok ? "** ok" : "** error";
    for (const std::string_view part : {_result, note->status}) {
        if (!part.empty()) {
            out += "\n\n";
            out += part;
        }
    }
    return out;
}

/// @brief The first non-blank line, trimmed.
inline std::string_view first_line(std::string_view _text) {
    const std::string_view s = detail::chat_trim(_text, " \t\r\n");
    return detail::chat_trim(s.substr(0, s.find('\n')), " \t\r");
}

} // namespace campcat::app
