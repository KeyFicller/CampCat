#pragma once

#include <string>
#include <string_view>

namespace campcat::llm_protocol {

/// Current protocol revision. The sidecar states its own in `ready`; a mismatch
/// means one side was rebuilt without the other, which breaks the tools.
/// 4: `tool_call` may carry `image_b64`, a screenshot the sidecar kept.
/// 5: `tool_call` no longer carries `image_b64` -- the host keeps the session's
///    numbered screenshots itself -- and `tool_result` carries the `shot`
///    number an image-returning tool filed its frame under.
/// 6: `turn` carries `memory`, the long-term notes the host read off disk, and
///    `memorize` asks the sidecar to fold the session into them.
/// 7: `turn` and `memorize` carry `provider`, the model provider the host's
///    settings selected.
/// 8: the host may send `interrupt` in place of a `tool_result` to stop the
///    turn, and `result` may carry `interrupted`.
/// 9: `turn` carries `scripter_memory`, and `memorize` carries `agent`.
/// 10: `consult` asks the sidecar's model a yes/no about a picture while a
///     script waits, and `consult_result` answers it.
constexpr int k_protocol_version = 10;

/**
 * @brief One decoded line from the sidecar's stdout.
 *
 * `type` is `unknown` for malformed input or an unrecognized `type` field, so
 * callers can ignore stray lines instead of treating them as fatal.
 */
struct message {
    enum class kind {
        ready,
        log,
        note,
        result,
        chunk,
        tool_call,
        consult_result,
        unknown
    };

    kind type = kind::unknown;
    long id = 0;

    // result
    bool ok = false;
    bool interrupted = false; ///< the user sent a new message; default false
    std::string text;
    std::string error;

    // chunk (shares `text` as the delta)
    bool thinking = false; ///< reasoning: `text` is its delta, empty = heartbeat

    // tool_call (the sidecar is asking us to run a tool)
    std::string call_id; ///< echoed back in the answering tool_result
    std::string tool_name;
    std::string tool_args_json; ///< the model's raw arguments object, unparsed

    // consult_result: `text` is unused; `ok` and `answer` are the reply
    /// "yes" / "no", or empty when the model did not answer one of those.
    std::string answer;

    // log
    std::string level;
    std::string log_message;

    // note (shares `text`: a line the sidecar wants in the conversation)

    // ready
    int protocol = 0;

    /// Turns the sidecar retains after this result; 0 after a reset.
    int turns = 0;

    /// Present only when the sidecar put the field on this line. Absent means
    /// "leave the display alone", not zero.
    bool has_llm_calls = false;
    int llm_calls = 0;
    bool has_tool_calls = false;
    int tool_calls = 0;
    bool has_context = false;
    int context_tokens = 0;
};

/// Counters the status bar shows. `context_tokens` of -1 means the provider
/// has not reported one since the last reset.
struct usage_stats {
    int llm_calls = 0;
    int tool_calls = 0;
    int context_tokens = -1;
};

/// Copy the usage fields this message actually carries. Missing fields stay.
inline void apply_usage(usage_stats *_dst, const message &_msg) {
    if (_dst == nullptr) {
        return;
    }
    if (_msg.has_llm_calls) {
        _dst->llm_calls = _msg.llm_calls;
    }
    if (_msg.has_tool_calls) {
        _dst->tool_calls = _msg.tool_calls;
    }
    if (_msg.has_context) {
        _dst->context_tokens = _msg.context_tokens;
    }
}

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
 * screenshots. Field names are the contract with `campcat/sources/py/main.py`;
 * keep both sides in sync.
 *
 * @param[in] _text The user's instruction; blank asks for nothing.
 * @param[in] _tools_json OpenAI tools array; empty to offer no tools.
 * @param[in] _memory Long-term notes about this user, read off disk by the
 * host. Sent even when empty, so the field's shape never moves.
 * @param[in] _scripter_memory The scripter's own notes. Sent even when empty.
 * @param[in] _provider Which model provider the sidecar should build, from the
 * host's settings. Sent even when empty, for the same reason as `_memory`.
 * @param[in] _operator_only Only the operator drives, and only with the tools
 * a training sample needs (`SOLO` on the sidecar). Set while the host records
 * training samples, so one sample holds one agent's regions. Omitting the
 * field (the default) leaves the ordinary toolset, which is what older
 * sidecars read.
 */
std::string build_turn_request(long _id, std::string_view _text,
                               std::string_view _tools_json,
                               std::string_view _memory,
                               std::string_view _scripter_memory,
                               std::string_view _provider,
                               int _max_model_rounds,
                               bool _operator_only = false);

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
 * @brief Build one `interrupt` line for the turn `_id` (no trailing newline).
 *
 * Sent in place of the `tool_result` the sidecar is waiting on. The sidecar
 * stops that turn and answers with a `result` whose `interrupted` is true.
 */
std::string build_interrupt_request(long _id);

/**
 * @brief Build one `memorize` request line: fold this session into the memory.
 *
 * The sidecar already holds the conversation, so nothing but the current notes
 * crosses the wire; the reply's `text` is the rewritten notes, for the host to
 * write to disk.
 *
 * @param[in] _memory The notes as they stand now; empty when there are none.
 * @param[in] _provider Which model provider the sidecar should build, so the
 * consolidation runs on the model the settings selected.
 * @param[in] _agent `operator` or `scripter`: which notebook this pass
 * rewrites.
 */
std::string build_memorize_request(long _id, std::string_view _memory,
                                   std::string_view _provider,
                                   std::string_view _agent);

/**
 * @brief Build one `consult` line: a question for the sidecar's model to
 * answer from the picture, with no conversation state involved.
 *
 * Carries its own `id` rather than riding a turn: the script that asks may
 * have been started from the Run button, with no turn in flight at all.
 *
 * @param[in] _repo_root The directory the sidecar is found under; the same one
 * `run_turn` takes.
 * @param[in] _provider Which model provider to build, as `turn` does.
 * @param[in] _question The script's `ask(...)` text, verbatim.
 * @param[in] _image_b64 PNG bytes of the screen, already base64.
 */
std::string build_consult(long _id, std::string_view _provider,
                          std::string_view _question,
                          std::string_view _image_b64);

/**
 * @brief Build one `consult_result` line answering a `consult`.
 * @param[in] _answer "yes" / "no", or empty when the model did not say either.
 */
std::string build_consult_result(long _id, bool _ok, std::string_view _answer,
                                 std::string_view _error);

/**
 * @brief Parse one sidecar line. Never throws.
 * @param[in] _line Raw line, with or without the trailing newline.
 * @param[out] _out Populated on success (`kind::unknown` otherwise).
 * @return True when the line was a JSON object.
 */
bool parse_line(std::string_view _line, message *_out);

} // namespace campcat::llm_protocol
