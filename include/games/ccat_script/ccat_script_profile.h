#pragma once

#include "core/automation/automation_profile.h"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace campcat {

/**
 * @brief Persisted bundle describing where `.ccat` lives plus optional PNG root overrides.
 */
struct ccat_script_profile final : automation_profile {
  /** @brief Bundle schema revision mirrored inside JSON `version` field. */
  int version = 1;

  /**
   * @brief Relative path (from config_home) pointing at `.ccat` source loaded each automation tick.
   */
  std::string source_rel;

  /**
   * @brief When empty: PNG operands resolve beside resolved `.ccat` parent directory.
   * When non-empty: interpreted relative to config_home for centralized asset folders.
   */
  std::string images_root_rel;

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
   * @brief Compute PNG search root honoring optional images_root override semantics.
   * @param[in] _config_home Shell persistence directory anchoring relative bundle paths.
   * @return Absolute/normalized directory preceding relative template filenames inside DSL.
   */
  std::filesystem::path
  images_base(const std::filesystem::path &_config_home) const;

  /**
   * @brief Factory defaults mirroring repository sample layout (`scripts/demo.ccat`).
   * @return Serializable defaults suitable before user-authored bundle JSON exists on disk.
   */
  static ccat_script_profile defaults();

  /**
   * @brief Hydrate profile JSON then preload `.ccat` body plus canonical filesystem hints.
   * @param[in] _bundle_json_path Absolute or workspace-relative bundle JSON pointer on disk.
   * @param[in] _config_home Shell directory pairing relative JSON entries inside maps.
   * @return Populated profile — falls back to defaults when JSON unreadable/missing fields.
   */
  static ccat_script_profile load_from_bundle_path(
      const std::filesystem::path &_bundle_json_path,
      const std::filesystem::path &_config_home);

  /**
   * @brief Persist bundle metadata JSON (does not rewrite `.ccat` itself).
   * @param[in] _bundle_json_path Destination JSON path honoring parent directory creation.
   * @param[in] _config_home Reserved hook for future relocations (currently informational only).
   */
  void save_to_bundle_path(const std::filesystem::path &_bundle_json_path,
                           const std::filesystem::path &_config_home) const;

  /**
   * @brief Serialize fields suitable for disk persistence round-trips.
   * @param[in] _config_home Mirrors save hook semantics for forward-compatible signatures.
   * @return JSON object persisted verbatim via nlohmann pretty printer.
   */
  nlohmann::json to_json(const std::filesystem::path &_config_home) const;

  /**
   * @brief Compact summary surfaced inside Dear ImGui logging hooks when switching scripts.
   * @return JSON snapshot enumerating paths/counts useful for operator verification.
   */
  nlohmann::json to_summary_json_for_log() const;
};

} // namespace campcat
