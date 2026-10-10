#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace campcat::ccat_lang {

/**
 * @brief Whether a name can safely be used as a directory component: non-empty,
 * not `.` or `..` (walking up), no separator (walking out or into a sibling),
 * and no control byte (a NUL would truncate the path in the OS call).
 *
 * Where a name becomes a directory -- creating a bundle, renaming one -- this is
 * the trust boundary, and it is checked in one place so the rule cannot drift.
 */
bool safe_leaf_name(std::string_view _name);

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

/**
 * @brief Resolve a `run()` target to an absolute path, exactly as the
 * interpreter does: a relative `_rel` hangs off `_base` (the referring script's
 * own directory), then canonicalization, falling back to lexical normalization.
 *
 * The interpreter calls this function rather than keeping its own copy, so
 * run()'s resolution and the dependency scan below cannot drift apart -- if they
 * drifted, a reference would go unseen and deleting the target would destroy it.
 */
std::filesystem::path resolve_script_path_from(const std::filesystem::path &_base,
                                               const std::string &_rel);

/**
 * @brief The other bundles `_name` reaches with `run()`, sorted, no duplicates.
 *
 * An edge is a resolved target landing inside another bundle's directory -- any
 * `.ccat` in there, not only its main.ccat, since deleting that bundle takes the
 * file with it. A self-reference is not an edge: a bundle cannot keep itself
 * alive. A bundle whose main.ccat is unreadable or unparseable simply has no
 * edges here; `check_bundle_deletable` is the caller that must not let that pass
 * silently.
 */
std::vector<std::string> bundle_dependencies(const std::filesystem::path &_script_dir,
                                             std::string_view _name);

/// @brief The other bundles whose `run()` reaches `_name`, sorted, no duplicates.
std::vector<std::string> bundle_referrers(const std::filesystem::path &_script_dir,
                                          std::string_view _name);

/// @brief Why `save_bundle_main` did not write.
enum class save_status {
  ok,              ///< written
  parse_error,     ///< `_text` does not parse, so nothing was written
  changed_on_disk, ///< the file no longer holds `_loaded`, so nothing was written
  write_failed,    ///< could not read or write (missing file, permissions)
};

/// @brief Why a bundle was not deleted.
enum class delete_status {
  ok,            ///< the guard passed; nothing was touched
  not_found,     ///< no such bundle (its main.ccat is not there)
  referenced,    ///< another bundle reaches it with run(), so it is refused
  scan_failed,   ///< another bundle would not parse, so nothing can be confirmed
  remove_failed, ///< the filesystem refused to delete it
};

/**
 * @brief Run the deletion guard and change nothing.
 *
 * Split from delete_bundle so a caller can ask before it acts: the editor asks
 * to choose between a confirmation and a refusal, and the tool asks before
 * putting a person in front of an approval that could only fail. `_err`, when
 * non-null, receives a sentence for that person, naming the referrers.
 */
delete_status check_bundle_deletable(const std::filesystem::path &_script_dir,
                                     std::string_view _name, std::string *_err);

/**
 * @brief Delete a bundle's whole directory, once the guard above passes.
 *
 * The directory goes, templates and all: a bundle is the unit a person deletes,
 * and a leftover directory with no main.ccat in it is not a skill.
 */
delete_status delete_bundle(const std::filesystem::path &_script_dir,
                            std::string_view _name, std::string *_err);

/// @brief Why a bundle was not renamed, or was renamed but not finished off.
enum class rename_status {
  ok,             ///< the directory moved and every referrer was rewritten
  not_found,      ///< no such bundle
  bad_name,       ///< the new name is unusable, or is the name it already has
  name_taken,     ///< a directory is already called that
  scan_failed,    ///< another bundle would not parse, so not every referrer is known
  rewrite_failed, ///< a referrer could not be rewritten, or was not written
  rename_failed,  ///< the directory move failed
};

/**
 * @brief Run the rename guard and change nothing.
 *
 * `_referrers`, when non-null, receives the scripts that will be rewritten --
 * the blast radius an approval or a confirmation dialog has to show. That the
 * rewrites themselves are tried out here too is deliberate: a person should
 * never approve a rename that cannot go through.
 */
rename_status check_bundle_renamable(const std::filesystem::path &_script_dir,
                                     std::string_view _name,
                                     std::string_view _new_name,
                                     std::vector<std::string> *_referrers,
                                     std::string *_err);

/**
 * @brief Rename a bundle and rewrite every `run()` that reached it.
 *
 * A referrer whose `run()` target is not a literal (it comes through a `defs`
 * name, so there is no string in the source to edit) is refused rather than left
 * pointing at a name that no longer exists. If a referrer cannot be written
 * after the directory has moved, that is reported and the move stands: the
 * bundle is intact, only that script is stale.
 *
 * `_rewritten`, when non-null, receives the scripts actually rewritten.
 */
rename_status rename_bundle(const std::filesystem::path &_script_dir,
                            std::string_view _name, std::string_view _new_name,
                            std::vector<std::string> *_rewritten,
                            std::string *_err);

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
