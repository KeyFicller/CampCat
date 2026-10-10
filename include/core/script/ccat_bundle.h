#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace campcat::ccat_lang {

/**
 * @brief The main script of a bundle: `<script_dir>/<name>/main.ccat`.
 *
 * Its existence is also what makes a bundle exist, for every caller that names
 * one.
 */
std::filesystem::path bundle_main(const std::filesystem::path &_script_dir,
                                  std::string_view _name);

/**
 * @brief The bundles under `_script_dir`, sorted.
 *
 * Sorted because directory order is not stable, and a list that reshuffles
 * between two identical calls reads as a change.
 */
std::vector<std::string> bundle_names(const std::filesystem::path &_script_dir);

/**
 * @brief The bundle's one-line description: the text after `//` on the first
 * line of its main.ccat, trimmed.
 *
 * Only the *first* line counts -- a comment further down is a comment about a
 * statement, not a summary of the skill -- so a bundle whose first line is blank
 * or code has no description.
 */
std::string bundle_description(const std::filesystem::path &_script_dir,
                               std::string_view _name);

/// @brief The template PNGs in a bundle, sorted.
std::vector<std::string> bundle_templates(const std::filesystem::path &_script_dir,
                                          std::string_view _name);

/// @brief Why `save_bundle_main` did not write.
enum class save_status {
  ok,              ///< written
  parse_error,     ///< `_text` does not parse, so nothing was written
  changed_on_disk, ///< the file no longer holds `_loaded`, so nothing was written
  write_failed,    ///< could not read or write (missing file, permissions)
};

/**
 * @brief Write `_text` to a bundle's main.ccat, refusing a write that would
 * destroy something.
 *
 * Two guards, both before the file is touched: `_text` must parse (a script that
 * cannot run is not saved), and the file on disk must still hold `_loaded` (the
 * model may have rewritten it through `update_script` while the editor was
 * open). `_err`, when non-null, receives a sentence for the user, carrying the
 * `line:col` of a syntax error.
 */
save_status save_bundle_main(const std::filesystem::path &_script_dir,
                             std::string_view _name, const std::string &_text,
                             const std::string &_loaded, std::string *_err);

} // namespace campcat::ccat_lang
