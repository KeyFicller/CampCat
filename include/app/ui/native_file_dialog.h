#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace campcat {

/**
 * @brief Native open-file dialog filtered to one extension (e.g. "ccat").
 * @return Absolute path, or nullopt if cancelled / unavailable.
 */
std::optional<std::filesystem::path>
pick_open_file(const std::filesystem::path &_start_dir,
               const std::string &_extension, const std::string &_title);

} // namespace campcat
