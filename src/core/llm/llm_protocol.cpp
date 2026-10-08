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

std::string build_describe_request(std::string_view _image_b64, long _id,
                                   std::string_view _text) {
  nlohmann::json j;
  j["type"] = "describe";
  j["id"] = _id;
  j["image_b64"] = std::string(_image_b64);
  if (!_text.empty()) {
    j["text"] = std::string(_text);
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

  return true; // valid JSON object, unrecognized type -> kind::unknown
}

} // namespace campcat::llm_protocol
