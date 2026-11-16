#include "core/script/bundle_store.h"
#include "core/file_io.h"
#include "core/script/ccat_bundle.h"
#include <mutex>
#include <stdexcept>

namespace campcat::ccat_lang {
namespace {
auto &store_mutex = library_io_mutex();
auto transaction_root(const std::filesystem::path &root) { return root / ".campcat-transactions"; }
void check(std::string_view name) {
    if (!safe_leaf_name(name) || name.front() == '.')
        throw std::runtime_error("invalid bundle name");
}
void recover(const std::filesystem::path &root, std::string_view name) {
    const auto live = root / name;
    const auto previous = transaction_root(root) / (std::string(name) + ".previous");
    if (!std::filesystem::exists(previous)) return;
    if (!std::filesystem::exists(live)) std::filesystem::rename(previous, live);
    else std::filesystem::remove_all(previous);
}
}
void recover_bundle(const std::filesystem::path &root, std::string_view name) {
    std::lock_guard lock(store_mutex); check(name); recover(root, name);
}
void recover_bundles(const std::filesystem::path &root) {
    std::lock_guard lock(store_mutex);
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(transaction_root(root), ec)) {
        if (e.path().extension() == ".previous") {
            const auto name = e.path().stem().string();
            check(name); recover(root, name);
        }
    }
}
void publish_bundle(const std::filesystem::path &root, std::string_view name,
                    const std::function<void(const std::filesystem::path &)> &write) {
    std::lock_guard lock(store_mutex); check(name); recover(root, name);
    const auto live = root / name;
    const auto stage = transaction_root(root) / (std::string(name) + ".staging");
    const auto previous = transaction_root(root) / (std::string(name) + ".previous");
    std::filesystem::create_directories(stage.parent_path());
    std::filesystem::remove_all(stage);
    try {
        if (std::filesystem::exists(live))
            std::filesystem::copy(live, stage, std::filesystem::copy_options::recursive);
        else std::filesystem::create_directories(stage);
        write(stage);
        if (!std::filesystem::is_regular_file(stage / "main.ccat"))
            throw std::runtime_error("candidate bundle has no main.ccat");
        if (std::filesystem::exists(live)) std::filesystem::rename(live, previous);
        std::filesystem::rename(stage, live);
    } catch (...) {
        std::error_code ignored;
        if (!std::filesystem::exists(live) && std::filesystem::exists(previous))
            std::filesystem::rename(previous, live, ignored);
        std::filesystem::remove_all(stage, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(previous, ignored);
}
}
