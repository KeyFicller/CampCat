#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace campcat {

/**
 * @brief Where the Script page's `.ccat` lives, derived from the selected
 * bundle name.
 *
 * Nothing here is persisted: the choice itself is `app_config::script_bundle`,
 * so there is one field that can be wrong rather than two that can disagree.
 *
 * PNG operands always resolve beside the `.ccat` file, which is the bundle
 * directory itself.
 */
struct ccat_script_profile {
    /**
     * @brief Relative path (from config_home) pointing at the `.ccat` source
     * loaded each automation tick. Empty = nothing selected.
     */
    std::string source_rel;

    /**
     * @brief `config_home / source_rel`, normalised.
     */
    std::filesystem::path script_path_absolute;

    /**
     * @brief Directory of the resolved `.ccat` file (PNG search root).
     * @param[in] _config_home Shell persistence directory anchoring the path.
     * @return Parent directory of the script path; empty when `source_rel` is
     * empty.
     */
    std::filesystem::path
    images_base(const std::filesystem::path &_config_home) const;

    /**
     * @brief Directory a console snippet resolves relative paths against.
     *
     * The bundle directory when one is selected. With nothing selected this is
     * `_config_home` (the library root); `images_base` stays empty then, so the
     * Script page can still tell that no bundle is chosen.
     */
    std::filesystem::path
    console_root(const std::filesystem::path &_config_home) const;

    /**
     * @brief True when a bundle is selected.
     */
    bool has_script_source() const;

    /**
     * @brief Derive the profile from a bundle name.
     *
     * Pure path arithmetic: a name that no longer exists on disk is not an
     * error here, it is the caller's problem when it goes looking for the file.
     *
     * @param[in] _config_home Root the bundle lives under.
     * @param[in] _bundle      Directory name under `_config_home`; empty means
     * nothing selected.
     */
    static ccat_script_profile
    load(const std::filesystem::path &_config_home, std::string_view _bundle);
};

} // namespace campcat
