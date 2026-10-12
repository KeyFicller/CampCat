// The provider rides with the request, so the two things worth pinning are that
// the field is written at all, and that the host's setting survives a save and
// a reload.

#include <core/app_config.h>
#include <core/llm/llm_protocol.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace {

using campcat::llm_protocol::build_memorize_request;
using campcat::llm_protocol::build_turn_request;

nlohmann::json turn_with(const std::string &_provider) {
    return nlohmann::json::parse(
        build_turn_request(1, "hi", "", "notes", _provider));
}

TEST(LlmProtocol, TurnCarriesTheSelectedProvider) {
    const nlohmann::json j = turn_with("qwen");

    EXPECT_EQ(j["type"], "turn");
    EXPECT_EQ(j["provider"], "qwen");
    // The field it rides next to still lands, and next to it.
    EXPECT_EQ(j["memory"], "notes");
    EXPECT_EQ(j["text"], "hi");
}

TEST(LlmProtocol, ProviderIsPresentEvenWhenEmpty) {
    // The shape must not depend on the value: the sidecar reads the field
    // rather than testing for it, the same way it treats `memory`.
    const nlohmann::json j = turn_with("");

    ASSERT_TRUE(j.contains("provider"));
    EXPECT_EQ(j["provider"], "");
}

TEST(LlmProtocol, MemorizeCarriesTheSelectedProvider) {
    // Consolidation builds a model too, so a Qwen-only setup fails here unless
    // the provider comes along.
    const nlohmann::json j =
        nlohmann::json::parse(build_memorize_request(2, "notes", "qwen"));

    EXPECT_EQ(j["type"], "memorize");
    EXPECT_EQ(j["provider"], "qwen");
    EXPECT_EQ(j["memory"], "notes");
}

TEST(AppConfig, ProviderRoundTripsThroughTheFile) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "campcat_provider_test.json";
    {
        std::ofstream out(path);
        out << R"({"llm":{"provider":"qwen"}})";
    }

    const campcat::app_config cfg = campcat::app_config::load_from_file(path);
    std::filesystem::remove(path);

    EXPECT_EQ(cfg.llm.provider, "qwen");
}

TEST(AppConfig, AConfigWithoutTheBlockStillLoads) {
    // Every config written before this feature exists: the field's default has
    // to come through, or the app loses its provider on first run.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "campcat_old_cfg_test.json";
    {
        std::ofstream out(path);
        out << R"({"input":{"tap_delay_ms":200}})";
    }

    const campcat::app_config cfg = campcat::app_config::load_from_file(path);
    std::filesystem::remove(path);

    EXPECT_EQ(cfg.llm.provider, "deepseek");
}

TEST(AppConfig, ConfigHomeIsTheLibraryBesideTheConfigFile) {
    // Not the config file's own directory: everything config_home anchors
    // (bundles, memory, match dumps) belongs in one place, and the sidecar
    // launch needs `config_home.parent_path()` to still be the repo root.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "campcat_home_test.json";
    {
        std::ofstream out(path);
        out << R"({"input":{"tap_delay_ms":200}})";
    }

    const campcat::app_config cfg = campcat::app_config::load_from_file(path);
    std::filesystem::remove(path);

    EXPECT_EQ(std::filesystem::path(cfg.config_home),
              std::filesystem::temp_directory_path() / "ccat_library");
}

} // namespace
