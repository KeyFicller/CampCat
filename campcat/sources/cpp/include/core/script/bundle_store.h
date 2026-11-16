#pragma once
#include <filesystem>
#include <functional>
#include <string_view>

namespace campcat::ccat_lang {
/// Stages a whole generation before touching the live directory. The previous
/// generation remains recoverable if the process dies between the two renames.
void publish_bundle(const std::filesystem::path &_root, std::string_view _name,
                    const std::function<void(const std::filesystem::path &)> &_write);
void recover_bundle(const std::filesystem::path &_root, std::string_view _name);
void recover_bundles(const std::filesystem::path &_root);
}
