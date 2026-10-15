// The graph tools at the seat the model sits in: what `read_graph` and
// `list_scripts` say about a graph made of bundles, and what the rename and
// delete tools refuse. The graph module's own half is in `test_ccat_graph.cpp`.

#include <core/adb_client.h>
#include <core/app_config.h>
#include <core/llm/llm_tools.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int kScreenW = 100;
constexpr int kScreenH = 80;

/// Where each anchor's mark sits on the screen, in pixels.
const cv::Rect kDisc(38, 28, 24, 24);
const cv::Rect kSquare(4, 4, 16, 16);

std::filesystem::path scratch(const std::string &_name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "campcat_graph_tools" / _name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path &_path, const std::string &_text) {
    std::filesystem::create_directories(_path.parent_path());
    std::ofstream out(_path);
    out << _text;
}

/// Deterministic noise: a template with no variance in it (a patch of one flat
/// colour) is degenerate, so both marks carry texture.
cv::Mat tex(unsigned _seed) {
    cv::Mat m(kSquare.height, kSquare.width, CV_8UC3);
    cv::RNG rng(_seed);
    rng.fill(m, cv::RNG::UNIFORM, 0, 256);
    return m;
}

/// A grey screen with a disc and a textured patch far apart, so a crop around
/// one of them cannot hold the other.
cv::Mat screen() {
    cv::Mat img(kScreenH, kScreenW, CV_8UC3, cv::Scalar(40, 40, 40));
    cv::circle(img, cv::Point(50, 40), 10, cv::Scalar(230, 230, 230),
               cv::FILLED);
    tex(7).copyTo(img(kSquare));
    return img;
}

void save_png(const std::filesystem::path &_path, const cv::Mat &_img) {
    std::filesystem::create_directories(_path.parent_path());
    cv::imwrite(_path.string(), _img);
}

std::string read_file(const std::filesystem::path &_path) {
    std::ifstream in(_path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

/// A device nobody talks to: anchor checks read the stored screenshot.
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
    bool tap(int, int) override { return false; }
    bool swipe(int, int, int, int, int) override { return false; }
    bool keyevent(int) override { return false; }
    bool home_and_kill_all(int) override { return false; }
    bool screencap_bgr(cv::Mat *, int, std::string *) override { return false; }
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }
};

/// Graph `g`: anchors entry (the disc) and a (the square), the edge between
/// them, and a plain task bundle.
class GraphTools : public ::testing::Test {
protected:
    void SetUp() override {
        m_dir = scratch(::testing::UnitTest::GetInstance()
                            ->current_test_info()
                            ->name());
        m_frame = screen();
        anchor("entry", m_frame(kDisc), "desktop\nicons vary");
        anchor("a", m_frame(kSquare), "main city");
        write_file(m_dir / "g.entry-a" / "main.ccat", "tap(\"icon.png\")\n");
        write_file(m_dir / "g.task" / "main.ccat", "// a task\nlog(\"t\")\n");
        m_cfg.config_home = m_dir.string();
        show(m_frame);
    }

    /// Make `_frame` the session's latest screenshot.
    void show(const cv::Mat &_frame) {
        std::vector<unsigned char> png;
        cv::imencode(".png", _frame, png);
        campcat::llm::tool_context ctx;
        ctx.screen.w = _frame.cols;
        ctx.screen.h = _frame.rows;
        ctx.screen.png.assign(png.begin(), png.end());
        // No `needs_approval`: these cases are about what the tools do, and a
        // context without one gates nothing.
        ctx.script_dir = m_dir;
        ctx.adb = &m_adb;
        ctx.cfg = &m_cfg;
        campcat::llm::set_context(ctx);
    }

    /// A grey screen holding only `_mark` at `_at`.
    static cv::Mat only(const cv::Mat &_mark, const cv::Rect &_at) {
        cv::Mat img(kScreenH, kScreenW, CV_8UC3, cv::Scalar(40, 40, 40));
        _mark.copyTo(img(_at));
        return img;
    }

    void TearDown() override { campcat::llm::set_context({}); }

    void anchor(const std::string &_id, const cv::Mat &_mark,
                const std::string &_description) {
        const std::filesystem::path dir = m_dir / ("g." + _id);
        save_png(dir / (_id + ".png"), _mark);
        write_file(dir / "golden.md", "---\nkind: page\n---\n" + _description);
        write_file(dir / "main.ccat", "if (has(\"" + _id +
                                          ".png\")) { return true }\nreturn false\n");
    }

    static campcat::llm::tool_reply call(const std::string &_name,
                                         const std::string &_args) {
        return campcat::llm::dispatch(_name, _args);
    }

    bool exists(const std::string &_rel) const {
        return std::filesystem::is_regular_file(m_dir / _rel);
    }

    std::filesystem::path m_dir;
    cv::Mat m_frame;
    NoAdb m_adb;
    campcat::app_config m_cfg;
};

/// `save_anchor` arguments for anchor `_id` of `g`, recognised by `<_id>.png`
/// cut from the box at `_at`.
std::string anchor_args(const std::string &_id, const cv::Rect &_at,
                        const std::string &_golden = "---\\n---\\nnew screen") {
    return R"({"graph": "g", "anchor": ")" + _id + R"(", "golden": ")" + _golden +
           R"(", "source": "if (has(\")" + _id +
           R"(.png\")) { return true }\nreturn false\n", "templates": [{"name": ")" +
           _id + R"(", "cut": {"x": )" + std::to_string(_at.x) +
           R"(, "y": )" + std::to_string(_at.y) + R"(, "w": )" +
           std::to_string(_at.width) + R"(, "h": )" +
           std::to_string(_at.height) + "}}]}";
}

const cv::Rect kCorner(80, 60, 16, 16);

TEST_F(GraphTools, SaveAnchorWritesThree) {
    show(only(tex(9), kCorner));
    const auto reply = call("save_anchor", anchor_args("b", kCorner));
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_TRUE(exists("g.b/golden.md"));
    EXPECT_TRUE(exists("g.b/main.ccat"));
    EXPECT_TRUE(exists("g.b/b.png"));
    EXPECT_NE(reply.text.find("g.b is not reachable from entry"),
              std::string::npos)
        << reply.text;
}

TEST_F(GraphTools, SaveAnchorRefusesDuplicate) {
    show(only(tex(7), kSquare));
    const auto reply = call("save_anchor", anchor_args("c", kSquare));
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("already anchor a"), std::string::npos)
        << reply.error;
    EXPECT_FALSE(std::filesystem::exists(m_dir / "g.c"));
}

TEST_F(GraphTools, SaveAnchorRefusesBadInput) {
    show(only(tex(9), kCorner));
    const auto golden =
        call("save_anchor", anchor_args("b", kCorner, "no front matter"));
    EXPECT_FALSE(golden.ok);
    EXPECT_NE(golden.error.find("golden.md"), std::string::npos) << golden.error;

    const auto tap = call("save_anchor", R"({"graph": "g", "anchor": "b",
        "golden": "---\n---\nx", "source": "tap(\"b.png\")\nreturn true\n",
        "templates": [{"name": "b", "cut": {"x": 80, "y": 60, "w": 16, "h": 16}}]})");
    EXPECT_FALSE(tap.ok);
    EXPECT_NE(tap.error.find("tap is not allowed in an anchor"),
              std::string::npos)
        << tap.error;

    const auto undeclared = call("save_anchor", R"({"graph": "g", "anchor": "b",
        "golden": "---\n---\nx",
        "source": "if (has(\"b.png\")) { return true }\nreturn false\n",
        "templates": []})");
    EXPECT_FALSE(undeclared.ok);
    EXPECT_NE(undeclared.error.find("b.png"), std::string::npos)
        << undeclared.error;

    for (const char *name : {"a-b", "Main", "entry.x"}) {
        const auto bad = call("save_anchor", anchor_args(name, kCorner));
        EXPECT_FALSE(bad.ok) << name;
    }
    EXPECT_FALSE(std::filesystem::exists(m_dir / "g.b"));
}

TEST_F(GraphTools, SaveAnchorUpdatesWithoutDedupe) {
    // The screen is anchor a itself, which a new anchor would be refused on.
    show(only(tex(7), kSquare));
    const auto reply = call("save_anchor", R"({"graph": "g", "anchor": "a",
        "golden": "---\n---\ncity, renamed",
        "source": "if (has(\"a.png\")) { return true }\nreturn false\n",
        "templates": [{"name": "a"}]})");
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_NE(read_file(m_dir / "g.a" / "golden.md").find("city, renamed"),
              std::string::npos);
}

TEST_F(GraphTools, UpdateScriptLintsAnchor) {
    const auto reply = call("update_script", R"({"name": "g.a",
        "source": "tap(\"a.png\")\nreturn true\n", "templates": [{"name": "a"}]})");
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("tap is not allowed in an anchor"),
              std::string::npos)
        << reply.error;
}

TEST_F(GraphTools, SaveScriptEdgeReportsGraph) {
    anchor("b", tex(9), "panel");
    const auto reply =
        call("save_script", R"({"name": "g.a-b", "source": "home\n"})");
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_NE(reply.text.find("graph g: no problems"), std::string::npos)
        << reply.text;
}

TEST_F(GraphTools, ReadGraphShowsWhatIsThere) {
    const auto reply = call("read_graph", R"({"name": "g"})");
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_NE(reply.text.find("  entry [page] desktop\n"), std::string::npos)
        << reply.text;
    EXPECT_NE(reply.text.find("entry -> a (g.entry-a)"), std::string::npos)
        << reply.text;
    EXPECT_NE(reply.text.find("problems: none"), std::string::npos)
        << reply.text;

    const auto unknown = call("read_graph", R"({"name": "nowhere"})");
    EXPECT_FALSE(unknown.ok);
    EXPECT_NE(unknown.error.find("existing graphs: g"), std::string::npos)
        << unknown.error;
}

TEST_F(GraphTools, ReadGraphListsProblems) {
    std::filesystem::remove(m_dir / "g.a" / "a.png");
    const auto reply = call("read_graph", R"({"name": "g"})");
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_NE(reply.text.find("g.a: missing template a.png"), std::string::npos)
        << reply.text;
}

TEST_F(GraphTools, ListScriptsMarksTheGraph) {
    const auto reply = call("list_scripts", "{}");
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_NE(reply.text.find("g.entry -- (no description) [graph g, 2 anchors, "
                              "1 edges]"),
              std::string::npos)
        << reply.text;
    EXPECT_NE(reply.text.find("g.a -- (no description) [anchor of g, page]"),
              std::string::npos)
        << reply.text;
    EXPECT_NE(reply.text.find("[edge of g]"), std::string::npos) << reply.text;
    EXPECT_NE(reply.text.find("g.task -- a task"), std::string::npos)
        << reply.text;
}

TEST_F(GraphTools, RenameAndDeleteKeepTheGraphWhole) {
    const auto rename = call("rename_script",
                             R"({"name": "g.a", "new_name": "g.city"})");
    EXPECT_FALSE(rename.ok);
    EXPECT_NE(rename.error.find("part of graph g"), std::string::npos)
        << rename.error;
    EXPECT_TRUE(exists("g.a/main.ccat"));

    const auto del = call("delete_script", R"({"name": "g.a"})");
    EXPECT_FALSE(del.ok);
    EXPECT_NE(del.error.find("g.entry-a"), std::string::npos) << del.error;
    EXPECT_TRUE(exists("g.a/main.ccat"));
}

TEST_F(GraphTools, AFlatTemplateIsRefused) {
    // A patch of one flat colour has no variance, and normalized correlation is
    // undefined without it: OpenCV scores such a template 1.0 everywhere, so it
    // would match every screen and the anchor over it would be useless.
    const auto flat = call("save_template_box",
                           R"({"bundle": "g.task", "name": "flat",
                               "box": {"x": 60, "y": 55, "w": 20, "h": 20}})");
    EXPECT_FALSE(flat.ok);
    EXPECT_NE(flat.error.find("flat colour"), std::string::npos)
        << flat.error;
    EXPECT_FALSE(exists("g.task/flat.png")) << "nothing is written for a refusal";

    const auto drawn = call("save_template_box",
                            R"({"bundle": "g.task", "name": "mark",
                                "box": {"x": 38, "y": 28, "w": 24, "h": 24}})");
    EXPECT_TRUE(drawn.ok) << drawn.error;
    EXPECT_TRUE(exists("g.task/mark.png"));
}

TEST_F(GraphTools, UpdateScriptRefusesAFlatCut) {
    // The same bar on the other cut path, before anything is written: an update
    // that would leave a template of one colour is not a bundle worth having.
    const auto reply = call("update_script", R"({
        "name": "g.task",
        "source": "tap(\"flat.png\")\n",
        "templates": [{"name": "flat", "cut": {"x": 60, "y": 55, "w": 20, "h": 20}}]
    })");
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("flat colour"), std::string::npos)
        << reply.error;
    EXPECT_FALSE(exists("g.task/flat.png"));
}

} // namespace
