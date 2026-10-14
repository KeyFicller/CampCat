#include <core/adb_client.h>
#include <core/app_config.h>
#include <core/script/ccat_bundle.h>
#include <core/script/ccat_graph.h>
#include <core/script/ccat_interpreter.h>
#include <core/script/ccat_parser.h>
#include <core/template_matcher.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using campcat::ccat_lang::bundle_run;
using campcat::ccat_lang::bundle_tags;
using campcat::ccat_lang::golden_meta;
using campcat::ccat_lang::golden_meta_from_text;
using campcat::ccat_lang::golden_meta_to_text;
using campcat::ccat_lang::golden_mismatches;
using campcat::ccat_lang::graph_summary;
using campcat::ccat_lang::graph_template_names;
using campcat::ccat_lang::load_graph;
using campcat::ccat_lang::load_graph_text;
using campcat::ccat_lang::locate;
using campcat::ccat_lang::locate_kind;
using campcat::ccat_lang::nav_io;
using campcat::ccat_lang::nav_limits;
using campcat::ccat_lang::navigate;
using campcat::ccat_lang::parse_error;
using campcat::ccat_lang::parse_program;
using campcat::ccat_lang::screen_graph;
using campcat::template_matcher;

std::filesystem::path scratch(const std::string &_name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "campcat_graph_test" / _name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path &_path, const std::string &_text) {
    std::filesystem::create_directories(_path.parent_path());
    std::ofstream out(_path);
    out << _text;
}

void save_png(const std::filesystem::path &_path, const cv::Mat &_img) {
    std::filesystem::create_directories(_path.parent_path());
    cv::imwrite(_path.string(), _img);
}

/// A golden shot as `save_golden` writes one: the crop, plus the sidecar saying
/// where on the screen it was cut from.
void save_golden(const std::filesystem::path &_dir, const std::string &_n,
                 const cv::Mat &_frame, int _x, int _y, int _w, int _h) {
    campcat::ccat_lang::golden_meta meta;
    meta.screen_w = _frame.cols;
    meta.screen_h = _frame.rows;
    meta.x = _x;
    meta.y = _y;
    meta.w = _w;
    meta.h = _h;
    save_png(_dir / (_n + ".png"), _frame(cv::Rect(_x, _y, _w, _h)).clone());
    write_file(_dir / (_n + ".json"),
               campcat::ccat_lang::golden_meta_to_text(meta));
}

cv::Mat tex(int _seed) {
    cv::Mat m(16, 16, CV_8UC3);
    cv::RNG rng(_seed);
    rng.fill(m, cv::RNG::UNIFORM, 0, 255);
    m.at<cv::Vec3b>(0, 0) = cv::Vec3b(0, 0, 0);
    m.at<cv::Vec3b>(1, 1) = cv::Vec3b(255, 255, 255);
    return m;
}

void paste(cv::Mat *_screen, const cv::Mat &_patch, int _x, int _y) {
    _patch.copyTo((*_screen)(cv::Rect(_x, _y, _patch.cols, _patch.rows)));
}

cv::Mat blank() { return cv::Mat(60, 80, CV_8UC3, cv::Scalar(30, 30, 30)); }

template_matcher matcher() { return template_matcher(0.8, 0.96, false); }

/// Two anchors, one edge, both bundles parse. Templates are real PNGs.
struct toy {
    std::filesystem::path dir;
    cv::Mat mark_a = tex(1);
    cv::Mat mark_b = tex(2);
    cv::Mat frame_a = blank();
    cv::Mat frame_b = blank();

    explicit toy(const std::string &_name) : dir(scratch(_name)) {
        paste(&frame_a, mark_a, 4, 4);
        paste(&frame_b, mark_b, 40, 20);
        save_png(dir / "g" / "a.png", mark_a);
        save_png(dir / "g" / "b.png", mark_b);
        write_file(dir / "g" / "main.ccat", "// toy graph\nlog(\"graph\")\n");
        write_file(dir / "g.a-b" / "main.ccat", "log(\"hop\")\n");
        write_file(dir / "g.recover" / "main.ccat", "log(\"recover\")\n");
        write_file(dir / "g" / "graph.json", R"({
          "root": "a",
          "recover": "g.recover",
          "anchors": { "a": {"must": ["a.png"]}, "b": {"must": ["b.png"]} },
          "edges": [{ "from": "a", "to": "b", "script": "g.a-b" }]
        })");
    }
};

TEST(CcatGraph, LoadReportsEveryProblem) {
    const std::filesystem::path dir = scratch("load_errors");
    std::filesystem::create_directories(dir / "g");
    write_file(dir / "g" / "main.ccat", "log(\"x\")\n");
    write_file(dir / "g" / "graph.json", R"({
      "root": "missing",
      "anchors": { "a": {"must": ["nope.png"], "extra": 1} },
      "edges": [],
      "nope": true
    })");
    const auto loaded = load_graph(dir, "g");
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error.find("unknown field \"nope\""), std::string::npos);
    EXPECT_NE(loaded.error.find("unknown field \"extra\""), std::string::npos);
    EXPECT_NE(loaded.error.find("missing template nope.png"), std::string::npos);
    EXPECT_NE(loaded.error.find("root \"missing\" is not an anchor"),
              std::string::npos);
}

TEST(CcatGraph, LoadRejectsAnUnreachableAnchorAndABadEdge) {
    const std::filesystem::path dir = scratch("unreachable");
    save_png(dir / "g" / "a.png", tex(1));
    save_png(dir / "g" / "b.png", tex(2));
    write_file(dir / "g" / "main.ccat", "log(\"x\")\n");
    write_file(dir / "g" / "graph.json", R"({
      "root": "a",
      "anchors": { "a": {"must": ["a.png"]}, "b": {"must": ["b.png"]} },
      "edges": [
        {"from": "a", "to": "b", "script": "missing.bundle"},
        {"from": "a", "to": "b", "script": "missing.bundle", "timeout_ms": 0}
      ]
    })");
    const auto loaded = load_graph(dir, "g");
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error.find("duplicate edge"), std::string::npos);
    EXPECT_NE(loaded.error.find("timeout_ms out of range"), std::string::npos);
    EXPECT_NE(loaded.error.find("cannot read"), std::string::npos) << loaded.error;
}

TEST(CcatGraph, LocateOneNoneManyAndAPopup) {
    toy g("locate");
    const screen_graph graph = load_graph(g.dir, "g").graph;
    const template_matcher m = matcher();

    const auto on_a = locate(graph, m, g.frame_a);
    EXPECT_EQ(on_a.kind, locate_kind::page);
    EXPECT_EQ(on_a.id, "a");

    const auto none = locate(graph, m, blank());
    EXPECT_EQ(none.kind, locate_kind::unknown);

    cv::Mat both = g.frame_a.clone();
    paste(&both, g.mark_b, 40, 20);
    const auto amb = locate(graph, m, both);
    EXPECT_EQ(amb.kind, locate_kind::ambiguous);
    EXPECT_NE(amb.detail.find("a"), std::string::npos);
    EXPECT_NE(amb.detail.find("b"), std::string::npos);
}

TEST(CcatGraph, MustNotAndRoi) {
    const std::filesystem::path dir = scratch("roi");
    const cv::Mat mark = tex(3);
    const cv::Mat other = tex(4);
    save_png(dir / "g" / "mark.png", mark);
    save_png(dir / "g" / "other.png", other);
    write_file(dir / "g" / "main.ccat", "log(\"x\")\n");
    write_file(dir / "g" / "graph.json", R"({
      "root": "left",
      "anchors": {
        "left": {"must": [{"png": "mark.png", "roi": [0, 0, 0.4, 1]}]},
        "quiet": {"must": ["other.png"], "must_not": ["mark.png"]}
      },
      "edges": []
    })");
    // "quiet" is not reachable. Give it an edge so the graph loads.
    write_file(dir / "g.left-quiet" / "main.ccat", "log(\"x\")\n");
    write_file(dir / "g" / "graph.json", R"({
      "root": "left",
      "anchors": {
        "left": {"must": [{"png": "mark.png", "roi": [0, 0, 0.4, 1]}]},
        "quiet": {"must": ["other.png"], "must_not": ["mark.png"]}
      },
      "edges": [{"from": "left", "to": "quiet", "script": "g.left-quiet"}]
    })");
    const auto loaded = load_graph(dir, "g");
    ASSERT_TRUE(loaded.ok) << loaded.error;
    const template_matcher m = matcher();

    cv::Mat right = blank();
    paste(&right, mark, 50, 4); // outside the left roi
    EXPECT_EQ(locate(loaded.graph, m, right).kind, locate_kind::unknown);

    cv::Mat left = blank();
    paste(&left, mark, 2, 4);
    const auto hit = locate(loaded.graph, m, left);
    EXPECT_EQ(hit.kind, locate_kind::page);
    EXPECT_EQ(hit.id, "left");

    cv::Mat noisy = blank();
    paste(&noisy, other, 4, 4);
    paste(&noisy, mark, 50, 20);
    EXPECT_EQ(locate(loaded.graph, m, noisy).kind, locate_kind::unknown);
}

TEST(CcatGraph, PopupWinsOverThePageUnderIt) {
    const std::filesystem::path dir = scratch("popup");
    const cv::Mat page = tex(5);
    const cv::Mat popup = tex(6);
    save_png(dir / "g" / "page.png", page);
    save_png(dir / "g" / "popup.png", popup);
    write_file(dir / "g" / "main.ccat", "log(\"x\")\n");
    write_file(dir / "g.close" / "main.ccat", "log(\"close\")\n");
    write_file(dir / "g" / "graph.json", R"({
      "root": "page",
      "anchors": {
        "page": {"must": ["page.png"]},
        "popup": {"must": ["popup.png"]}
      },
      "edges": [],
      "interrupts": [{"anchor": "popup", "script": "g.close"}]
    })");
    const auto loaded = load_graph(dir, "g");
    ASSERT_TRUE(loaded.ok) << loaded.error;
    cv::Mat frame = blank();
    paste(&frame, page, 4, 4);
    paste(&frame, popup, 40, 20);
    const auto hit = locate(loaded.graph, matcher(), frame);
    EXPECT_EQ(hit.kind, locate_kind::interrupt);
    EXPECT_EQ(hit.id, "popup");
}

nav_io io_of(std::function<bool(cv::Mat *)> _cap,
            std::function<bundle_run(const std::string &)> _run,
            std::vector<std::string> *_logs) {
    nav_io io;
    io.capture = std::move(_cap);
    io.should_stop = [] { return false; };
    io.run_bundle = std::move(_run);
    io.log = [_logs](const std::string &_line) { _logs->push_back(_line); };
    return io;
}

TEST(CcatGraph, GotoAlreadyThereTakesOneCapture) {
    toy g("already");
    const screen_graph graph = load_graph(g.dir, "g").graph;
    int caps = 0;
    std::vector<std::string> logs;
    const nav_io io = io_of(
        [&](cv::Mat *_out) {
            ++caps;
            *_out = g.frame_b;
            return true;
        },
        [](const std::string &) { return bundle_run{}; }, &logs);
    const auto got = navigate(graph, "b", matcher(), io);
    EXPECT_TRUE(got.ok);
    EXPECT_EQ(caps, 1);
}

TEST(CcatGraph, GotoOneHopThenAReplan) {
    toy g("hop");
    // c is a side page the first hop lands on, with its own edge to b.
    save_png(g.dir / "g" / "c.png", tex(7));
    write_file(g.dir / "g.c-b" / "main.ccat", "log(\"side\")\n");
    write_file(g.dir / "g" / "graph.json", R"({
      "root": "a",
      "anchors": {
        "a": {"must": ["a.png"]},
        "b": {"must": ["b.png"]},
        "c": {"must": ["c.png"]}
      },
      "edges": [
        {"from": "a", "to": "b", "script": "g.a-b"},
        {"from": "a", "to": "c", "script": "g.a-b"},
        {"from": "c", "to": "b", "script": "g.c-b"}
      ]
    })");
    const auto loaded = load_graph(g.dir, "g");
    ASSERT_TRUE(loaded.ok) << loaded.error;
    cv::Mat frame_c = blank();
    paste(&frame_c, tex(7), 4, 30);

    int step = 0;
    std::vector<std::string> ran;
    std::vector<std::string> logs;
    const nav_io io = io_of(
        [&](cv::Mat *_out) {
            if (step == 0) {
                *_out = g.frame_a;
            } else if (step == 1) {
                *_out = frame_c;
            } else {
                *_out = g.frame_b;
            }
            return true;
        },
        [&](const std::string &_bundle) {
            ran.push_back(_bundle);
            ++step;
            return bundle_run{};
        },
        &logs);
    const auto got = navigate(loaded.graph, "b", matcher(), io);
    EXPECT_TRUE(got.ok) << got.message;
    ASSERT_EQ(ran.size(), 2u);
    EXPECT_EQ(ran[0], "g.a-b");
    EXPECT_EQ(ran[1], "g.c-b");
    EXPECT_FALSE(logs.empty());
}

TEST(CcatGraph, TimeoutThenRecover) {
    toy g("recover");
    const screen_graph graph = load_graph(g.dir, "g").graph;
    // The edge's timeout is edited to 0: one look, then it counts as a miss.
    screen_graph edited = graph;
    edited.edges[0].timeout_ms = 0;
    bool recovered = false;
    int hops = 0;
    std::vector<std::string> logs;
    const nav_io io = io_of(
        [&](cv::Mat *_out) {
            *_out = recovered && hops > 0 ? g.frame_b : g.frame_a;
            return true;
        },
        [&](const std::string &_bundle) {
            if (_bundle == "g.recover") {
                recovered = true;
                return bundle_run{};
            }
            ++hops;
            return bundle_run{};
        },
        &logs);
    nav_limits limits;
    limits.max_failures = 2;
    limits.poll_ms = 0;
    const auto got = navigate(edited, "b", matcher(), io, limits);
    EXPECT_TRUE(got.ok) << got.message;
    EXPECT_TRUE(recovered);
}

TEST(CcatGraph, NoPathAndNoRecoverFailsWithTheTrace) {
    toy g("nopath");
    write_file(g.dir / "g" / "graph.json", R"({
      "root": "a",
      "anchors": { "a": {"must": ["a.png"]}, "b": {"must": ["b.png"]} },
      "edges": []
    })");
    // b is unreachable, so this graph does not load. Navigate is given the
    // struct anyway: the walk is what this case checks.
    screen_graph graph;
    graph.name = "g";
    graph.dir = g.dir / "g";
    graph.root = "a";
    graph.anchors.push_back({"a", {{"a.png"}}, {}, false});
    graph.anchors.push_back({"b", {{"b.png"}}, {}, false});
    std::vector<std::string> logs;
    const nav_io io = io_of(
        [&](cv::Mat *_out) {
            *_out = g.frame_a;
            return true;
        },
        [](const std::string &) { return bundle_run{}; }, &logs);
    nav_limits limits;
    limits.max_failures = 1;
    const auto got = navigate(graph, "b", matcher(), io, limits);
    EXPECT_FALSE(got.ok);
    EXPECT_NE(got.message.find("no path from a"), std::string::npos) << got.message;
}

TEST(CcatGraph, DeadDeviceIsNotAMiss) {
    toy g("device");
    const screen_graph graph = load_graph(g.dir, "g").graph;
    std::vector<std::string> logs;
    const nav_io io = io_of([](cv::Mat *) { return false; },
                            [](const std::string &) { return bundle_run{}; },
                            &logs);
    const auto got = navigate(graph, "b", matcher(), io);
    EXPECT_FALSE(got.ok);
    EXPECT_TRUE(got.device);
}

TEST(CcatGraph, GoldenShotMustLocateAsItself) {
    toy g("golden");
    const auto loaded = load_graph(g.dir, "g");
    ASSERT_TRUE(loaded.ok);
    save_golden(g.dir / "g" / "golden" / "a", "1", g.frame_a, 0, 0, 80, 60);
    save_golden(g.dir / "g" / "golden" / "b", "1", g.frame_a, 0, 0, 80, 60);
    const std::vector<std::string> bad =
        golden_mismatches(loaded.graph, matcher());
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_NE(bad[0].find("b"), std::string::npos);
}

TEST(CcatGraph, GoldenCropKeepsRoiSemantics) {
    toy g("golden_roi");
    // a looks only at the left half, b only at the right: a golden crop put
    // back at the pixel it came from keeps meaning the region it meant.
    write_file(g.dir / "g" / "graph.json", R"({
      "root": "a",
      "anchors": {
        "a": {"must": [{"png": "a.png", "roi": [0, 0, 0.5, 1]}]},
        "b": {"must": [{"png": "b.png", "roi": [0.5, 0, 0.5, 1]}]}
      },
      "edges": [{ "from": "a", "to": "b", "script": "g.a-b" }]
    })");
    const auto loaded = load_graph(g.dir, "g");
    ASSERT_TRUE(loaded.ok) << loaded.error;

    // The left half holds a's mark, so this crop still locates as a.
    save_golden(g.dir / "g" / "golden" / "a", "1", g.frame_a, 0, 0, 40, 60);
    EXPECT_TRUE(golden_mismatches(loaded.graph, matcher()).empty());

    // The right half is blank: put back where it came from it still leaves the
    // left half blank, so a no longer matches. That is the blind spot a crop
    // buys, and why the box should be as wide as it can be.
    save_golden(g.dir / "g" / "golden" / "a", "2", g.frame_a, 40, 0, 40, 60);
    const std::vector<std::string> bad =
        golden_mismatches(loaded.graph, matcher());
    ASSERT_EQ(bad.size(), 1u) << (bad.empty() ? "" : bad[0]);
    EXPECT_NE(bad[0].find("a"), std::string::npos) << bad[0];
}

TEST(CcatGraph, GoldenWithoutSidecarIsReported) {
    toy g("golden_nosidecar");
    const auto loaded = load_graph(g.dir, "g");
    ASSERT_TRUE(loaded.ok);
    save_png(g.dir / "g" / "golden" / "a" / "1.png", g.frame_a);
    const std::vector<std::string> bad =
        golden_mismatches(loaded.graph, matcher());
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_NE(bad[0].find("no sidecar"), std::string::npos) << bad[0];
}

TEST(CcatGraph, GoldenSidecarRoundTrips) {
    golden_meta meta;
    meta.screen_w = 80;
    meta.screen_h = 60;
    meta.x = 4;
    meta.y = 4;
    meta.w = 16;
    meta.h = 16;
    std::string err;
    const auto back = golden_meta_from_text(golden_meta_to_text(meta), &err);
    ASSERT_TRUE(back.has_value()) << err;
    EXPECT_EQ(back->screen_w, 80);
    EXPECT_EQ(back->screen_h, 60);
    EXPECT_EQ(back->x, 4);
    EXPECT_EQ(back->y, 4);
    EXPECT_EQ(back->w, 16);
    EXPECT_EQ(back->h, 16);

    EXPECT_FALSE(golden_meta_from_text("{}", &err).has_value());
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(golden_meta_from_text("not json", &err).has_value());
    // A crop that does not fit the screen it names is refused too: nothing
    // downstream could place it.
    golden_meta outside = meta;
    outside.x = 70; // 70 + 16 > 80
    EXPECT_FALSE(golden_meta_from_text(golden_meta_to_text(outside), &err)
                     .has_value());
}

TEST(CcatGraph, GraphTemplateNamesKeepsAnchors) {
    toy g("templates");
    write_file(g.dir / "g" / "graph.json", R"({
      "root": "a",
      "anchors": {
        "a": {"must": ["a.png"], "must_not": ["b.png"]},
        "b": {"must": [{"png": "b.png", "roi": [0.5, 0, 0.5, 1]}]}
      },
      "edges": [{ "from": "a", "to": "b", "script": "g.a-b" }]
    })");
    std::string err;
    const auto names = graph_template_names(g.dir / "g" / "graph.json", &err);
    ASSERT_TRUE(names.has_value()) << err;
    EXPECT_EQ(*names, (std::vector<std::string>{"a.png", "b.png"}));

    // No graph.json is an empty list, not an error.
    const auto none = graph_template_names(g.dir / "g.recover" / "graph.json",
                                           &err);
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->empty());

    // A file that is there but unreadable must not read as "nothing to keep",
    // or update_script would delete the anchors.
    write_file(g.dir / "g" / "graph.json", "{ not json");
    EXPECT_FALSE(graph_template_names(g.dir / "g" / "graph.json", &err)
                     .has_value());
    EXPECT_FALSE(err.empty());
}

TEST(CcatGraph, BundleTagsMarksGraphAndEdge) {
    toy g("tags");
    const auto graph = bundle_tags(g.dir, "g");
    EXPECT_TRUE(graph.graph);
    EXPECT_EQ(graph.anchors, 2);
    EXPECT_EQ(graph.edges, 1);
    EXPECT_TRUE(graph.edge_of.empty());

    const auto edge = bundle_tags(g.dir, "g.a-b");
    EXPECT_FALSE(edge.graph);
    EXPECT_EQ(edge.edge_of, "g");

    // A bundle that shares the graph's prefix but is not one of its edges is
    // not tagged: the name is a convention, `edges[].script` is not.
    const auto recover = bundle_tags(g.dir, "g.recover");
    EXPECT_FALSE(recover.graph);
    EXPECT_TRUE(recover.edge_of.empty());

    const auto plain = bundle_tags(g.dir, "nowhere");
    EXPECT_FALSE(plain.graph);
    EXPECT_TRUE(plain.edge_of.empty());
}

TEST(CcatGraph, GraphSummaryListsTemplatesAndGolden) {
    toy g("summary");
    auto loaded = load_graph(g.dir, "g");
    ASSERT_TRUE(loaded.ok) << loaded.error;
    save_golden(g.dir / "g" / "golden" / "a", "1", g.frame_a, 0, 0, 80, 60);
    std::filesystem::remove(g.dir / "g" / "b.png");
    const std::string text = graph_summary(loaded);
    EXPECT_NE(text.find("anchors: 2 (a, b)"), std::string::npos) << text;
    EXPECT_NE(text.find("edges: 1"), std::string::npos) << text;
    EXPECT_NE(text.find("root: a"), std::string::npos) << text;
    EXPECT_NE(text.find("a.png ok"), std::string::npos) << text;
    EXPECT_NE(text.find("b.png missing"), std::string::npos) << text;
    EXPECT_NE(text.find("golden: a 1, b 0"), std::string::npos) << text;
}

TEST(CcatGraph, UpdateGraphTextIsCheckedAgainstTheDisk) {
    toy g("update_text");
    std::ifstream in(g.dir / "g" / "graph.json");
    const std::string good{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    EXPECT_TRUE(load_graph_text(g.dir, "g", good).ok);
    EXPECT_TRUE(load_graph(g.dir, "g").ok);

    // An edge naming a bundle that is not there: the submission is refused, so
    // nothing would be written.
    std::string swapped = good;
    swapped.replace(swapped.find("g.a-b"), 5, "g.a-x");
    const auto bad_edge = load_graph_text(g.dir, "g", swapped);
    EXPECT_FALSE(bad_edge.ok);
    EXPECT_NE(bad_edge.error.find("cannot read"), std::string::npos)
        << bad_edge.error;

    const auto bad_json = load_graph_text(g.dir, "g", "{ not json");
    EXPECT_FALSE(bad_json.ok);
    EXPECT_NE(bad_json.error.find("graph.json"), std::string::npos)
        << bad_json.error;

    // And the file itself still holds the graph it held before.
    std::ifstream again(g.dir / "g" / "graph.json");
    const std::string after{std::istreambuf_iterator<char>(again),
                            std::istreambuf_iterator<char>()};
    EXPECT_EQ(after, good);
}

TEST(CcatGraph, DeleteAndRenameFollowGraphJson) {
    toy g("refs");
    std::string err;
    EXPECT_EQ(campcat::ccat_lang::check_bundle_deletable(g.dir, "g.a-b", &err),
              campcat::ccat_lang::delete_status::referenced);
    EXPECT_NE(err.find("g"), std::string::npos) << err;

    std::vector<std::string> rewritten;
    EXPECT_EQ(campcat::ccat_lang::rename_bundle(g.dir, "g.a-b", "g.a-b2",
                                                &rewritten, &err),
              campcat::ccat_lang::rename_status::ok)
        << err;
    std::ifstream in(g.dir / "g" / "graph.json");
    const std::string text{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("g.a-b2"), std::string::npos) << text;
    EXPECT_EQ(text.find("g.a-b\""), std::string::npos) << text;
}

class ScriptAdb final : public campcat::adb_api {
public:
    std::vector<cv::Mat> frames;
    std::size_t at = 0;
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
    bool swipe(int, int, int, int, int) override { return true; }
    bool keyevent(int) override { return true; }
    bool home_and_kill_all(int) override { return true; }
    bool screencap_bgr(cv::Mat *_bgr_out, int, std::string *) override {
        if (frames.empty()) {
            return false;
        }
        if (at >= frames.size()) {
            *_bgr_out = frames.back();
            return true;
        }
        *_bgr_out = frames[at++];
        return true;
    }
    bool delay_after_action(std::chrono::milliseconds) const override {
        return true;
    }
};

TEST(CcatGraph, GotoStatementReachesTheAnchor) {
    toy g("stmt");
    ScriptAdb adb;
    adb.frames = {g.frame_a, g.frame_b};
    campcat::app_config cfg;
    cfg.config_home = g.dir.string();
    const auto prog = parse_program("goto(\"g:b\")");
    campcat::ccat_lang::CcatInterpreter interp(
        &adb, &cfg, matcher(), g.dir, std::filesystem::path{});
    const auto result = interp.run(*prog, [] { return false; });
    EXPECT_TRUE(result.ok) << result.message;
}

TEST(CcatGraph, AtIsAConditionAndGotoInsideAnEdgeIsAnError) {
    toy g("at");
    write_file(g.dir / "g.a-b" / "main.ccat", "goto(\"g:b\")\n");
    ScriptAdb adb;
    adb.frames = {g.frame_b, g.frame_a, g.frame_a, g.frame_a};
    campcat::app_config cfg;
    cfg.config_home = g.dir.string();
    campcat::ccat_lang::CcatInterpreter interp(
        &adb, &cfg, matcher(), g.dir, std::filesystem::path{});

    const auto here = interp.run(*parse_program("if (at(\"g:b\")) { log(\"yes\") }"),
                                 [] { return false; });
    EXPECT_TRUE(here.ok) << here.message;

    const auto nested =
        interp.run(*parse_program("goto(\"g:b\")"), [] { return false; });
    EXPECT_FALSE(nested.ok);
    EXPECT_NE(nested.message.find("goto inside a graph script"),
              std::string::npos)
        << nested.message;
}

TEST(CcatGraph, ABadReferenceDoesNotParse) {
    EXPECT_THROW(parse_program("goto(g:b)"), parse_error);
    EXPECT_THROW(parse_program("goto(\"b\")"), parse_error);
    EXPECT_THROW(parse_program("at(\"g:b\")"), parse_error);
}

} // namespace
