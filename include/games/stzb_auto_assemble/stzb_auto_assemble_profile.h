#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/app_config.h"
#include "core/automation/automation_profile.h"

namespace campcat {

/// STZB automation bundle: logical template keys map under
/// `bundle_dir / resolution_subdir`.
struct stzb_auto_assemble_profile : automation_profile {
  int version = 1;

  /// Relative to project root (parent of `config/` when using default layout).
  std::filesystem::path bundle_dir = "resources/scripts/stzb_auto_assemble";
  std::string resolution_subdir = "2560x1440";

  std::unordered_map<std::string, std::string> templates;
  int team_count = 5;
  std::array<rect_norm, 5> team_rois{};

  std::filesystem::path template_resolution_dir() const;

  const std::string &tmpl(std::string_view logical_name) const;

  static stzb_auto_assemble_profile defaults();
  static void from_json(const nlohmann::json &j, stzb_auto_assemble_profile &p);
  static nlohmann::json to_json(const stzb_auto_assemble_profile &p);

  static stzb_auto_assemble_profile load_from_file(const std::filesystem::path &_path,
                                                  const std::filesystem::path &_project_root);

  /// Resolves bundle_dir relative to project root before write.
  void save_to_file(const std::filesystem::path &_path,
                    const std::filesystem::path &_project_root) const;

  nlohmann::json to_summary_json_for_log() const;
};

namespace detail {

stzb_auto_assemble_profile profile_from_legacy_top_level_game(
    const nlohmann::json &root_document);

}

} // namespace campcat
