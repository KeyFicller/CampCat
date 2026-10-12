#include "ccat_driver/ccat_script_profile.h"

#include <cctype>
#include <string_view>
#include <system_error>

namespace campcat {

namespace {

std::string trim_copy(std::string_view _s) {
    size_t b = 0;
    while (b < _s.size() && std::isspace(static_cast<unsigned char>(_s[b]))) {
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

std::filesystem::path ccat_script_profile::images_base(
    const std::filesystem::path &_config_home) const {
    if (!has_script_source()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path script_path =
        _config_home / trim_copy(source_rel);
    const std::filesystem::path canon =
        std::filesystem::weakly_canonical(script_path, ec);
    const std::filesystem::path resolved =
        ec ? script_path.lexically_normal() : canon;
    return resolved.parent_path();
}

ccat_script_profile
ccat_script_profile::load(const std::filesystem::path &_config_home,
                          std::string_view _bundle) {
    ccat_script_profile p;
    if (_bundle.empty()) {
        return p;
    }
    p.source_rel =
        (std::filesystem::path(_bundle) / "main.ccat").generic_string();
    p.script_path_absolute = (_config_home / p.source_rel).lexically_normal();
    return p;
}

} // namespace campcat
