#pragma once

#include <string>
#include <string_view>

namespace campcat::llm_protocol {

/// Current protocol revision. The sidecar states its own in `ready`; a mismatch
/// means one side was rebuilt without the other, which breaks the tools.
/// 4: `tool_call` may carry `image_b64`, a screenshot the sidecar kept.
/// 5: `tool_call` no longer carries `image_b64` -- the host keeps the session's
///    numbered screenshots itself -- and `tool_result` carries the `shot` number
///    an image-returning tool filed its frame under.
constexpr int k_protocol_version = 5;

/**
 * @brief One decoded line from the sidecar's stdout.
 *
 * `type` is `unknown` for malformed input or an unrecognized `type` field, so
 * callers can ignore stray lines instead of treating them as fatal.
 */
struct message {
  enum class kind { ready, log, result, chunk, tool_call, unknown };

  kind type = kind::unknown;
  long id = 0;

  // result
  bool ok = false;
  std::string text;
  std::string error;

  // chunk (shares `text` as the delta)
  bool thinking = false; ///< reasoning heartbeat, `text` is empty

  // tool_call (the sidecar is asking us to run a tool)
  std::string call_id;        ///< echoed back in the answering tool_result
  std::string tool_name;
  std::string tool_args_json; ///< the model's raw arguments object, unparsed

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
 * @brief Build one `turn` request line (no trailing newline).
 *
 * The request carries an id, the user's instruction and the tool schemas. Model
 * selection, prompts and the device itself belong to the sidecar and the tools:
 * there is deliberately no image here, because the model now takes its own
 * screenshots. Field names are the contract with `llm/main.py`; keep both sides
 * in sync.
 *
 * @param[in] _text The user's instruction; blank asks for nothing.
 * @param[in] _tools_json OpenAI tools array; empty to offer no tools.
 */
std::string build_turn_request(long _id, std::string_view _text,
                               std::string_view _tools_json);

/**
 * @brief Build one `tool_result` line answering a `tool_call`.
 *
 * Carries no `id`: the sidecar matches the reply to its waiting request by
 * `call_id`, and the request ids stay unambiguous.
 *
 * @param[in] _image_b64 Set only by image-returning tools.
 * @param[in] _shot The frame number that image was filed under, so the sidecar
 *                  can caption it and name it in later `shot` arguments. 0 when
 *                  the result has no picture.
 */
std::string build_tool_result(std::string_view _call_id, bool _ok,
                              std::string_view _text, std::string_view _error,
                              std::string_view _image_b64, int _shot = 0);

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
