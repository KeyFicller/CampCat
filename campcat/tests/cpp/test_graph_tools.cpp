// The graph tools at the seat the model sits in: what `update_graph` refuses and
// leaves on disk, the anchors `update_script` must not delete, and what a golden
// shot writes. The graph module's own half is in `test_ccat_graph.cpp`.

#include <core/llm/llm_tools.h>
#include <core/script/ccat_graph.h>
#include <core/template_matcher.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <nlohmann/json.hpp>

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

std::string read_file(const std::filesystem::path &_path) {
    std::ifstream in(_path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

/// The matcher the app ships with: the same bar `locate` is held to there.
campcat::template_matcher matcher() {
    return campcat::template_matcher(0.8, 0.96, false);
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

/// A bundle `g` with a graph over two anchors, and the edge between them.
class GraphTools : public ::testing::Test {
protected:
    void SetUp() override {
        m_dir = scratch(::testing::UnitTest::GetInstance()
                            ->current_test_info()
                            ->name());
        m_frame = screen();
        save_png(m_dir / "g" / "a.png", m_frame(kDisc));
        save_png(m_dir / "g" / "b.png", m_frame(kSquare));
        write_file(m_dir / "g" / "main.ccat", "// toy\nlog(\"g\")\n");
        write_file(m_dir / "g.a-b" / "main.ccat", "log(\"hop\")\n");
        write_file(m_dir / "g" / "graph.json", kGraph);

        std::vector<unsigned char> png;
        cv::imencode(".png", m_frame, png);
        campcat::llm::tool_context ctx;
        ctx.screen.w = kScreenW;
        ctx.screen.h = kScreenH;
        ctx.screen.png.assign(png.begin(), png.end());
        // No `needs_approval`: these cases are about what the tools do, and a
        // context without one gates nothing.
        ctx.script_dir = m_dir;
        campcat::llm::set_context(ctx);
    }

    void TearDown() override { campcat::llm::set_context({}); }

    static campcat::llm::tool_reply call(const std::string &_name,
                                         const std::string &_args) {
        return campcat::llm::dispatch(_name, _args);
    }

    bool exists(const std::string &_rel) const {
        return std::filesystem::is_regular_file(m_dir / _rel);
    }

    std::filesystem::path m_dir;
    cv::Mat m_frame;

    static constexpr const char *kGraph = R"({
  "root": "a",
  "anchors": { "a": {"must": ["a.png"]}, "b": {"must": ["b.png"]} },
  "edges": [{ "from": "a", "to": "b", "script": "g.a-b" }]
})";
};

/// The hazard one update away: the anchors live in graph.json, not in the
/// script, so an update that only declared its own operands would delete them.
TEST_F(GraphTools, UpdateScriptKeepsTheGraphsAnchors) {
    write_file(m_dir / "g" / "stray.png", read_file(m_dir / "g" / "a.png"));
    const std::string args = R"({
        "name": "g",
        "source": "tap(\"c.png\")\n",
        "templates": [{"name": "c", "cut": {"x": 0, "y": 0, "w": 100, "h": 80}}]
    })";
    const auto reply = call("update_script", args);
    ASSERT_TRUE(reply.ok) << reply.error;

    EXPECT_TRUE(exists("g/c.png")) << "the cut the update declared is written";
    EXPECT_TRUE(exists("g/a.png")) << "an anchor of the graph survives";
    EXPECT_TRUE(exists("g/b.png")) << "an anchor of the graph survives";
    EXPECT_FALSE(exists("g/stray.png")) << "what nobody declares still goes";
    EXPECT_NE(reply.text.find("1 deleted"), std::string::npos) << reply.text;
}

TEST_F(GraphTools, UpdateScriptRefusesWhileTheGraphWillNotParse) {
    write_file(m_dir / "g" / "graph.json", "{ not json");
    const auto reply = call("update_script", R"({
        "name": "g",
        "source": "log(\"hi\")\n",
        "templates": []
    })");
    EXPECT_FALSE(reply.ok);
    // Refused rather than deleting what it cannot account for.
    EXPECT_NE(reply.error.find("graph.json"), std::string::npos) << reply.error;
    EXPECT_TRUE(exists("g/a.png"));
}

TEST_F(GraphTools, UpdateGraphRefusesWithoutWritingAnything) {
    // An edge whose bundle is not there: the submission is checked against the
    // disk, so this is refused before the file is touched.
    std::string bad = kGraph;
    bad.replace(bad.find("g.a-b"), 5, "g.a-x");
    const std::string args =
        nlohmann::json{{"name", "g"}, {"graph_json", bad}}.dump();
    const auto reply = call("update_graph", args);
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("not written"), std::string::npos)
        << reply.error;
    EXPECT_NE(reply.error.find("g.a-x"), std::string::npos) << reply.error;
    EXPECT_EQ(read_file(m_dir / "g" / "graph.json"), kGraph);
}

TEST_F(GraphTools, ReadGraphShowsWhatIsThere) {
    const auto reply = call("read_graph", R"({"name": "g"})");
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_NE(reply.text.find("anchors: 2 (a, b)"), std::string::npos)
        << reply.text;
    EXPECT_NE(reply.text.find("a.png ok"), std::string::npos) << reply.text;
    EXPECT_NE(reply.text.find("\"root\": \"a\""), std::string::npos)
        << reply.text;

    const auto missing = call("read_graph", R"({"name": "g.a-b"})");
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.find("no graph.json"), std::string::npos)
        << missing.error;

    const auto unknown = call("read_graph", R"({"name": "nowhere"})");
    EXPECT_FALSE(unknown.ok);
    EXPECT_NE(unknown.error.find("save_script"), std::string::npos)
        << unknown.error;
}

TEST_F(GraphTools, ReadGraphHandsBackAFileThatWillNotLoad) {
    // A hand-broken graph.json is still readable, with its problems named: that
    // is the one way the model can fix it without a file editor.
    write_file(m_dir / "g" / "graph.json", R"({
      "root": "a",
      "anchors": { "a": {"must": ["gone.png"]} },
      "edges": []
    })");
    const auto reply = call("read_graph", R"({"name": "g"})");
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_NE(reply.text.find("does not load"), std::string::npos) << reply.text;
    EXPECT_NE(reply.text.find("missing template gone.png"), std::string::npos)
        << reply.text;
    EXPECT_NE(reply.text.find("\"root\": \"a\""), std::string::npos)
        << reply.text;
}

TEST_F(GraphTools, SaveGoldenCutsACropAndItsSidecar) {
    const auto whole = call("save_golden", R"({
        "name": "g", "anchor": "a",
        "box": {"x": 0, "y": 0, "w": 100, "h": 80}
    })");
    ASSERT_TRUE(whole.ok) << whole.error;
    EXPECT_TRUE(exists("g/golden/a/0.png"));
    EXPECT_TRUE(exists("g/golden/a/0.json"));
    std::string err;
    const auto meta = campcat::ccat_lang::golden_meta_from_text(
        read_file(m_dir / "g" / "golden" / "a" / "0.json"), &err);
    ASSERT_TRUE(meta.has_value()) << err;
    EXPECT_EQ(meta->screen_w, kScreenW);
    EXPECT_EQ(meta->w, kScreenW);

    // The same number again keeps both rather than overwriting.
    const auto again = call("save_golden", R"({
        "name": "g", "anchor": "a",
        "box": {"x": 0, "y": 0, "w": 100, "h": 80}
    })");
    ASSERT_TRUE(again.ok) << again.error;
    EXPECT_TRUE(exists("g/golden/a/0-2.png"));
}

TEST_F(GraphTools, AGoldenCropOnlyCoversItsOwnBox) {
    const auto loaded = campcat::ccat_lang::load_graph(m_dir, "g");
    ASSERT_TRUE(loaded.ok) << loaded.error;

    // A crop of the disc: put back on a blank screen it holds a and not b.
    const auto tight = call("save_golden", R"({
        "name": "g", "anchor": "a",
        "box": {"x": 38, "y": 28, "w": 24, "h": 24}
    })");
    ASSERT_TRUE(tight.ok) << tight.error;
    EXPECT_TRUE(
        campcat::ccat_lang::golden_mismatches(loaded.graph, matcher()).empty());

    // The whole screen holds both marks, so this one is ambiguous -- the
    // mismatch a golden shot exists to report.
    const auto wide = call("save_golden", R"({
        "name": "g", "anchor": "a",
        "box": {"x": 0, "y": 0, "w": 100, "h": 80}
    })");
    ASSERT_TRUE(wide.ok) << wide.error;
    const std::vector<std::string> bad =
        campcat::ccat_lang::golden_mismatches(loaded.graph, matcher());
    ASSERT_EQ(bad.size(), 1u) << (bad.empty() ? "" : bad[0]);
    EXPECT_NE(bad[0].find("ambiguous"), std::string::npos) << bad[0];
}

TEST_F(GraphTools, SaveGoldenRefusesAnAnchorNobodyDeclared) {
    const auto reply = call("save_golden", R"({
        "name": "g", "anchor": "c",
        "box": {"x": 0, "y": 0, "w": 100, "h": 80}
    })");
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("has no anchor \"c\""), std::string::npos)
        << reply.error;
    EXPECT_NE(reply.error.find("a, b"), std::string::npos) << reply.error;
}

TEST_F(GraphTools, ListScriptsMarksTheGraphAndItsEdge) {
    const auto reply = call("list_scripts", "{}");
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_NE(reply.text.find("[graph, 2 anchors, 1 edges]"), std::string::npos)
        << reply.text;
    EXPECT_NE(reply.text.find("[edge of g]"), std::string::npos) << reply.text;
}

TEST_F(GraphTools, AFlatTemplateIsRefused) {
    // A patch of one flat colour has no variance, and normalized correlation is
    // undefined without it: OpenCV scores such a template 1.0 everywhere, so it
    // would match every screen and the anchor over it would be useless.
    const auto flat = call("save_template_box",
                           R"({"bundle": "g", "name": "flat",
                               "box": {"x": 60, "y": 55, "w": 20, "h": 20}})");
    EXPECT_FALSE(flat.ok);
    EXPECT_NE(flat.error.find("flat colour"), std::string::npos)
        << flat.error;
    EXPECT_FALSE(exists("g/flat.png")) << "nothing is written for a refusal";

    const auto drawn = call("save_template_box",
                            R"({"bundle": "g", "name": "mark",
                                "box": {"x": 38, "y": 28, "w": 24, "h": 24}})");
    EXPECT_TRUE(drawn.ok) << drawn.error;
    EXPECT_TRUE(exists("g/mark.png"));
}

TEST_F(GraphTools, UpdateScriptRefusesAFlatCut) {
    // The same bar on the other cut path, before anything is written: an update
    // that would leave a template of one colour is not a bundle worth having.
    const auto reply = call("update_script", R"({
        "name": "g",
        "source": "tap(\"flat.png\")\n",
        "templates": [{"name": "flat", "cut": {"x": 60, "y": 55, "w": 20, "h": 20}}]
    })");
    EXPECT_FALSE(reply.ok);
    EXPECT_NE(reply.error.find("flat colour"), std::string::npos)
        << reply.error;
    EXPECT_FALSE(exists("g/flat.png"));
    EXPECT_TRUE(exists("g/a.png")) << "the refusal touches nothing";
}

} // namespace
