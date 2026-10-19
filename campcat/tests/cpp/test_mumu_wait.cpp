// The launch's last step: a device that is already up can sit behind an adb
// transport adb itself calls `offline`, and `adb connect` alone does not revive
// it. These cases drive the poll with no emulator.

#include <core/mumu_launch.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

class DeadTransportAdb final : public campcat::adb_api {
public:
    std::vector<std::string> list_devices(int) override { return {}; }
    void set_serial(const std::string &) override {}
    bool run(const std::vector<std::string> &_args, std::string *_out,
             std::string *, int) override {
        if (_args.size() >= 2 && _args.front() == "disconnect") {
            disconnects.push_back(_args.at(1));
            online = false;
            return true;
        }
        if (_args.size() >= 3 && _args.at(0) == "shell" &&
            _args.at(1) == "getprop") {
            if (!online) {
                return false;
            }
            // `bootanim` is absent on this image, which is "done", not "error".
            *_out = _args.at(2) == "sys.boot_completed" ? "1\n" : "";
            return true;
        }
        return false;
    }
    bool connect_remote(std::string_view _address, int, std::string *,
                        std::string *) override {
        connects.emplace_back(_address);
        online = true;
        return true;
    }
    bool tap(int, int) override { return false; }
    bool swipe(int, int, int, int, int) override { return false; }
    bool keyevent(int) override { return false; }
    bool home_and_kill_all(int) override { return false; }
    bool screencap_bgr(cv::Mat *, int, std::string *) override { return false; }
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }

    bool online = false;
    std::vector<std::string> disconnects;
    std::vector<std::string> connects;
};

} // namespace

TEST(MumuWait, ADeadTransportIsDroppedAndReadded) {
    DeadTransportAdb adb;
    // The transport is listed but dead, exactly as adb leaves it.
    adb.online = false;

    std::string why;
    const bool ready = campcat::wait_device_ready(
        adb, 6000, [] { return false; }, "127.0.0.1:16384", &why);

    EXPECT_TRUE(ready) << why;
    ASSERT_EQ(adb.disconnects.size(), 1u);
    EXPECT_EQ(adb.disconnects.at(0), "127.0.0.1:16384");
    ASSERT_EQ(adb.connects.size(), 1u);
    EXPECT_EQ(adb.connects.at(0), "127.0.0.1:16384");
}

TEST(MumuWait, NoAddressMeansNoReconnect) {
    // A serial with no port named by mumutool has no transport to re-add, and
    // guessing one would point adb at another device.
    DeadTransportAdb adb;

    std::string why;
    const bool ready =
        campcat::wait_device_ready(adb, 1000, [] { return false; }, "", &why);

    EXPECT_FALSE(ready);
    EXPECT_TRUE(adb.disconnects.empty());
    EXPECT_TRUE(adb.connects.empty());
    EXPECT_NE(why.find("device not ready"), std::string::npos) << why;
}
