#include "core/app_config.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <llm/reflection_json.h>
#include <nlohmann/json.hpp>

#include "core/file_io.h"

namespace campcat {

app_config app_config::defaults() { return app_config{}; }

app_config app_config::load_from_file(const std::filesystem::path &_path) {
    app_config c{};
    std::ifstream in(_path);
    nlohmann::json j;
    try {
        in >> j; // a missing file throws here too: "no config yet" is not an
                 // error
        c = js::from_json<app_config>(j);
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "[cfg] %s: %s; using defaults\n",
                     _path.string().c_str(), ex.what());
    }
    // No reflection can know where the file lives, so the loader always sets it
    // -- including over the (necessarily stale) value the file itself carries.
    //
    // The library is the sibling `ccat_library/`, not the config file's own
    // directory: the config sits at the repo root where a human can find it,
    // while everything it anchors (bundles, the language reference, memory,
    // match dumps) stays in one directory. That also keeps
    // `config_home.parent_path()` equal to the repo root, which is what the
    // sidecar's launch path needs.
    c.config_home =
        (std::filesystem::absolute(_path).parent_path() / k_library_dir_name)
            .string();
    return c;
}

void app_config::save_to_file(const std::filesystem::path &_path) const {
    write_file_atomic(_path,
                      nlohmann::json::parse(js::to_json(*this)).dump(2) + "\n");
}

} // namespace campcat
