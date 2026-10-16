#include <core/adb_client.h>

#include <gtest/gtest.h>

namespace {

// MuMu's recents header. The uid prefix is why force-stop used to miss.
const char kMumuDump[] =
    "  * Recent #0: Task{81ad122 #6 type=standard A=10031:com.android.gallery3d "
    "U=0 visible=true sz=1}\n"
    "  * Recent #1: Task{5f40fa2 #2 type=home "
    "I=app.lawnchair/.LawnchairLauncher U=0 sz=1}\n";

TEST(RecentsPackages, StripsUidPrefixAndSkipsLauncher) {
    const auto pkgs = campcat::packages_from_recents_dump(kMumuDump);
    ASSERT_EQ(pkgs.size(), 1u);
    EXPECT_EQ(pkgs[0], "com.android.gallery3d");
}

TEST(RecentsPackages, KeepsBarePackageAndStripsActivity) {
    const char dump[] =
        "  * Recent #0: Task{a #1 type=standard A=com.example.app U=0 sz=1}\n"
        "  * Recent #1: Task{b #2 type=standard A=10031:com.example.app/.Main "
        "U=0 sz=1}\n";
    const auto pkgs = campcat::packages_from_recents_dump(dump);
    ASSERT_EQ(pkgs.size(), 1u);
    EXPECT_EQ(pkgs[0], "com.example.app");
}

} // namespace
