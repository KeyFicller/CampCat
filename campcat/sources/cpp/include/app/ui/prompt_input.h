#pragma once

#include <string>

/**
 * @brief Single-line input box that paints the macOS IME composition at the
 * caret.
 *
 * GLFW stores the marked text and never draws it, so without this a pinyin
 * composition stays invisible until a candidate is committed.
 *
 * It draws nothing else: the caller owns `SetNextItemWidth`, any label before
 * it and any `SameLine`/button after it.
 *
 * @param[in] _id ImGui id; a `##`-prefixed id carries no visible label.
 * @param[in] _hint Placeholder text, or null for none.
 * @param[in,out] _buf Text buffer.
 * @return True when Enter committed the buffer this frame.
 */
bool prompt_input_text(const char *_id, const char *_hint, std::string *_buf);
