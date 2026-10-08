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
  enum class kind { ready, log, result, unknown };

  kind type = kind::unknown;
  long id = 0;

  // result
  bool ok = false;
  std::string text;
  std::string error;

  // log
  std::string level;
  std::string log_message;

  // ready
  int protocol = 0;
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
 * The request carries an id and a base64 PNG, nothing else: model selection
 * and prompts belong to the sidecar. Field names are the contract with
 * `llm/main.py`; keep both sides in sync.
 */
std::string build_describe_request(std::string_view _image_b64, long _id);

/**
 * @brief Parse one sidecar line. Never throws.
 * @param[in] _line Raw line, with or without the trailing newline.
 * @param[out] _out Populated on success (`kind::unknown` otherwise).
 * @return True when the line was a JSON object.
 */
bool parse_line(std::string_view _line, message *_out);

} // namespace campcat::llm_protocol
