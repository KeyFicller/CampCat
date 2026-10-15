// The provider rides with the request, so the two things worth pinning are that
// the field is written at all, and that the host's setting survives a save and
// a reload.

#include <core/app_config.h>
#include <core/llm/llm_fuse.h>
#include <core/llm/llm_host.h>
#include <core/llm/llm_protocol.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace {

using campcat::llm_protocol::build_interrupt_request;
using campcat::llm_protocol::build_memorize_request;
using campcat::llm_protocol::build_turn_request;
using campcat::llm_protocol::parse_line;

nlohmann::json turn_with(const std::string &_provider) {
    return nlohmann::json::parse(build_turn_request(
        1, "hi", "", "notes", _provider, campcat::llm::k_default_max_tool_rounds));
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

TEST(LlmProtocol, InterruptNamesTheTurn) {
    const nlohmann::json j = nlohmann::json::parse(build_interrupt_request(4));

    EXPECT_EQ(j["type"], "interrupt");
    EXPECT_EQ(j["id"], 4);
}

TEST(LlmProtocol, ResultInterruptedDefaultsFalse) {
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(parse_line(
        R"({"type":"result","id":1,"ok":true,"text":"hi"})", &msg));

    EXPECT_TRUE(msg.ok);
    EXPECT_FALSE(msg.interrupted);
}

TEST(LlmProtocol, ResultInterruptedTrueIsRead) {
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(parse_line(
        R"({"type":"result","id":1,"ok":true,"text":"hi","interrupted":true})",
        &msg));

    EXPECT_TRUE(msg.interrupted);
    EXPECT_EQ(msg.text, "hi");
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

TEST(LlmFuse, UnlimitedRendersAsInfinity) {
    EXPECT_EQ(campcat::llm::format_fuse(100000), "\xE2\x88\x9E");
}

TEST(LlmFuse, AFiniteCapRendersAsTheNumber) {
    EXPECT_EQ(campcat::llm::format_fuse(20), "20");
}

TEST(LlmFuse, PairRequiresRoundsWithinCalls) {
    EXPECT_TRUE(campcat::llm::fuse_pair_ok(20, 20));
    EXPECT_FALSE(campcat::llm::fuse_pair_ok(20, 21));
    EXPECT_FALSE(campcat::llm::fuse_pair_ok(0, 1));
}

TEST(LlmProtocol, MissingUsageFieldsDoNotZero) {
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(parse_line(
        R"({"type":"result","id":1,"ok":true,"text":"hi"})", &msg));
    EXPECT_FALSE(msg.has_llm_calls);
    EXPECT_FALSE(msg.has_tool_calls);
    EXPECT_FALSE(msg.has_context);

    campcat::llm_protocol::usage_stats stats;
    stats.llm_calls = 7;
    stats.tool_calls = 99;
    stats.context_tokens = 5;
    campcat::llm_protocol::apply_usage(&stats, msg);
    EXPECT_EQ(stats.llm_calls, 7);
    EXPECT_EQ(stats.tool_calls, 99);
    EXPECT_EQ(stats.context_tokens, 5);
}

TEST(LlmProtocol, ToolCallCarriesUsage) {
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(parse_line(
        R"({"type":"tool_call","id":1,"call_id":"c","name":"tap","args":{},)"
        R"("llm_calls":3,"tool_calls":4,"context":8})",
        &msg));
    campcat::llm_protocol::usage_stats stats;
    campcat::llm_protocol::apply_usage(&stats, msg);
    EXPECT_EQ(stats.llm_calls, 3);
    EXPECT_EQ(stats.tool_calls, 4);
    EXPECT_EQ(stats.context_tokens, 8);
}

TEST(LlmProtocol, TurnCarriesTheRoundCap) {
    const nlohmann::json j =
        nlohmann::json::parse(build_turn_request(1, "hi", "", "notes", "qwen", 20));
    EXPECT_EQ(j["max_tool_rounds"], 20);
}

TEST(AppConfig, MissingFuseFieldsKeepTheDefaults) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "campcat_fuse_cfg_test.json";
    {
        std::ofstream out(path);
        out << R"({"llm":{"provider":"qwen"}})";
    }
    const campcat::app_config cfg = campcat::app_config::load_from_file(path);
    std::filesystem::remove(path);
    EXPECT_EQ(cfg.llm.provider, "qwen");
    EXPECT_EQ(cfg.llm.max_tool_calls, 1000000);
    EXPECT_EQ(cfg.llm.max_tool_rounds, 100000);
}

TEST(LlmHost, ResetClearsUsage) {
    campcat::llm_host host;
    const campcat::llm_result r =
        host.reset(std::filesystem::temp_directory_path());
    EXPECT_TRUE(r.ok);
    const campcat::llm_protocol::usage_stats u = host.usage();
    EXPECT_EQ(u.llm_calls, 0);
    EXPECT_EQ(u.tool_calls, 0);
    EXPECT_EQ(u.context_tokens, -1);
}

} // namespace
