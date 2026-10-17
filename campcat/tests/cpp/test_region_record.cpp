// The one place a human's redrawn region becomes training data. What the
// scorer is later trained on is exactly this file, so a sample that quietly
// loses a field, or a frame that never lands, is worth a case: nothing
// downstream can tell a missing sample from a human who redraws nothing.

#include <core/adb_client.h>
#include <core/app_config.h>
#include <core/llm/llm_tools.h>
#include <core/region_record.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kW = 120;
constexpr int kH = 90;
const campcat::llm::box kBox{20, 20, 30, 30};
const campcat::llm::box kRedrawn{21, 19, 29, 31};

/// The frame's textured patch, in pixels: `kBox` in OpenCV's spelling.
const cv::Rect kBoxRect(kBox.x, kBox.y, kBox.w, kBox.h);

std::filesystem::path scratch(const std::string &_name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      "campcat_region_record" / _name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path &_path, const std::string &_text) {
    std::filesystem::create_directories(_path.parent_path());
    std::ofstream out(_path);
    out << _text;
}

/// A screenshot with one textured patch, so a cut of it passes the flat-colour
/// refusal and the frame is a real picture on disk.
cv::Mat frame_with(const cv::Rect &_at) {
    cv::Mat img(kH, kW, CV_8UC3, cv::Scalar(40, 40, 40));
    cv::Mat patch(_at.height, _at.width, CV_8UC3);
    cv::RNG rng(7);
    rng.fill(patch, cv::RNG::UNIFORM, 0, 256);
    patch.copyTo(img(_at));
    return img;
}

/// The sample folders under `_root`, in name (time) order.
std::vector<std::filesystem::path>
sample_dirs(const std::filesystem::path &_root) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(_root, ec)) {
        if (entry.is_directory()) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// The meta.json of every sample, in the same order as `sample_dirs`.
std::vector<nlohmann::json> metas(const std::filesystem::path &_root) {
    std::vector<nlohmann::json> out;
    for (const auto &dir : sample_dirs(_root)) {
        std::ifstream in(dir / "meta.json");
        out.push_back(nlohmann::json::parse(in));
    }
    return out;
}

campcat::region_sample sample_of() {
    campcat::region_sample sample;
    sample.frame = frame_with(kBoxRect);
    sample.given = kBox;
    sample.human = kRedrawn;
    return sample;
}

/// A device nobody talks to: these cases stop at the approval, before adb.
class NoAdb final : public campcat::adb_api {
public:
    std::vector<std::string> list_devices(int) override { return {}; }
    void set_serial(const std::string &) override {}
    bool run(const std::vector<std::string> &, std::string *, std::string *,
             int) override {
        return false;
    }
    bool connect_remote(std::string_view, int, std::string *,
                        std::string *) override {
        return false;
    }
    bool tap(int, int) override { return true; }
    bool swipe(int, int, int, int, int) override { return false; }
    bool keyevent(int) override { return false; }
    bool home_and_kill_all(int) override { return false; }
    bool screencap_bgr(cv::Mat *, int, std::string *) override { return false; }
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }
};

/// A shell context that asks every gate, and answers with `_decision`. The
/// frame is installed directly: `tap` read its region off the screen state.
class Recorder : public ::testing::Test {
protected:
    void SetUp() override {
        m_root = scratch(
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        m_cfg.config_home = (m_root / "ccat_library").string();
        m_cfg.region_score.record = true;
        m_cfg.region_score.data_dir = "training_data";
        m_frame = frame_with(kBoxRect);
        install({false, "not quite", kRedrawn});
    }

    void TearDown() override {
        campcat::llm::set_context({});
        campcat::llm::forget_frames();
    }

    void install(const campcat::llm::approval_decision &_decision) {
        std::vector<unsigned char> png;
        cv::imencode(".png", m_frame, png);
        campcat::llm::tool_context ctx;
        ctx.screen.w = m_frame.cols;
        ctx.screen.h = m_frame.rows;
        ctx.screen.png.assign(png.begin(), png.end());
        ctx.adb = &m_adb;
        ctx.cfg = &m_cfg;
        ctx.script_dir = m_cfg.config_home;
        ctx.needs_approval = [](std::string_view) { return true; };
        ctx.request_approval =
            [_decision](const campcat::llm::approval_request &) {
                return _decision;
            };
        campcat::llm::set_context(ctx);
    }

    std::filesystem::path data() const { return m_root / "training_data"; }

    std::filesystem::path m_root;
    campcat::app_config m_cfg;
    cv::Mat m_frame;
    NoAdb m_adb;
};

} // namespace

TEST(RegionRecord, OneFolderPerSampleWithItsMeta) {
    const std::filesystem::path root = scratch("OneFolderPerSampleWithItsMeta");
    ASSERT_TRUE(campcat::record_region_sample(root, sample_of()));

    const auto dirs = sample_dirs(root);
    ASSERT_EQ(dirs.size(), 1U);
    const nlohmann::json meta = metas(root)[0];
    EXPECT_EQ(
        meta["given"],
        nlohmann::json(
            {{"shape", "box"}, {"x", 20}, {"y", 20}, {"w", 30}, {"h", 30}}));
    EXPECT_EQ(
        meta["human"],
        nlohmann::json(
            {{"shape", "box"}, {"x", 21}, {"y", 19}, {"w", 29}, {"h", 31}}));

    const cv::Mat stored =
        cv::imread((dirs[0] / "frame.png").string(), cv::IMREAD_COLOR);
    ASSERT_FALSE(stored.empty());
    EXPECT_EQ(stored.cols, kW);
    EXPECT_EQ(stored.rows, kH);
}

TEST(RegionRecord, EachSampleGetsItsOwnFolder) {
    const std::filesystem::path root = scratch("EachSampleGetsItsOwnFolder");
    ASSERT_TRUE(campcat::record_region_sample(root, sample_of()));
    ASSERT_TRUE(campcat::record_region_sample(root, sample_of()));

    const auto dirs = sample_dirs(root);
    ASSERT_EQ(dirs.size(), 2U);
    EXPECT_NE(dirs[0], dirs[1]);
    EXPECT_TRUE(std::filesystem::exists(dirs[0] / "frame.png"));
    EXPECT_TRUE(std::filesystem::exists(dirs[1] / "frame.png"));
}

TEST(RegionRecord, ACircleIsWrittenWithItsOwnFields) {
    // Not as its bounding box: the disc is a different region, and a scorer
    // trained on the box would learn the corners the human never drew.
    const std::filesystem::path root = scratch("ACircleIsWritten");
    campcat::region_sample sample = sample_of();
    sample.human = campcat::llm::circle{35, 35, 12};

    ASSERT_TRUE(campcat::record_region_sample(root, sample));
    EXPECT_EQ(metas(root)[0]["human"],
              nlohmann::json(
                  {{"shape", "circle"}, {"cx", 35}, {"cy", 35}, {"r", 12}}));
}

TEST(RegionRecord, ADiamondIsWrittenWithItsOwnFields) {
    const std::filesystem::path root = scratch("ADiamondIsWritten");
    campcat::region_sample sample = sample_of();
    sample.human = campcat::llm::diamond{35, 35, 12, 8};

    ASSERT_TRUE(campcat::record_region_sample(root, sample));
    EXPECT_EQ(metas(root)[0]["human"], nlohmann::json({{"shape", "diamond"},
                                                       {"cx", 35},
                                                       {"cy", 35},
                                                       {"rx", 12},
                                                       {"ry", 8}}));
}

TEST(RegionRecord, EmptyFrameWritesNothing) {
    const std::filesystem::path root = scratch("EmptyFrameWritesNothing");
    campcat::region_sample sample = sample_of();
    sample.frame = cv::Mat();

    EXPECT_FALSE(campcat::record_region_sample(root, sample));
    EXPECT_TRUE(sample_dirs(root).empty());
}

TEST(RegionRecord, ARegionWithNoAreaWritesNothing) {
    const std::filesystem::path root =
        scratch("ARegionWithNoAreaWritesNothing");
    campcat::region_sample sample = sample_of();
    sample.human = campcat::llm::box{21, 19, 0, 31};

    EXPECT_FALSE(campcat::record_region_sample(root, sample));
    EXPECT_TRUE(sample_dirs(root).empty());
}

TEST(RegionRecord, AShapeWithNoSizeWritesNothing) {
    const std::filesystem::path root = scratch("AShapeWithNoSizeWritesNothing");

    campcat::region_sample disc = sample_of();
    disc.human = campcat::llm::circle{35, 35, 0};
    EXPECT_FALSE(campcat::record_region_sample(root, disc));

    campcat::region_sample diamond = sample_of();
    diamond.human = campcat::llm::diamond{35, 35, 12, 0};
    EXPECT_FALSE(campcat::record_region_sample(root, diamond));

    EXPECT_TRUE(sample_dirs(root).empty());
}

TEST(RegionRecord, UnwritableRootReportsFalse) {
    const std::filesystem::path root = scratch("UnwritableRootReportsFalse");
    const std::filesystem::path blocked = root / "blocked";
    write_file(blocked, "a file, not a directory");

    EXPECT_FALSE(campcat::record_region_sample(blocked, sample_of()));
}

TEST(RegionRecord, ConfigBlockRoundTrips) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       "campcat_region_score_cfg.json";
    {
        std::ofstream out(path);
        out << R"({"region_score":{"record":true,"data_dir":"elsewhere"}})";
    }
    const campcat::app_config cfg = campcat::app_config::load_from_file(path);
    std::filesystem::remove(path);

    EXPECT_TRUE(cfg.region_score.record);
    EXPECT_EQ(cfg.region_score.data_dir, "elsewhere");
}

TEST(RegionRecord, ConfigWithoutTheBlockKeepsSamplingOff) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       "campcat_region_score_old.json";
    {
        std::ofstream out(path);
        out << R"({"input":{"tap_delay_ms":200}})";
    }
    const campcat::app_config cfg = campcat::app_config::load_from_file(path);
    std::filesystem::remove(path);

    EXPECT_FALSE(cfg.region_score.record);
    EXPECT_EQ(cfg.region_score.data_dir, "training_data");
}

TEST_F(Recorder, TapRedrawIsRecorded) {
    const auto reply = campcat::llm::dispatch(
        "tap", R"({"region":{"x":20,"y":20,"w":30,"h":30}})");
    EXPECT_FALSE(reply.ok);

    const auto dirs = sample_dirs(data());
    ASSERT_EQ(dirs.size(), 1U);
    const nlohmann::json meta = metas(data())[0];
    EXPECT_EQ(
        meta["given"],
        nlohmann::json(
            {{"shape", "box"}, {"x", 20}, {"y", 20}, {"w", 30}, {"h", 30}}));
    EXPECT_EQ(
        meta["human"],
        nlohmann::json(
            {{"shape", "box"}, {"x", 21}, {"y", 19}, {"w", 29}, {"h", 31}}));
}

TEST_F(Recorder, TapApprovalIsNotRecorded) {
    install({true, {}, std::nullopt});
    const auto reply = campcat::llm::dispatch(
        "tap", R"({"region":{"x":20,"y":20,"w":30,"h":30}})");
    EXPECT_TRUE(reply.ok) << reply.error;

    EXPECT_TRUE(sample_dirs(data()).empty());
}

TEST_F(Recorder, TapCircleRedrawIsRecorded) {
    install({false, "round it", campcat::llm::circle{35, 35, 12}});
    const auto reply = campcat::llm::dispatch(
        "tap", R"({"region":{"x":20,"y":20,"w":30,"h":30}})");
    EXPECT_FALSE(reply.ok);

    ASSERT_EQ(sample_dirs(data()).size(), 1U);
    EXPECT_EQ(metas(data())[0]["human"],
              nlohmann::json(
                  {{"shape", "circle"}, {"cx", 35}, {"cy", 35}, {"r", 12}}));
}

TEST_F(Recorder, SaveTemplateRedrawIsRecorded) {
    write_file(m_cfg.config_home + "/b/main.ccat", "log(\"hi\")\n");
    const auto reply = campcat::llm::dispatch(
        "save_template",
        R"({"bundle":"b","name":"t.png","region":{"x":20,"y":20,"w":30,"h":30}})");
    EXPECT_FALSE(reply.ok) << reply.error;

    const auto dirs = sample_dirs(data());
    ASSERT_EQ(dirs.size(), 1U);
    EXPECT_EQ(
        metas(data())[0]["human"],
        nlohmann::json(
            {{"shape", "box"}, {"x", 21}, {"y", 19}, {"w", 29}, {"h", 31}}));
    // The refusal still happened, and nothing was written to the bundle.
    EXPECT_FALSE(std::filesystem::exists(m_cfg.config_home + "/b/t.png"));
}

TEST_F(Recorder, SamplingOffWritesNothing) {
    m_cfg.region_score.record = false;
    install({false, "not quite", kRedrawn});
    const auto reply = campcat::llm::dispatch(
        "tap", R"({"region":{"x":20,"y":20,"w":30,"h":30}})");
    EXPECT_FALSE(reply.ok);

    EXPECT_TRUE(sample_dirs(data()).empty());
}
