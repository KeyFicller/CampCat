#include "core/llm/llm_protocol.h"

#include <cstddef>

#include <nlohmann/json.hpp>

namespace campcat::llm_protocol {

namespace {

constexpr char k_base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string base64_encode(const unsigned char *_data, std::size_t _len) {
    std::string out;
    if (_data == nullptr || _len == 0) {
        return out;
    }
    out.reserve(((_len + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= _len) {
        const unsigned int n = (static_cast<unsigned int>(_data[i]) << 16) |
                               (static_cast<unsigned int>(_data[i + 1]) << 8) |
                               static_cast<unsigned int>(_data[i + 2]);
        out.push_back(k_base64_table[(n >> 18) & 0x3F]);
        out.push_back(k_base64_table[(n >> 12) & 0x3F]);
        out.push_back(k_base64_table[(n >> 6) & 0x3F]);
        out.push_back(k_base64_table[n & 0x3F]);
        i += 3;
    }

    const std::size_t rem = _len - i;
    if (rem == 1) {
        const unsigned int n = static_cast<unsigned int>(_data[i]) << 16;
        out.push_back(k_base64_table[(n >> 18) & 0x3F]);
        out.push_back(k_base64_table[(n >> 12) & 0x3F]);
        out += "==";
    } else if (rem == 2) {
        const unsigned int n = (static_cast<unsigned int>(_data[i]) << 16) |
                               (static_cast<unsigned int>(_data[i + 1]) << 8);
        out.push_back(k_base64_table[(n >> 18) & 0x3F]);
        out.push_back(k_base64_table[(n >> 12) & 0x3F]);
        out.push_back(k_base64_table[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string build_turn_request(long _id, std::string_view _text,
                               std::string_view _tools_json,
                               std::string_view _memory,
                               std::string_view _scripter_memory,
                               std::string_view _provider,
                               int _max_model_rounds, bool _operator_only) {
    nlohmann::json j;
    j["type"] = "turn";
    j["id"] = _id;
    j["text"] = std::string(_text);
    if (!_tools_json.empty()) {
        // Parsed rather than embedded: the tools have to reach bind_tools as a
        // JSON array, not as a string the sidecar would then have to re-parse.
        j["tools"] = nlohmann::json::parse(_tools_json, nullptr, false);
    }
    j["memory"] = std::string(_memory);
    j["scripter_memory"] = std::string(_scripter_memory);
    j["provider"] = std::string(_provider);
    j["max_model_rounds"] = _max_model_rounds;
    // Absent rather than false: a sidecar that predates the field reads a
    // missing key as "the handoff is on", which is what it already did.
    if (_operator_only) {
        j["operator_only"] = true;
    }
    return j.dump();
}

std::string build_tool_result(std::string_view _call_id, bool _ok,
                              std::string_view _text, std::string_view _error,
                              std::string_view _image_b64, int _shot) {
    nlohmann::json j;
    j["type"] = "tool_result";
    j["call_id"] = std::string(_call_id);
    j["ok"] = _ok;
    j["text"] = std::string(_text);
    j["error"] = std::string(_error);
    if (!_image_b64.empty()) {
        j["image_b64"] = std::string(_image_b64);
    }
    // Only an image-returning tool has a frame to name; every other result
    // leaves the field out rather than sending a 0 the sidecar would have to
    // ignore.
    if (_shot > 0) {
        j["shot"] = _shot;
    }
    return j.dump();
}

std::string build_reset_request(long _id) {
    nlohmann::json j;
    j["type"] = "reset";
    j["id"] = _id;
    return j.dump();
}

std::string build_interrupt_request(long _id) {
    nlohmann::json j;
    j["type"] = "interrupt";
    j["id"] = _id;
    return j.dump();
}

std::string build_memorize_request(long _id, std::string_view _memory,
                                   std::string_view _provider,
                                   std::string_view _agent) {
    nlohmann::json j;
    j["type"] = "memorize";
    j["id"] = _id;
    j["memory"] = std::string(_memory);
    j["provider"] = std::string(_provider);
    j["agent"] = std::string(_agent);
    return j.dump();
}

std::string build_consult(long _id, std::string_view _provider,
                          std::string_view _question,
                          std::string_view _image_b64) {
    nlohmann::json j;
    j["type"] = "consult";
    j["id"] = _id;
    j["provider"] = std::string(_provider);
    j["question"] = std::string(_question);
    j["image_b64"] = std::string(_image_b64);
    return j.dump();
}

std::string build_consult_result(long _id, bool _ok, std::string_view _answer,
                                 std::string_view _error) {
    nlohmann::json j;
    j["type"] = "consult_result";
    j["id"] = _id;
    j["ok"] = _ok;
    j["answer"] = std::string(_answer);
    j["error"] = std::string(_error);
    return j.dump();
}

namespace {

void read_usage(const nlohmann::json &_j, message *_out) {
    if (_j.contains("llm_calls")) {
        _out->has_llm_calls = true;
        _out->llm_calls = _j.value("llm_calls", 0);
    }
    if (_j.contains("tool_calls")) {
        _out->has_tool_calls = true;
        _out->tool_calls = _j.value("tool_calls", 0);
    }
    if (_j.contains("context")) {
        _out->has_context = true;
        _out->context_tokens = _j.value("context", 0);
    }
}

} // namespace

bool parse_line(std::string_view _line, message *_out) {
    if (_out == nullptr) {
        return false;
    }
    *_out = message{};

    while (!_line.empty() && (_line.back() == '\n' || _line.back() == '\r')) {
        _line.remove_suffix(1);
    }
    if (_line.empty()) {
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(_line);
    } catch (const std::exception &) {
        return false;
    }
    if (!j.is_object()) {
        return false;
    }

    const std::string type = j.value("type", std::string());
    if (type == "ready") {
        _out->type = message::kind::ready;
        _out->protocol = j.value("protocol", 0);
        return true;
    }
    if (type == "log") {
        _out->type = message::kind::log;
        _out->level = j.value("level", std::string());
        _out->log_message = j.value("message", std::string());
        return true;
    }
    if (type == "note") {
        _out->type = message::kind::note;
        _out->text = j.value("text", std::string());
        return true;
    }
    if (type == "result") {
        _out->type = message::kind::result;
        _out->id = j.value("id", 0L);
        _out->ok = j.value("ok", false);
        _out->interrupted = j.value("interrupted", false);
        _out->text = j.value("text", std::string());
        _out->error = j.value("error", std::string());
        _out->turns = j.value("turns", 0);
        read_usage(j, _out);
        return true;
    }
    if (type == "chunk") {
        _out->type = message::kind::chunk;
        _out->id = j.value("id", 0L);
        _out->text = j.value("text", std::string());
        _out->thinking = j.value("thinking", false);
        return true;
    }
    if (type == "tool_call") {
        _out->type = message::kind::tool_call;
        _out->id = j.value("id", 0L);
        _out->call_id = j.value("call_id", std::string());
        _out->tool_name = j.value("name", std::string());
        // Kept as text: the C++ binder parses it, so malformed arguments
        // surface as a tool error the model can see rather than being dropped
        // here.
        _out->tool_args_json =
            j.contains("args") ? j["args"].dump() : std::string("{}");
        read_usage(j, _out);
        return true;
    }

    if (type == "subagent") {
        _out->type = message::kind::subagent;
        _out->id = j.value("id", 0L);
        _out->agent = j.value("agent", std::string());
        _out->subagent_event = j.value("event", std::string());
        _out->text = j.value("text", std::string());
        _out->ok = j.value("ok", true);
        return true;
    }

    if (type == "consult_result") {
        _out->type = message::kind::consult_result;
        _out->id = j.value("id", 0L);
        _out->ok = j.value("ok", false);
        _out->answer = j.value("answer", std::string());
        _out->error = j.value("error", std::string());
        return true;
    }

    return true; // valid JSON object, unrecognized type -> kind::unknown
}

} // namespace campcat::llm_protocol
