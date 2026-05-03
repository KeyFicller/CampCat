#include "games/stzb_auto_assemble/stzb_auto_assemble_profile.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace campcat {

namespace {

rect_norm parse_roi(const nlohmann::json &_j) {
  rect_norm r;
  r.x = _j.value("x", 0.0);
  r.y = _j.value("y", 0.0);
  r.w = _j.value("w", 0.0);
  r.h = _j.value("h", 0.0);
  return r;
}

} // namespace

std::filesystem::path stzb_auto_assemble_profile::template_resolution_dir() const {
  return bundle_dir / resolution_subdir;
}

const std::string &
stzb_auto_assemble_profile::tmpl(std::string_view logical_name) const {
  static const std::string k_empty{};
  std::string k(logical_name);
  const auto it = templates.find(k);
  if (it == templates.end()) {
    return k_empty;
  }
  return it->second;
}

stzb_auto_assemble_profile stzb_auto_assemble_profile::defaults() {
  stzb_auto_assemble_profile p;
  p.templates["dismiss_notice"] = "dismiss_notice.png";
  p.templates["game_main"] = "game_main.png";
  p.templates["main_menu_anchor"] = "main_menu_anchor.png";
  p.templates["enter_city_location"] = "location.png";
  p.templates["enter_city_city"] = "city.png";
  p.templates["enter_city_detail"] = "city_detail.png";
  p.templates["recruit_detail"] = "recruit.png";
  p.templates["recruit_back"] = "back.png";
  p.templates["recruit_back_2"] = "back_2.png";
  p.templates["assemble"] = "assemble.png";

  p.team_rois[0] = {0.1414, 0.6646, 0.1219, 0.2194};
  p.team_rois[1] = {0.2914, 0.6632, 0.1223, 0.2201};
  p.team_rois[2] = {0.443, 0.6646, 0.1223, 0.2201};
  p.team_rois[3] = {0.593, 0.6639, 0.1223, 0.2201};
  p.team_rois[4] = {0.7441, 0.6639, 0.1223, 0.2201};

  return p;
}

void stzb_auto_assemble_profile::from_json(const nlohmann::json &j,
                                           stzb_auto_assemble_profile &p) {
  p.version = j.value("version", 1);
  if (j.contains("bundle_dir")) {
    if (j["bundle_dir"].is_string()) {
      p.bundle_dir = j["bundle_dir"].get<std::string>();
    }
  }
  p.resolution_subdir = j.value("resolution_subdir", p.resolution_subdir);
  p.templates.clear();
  if (j.contains("templates") && j["templates"].is_object()) {
    for (const auto &item : j["templates"].items()) {
      if (item.value().is_string()) {
        p.templates[item.key()] = item.value().get<std::string>();
      }
    }
  }
  p.team_count = j.value("team_count", p.team_count);
  if (j.contains("team_rois") && j["team_rois"].is_array()) {
    size_t i = 0;
    for (const auto &item : j["team_rois"]) {
      if (i >= p.team_rois.size()) {
        break;
      }
      p.team_rois[i++] = parse_roi(item);
    }
  }
}

nlohmann::json stzb_auto_assemble_profile::to_json(const stzb_auto_assemble_profile &p) {
  nlohmann::json j;
  j["version"] = p.version;
  j["bundle_dir"] = p.bundle_dir.string();
  j["resolution_subdir"] = p.resolution_subdir;
  j["templates"] = nlohmann::json::object();
  std::vector<std::string> keys;
  keys.reserve(p.templates.size());
  for (const auto &kv : p.templates) {
    keys.push_back(kv.first);
  }
  std::sort(keys.begin(), keys.end());
  for (const auto &k : keys) {
    j["templates"][k] = p.templates.at(k);
  }
  j["team_count"] = p.team_count;
  j["team_rois"] = nlohmann::json::array();
  for (const auto &r : p.team_rois) {
    nlohmann::json jr;
    jr["x"] = r.x;
    jr["y"] = r.y;
    jr["w"] = r.w;
    jr["h"] = r.h;
    j["team_rois"].push_back(jr);
  }
  return j;
}

stzb_auto_assemble_profile stzb_auto_assemble_profile::load_from_file(
    const std::filesystem::path &_path,
    const std::filesystem::path &_project_root) {
  stzb_auto_assemble_profile p = defaults();
  std::ifstream in(_path);
  if (!in) {
    return p;
  }
  nlohmann::json j;
  in >> j;
  from_json(j, p);
  if (p.bundle_dir.is_relative()) {
    p.bundle_dir = std::filesystem::weakly_canonical(_project_root / p.bundle_dir);
  }
  return p;
}

void stzb_auto_assemble_profile::save_to_file(
    const std::filesystem::path &_path,
    const std::filesystem::path &_project_root) const {
  stzb_auto_assemble_profile copy = *this;
  if (copy.bundle_dir.is_absolute()) {
    try {
      const auto rel = std::filesystem::relative(copy.bundle_dir, _project_root);
      copy.bundle_dir = rel;
    } catch (...) {
      // keep absolute if not under project root
    }
  }
  std::filesystem::create_directories(_path.parent_path());
  std::ofstream out(_path);
  if (!out) {
    throw std::runtime_error("failed to open script profile for write");
  }
  out << to_json(copy).dump(2) << '\n';
}

nlohmann::json stzb_auto_assemble_profile::to_summary_json_for_log() const {
  nlohmann::json j;
  j["bundle_dir"] = bundle_dir.string();
  j["resolution_subdir"] = resolution_subdir;
  j["template_resolution_dir"] = template_resolution_dir().string();
  j["templates"] = nlohmann::json::object();
  std::vector<std::string> keys;
  keys.reserve(templates.size());
  for (const auto &kv : templates) {
    keys.push_back(kv.first);
  }
  std::sort(keys.begin(), keys.end());
  for (const auto &k : keys) {
    j["templates"][k] = templates.at(k);
  }
  j["team_count"] = team_count;
  j["team_rois"] = nlohmann::json::array();
  for (const auto &r : team_rois) {
    nlohmann::json jr;
    jr["x"] = r.x;
    jr["y"] = r.y;
    jr["w"] = r.w;
    jr["h"] = r.h;
    j["team_rois"].push_back(jr);
  }
  return j;
}

stzb_auto_assemble_profile detail::profile_from_legacy_top_level_game(
    const nlohmann::json &root_document) {
  stzb_auto_assemble_profile p = stzb_auto_assemble_profile::defaults();
  if (!root_document.contains("game") || !root_document["game"].is_object()) {
    return p;
  }
  const auto &g = root_document["game"];
  if (g.contains("templates") && g["templates"].is_object()) {
    for (const auto &item : g["templates"].items()) {
      if (item.value().is_string()) {
        p.templates[item.key()] = item.value().get<std::string>();
      }
    }
  }
  p.team_count = g.value("team_count", p.team_count);
  if (g.contains("team_rois") && g["team_rois"].is_array()) {
    size_t i = 0;
    for (const auto &item : g["team_rois"]) {
      if (i >= p.team_rois.size()) {
        break;
      }
      p.team_rois[i++] = parse_roi(item);
    }
  }
  if (root_document.contains("template_root")) {
    p.bundle_dir = root_document["template_root"].get<std::string>();
  }
  p.resolution_subdir =
      root_document.value("resolution_subdir", p.resolution_subdir);
  return p;
}

} // namespace campcat
