#pragma once

#include "core/automation/automation_profile.h"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace campcat {

/**
 * @brief Persisted bundle describing where `.ccat` lives.
 * PNG operands always resolve beside the `.ccat` file.
 */
struct ccat_script_profile final : automation_profile {
  /** @brief Bundle schema revision mirrored inside JSON `version` field. */
  int version = 1;

  /**
   * @brief Relative path (from config_home) pointing at `.ccat` source loaded each automation tick.
   */
  std::string source_rel;

  /**
   * @brief Cached UTF-8 snapshot populated during bundle load (optional diagnostics/UI parity).
   */
  std::string script_text;

  /**
   * @brief Canonical-ish absolute bundle JSON path assisting saves/logging workflows.
   */
  std::filesystem::path bundle_path_absolute;

  /**
   * @brief Canonical-ish absolute `.ccat` path derived during load from source_rel + config_home.
   */
  std::filesystem::path script_path_absolute;

  /**
   * @brief Directory of the resolved `.ccat` file (PNG search root).
   * @param[in] _config_home Shell persistence directory anchoring relative bundle paths.
   * @return Parent directory of the script path; empty when `source_rel` is empty.
   */
  std::filesystem::path
  images_base(const std::filesystem::path &_config_home) const;

  /**
   * @brief True when `source_rel` names a script path (required before save).
   */
  bool has_script_source() const;

  /**
   * @brief Factory defaults (empty script path until the user sets one).
   */
  static ccat_script_profile defaults();

  /**
   * @brief Hydrate profile JSON then preload `.ccat` body plus canonical filesystem hints.
   */
  static ccat_script_profile load_from_bundle_path(
      const std::filesystem::path &_bundle_json_path,
      const std::filesystem::path &_config_home);

  /**
   * @brief Persist bundle metadata JSON (does not rewrite `.ccat` itself).
   * @throws std::runtime_error when `source_rel` is empty or the file cannot be written.
   */
  void save_to_bundle_path(const std::filesystem::path &_bundle_json_path,
                           const std::filesystem::path &_config_home) const;

  nlohmann::json to_json(const std::filesystem::path &_config_home) const;

  nlohmann::json to_summary_json_for_log() const;
};

} // namespace campcat
