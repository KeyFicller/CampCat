// The gate decides what the model may do without asking, so the truth table is
// worth pinning: it is what stands between an unapproved tap and the device.

#include <core/app_config.h>
#include <core/llm/llm_tools.h>

#include <gtest/gtest.h>

#include <string_view>

namespace {

using campcat::approval_config;
using campcat::approval_gates;
using campcat::llm::approval_gate_required;

struct gate_case {
    std::string_view tool;
    bool approval_gates::*flag;
};

const gate_case k_cases[] = {
    {"tap", &approval_gates::tap},
    {"run_script", &approval_gates::run_script},
    {"save_script", &approval_gates::save_script},
    {"save_template", &approval_gates::save_template},
    {"update_script", &approval_gates::update_script},
    {"delete_script", &approval_gates::delete_script},
    {"rename_script", &approval_gates::rename_script},
};

approval_config all_on() {
    approval_config c;
    c.require = true;
    return c;
}

TEST(ApprovalGate, MasterOffAsksForNothing) {
    const approval_config c{}; // require defaults to false
    for (const auto &[tool, flag] : k_cases) {
        (void)flag;
        EXPECT_FALSE(approval_gate_required(c, tool)) << tool;
    }
    EXPECT_FALSE(approval_gate_required(c, "something_new")) << "unknown tool";
}

TEST(ApprovalGate, EverythingOnAsksForEverything) {
    const approval_config c = all_on();
    for (const auto &[tool, flag] : k_cases) {
        (void)flag;
        EXPECT_TRUE(approval_gate_required(c, tool)) << tool;
    }
    EXPECT_TRUE(approval_gate_required(c, "something_new")) << "unknown tool";
}

TEST(ApprovalGate, EachGateFollowsItsOwnFlag) {
    for (auto &c : k_cases) {
        approval_config cfg = all_on();
        cfg.gates.*(c.flag) = false;
        for (const auto &[tool, flag] : k_cases) {
            EXPECT_EQ(approval_gate_required(cfg, tool), flag != c.flag)
                << tool << " with " << c.tool << " off";
        }
    }
}

} // namespace
