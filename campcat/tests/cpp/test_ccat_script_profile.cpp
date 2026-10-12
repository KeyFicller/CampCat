// The Script page's profile is derived, not stored: the bundle name in
// `config.json` is the only truth, and everything else is path arithmetic off
// it. These cases pin that arithmetic. `images_base` is where the automation
// looks for template PNGs, so getting it wrong shows up as a run that matches
// nothing rather than as an error.

#include <ccat_driver/ccat_script_profile.h>

#include <gtest/gtest.h>

#include <string>

namespace {

using campcat::ccat_script_profile;

TEST(CcatScriptProfile, ABundleNameBecomesItsMainCcat) {
    const ccat_script_profile p = ccat_script_profile::load("/cfg", "demo");
    EXPECT_EQ(p.source_rel, "demo/main.ccat");
    EXPECT_TRUE(p.has_script_source());
    EXPECT_EQ(p.script_path_absolute.string(), "/cfg/demo/main.ccat");
    EXPECT_EQ(p.images_base("/cfg").string(), "/cfg/demo");
}

TEST(CcatScriptProfile, NoBundleIsNothingSelected) {
    const ccat_script_profile p = ccat_script_profile::load("/cfg", "");
    EXPECT_FALSE(p.has_script_source());
    EXPECT_TRUE(p.script_path_absolute.empty());
    EXPECT_TRUE(p.images_base("/cfg").empty());
}

TEST(CcatScriptProfile, ANameThatIsNotOnDiskIsNotAnError) {
    const ccat_script_profile p =
        ccat_script_profile::load("/no-such-root", "ghost");
    EXPECT_EQ(p.images_base("/no-such-root").string(), "/no-such-root/ghost");
}

} // namespace
