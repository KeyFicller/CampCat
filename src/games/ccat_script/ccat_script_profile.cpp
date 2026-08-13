#include "games/ccat_script/ccat_script_profile.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace campcat {

namespace {

void from_json(const nlohmann::json &_j, ccat_script_profile &_p) {
  _p.version = _j.value("version", 1);
  _p.source_rel = _j.value("source", _p.source_rel);
  // Legacy `images_root` keys are ignored; PNGs always sit beside the .ccat.
}

std::string trim_copy(std::string_view _s) {
  size_t b = 0;
  while (b < _s.size() &&
         std::isspace(static_cast<unsigned char>(_s[b]))) {
    ++b;
  }
  size_t e = _s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(_s[e - 1]))) {
    --e;
  }
  return std::string(_s.substr(b, e - b));
}

} // namespace

bool ccat_script_profile::has_script_source() const {
  return !trim_copy(source_rel).empty();
}

std::filesystem::path
ccat_script_profile::images_base(const std::filesystem::path &_config_home) const {
  if (!has_script_source()) {
    return {};
  }
  std::error_code ec;
  const std::filesystem::path script_path = _config_home / trim_copy(source_rel);
  const std::filesystem::path canon =
      std::filesystem::weakly_canonical(script_path, ec);
  const std::filesystem::path resolved =
      ec ? script_path.lexically_normal() : canon;
  return resolved.parent_path();
}

ccat_script_profile ccat_script_profile::defaults() {
  ccat_script_profile p;
  p.version = 1;
  p.source_rel = "scripts/demo.ccat";
  return p;
}

ccat_script_profile ccat_script_profile::load_from_bundle_path(
    const std::filesystem::path &_bundle_json_path,
    const std::filesystem::path &_config_home) {
  ccat_script_profile p = defaults();
  std::error_code ec;
  p.bundle_path_absolute =
      std::filesystem::weakly_canonical(_bundle_json_path, ec);
  if (ec) {
    p.bundle_path_absolute = _bundle_json_path.lexically_normal();
  }

  std::ifstream in(_bundle_json_path);
  if (!in) {
    return p;
  }
  nlohmann::json j;
  in >> j;
  from_json(j, p);
  p.source_rel = trim_copy(p.source_rel);

  if (!p.has_script_source()) {
    return p;
  }

  const std::filesystem::path script_path = _config_home / p.source_rel;
  ec.clear();
  p.script_path_absolute =
      std::filesystem::weakly_canonical(script_path, ec);
  if (ec) {
    p.script_path_absolute = script_path.lexically_normal();
  }

  std::ifstream sin(p.script_path_absolute, std::ios::binary);
  if (sin) {
    std::ostringstream oss;
    oss << sin.rdbuf();
    p.script_text = oss.str();
  }
  return p;
}

void ccat_script_profile::save_to_bundle_path(
    const std::filesystem::path &_bundle_json_path,
    const std::filesystem::path &_config_home) const {
  if (!has_script_source()) {
    throw std::runtime_error(
        "ccat script source path is empty; set source before save");
  }
  std::filesystem::create_directories(_bundle_json_path.parent_path());
  std::ofstream out(_bundle_json_path);
  if (!out) {
    throw std::runtime_error("failed to open ccat script bundle for write");
  }
  out << to_json(_config_home).dump(2) << '\n';
}

nlohmann::json
ccat_script_profile::to_json(const std::filesystem::path &) const {
  nlohmann::json j;
  j["version"] = version;
  j["source"] = trim_copy(source_rel);
  return j;
}

nlohmann::json ccat_script_profile::to_summary_json_for_log() const {
  nlohmann::json j;
  j["version"] = version;
  j["source"] = source_rel;
  j["images_base"] = script_path_absolute.empty()
                         ? std::string{}
                         : script_path_absolute.parent_path().string();
  j["bundle_path"] = bundle_path_absolute.string();
  j["script_path"] = script_path_absolute.string();
  j["script_characters"] = script_text.size();
  return j;
}

} // namespace campcat
