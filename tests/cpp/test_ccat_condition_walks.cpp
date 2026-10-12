// The two walks a condition reaches outside the parser: `update_script`'s
// required-template check and the bundle dependency scan. Both guard a user's
// files -- one refuses a save that could not run, the other refuses a delete
// that would take a live target with it -- so an operand hidden inside a
// condition is a real loss, not a missing assertion. Neither walker is
// reachable directly (both sit in anonymous namespaces), which is why these
// cases go through the public entry points.

#include <core/llm/llm_tools.h>
#include <core/script/ccat_bundle.h>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using campcat::ccat_lang::bundle_dependencies;
using campcat::ccat_lang::check_bundle_deletable;
using campcat::ccat_lang::delete_status;
using campcat::llm::tool_context;
using campcat::llm::tool_reply;

/// A script root holding the bundles named in `_files`, each as
/// `<name>/main.ccat`. `_case` separates two tests' directories: `ctest` runs
/// each case as its own process, so a shared path would be two processes
/// writing one tree.
std::filesystem::path
write_bundles(std::string_view _case,
              const std::vector<std::pair<std::string, std::string>> &_files) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("campcat_condition_walks_" + std::string(_case));
    std::filesystem::remove_all(dir);
    for (const auto &[name, source] : _files) {
        std::filesystem::create_directories(dir / name);
        std::ofstream out(dir / name / "main.ccat");
        out << source;
    }
    return dir;
}

/// `update_script`'s `arguments` for `_source` with nothing declared: every case
/// here is about the check, so none of them needs a template to exist.
std::string args_for(const std::string &_source) {
    nlohmann::json args;
    args["name"] = "demo";
    args["source"] = _source;
    args["templates"] = nlohmann::json::array();
    return args.dump();
}

// Tools read one process-wide context, so a leaked one changes the next case.
class CcatConditionWalks : public testing::Test {
protected:
    void TearDown() override { campcat::llm::set_context(tool_context{}); }
};

TEST_F(CcatConditionWalks, HasIsNotAnOperandButAConditionActionIs) {
    const std::filesystem::path dir =
        write_bundles("update", {{"demo", "log(\"hi\")\n"}});
    tool_context ctx;
    ctx.script_dir = dir;
    campcat::llm::set_context(ctx);

    // `has(...)` names no operand: "that template is not here" is the `false` it
    // exists to report, so an undeclared one must not block the update.
    const tool_reply has = campcat::llm::dispatch(
        "update_script", args_for("if (has(\"x.png\")) { log(\"y\") }"));
    EXPECT_TRUE(has.ok) << has.error;

    // The same template behind an action is an operand, because the condition
    // really taps: left out, the update would write a script the bundle cannot
    // run.
    const tool_reply tap = campcat::llm::dispatch(
        "update_script", args_for("if (tap(\"x.png\")) { log(\"y\") }"));
    EXPECT_FALSE(tap.ok);
    EXPECT_NE(tap.error.find("x.png"), std::string::npos) << tap.error;

    std::filesystem::remove_all(dir);
}

TEST_F(CcatConditionWalks, ADoWhileConditionActionIsAnOperandToo) {
    const std::filesystem::path dir =
        write_bundles("while", {{"demo", "log(\"hi\")\n"}});
    tool_context ctx;
    ctx.script_dir = dir;
    campcat::llm::set_context(ctx);

    const tool_reply r = campcat::llm::dispatch(
        "update_script",
        args_for("do { log(\"y\") } while (tap(\"x.png\"))"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("x.png"), std::string::npos) << r.error;

    std::filesystem::remove_all(dir);
}

TEST_F(CcatConditionWalks, ARunInAConditionIsARealDependency) {
    // A `run` in the condition is an edge like any other, in both spellings
    // (`if` and the `do`/`while` that is a second copy of the same walk).
    // Unseen, deleting the bundle it names would take that file with it and
    // leave the caller referring to nothing.
    //
    // `../callee/` rather than `callee/`: a bundle's own directory is the base
    // both the scan and the interpreter resolve against, so a sibling bundle is
    // one level up.
    const std::filesystem::path dir = write_bundles(
        "deps",
        {{"ifcaller", "if (run(\"../callee/main.ccat\")) { log(\"y\") }\n"},
         {"whilecaller",
          "do { log(\"y\") } while (run(\"../callee/main.ccat\"))\n"},
         {"callee", "log(\"callee\")\n"}});

    EXPECT_EQ(bundle_dependencies(dir, "ifcaller"),
              std::vector<std::string>{"callee"});
    EXPECT_EQ(bundle_dependencies(dir, "whilecaller"),
              std::vector<std::string>{"callee"});

    std::string err;
    EXPECT_EQ(check_bundle_deletable(dir, "callee", &err),
              delete_status::referenced);
    EXPECT_NE(err.find("ifcaller"), std::string::npos) << err;
    EXPECT_NE(err.find("whilecaller"), std::string::npos) << err;

    std::filesystem::remove_all(dir);
}

} // namespace
