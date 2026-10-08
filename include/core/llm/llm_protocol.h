#pragma once

#include <string>
#include <string_view>

namespace campcat::llm_protocol {

/**
 * @brief One decoded line from the sidecar's stdout.
 *
 * `type` is `unknown` for malformed input or an unrecognized `type` field, so
 * callers can ignore stray lines instead of treating them as fatal.
 */
struct message {
  enum class kind { ready, log, result, chunk, unknown };

  kind type = kind::unknown;
  long id = 0;

  // result
  bool ok = false;
  std::string text;
  std::string error;

  // chunk (shares `text` as the delta)
  bool thinking = false; ///< reasoning heartbeat, `text` is empty

  // log
  std::string level;
  std::string log_message;

  // ready
  int protocol = 0;

  /// Turns the sidecar retains after this result; 0 after a reset.
  int turns = 0;
};

/**
 * @brief Standard base64 (RFC 4648, no line breaks).
 * @param[in] _data Raw bytes; may be null when `_len` is 0.
 * @param[in] _len Number of bytes to encode.
 */
std::string base64_encode(const unsigned char *_data, std::size_t _len);

/**
 * @brief Build one `describe` request line (no trailing newline).
 *
 * The request carries an id, an optional base64 PNG and optional text. Model
 * selection and prompts belong to the sidecar. Field names are the contract
 * with `llm/main.py`; keep both sides in sync.
 *
 * @param[in] _image_b64 Empty for a text-only turn.
 * @param[in] _text Empty to let the sidecar use its default prompt.
 */
std::string build_describe_request(std::string_view _image_b64, long _id,
                                   std::string_view _text);

/**
 * @brief Build one `reset` request line, dropping the sidecar's history.
 */
std::string build_reset_request(long _id);

/**
 * @brief Parse one sidecar line. Never throws.
 * @param[in] _line Raw line, with or without the trailing newline.
 * @param[out] _out Populated on success (`kind::unknown` otherwise).
 * @return True when the line was a JSON object.
 */
bool parse_line(std::string_view _line, message *_out);

} // namespace campcat::llm_protocol
