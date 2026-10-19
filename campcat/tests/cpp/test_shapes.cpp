// Circle support fails at three independent seams: region geometry at the screen
// edge, reflection binding one region out of either shape, and masked disc
// correlation. The first two are only reachable through `dispatch`, so those
// cases call tools by name -- the same seat the model sits in. A disc on a real
// screencap is not covered: that is calibration, not a unit test.

#include <core/adb_client.h>
#include <core/llm/llm_tool_schema.h>
#include <core/llm/llm_tools.h>
#include <core/template_matcher.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kScreenW = 100;
constexpr int kScreenH = 80;

/// A device that answers everything with success and records nothing. `tap` is
/// the only member any case here reaches; the rest are honest no-ops rather than
/// a mock framework.
class SilentAdb final : public campcat::adb_api {
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
    bool home_and_kill_all(int _gap_ms) override {
        home_gap_ms = _gap_ms;
        ++home_calls;
        return home_ok;
    }
    bool screencap_bgr(cv::Mat *, int, std::string *) override { return false; }
    int home_gap_ms = -1;
    int home_calls = 0;
    bool home_ok = false;
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }
};

/// A grey screen with one bright disc, so a crop of it is distinguishable from a
/// crop of anywhere else.
cv::Mat test_screen() {
    cv::Mat img(kScreenH, kScreenW, CV_8UC3, cv::Scalar(40, 40, 40));
    cv::circle(img, cv::Point(50, 40), 12, cv::Scalar(220, 220, 220),
               cv::FILLED);
    return img;
}

/// Deterministic noise. A correlation assertion needs the same screen every run,
/// and a flat one would leave nothing for a disc to be told apart from.
cv::Mat noise(int _rows, int _cols, unsigned _seed) {
    cv::Mat img(_rows, _cols, CV_8UC3);
    cv::RNG rng(_seed);
    rng.fill(img, cv::RNG::UNIFORM, 0, 256);
    return img;
}

/// The alpha a circular template carries on disk: 255 inside the disc, 0
/// outside. The same convention `load_template` reads back off a BGRA png.
cv::Mat disc_mask(int _side, int _radius) {
    cv::Mat mask(_side, _side, CV_8U, cv::Scalar(0));
    cv::circle(mask, cv::Point(_side / 2, _side / 2), _radius,
               cv::Scalar(255), cv::FILLED);
    return mask;
}

/// Decodes a PNG the tools handed back, to read the size or the pixels of it.
cv::Mat decode_png(const std::string &_bytes) {
    const cv::Mat buf(1, static_cast<int>(_bytes.size()), CV_8UC1,
                      const_cast<char *>(_bytes.data()));
    return cv::imdecode(buf, cv::IMREAD_UNCHANGED);
}

/// Installs the screen and the device every tool reads. Without them a case
/// reports "no screenshot yet" instead of the thing it meant to test.
class ShapeTools : public ::testing::Test {
protected:
    void SetUp() override {
        std::vector<unsigned char> png;
        cv::imencode(".png", test_screen(), png);
        campcat::llm::tool_context ctx;
        ctx.screen.w = kScreenW;
        ctx.screen.h = kScreenH;
        ctx.screen.png.assign(png.begin(), png.end());
        ctx.adb = &m_adb;
        campcat::llm::set_context(ctx);
    }

    void TearDown() override { campcat::llm::set_context({}); }

    /// Dispatches `_name` with `_args`, the same call and the same JSON string
    /// the host passes. A refusal lands in `error`, so cases assert on which
    /// field is filled rather than catching.
    static campcat::llm::tool_reply call(const std::string &_name,
                                         const std::string &_args) {
        return campcat::llm::dispatch(_name, _args);
    }

private:
    SilentAdb m_adb;
};

TEST(GoHome, ClosesRecentsAndReportsTheLauncher) {
    SilentAdb adb;
    adb.home_ok = true;
    campcat::app_config cfg;
    cfg.input.tap_delay_ms = 150;
    campcat::llm::tool_context ctx;
    ctx.adb = &adb;
    ctx.cfg = &cfg;
    campcat::llm::set_context(ctx);
    const campcat::llm::tool_reply reply =
        campcat::llm::dispatch("go_home", "{}");
    campcat::llm::set_context({});
    EXPECT_TRUE(reply.ok) << reply.error;
    EXPECT_EQ(reply.text, "on the launcher; recent apps were closed");
    EXPECT_EQ(adb.home_calls, 1);
    EXPECT_EQ(adb.home_gap_ms, 150);
}

/// `_have_callback` false is the gate with nobody to ask. The decision is
/// ignored in that case.
campcat::llm::tool_reply ask_go_home(SilentAdb *_adb,
                                     campcat::llm::approval_decision _decision,
                                     bool _have_callback,
                                     campcat::llm::approval_request *_seen) {
    campcat::llm::tool_context ctx;
    ctx.adb = _adb;
    ctx.needs_approval = [](std::string_view _tool) {
        return _tool == "go_home";
    };
    if (_have_callback) {
        ctx.request_approval = [_decision, _seen](
                                   const campcat::llm::approval_request &_req) {
            if (_seen != nullptr) {
                *_seen = _req;
            }
            return _decision;
        };
    }
    campcat::llm::set_context(ctx);
    const campcat::llm::tool_reply reply =
        campcat::llm::dispatch("go_home", "{}");
    campcat::llm::set_context({});
    return reply;
}

TEST(GoHome, RejectionDoesNotTouchTheDevice) {
    SilentAdb adb;
    adb.home_ok = true;
    campcat::llm::approval_request seen;
    campcat::llm::approval_decision decision;
    decision.guidance = "stop";
    const campcat::llm::tool_reply reply =
        ask_go_home(&adb, decision, true, &seen);
    EXPECT_FALSE(reply.ok);
    EXPECT_EQ(reply.error, "user rejected going home: stop");
    EXPECT_EQ(adb.home_calls, 0);
    EXPECT_EQ(seen.tool, "go_home");
    EXPECT_EQ(seen.summary,
              "return to the launcher and force-stop every recent app");
    EXPECT_FALSE(seen.editable);
    EXPECT_TRUE(seen.screen_png.empty());
    EXPECT_TRUE(seen.highlight.empty());
}

TEST(GoHome, ApprovalThenRuns) {
    SilentAdb adb;
    adb.home_ok = true;
    campcat::llm::approval_decision decision;
    decision.approved = true;
    const campcat::llm::tool_reply reply =
        ask_go_home(&adb, decision, true, nullptr);
    EXPECT_TRUE(reply.ok) << reply.error;
    EXPECT_EQ(reply.text, "on the launcher; recent apps were closed");
    EXPECT_EQ(adb.home_calls, 1);
}

TEST(GoHome, AsksWithNobodyThere) {
    SilentAdb adb;
    adb.home_ok = true;
    const campcat::llm::tool_reply reply =
        ask_go_home(&adb, {}, false, nullptr);
    EXPECT_FALSE(reply.ok);
    EXPECT_EQ(reply.error, "no human available to approve going home");
    EXPECT_EQ(adb.home_calls, 0);
}

TEST(GoHome, ApprovalThenAdbFailure) {
    SilentAdb adb;
    campcat::llm::approval_decision decision;
    decision.approved = true;
    const campcat::llm::tool_reply reply =
        ask_go_home(&adb, decision, true, nullptr);
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("returning home failed"), std::string::npos)
        << reply.error;
    EXPECT_EQ(adb.home_calls, 1);
}

const campcat::llm::js::tool_entry *tool_named(const std::string &_name) {
    for (const auto &entry : campcat::llm::js::tool_registry()) {
        if (entry.name == _name) {
            return &entry;
        }
    }
    return nullptr;
}

// --- where a shape may sit --------------------------------------------------

// A disc is refused, not moved, when any pixel it covers is off-screen: a
// clamped tap would look like a success to the model. Its bounding box is 2r+1
// wide, so the last covered pixel is cx+r -- the pair below sits exactly on it.
TEST_F(ShapeTools, CircleStopsAtTheLastPixelItCanCover) {
    const auto last = call("view_crop",
                           R"({"region":{"cx":61,"cy":40,"r":38}})");
    EXPECT_TRUE(last.ok) << last.error;

    const auto past = call("view_crop",
                           R"({"region":{"cx":62,"cy":40,"r":38}})");
    EXPECT_FALSE(past.ok);
    EXPECT_NE(past.error.find("out of range"), std::string::npos) << past.error;
}

TEST_F(ShapeTools, CircleStopsAtTheLastRowItCanCover) {
    const auto last =
        call("view_crop", R"({"region":{"cx":50,"cy":41,"r":38}})");
    EXPECT_TRUE(last.ok) << last.error;

    const auto past =
        call("view_crop", R"({"region":{"cx":50,"cy":42,"r":38}})");
    EXPECT_FALSE(past.ok);
    EXPECT_NE(past.error.find("out of range"), std::string::npos) << past.error;
}

// A radius of zero covers one pixel and a negative one covers nothing; neither
// is a shape anyone meant, so both are refused with a radius that cannot crop.
TEST_F(ShapeTools, ANonPositiveRadiusIsRefused) {
    for (const char *args : {R"({"region":{"cx":50,"cy":40,"r":0}})",
                             R"({"region":{"cx":50,"cy":40,"r":-3}})"}) {
        const auto reply = call("view_crop", args);
        EXPECT_FALSE(reply.ok) << args;
        EXPECT_NE(reply.error.find("out of range"), std::string::npos)
            << args << ": " << reply.error;
    }
}

// A box is half-open -- x+w is one past its last pixel -- so it may sit flush
// against the edge where a disc reaching the same distance may not. The two
// cases above and this one together pin that difference, which is easy to get
// wrong by copying one shape's test into the other.
TEST_F(ShapeTools, BoxMaySitFlushAgainstTheEdge) {
    const auto flush =
        call("view_crop", R"({"region":{"x":90,"y":70,"w":10,"h":10}})");
    EXPECT_TRUE(flush.ok) << flush.error;

    const auto past =
        call("view_crop", R"({"region":{"x":91,"y":70,"w":10,"h":10}})");
    EXPECT_FALSE(past.ok);
    EXPECT_NE(past.error.find("out of range"), std::string::npos) << past.error;
}

TEST_F(ShapeTools, AZeroSizedBoxIsRefused) {
    const auto reply = call("view_crop", R"({"region":{"x":10,"y":10,"w":0,"h":10}})");
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("out of range"), std::string::npos)
        << reply.error;
}

// --- where a region's bounds and centre land --------------------------------

// view_crop shows 16 pixels of screen around the cut, clipped at the frame,
// and strokes the cut. A tight crop is what `tight_crop` still returns.
const cv::Vec3b k_stroke(60, 200, 255);
const cv::Vec3b k_grey(40, 40, 40);

TEST_F(ShapeTools, ACropIsTheRegionsBoundingBox) {
    const auto box =
        call("view_crop", R"({"region":{"x":20,"y":20,"w":30,"h":15}})");
    ASSERT_TRUE(box.ok) << box.error;
    const cv::Mat padded = decode_png(box.image_png);
    ASSERT_EQ(padded.type(), CV_8UC3);
    ASSERT_EQ(padded.size(), cv::Size(62, 47));
    EXPECT_EQ(padded.at<cv::Vec3b>(0, 0), k_grey);
    EXPECT_EQ(padded.at<cv::Vec3b>(16, 31), k_stroke);

    const auto clipped =
        call("view_crop", R"({"region":{"x":10,"y":20,"w":30,"h":15}})");
    ASSERT_TRUE(clipped.ok) << clipped.error;
    EXPECT_EQ(decode_png(clipped.image_png).size(), cv::Size(56, 47));

    const auto disc =
        call("view_crop", R"({"region":{"cx":50,"cy":40,"r":12}})");
    ASSERT_TRUE(disc.ok) << disc.error;
    EXPECT_EQ(decode_png(disc.image_png).size(), cv::Size(57, 57));

    const auto diamond =
        call("view_crop", R"({"region":{"cx":50,"cy":40,"rx":10,"ry":6}})");
    ASSERT_TRUE(diamond.ok) << diamond.error;
    const cv::Mat marked = decode_png(diamond.image_png);
    ASSERT_EQ(marked.type(), CV_8UC3);
    EXPECT_EQ(marked.size(), cv::Size(53, 45));
    EXPECT_NE(marked.at<cv::Vec3b>(16, 16), k_stroke);
    EXPECT_EQ(marked.at<cv::Vec3b>(22, 36), k_stroke);

    const auto flush =
        call("view_crop", R"({"region":{"x":90,"y":70,"w":10,"h":10}})");
    ASSERT_TRUE(flush.ok) << flush.error;
    EXPECT_EQ(decode_png(flush.image_png).size(), cv::Size(26, 26));
}

TEST_F(ShapeTools, TightCropIsTheRegionsBoundingBox) {
    const auto box =
        call("tight_crop", R"({"region":{"x":10,"y":20,"w":30,"h":15}})");
    ASSERT_TRUE(box.ok) << box.error;
    EXPECT_EQ(decode_png(box.image_png).size(), cv::Size(30, 15));

    const auto disc =
        call("tight_crop", R"({"region":{"cx":50,"cy":40,"r":12}})");
    ASSERT_TRUE(disc.ok) << disc.error;
    EXPECT_EQ(decode_png(disc.image_png).size(), cv::Size(25, 25));

    const auto diamond =
        call("tight_crop", R"({"region":{"cx":50,"cy":40,"rx":10,"ry":6}})");
    ASSERT_TRUE(diamond.ok) << diamond.error;
    EXPECT_EQ(decode_png(diamond.image_png).size(), cv::Size(21, 13));
}

// The bounding-box corners sit outside the diamond; the centre and the axis
// vertices sit inside. A full rectangle would make those corners opaque.
TEST_F(ShapeTools, ADiamondCropIsTransparentOutsideTheVertices) {
    const auto reply =
        call("tight_crop", R"({"region":{"cx":50,"cy":40,"rx":10,"ry":6}})");
    ASSERT_TRUE(reply.ok) << reply.error;
    const cv::Mat crop = decode_png(reply.image_png);
    ASSERT_EQ(crop.type(), CV_8UC4);
    const auto alpha = [&](int x, int y) {
        return crop.at<cv::Vec4b>(y, x)[3];
    };
    EXPECT_EQ(alpha(10, 6), 255);  // centre
    EXPECT_EQ(alpha(20, 6), 255);  // right vertex
    EXPECT_EQ(alpha(10, 0), 255);  // top vertex
    EXPECT_EQ(alpha(0, 0), 0);     // bounding-box corner
}

// Same strict edge as a disc: the rightmost vertex is cx+rx, so it may not sit
// on the column past the last pixel. A zero half-diagonal is not a diamond.
TEST_F(ShapeTools, ADiamondStopsAtTheLastPixelItCanCover) {
    const auto inside =
        call("view_crop", R"({"region":{"cx":89,"cy":40,"rx":10,"ry":5}})");
    EXPECT_TRUE(inside.ok) << inside.error;

    const auto past =
        call("view_crop", R"({"region":{"cx":90,"cy":40,"rx":10,"ry":5}})");
    EXPECT_FALSE(past.ok);
    EXPECT_NE(past.error.find("out of range"), std::string::npos) << past.error;

    const auto flat =
        call("view_crop", R"({"region":{"cx":50,"cy":40,"rx":10,"ry":0}})");
    EXPECT_FALSE(flat.ok);
    EXPECT_NE(flat.error.find("out of range"), std::string::npos) << flat.error;
}

// The tap sentence names the point `region_center` picked: a box acts at its
// middle, a disc at its own centre. A tap on the wrong pixel is otherwise
// silent, so the number is worth reading back.
TEST_F(ShapeTools, TapLandsOnTheRegionsCentre) {
    const auto box = call("tap", R"({"region":{"x":10,"y":20,"w":30,"h":15}})");
    ASSERT_TRUE(box.ok) << box.error;
    EXPECT_EQ(box.text, "tapped 25,27");

    const auto disc = call("tap", R"({"region":{"cx":50,"cy":40,"r":12}})");
    ASSERT_TRUE(disc.ok) << disc.error;
    EXPECT_EQ(disc.text, "tapped 50,40");

    const auto diamond =
        call("tap", R"({"region":{"cx":50,"cy":40,"rx":10,"ry":6}})");
    ASSERT_TRUE(diamond.ok) << diamond.error;
    EXPECT_EQ(diamond.text, "tapped 50,40");
}

// --- either shape into one parameter ----------------------------------------

// `update_script` takes either shape in one parameter (`template_cut::cut`),
// told apart structurally: each carries its own field names and none has a
// default, so a bare circle fits `circle` alone. The bundle is deliberately
// missing, so reaching "unknown script bundle" proves the variant bound.
TEST_F(ShapeTools, UpdateScriptBindsEitherShapeIntoOneCut) {
    const std::string circle = R"({
        "name": "no_such_bundle",
        "source": "log(\"hi\")\n",
        "templates": [{"name": "a", "cut": {"cx": 5, "cy": 6, "r": 3}}]
    })";
    const auto with_circle = call("update_script", circle);
    EXPECT_NE(with_circle.error.find("unknown script bundle"), std::string::npos)
        << with_circle.error;

    const std::string box = R"({
        "name": "no_such_bundle",
        "source": "log(\"hi\")\n",
        "templates": [{"name": "a", "cut": {"x": 1, "y": 2, "w": 3, "h": 4}}]
    })";
    const auto with_box = call("update_script", box);
    EXPECT_NE(with_box.error.find("unknown script bundle"), std::string::npos)
        << with_box.error;

    const std::string diamond = R"({
        "name": "no_such_bundle",
        "source": "log(\"hi\")\n",
        "templates": [{"name": "a", "cut": {"cx": 5, "cy": 6, "rx": 4, "ry": 2}}]
    })";
    const auto with_diamond = call("update_script", diamond);
    EXPECT_NE(with_diamond.error.find("unknown script bundle"), std::string::npos)
        << with_diamond.error;
}

// An object with none of either shape's fields fits neither alternative. That
// it is reported as matching nothing -- rather than silently binding the first
// one -- is what a default member initializer would break: a defaulted struct
// matches every object, and the pair would then be ambiguous.
TEST_F(ShapeTools, AnEmptyCutMatchesNoShape) {
    const std::string args = R"({
        "name": "no_such_bundle",
        "source": "log(\"hi\")\n",
        "templates": [{"name": "a", "cut": {}}]
    })";
    const auto reply = call("update_script", args);
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("no variant alternative matched"),
              std::string::npos)
        << reply.error;
}

// The schema is where the model learns a circle is allowed at all. `anyOf` and
// not `oneOf`: both OpenAI and Anthropic reject the exclusive form, and a
// schema that offered only boxes would leave the disc tools unreachable.
TEST(ShapeSchema, TheCutOffersBothShapesAsAlternatives) {
    const campcat::llm::js::tool_entry *entry = tool_named("update_script");
    ASSERT_NE(entry, nullptr);
    const std::string_view schema = entry->schema;
    EXPECT_NE(schema.find("\"anyOf\""), std::string_view::npos);
    EXPECT_NE(schema.find("\"cx\""), std::string_view::npos); // circle
    EXPECT_NE(schema.find("\"cy\""), std::string_view::npos);
    EXPECT_NE(schema.find("\"rx\""), std::string_view::npos); // diamond
    EXPECT_NE(schema.find("\"w\""), std::string_view::npos); // box
}

// A region tool offers both shapes, told apart by their fields, the same way
// update_script's cut does. `anyOf`, not `oneOf`: both providers reject the
// exclusive form.
TEST(ShapeSchema, ARegionToolOffersBothShapes) {
    for (const char *name : {"tap", "view_crop", "save_template", "mark"}) {
        const campcat::llm::js::tool_entry *entry = tool_named(name);
        ASSERT_NE(entry, nullptr) << name;
        EXPECT_NE(entry->schema.find("\"anyOf\""), std::string_view::npos) << name;
        EXPECT_NE(entry->schema.find("\"cx\""), std::string_view::npos) << name;
        EXPECT_NE(entry->schema.find("\"rx\""), std::string_view::npos) << name;
        EXPECT_NE(entry->schema.find("\"x\""), std::string_view::npos) << name;
    }
}

// --- a named frame, not only the current screen ----------------------------

/// Hands back the pictures it was given, in order. `screenshot` is the only
/// call that reaches it; everything else is a no-op, same as `SilentAdb`.
class SequencedAdb final : public campcat::adb_api {
public:
    explicit SequencedAdb(std::vector<cv::Mat> _frames)
        : m_frames(std::move(_frames)) {}

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
    bool screencap_bgr(cv::Mat *_out, int, std::string *) override {
        if (m_next >= m_frames.size()) {
            return false;
        }
        *_out = m_frames[m_next++].clone();
        return true;
    }
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }

private:
    std::vector<cv::Mat> m_frames;
    std::size_t m_next = 0;
};

/// 100x80, left half white and right half black, so a crop of the corner is
/// nothing like a crop of the grey frame that follows it.
cv::Mat split_frame() {
    cv::Mat img(kScreenH, kScreenW, CV_8UC3, cv::Scalar(255, 255, 255));
    img(cv::Rect(kScreenW / 2, 0, kScreenW / 2, kScreenH))
        .setTo(cv::Scalar(0, 0, 0));
    return img;
}

cv::Mat grey_frame() {
    return cv::Mat(kScreenH, kScreenW, CV_8UC3, cv::Scalar(128, 128, 128));
}

/// Two screenshots, so frame 1 and frame 2 both exist and the current screen
/// is frame 2. `forget_frames` first: numbering is process-global.
class FrameTools : public ::testing::Test {
protected:
    void SetUp() override {
        campcat::llm::forget_frames();
        m_adb = std::make_unique<SequencedAdb>(
            std::vector<cv::Mat>{split_frame(), grey_frame()});
        campcat::llm::tool_context ctx;
        ctx.adb = m_adb.get();
        campcat::llm::set_context(ctx);
        ASSERT_TRUE(call("screenshot", "{}").ok);
        ASSERT_TRUE(call("screenshot", "{}").ok);
    }

    void TearDown() override {
        campcat::llm::set_context({});
        campcat::llm::forget_frames();
    }

    static campcat::llm::tool_reply call(const std::string &_name,
                                         const std::string &_args) {
        return campcat::llm::dispatch(_name, _args);
    }

private:
    std::unique_ptr<SequencedAdb> m_adb;
};

/// `view_crop` pads 16px and strokes the cut, so the frame colour only survives
/// past that stroke. (24, 24) is in the pad, still inside frame 1's white half.
cv::Vec3b pad_pixel(const std::string &_png) {
    const cv::Mat img = decode_png(_png);
    if (img.size() != cv::Size(26, 26)) {
        ADD_FAILURE() << "view is " << img.cols << "x" << img.rows
                      << ", expected 26x26";
        return {};
    }
    return img.at<cv::Vec3b>(24, 24);
}

TEST_F(FrameTools, CropReadsTheNamedFrame) {
    const auto named = call(
        "view_crop", R"({"region":{"x":0,"y":0,"w":10,"h":10},"shot":1})");
    ASSERT_TRUE(named.ok) << named.error;
    EXPECT_EQ(pad_pixel(named.image_png), cv::Vec3b(255, 255, 255));

    const auto current =
        call("view_crop", R"({"region":{"x":0,"y":0,"w":10,"h":10}})");
    ASSERT_TRUE(current.ok) << current.error;
    EXPECT_EQ(pad_pixel(current.image_png), cv::Vec3b(128, 128, 128));
}

TEST_F(FrameTools, CropOfAnOldFrameKeepsTheCurrentScreen) {
    const auto named = call(
        "view_crop", R"({"region":{"x":0,"y":0,"w":10,"h":10},"shot":1})");
    ASSERT_TRUE(named.ok) << named.error;
    // The named crop has to be frame 1, or this only checks the current screen
    // twice and passes while `shot` is ignored.
    EXPECT_EQ(pad_pixel(named.image_png), cv::Vec3b(255, 255, 255));
    const auto current =
        call("view_crop", R"({"region":{"x":0,"y":0,"w":10,"h":10}})");
    ASSERT_TRUE(current.ok) << current.error;
    EXPECT_EQ(pad_pixel(current.image_png), cv::Vec3b(128, 128, 128));
}

TEST_F(FrameTools, MarkDrawsOnTheNamedFrame) {
    const auto marked = call(
        "mark",
        R"({"label":"","region":{"x":0,"y":0,"w":10,"h":10},"shot":1})");
    ASSERT_TRUE(marked.ok) << marked.error;
    const cv::Vec3b px = decode_png(marked.image_png).at<cv::Vec3b>(5, 95);
    EXPECT_EQ(px, cv::Vec3b(0, 0, 0));
}

TEST_F(FrameTools, UnknownShotIsRefusedWithFrameForWording) {
    const auto reply = call(
        "view_crop", R"({"region":{"cx":50,"cy":40,"r":12},"shot":9})");
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("no screenshot #9 in this session"),
              std::string::npos)
        << reply.error;
}

TEST(FrameToolsSchema, ShotIsNullableForTheCurrentScreen) {
    // The kit marks every parameter required and spells an optional as
    // `anyOf[T, null]` until it can recover C++ defaults at dispatch time, so
    // "omit it" survives in the binder (see CropReadsTheNamedFrame) but not in
    // the schema text: what the model is offered is `null`. Asserting the shape
    // is what makes a future kit change show up here.
    const campcat::llm::js::tool_entry *entry = tool_named("mark");
    ASSERT_NE(entry, nullptr);
    const auto schema = nlohmann::json::parse(std::string(entry->schema));
    const auto &shot = schema.at("function").at("parameters").at("properties").at(
        "shot");
    ASSERT_TRUE(shot.contains("anyOf")) << shot.dump();
    EXPECT_EQ(shot.at("anyOf").back().at("type"), "null");
}

// --- scoring a disc ---------------------------------------------------------

// The masked path is the one that had to be replaced: scoring a disc with
// TM_CCORR_NORMED gave any bright flat patch ~1.0, so a threshold that admitted
// a real hit also admitted everything else. These cases hold the two properties
// a usable threshold needs.
class MaskedMatch : public ::testing::Test {
protected:
    static constexpr int kRadius = 12;
    static constexpr int kSide = 2 * kRadius + 1;

    static campcat::template_matcher matcher() {
        return campcat::template_matcher(0.5, 0.5, false);
    }
};

// Pixels outside the mask must not move the score at all, or a disc would be
// dragged around by whatever sits in its corners -- which for a circular icon is
// most of its bounding box. Painting those corners white changes nothing the
// scorer may read, so the score has to come back bit for bit.
TEST_F(MaskedMatch, PixelsOutsideTheMaskCannotMoveTheScore) {
    const cv::Mat screen = noise(240, 320, 7);
    const cv::Point at(100, 100);
    const cv::Mat templ = screen(cv::Rect(at.x, at.y, kSide, kSide)).clone();
    const cv::Mat mask = disc_mask(kSide, kRadius);
    const campcat::template_matcher m = matcher();

    const campcat::match_result before = m.match(screen, templ, 0.5, {}, mask);
    EXPECT_TRUE(before.found) << before.confidence;

    cv::Mat painted = screen.clone();
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            if (mask.at<unsigned char>(y, x) == 0) {
                painted.at<cv::Vec3b>(at.y + y, at.x + x) =
                    cv::Vec3b(255, 255, 255);
            }
        }
    }
    const campcat::match_result after = m.match(painted, templ, 0.5, {}, mask);

    EXPECT_EQ(after.center, before.center);
    EXPECT_NEAR(after.confidence, before.confidence, 1e-9)
        << "corners the mask excludes changed the score";
}

// With the whole template taking part there is nothing to exclude, so the masked
// path has to agree with the plain square one. This is the check that the mask
// changes which pixels count and nothing else -- a mask accidentally applied as
// a multiplier would drift here.
TEST_F(MaskedMatch, AFullMaskScoresLikeTheUnmaskedSquare) {
    const cv::Mat screen = noise(240, 320, 11);
    const cv::Mat templ = screen(cv::Rect(40, 30, 32, 32)).clone();
    const cv::Mat full(templ.size(), CV_8U, cv::Scalar(255));
    const campcat::template_matcher m = matcher();

    const campcat::match_result plain = m.match(screen, templ, 0.5);
    const campcat::match_result masked = m.match(screen, templ, 0.5, {}, full);

    ASSERT_TRUE(plain.found) << plain.confidence;
    EXPECT_EQ(masked.center, plain.center);
    EXPECT_NEAR(masked.confidence, plain.confidence, 1e-4);
}

// A screen of one flat colour is the extreme of the bright flat patch that broke
// the old scorer. Nothing here resembles the disc, so nothing here may be scored
// as one.
TEST_F(MaskedMatch, ADiscIsNotFoundOnAFlatScreen) {
    const cv::Mat screen = noise(240, 320, 7);
    const cv::Mat templ =
        screen(cv::Rect(100, 100, kSide, kSide)).clone();
    const cv::Mat mask = disc_mask(kSide, kRadius);
    const cv::Mat flat(240, 320, CV_8UC3, cv::Scalar(200, 200, 200));

    const campcat::match_result r =
        matcher().match(flat, templ, 0.5, {}, mask);
    EXPECT_FALSE(r.found) << "scored " << r.confidence << " on a flat screen";
}

// The disc is on the screen once, and the same noise with its own pixels
// replaced puts it nowhere. The gap between the two is the margin a threshold
// works in, so it is asserted rather than left to a comment.
TEST_F(MaskedMatch, ADiscScoresHigherWhereItIsThanWhereItIsNot) {
    const cv::Mat screen = noise(240, 320, 7);
    const cv::Rect at(100, 100, kSide, kSide);
    const cv::Mat templ = screen(at).clone();
    const cv::Mat mask = disc_mask(kSide, kRadius);
    const campcat::template_matcher m = matcher();

    cv::Mat without = screen.clone();
    noise(kSide, kSide, 9999).copyTo(without(at));

    const campcat::match_result hit = m.match(screen, templ, 0.5, {}, mask);
    const campcat::match_result miss = m.match(without, templ, 0.5, {}, mask);

    EXPECT_TRUE(hit.found) << hit.confidence;
    EXPECT_GT(hit.confidence, miss.confidence + 0.2)
        << "hit " << hit.confidence << " vs best elsewhere " << miss.confidence;
}

} // namespace
