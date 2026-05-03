#include "core/app_config.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace campcat {

namespace {

constexpr const char k_stzb_script_id[] = "stzb_auto_assemble";
constexpr const char k_stzb_script_rel_json[] = "scripts/stzb_auto_assemble.json";

constexpr const char k_ccat_script_id[] = "ccat_script";
constexpr const char k_ccat_script_rel_json[] = "scripts/ccat_script.json";

} // namespace

void app_config::from_json(const nlohmann::json &_j, app_config &_c) {
  _c.version = _j.value("version", 1);
  _c.adb_path = _j.value("adb_path", _c.adb_path);
  _c.adb_serial = _j.value("adb_serial", _c.adb_serial);
  _c.adb_connect_address =
      _j.value("adb_connect_address", _c.adb_connect_address);
  _c.tap_delay_ms = _j.value("tap_delay_ms", _c.tap_delay_ms);
  _c.action_gap_ms = _j.value("action_gap_ms", _c.action_gap_ms);
  _c.swipe_duration_ms = _j.value("swipe_duration_ms", _c.swipe_duration_ms);
  _c.match_threshold = _j.value("match_threshold", _c.match_threshold);
  _c.match_multiscale = _j.value("match_multiscale", _c.match_multiscale);
  _c.max_step_retries = _j.value("max_step_retries", _c.max_step_retries);
  _c.step_retry_interval_ms =
      _j.value("step_retry_interval_ms", _c.step_retry_interval_ms);
  _c.debug_screenshots = _j.value("debug_screenshots", _c.debug_screenshots);
  if (_j.contains("debug_dir")) {
    _c.debug_dir = _j["debug_dir"].get<std::string>();
  }

  if (_j.contains("scheduler")) {
    const auto &s = _j["scheduler"];
    _c.scheduler.enabled = s.value("enabled", _c.scheduler.enabled);
    _c.scheduler.interval_seconds =
        s.value("interval_seconds", _c.scheduler.interval_seconds);
    _c.scheduler.jitter_seconds =
        s.value("jitter_seconds", _c.scheduler.jitter_seconds);
    _c.scheduler.skip_if_busy =
        s.value("skip_if_busy", _c.scheduler.skip_if_busy);
  }

  if (_j.contains("active_script")) {
    _c.active_script_id = _j["active_script"].get<std::string>();
  }

  if (_j.contains("scripts") && _j["scripts"].is_object()) {
    _c.script_config_paths.clear();
    for (const auto &item : _j["scripts"].items()) {
      if (item.value().is_string()) {
        _c.script_config_paths[item.key()] = item.value().get<std::string>();
      }
    }
  }

  if (!_c.script_config_paths.contains(k_ccat_script_id)) {
    _c.script_config_paths[k_ccat_script_id] = k_ccat_script_rel_json;
  }
}

nlohmann::json app_config::to_json(const app_config &_c) {
  nlohmann::json j;
  j["version"] = _c.version;
  j["adb_path"] = _c.adb_path;
  j["adb_serial"] = _c.adb_serial;
  j["adb_connect_address"] = _c.adb_connect_address;
  j["tap_delay_ms"] = _c.tap_delay_ms;
  j["action_gap_ms"] = _c.action_gap_ms;
  j["swipe_duration_ms"] = _c.swipe_duration_ms;
  j["match_threshold"] = _c.match_threshold;
  j["match_multiscale"] = _c.match_multiscale;
  j["max_step_retries"] = _c.max_step_retries;
  j["step_retry_interval_ms"] = _c.step_retry_interval_ms;
  j["debug_screenshots"] = _c.debug_screenshots;
  j["debug_dir"] = _c.debug_dir.string();

  j["scheduler"]["enabled"] = _c.scheduler.enabled;
  j["scheduler"]["interval_seconds"] = _c.scheduler.interval_seconds;
  j["scheduler"]["jitter_seconds"] = _c.scheduler.jitter_seconds;
  j["scheduler"]["skip_if_busy"] = _c.scheduler.skip_if_busy;

  j["active_script"] = _c.active_script_id;

  j["scripts"] = nlohmann::json::object();
  std::vector<std::string> keys;
  keys.reserve(_c.script_config_paths.size());
  for (const auto &kv : _c.script_config_paths) {
    keys.push_back(kv.first);
  }
  std::sort(keys.begin(), keys.end());
  for (const auto &k : keys) {
    j["scripts"][k] = _c.script_config_paths.at(k);
  }

  return j;
}

std::filesystem::path app_config::resolve_script_json(const std::string &_id) const {
  const auto it = script_config_paths.find(_id);
  if (it == script_config_paths.end()) {
    return {};
  }
  if (config_home.empty()) {
    return {};
  }
  const std::filesystem::path p =
      config_home / std::filesystem::path(it->second);
  std::error_code ec;
  const std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
  if (!ec) {
    return c;
  }
  return p.lexically_normal();
}

app_config app_config::defaults() {
  app_config c;
  c.script_config_paths[k_stzb_script_id] = k_stzb_script_rel_json;
  c.script_config_paths[k_ccat_script_id] = k_ccat_script_rel_json;
  c.active_script_id = k_stzb_script_id;
  return c;
}

app_config app_config::load_from_file(const std::filesystem::path &_path) {
  app_config c = defaults();
  std::filesystem::path canon_main;
  try {
    canon_main = std::filesystem::weakly_canonical(
        _path.is_absolute() ? _path : std::filesystem::absolute(_path));
  } catch (...) {
    canon_main = std::filesystem::absolute(_path);
  }
  c.config_home = canon_main.parent_path();

  std::ifstream in(canon_main);
  if (!in) {
    return c;
  }
  nlohmann::json j;
  in >> j;
  from_json(j, c);
  return c;
}

void app_config::save_to_file(const std::filesystem::path &_path) const {
  std::ofstream out(_path);
  if (!out) {
    throw std::runtime_error("failed to open config for write");
  }
  out << to_json(*this).dump(2) << '\n';
}

} // namespace campcat
