// The provider rides with the request, so the two things worth pinning are that
// the field is written at all, and that the host's setting survives a save and
// a reload.

#include <core/app_config.h>
#include <core/llm/llm_fuse.h>
#include <core/llm/llm_host.h>
#include <core/llm/llm_protocol.h>
#include <core/llm/llm_tools.h>

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
    return nlohmann::json::parse(
        build_turn_request(1, "hi", "", "notes", "", _provider,
                           campcat::llm::k_default_max_model_rounds));
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
    ASSERT_TRUE(
        parse_line(R"({"type":"result","id":1,"ok":true,"text":"hi"})", &msg));

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
    const nlohmann::json j = nlohmann::json::parse(
        build_memorize_request(2, "notes", "qwen", "operator"));

    EXPECT_EQ(j["type"], "memorize");
    EXPECT_EQ(j["provider"], "qwen");
    EXPECT_EQ(j["memory"], "notes");
}

TEST(LlmProtocol, SubagentCarriesAgentAndEvent) {
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(parse_line(
        R"({"type":"subagent","id":3,"agent":"scripter","event":"start",
            "text":"add an action"})",
        &msg));

    EXPECT_EQ(msg.type, campcat::llm_protocol::message::kind::subagent);
    EXPECT_EQ(msg.id, 3);
    EXPECT_EQ(msg.agent, "scripter");
    EXPECT_EQ(msg.subagent_event, "start");
    EXPECT_EQ(msg.text, "add an action");
    // The window shows a tool as fine unless the line itself says otherwise.
    EXPECT_TRUE(msg.ok);
}

TEST(LlmProtocol, SubagentToolEndCarriesTheFailure) {
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(parse_line(
        R"({"type":"subagent","id":3,"agent":"scripter","event":"tool_end",
            "ok":false,"text":"no such bundle"})",
        &msg));

    EXPECT_EQ(msg.subagent_event, "tool_end");
    EXPECT_FALSE(msg.ok);
    EXPECT_EQ(msg.text, "no such bundle");
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

    EXPECT_EQ(cfg.llm.provider, "qwen");
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

TEST(LlmTools, CcatHelpReadsTheDocBesideTheLibrary) {
    // The language reference belongs to the interpreter, not to a skill pack:
    // it sits beside config.json, one level above config_home. Asking the tool
    // is the only way to pin where it looks, since a miss is a thrown error.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "campcat_ccat_help";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "ccat_library");
    {
        std::ofstream out(root / "CCAT.md");
        out << "the language\n";
    }

    campcat::app_config cfg{};
    cfg.config_home = (root / "ccat_library").string();
    campcat::llm::tool_context ctx;
    ctx.cfg = &cfg;
    campcat::llm::set_context(ctx);
    const campcat::llm::tool_reply reply =
        campcat::llm::dispatch("ccat_help", "{}");
    campcat::llm::set_context({});
    std::filesystem::remove_all(root);

    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_EQ(reply.text, "the language\n");
}

TEST(AppConfig, ThePopupSettingsSurviveASave) {
    // What the settings popup changes -- provider, the approval gates, the
    // snap -- used to live only in memory until the file was saved by hand.
    // The exit save leans on this being a faithful round trip.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "campcat_popup_cfg.json";
    {
        std::ofstream out(path);
        out << R"({"input":{"tap_delay_ms":200}})";
    }
    campcat::app_config cfg = campcat::app_config::load_from_file(path);
    cfg.llm.provider = "qwen";
    cfg.approval.require = false;
    cfg.approval.gates.tap = false;
    cfg.box_snap.strategy = campcat::box_snap_strategy::step;
    cfg.region_score.record = true;
    cfg.save_to_file(path);

    const campcat::app_config again = campcat::app_config::load_from_file(path);
    std::filesystem::remove(path);

    EXPECT_EQ(again.llm.provider, "qwen");
    EXPECT_FALSE(again.approval.require);
    EXPECT_FALSE(again.approval.gates.tap);
    EXPECT_TRUE(again.approval.gates.save_template); // untouched gates stay on
    EXPECT_EQ(again.box_snap.strategy, campcat::box_snap_strategy::step);
    EXPECT_TRUE(again.region_score.record);
    EXPECT_EQ(again.input.tap_delay_ms, 200); // and the rest is not lost
}

TEST(AppConfig, ABareFilenameSaves) {
    // `config.json` is passed as a bare relative name unless argv says
    // otherwise, so its parent path is empty. A create_directories("") in the
    // shell's save threw here and lost the whole write.
    const std::filesystem::path bare = "campcat_bare_cfg_test.json";
    campcat::app_config cfg = campcat::app_config::defaults();
    cfg.llm.provider = "qwen";
    cfg.region_score.record = true;
    cfg.save_to_file(bare);
    const campcat::app_config again = campcat::app_config::load_from_file(bare);
    std::filesystem::remove(bare);

    EXPECT_EQ(again.llm.provider, "qwen");
    EXPECT_TRUE(again.region_score.record);
}

TEST(LlmFuse, UnlimitedRendersAsInfinity) {    EXPECT_EQ(campcat::llm::format_fuse(100000), "\xE2\x88\x9E");
}

TEST(LlmFuse, AFiniteCapRendersAsTheNumber) {
    EXPECT_EQ(campcat::llm::format_fuse(20), "20");
}

TEST(LlmFuse, PairRequiresRoundsWithinCalls) {
    EXPECT_TRUE(campcat::llm::fuse_pair_ok(20, 20));
    EXPECT_FALSE(campcat::llm::fuse_pair_ok(20, 21));
    EXPECT_FALSE(campcat::llm::fuse_pair_ok(0, 1));
}

TEST(LlmFuse, AlarmFiresOnAViewCropStreakOrHalfTheCap) {
    using campcat::llm::dispatch_alarm;
    EXPECT_TRUE(dispatch_alarm(1, 10, 2).empty());
    const std::string crops = dispatch_alarm(1, 10, 3);
    EXPECT_NE(crops.find("view_crop"), std::string::npos);
    EXPECT_EQ(crops.find("half the limit"), std::string::npos);

    const std::string half = dispatch_alarm(5, 10, 0);
    EXPECT_NE(half.find("5/10"), std::string::npos);
    EXPECT_EQ(half.find("view_crop"), std::string::npos);
    EXPECT_TRUE(dispatch_alarm(4, 10, 0).empty());
    EXPECT_TRUE(dispatch_alarm(6, 10, 0).empty());

    const std::string both = dispatch_alarm(5, 10, 3);
    EXPECT_NE(both.find("view_crop"), std::string::npos);
    EXPECT_NE(both.find("5/10"), std::string::npos);

    // An unlimited cap never reaches a half, and a cap of 1 has no half above
    // 0.
    EXPECT_TRUE(
        dispatch_alarm(50000, campcat::llm::k_fuse_unlimited, 0).empty());
    EXPECT_TRUE(dispatch_alarm(1, 1, 0).empty());
    EXPECT_NE(
        dispatch_alarm(1, campcat::llm::k_fuse_unlimited, 3).find("view_crop"),
        std::string::npos);
}

TEST(LlmProtocol, ANoteCarriesItsLine) {
    // The sidecar's self-check score rides here: a tool result only exists
    // after the approval gate is answered, so the score has to arrive first.
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(parse_line(R"({"type":"note","text":"self-check 0.42"})", &msg));
    EXPECT_EQ(msg.type, campcat::llm_protocol::message::kind::note);
    EXPECT_EQ(msg.text, "self-check 0.42");
}

TEST(LlmProtocol, MissingUsageFieldsDoNotZero) {
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(
        parse_line(R"({"type":"result","id":1,"ok":true,"text":"hi"})", &msg));
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
    const nlohmann::json j = nlohmann::json::parse(
        build_turn_request(1, "hi", "", "notes", "", "qwen", 20));
    EXPECT_EQ(j["max_model_rounds"], 20);
}

TEST(LlmProtocol, TurnCarriesScripterMemoryEvenWhenEmpty) {
    const nlohmann::json j = nlohmann::json::parse(
        build_turn_request(1, "hi", "", "notes", "", "qwen", 20));
    EXPECT_EQ(j["memory"], "notes");
    ASSERT_TRUE(j.contains("scripter_memory"));
    EXPECT_EQ(j["scripter_memory"], "");
    EXPECT_EQ(j["max_model_rounds"], 20);
}

TEST(LlmProtocol, OperatorOnlyIsOmittedUnlessAskedFor) {
    // A sidecar that predates the field reads a missing key as "handoff on",
    // so sending `false` and omitting it must be the same message shape.
    const nlohmann::json j = nlohmann::json::parse(
        build_turn_request(1, "hi", "", "notes", "", "qwen", 20));
    EXPECT_FALSE(j.contains("operator_only"));
}

TEST(LlmProtocol, OperatorOnlyWithholdsTheHandoff) {
    const nlohmann::json j = nlohmann::json::parse(
        build_turn_request(1, "hi", "", "notes", "", "qwen", 20, true));
    EXPECT_EQ(j["operator_only"], true);
}

TEST(LlmProtocol, MemorizeNamesTheAgent) {
    const nlohmann::json j = nlohmann::json::parse(
        build_memorize_request(2, "notes", "qwen", "scripter"));
    EXPECT_EQ(j["type"], "memorize");
    EXPECT_EQ(j["agent"], "scripter");
    EXPECT_EQ(j["memory"], "notes");
}

TEST(LlmProtocol, ConsultCarriesTheQuestionAndThePicture) {
    // The id is the consult's own: the script that asks may have been started
    // from the Run button, with no turn in flight.
    const nlohmann::json j = nlohmann::json::parse(
        campcat::llm_protocol::build_consult(9, "qwen", "is it?", "QUJD"));

    EXPECT_EQ(j["type"], "consult");
    EXPECT_EQ(j["id"], 9);
    EXPECT_EQ(j["provider"], "qwen");
    EXPECT_EQ(j["question"], "is it?");
    EXPECT_EQ(j["image_b64"], "QUJD");
}

TEST(LlmProtocol, ConsultResultCarriesYesOrNo) {
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(parse_line(
        campcat::llm_protocol::build_consult_result(9, true, "no", ""), &msg));

    EXPECT_EQ(msg.type, campcat::llm_protocol::message::kind::consult_result);
    EXPECT_EQ(msg.id, 9);
    EXPECT_TRUE(msg.ok);
    EXPECT_EQ(msg.answer, "no");
}

TEST(LlmProtocol, ConsultResultCarriesTheFailureAndAnEmptyAnswer) {
    campcat::llm_protocol::message msg;
    ASSERT_TRUE(parse_line(campcat::llm_protocol::build_consult_result(
                               3, false, "", "no model"),
                           &msg));

    EXPECT_EQ(msg.type, campcat::llm_protocol::message::kind::consult_result);
    EXPECT_FALSE(msg.ok);
    EXPECT_EQ(msg.answer, "");
    EXPECT_EQ(msg.error, "no model");
}

TEST(LlmMemory, TheTwoFilesStayApart) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "campcat_memory_apart";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::string missing;
    ASSERT_TRUE(campcat::llm::read_memory(
        dir, campcat::llm::memory_kind::scripter_notes, &missing));
    EXPECT_TRUE(missing.empty());
    EXPECT_TRUE(campcat::llm::write_memory(
        dir, campcat::llm::memory_kind::operator_notes, "keep"));
    EXPECT_TRUE(campcat::llm::write_memory(
        dir, campcat::llm::memory_kind::scripter_notes, "pit"));
    EXPECT_FALSE(campcat::llm::write_memory(
        dir, campcat::llm::memory_kind::scripter_notes, ""));
    EXPECT_FALSE(campcat::llm::write_memory(
        dir, campcat::llm::memory_kind::scripter_notes,
        std::string(4001, 'x')));
    std::string op;
    ASSERT_TRUE(campcat::llm::read_memory(
        dir, campcat::llm::memory_kind::operator_notes, &op));
    EXPECT_EQ(op, "keep");
    EXPECT_TRUE(campcat::llm::clear_memory(
        dir, campcat::llm::memory_kind::scripter_notes));
    ASSERT_TRUE(campcat::llm::read_memory(
        dir, campcat::llm::memory_kind::operator_notes, &op));
    EXPECT_EQ(op, "keep");
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, LegacyMemoryFileIsRenamedOnce) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "campcat_memory_rename";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "memory.md");
        out << "keep";
    }
    std::string op;
    ASSERT_TRUE(campcat::llm::read_memory(
        dir, campcat::llm::memory_kind::operator_notes, &op));
    EXPECT_EQ(op, "keep");
    EXPECT_TRUE(std::filesystem::exists(dir / "operator.md"));
    EXPECT_FALSE(std::filesystem::exists(dir / "memory.md"));
    {
        std::ofstream out(dir / "memory.md");
        out << "stale";
    }
    ASSERT_TRUE(campcat::llm::read_memory(
        dir, campcat::llm::memory_kind::operator_notes, &op));
    EXPECT_EQ(op, "keep");
    EXPECT_TRUE(std::filesystem::exists(dir / "memory.md"));
    std::filesystem::remove_all(dir);
}

TEST(LlmMemory, AnUnreadableFileIsNotReplaced) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "campcat_memory_unread";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    EXPECT_TRUE(campcat::llm::write_memory(
        dir, campcat::llm::memory_kind::operator_notes, "keep"));
    std::filesystem::create_directory(campcat::llm::memory_path(
        dir, campcat::llm::memory_kind::scripter_notes));
    EXPECT_FALSE(campcat::llm::write_memory(
        dir, campcat::llm::memory_kind::scripter_notes, "new"));
    std::string op;
    ASSERT_TRUE(campcat::llm::read_memory(
        dir, campcat::llm::memory_kind::operator_notes, &op));
    EXPECT_EQ(op, "keep");
    std::filesystem::remove_all(dir);
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
    EXPECT_EQ(cfg.llm.max_dispatches, campcat::llm::k_default_max_dispatches);
    EXPECT_EQ(cfg.llm.max_model_rounds,
              campcat::llm::k_default_max_model_rounds);
}

TEST(AppConfig, MissingGoHomeGateStaysOn) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "campcat_gohome_cfg_test.json";
    {
        std::ofstream out(path);
        out << R"({"approval":{"require":true}})";
    }
    const campcat::app_config cfg = campcat::app_config::load_from_file(path);
    std::filesystem::remove(path);
    EXPECT_TRUE(cfg.approval.require);
    EXPECT_TRUE(cfg.approval.gates.go_home);
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
