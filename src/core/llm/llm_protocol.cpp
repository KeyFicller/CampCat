#include "core/llm/llm_protocol.h"

#include <cstddef>

#include <nlohmann/json.hpp>

namespace campcat::llm_protocol {

namespace {

constexpr char k_base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Value of one base64 character, or -1 when it is not in the alphabet.
int base64_value(unsigned char _c) {
  if (_c >= 'A' && _c <= 'Z') {
    return _c - 'A';
  }
  if (_c >= 'a' && _c <= 'z') {
    return _c - 'a' + 26;
  }
  if (_c >= '0' && _c <= '9') {
    return _c - '0' + 52;
  }
  if (_c == '+') {
    return 62;
  }
  if (_c == '/') {
    return 63;
  }
  return -1;
}

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

bool base64_decode(std::string_view _in, std::string *_out) {
  if (_out == nullptr) {
    return false;
  }
  _out->clear();

  std::size_t len = _in.size();
  std::size_t pad = 0;
  while (len > 0 && _in[len - 1] == '=') {
    if (++pad > 2) {
      return false;
    }
    --len;
  }
  // A lone trailing character cannot encode a byte, and padding has to complete
  // the group it was added to.
  if (len % 4 == 1 || (pad > 0 && (len + pad) % 4 != 0)) {
    return false;
  }

  _out->reserve(len / 4 * 3 + 2);
  unsigned int acc = 0;
  int bits = 0;
  for (std::size_t i = 0; i < len; ++i) {
    const int v = base64_value(static_cast<unsigned char>(_in[i]));
    if (v < 0) {
      _out->clear();
      return false;
    }
    acc = (acc << 6) | static_cast<unsigned int>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      _out->push_back(static_cast<char>((acc >> bits) & 0xFFU));
    }
  }
  return true;
}

std::string build_turn_request(long _id, std::string_view _text,
                               std::string_view _tools_json) {
  nlohmann::json j;
  j["type"] = "turn";
  j["id"] = _id;
  j["text"] = std::string(_text);
  if (!_tools_json.empty()) {
    // Parsed rather than embedded: the tools have to reach bind_tools as a
    // JSON array, not as a string the sidecar would then have to re-parse.
    j["tools"] = nlohmann::json::parse(_tools_json, nullptr, false);
  }
  return j.dump();
}

std::string build_tool_result(std::string_view _call_id, bool _ok,
                              std::string_view _text, std::string_view _error,
                              std::string_view _image_b64) {
  nlohmann::json j;
  j["type"] = "tool_result";
  j["call_id"] = std::string(_call_id);
  j["ok"] = _ok;
  j["text"] = std::string(_text);
  j["error"] = std::string(_error);
  if (!_image_b64.empty()) {
    j["image_b64"] = std::string(_image_b64);
  }
  return j.dump();
}

std::string build_reset_request(long _id) {
  nlohmann::json j;
  j["type"] = "reset";
  j["id"] = _id;
  return j.dump();
}

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
  if (type == "result") {
    _out->type = message::kind::result;
    _out->id = j.value("id", 0L);
    _out->ok = j.value("ok", false);
    _out->text = j.value("text", std::string());
    _out->error = j.value("error", std::string());
    _out->turns = j.value("turns", 0);
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
    // Kept as text: the C++ binder parses it, so malformed arguments surface as
    // a tool error the model can see rather than being dropped here.
    _out->tool_args_json = j.contains("args") ? j["args"].dump() : std::string("{}");
    _out->image_b64 = j.value("image_b64", std::string());
    return true;
  }

  return true; // valid JSON object, unrecognized type -> kind::unknown
}

} // namespace campcat::llm_protocol
