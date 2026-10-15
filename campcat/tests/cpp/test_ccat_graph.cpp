#include <core/adb_client.h>
#include <core/app_config.h>
#include <core/automation_log.h>
#include <core/script/ccat_bundle.h>
#include <core/script/ccat_graph.h>
#include <core/script/ccat_interpreter.h>
#include <core/script/ccat_parser.h>
#include <core/template_matcher.h>

#include <opencv2/imgcodecs.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace {

using campcat::ccat_lang::anchor_check;
using campcat::ccat_lang::anchor_check_fn;
using campcat::ccat_lang::anchor_def;
using campcat::ccat_lang::anchor_kind;
using campcat::ccat_lang::bundle_run;
using campcat::ccat_lang::bundle_tags;
using campcat::ccat_lang::graph_action;
using campcat::ccat_lang::graph_edge;
using campcat::ccat_lang::graph_names;
using campcat::ccat_lang::graph_summary;
using campcat::ccat_lang::load_graph;
using campcat::ccat_lang::locate;
using campcat::ccat_lang::locate_kind;
using campcat::ccat_lang::nav_io;
using campcat::ccat_lang::nav_limits;
using campcat::ccat_lang::navigate;
using campcat::ccat_lang::parse_error;
using campcat::ccat_lang::parse_program;
using campcat::ccat_lang::run_action;
using campcat::ccat_lang::running_graph_bundle;
using campcat::ccat_lang::run_edge;
using campcat::ccat_lang::scan_graph;
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

/// Anchor bundle `<dir>/<bundle>` recognised by `<png>` alone.
void anchor(const std::filesystem::path &_dir, const std::string &_bundle,
            const std::string &_png, const cv::Mat &_mark,
            const std::string &_golden = "---\n---\nscreen\n") {
    save_png(_dir / _bundle / _png, _mark);
    write_file(_dir / _bundle / "golden.md", _golden);
    write_file(_dir / _bundle / "main.ccat",
               "if (has(\"" + _png + "\")) { return true }\nreturn false\n");
}

/// entry -> a -> b, every bundle on disk. Each anchor's mark is a real PNG.
struct toy {
    std::filesystem::path dir;
    cv::Mat mark_entry = tex(3);
    cv::Mat mark_a = tex(1);
    cv::Mat mark_b = tex(2);
    cv::Mat frame_entry = blank();
    cv::Mat frame_a = blank();
    cv::Mat frame_b = blank();

    explicit toy(const std::string &_name) : dir(scratch(_name)) {
        paste(&frame_entry, mark_entry, 4, 30);
        paste(&frame_a, mark_a, 4, 4);
        paste(&frame_b, mark_b, 40, 20);
        anchor(dir, "g.entry", "entry.png", mark_entry);
        anchor(dir, "g.a", "a.png", mark_a);
        anchor(dir, "g.b", "b.png", mark_b);
        write_file(dir / "g.entry-a" / "main.ccat", "home\n");
        write_file(dir / "g.a-b" / "main.ccat", "home\n");
    }
};

/// A graph built in memory, for the walk itself.
screen_graph mem_graph(std::vector<std::pair<std::string, anchor_kind>> _anchors,
                       std::vector<std::pair<std::string, std::string>> _edges) {
    screen_graph g;
    g.name = "g";
    for (auto &[id, kind] : _anchors) {
        anchor_def a;
        a.id = id;
        a.kind = kind;
        if (kind == anchor_kind::popup) {
            a.close = "g.close";
        }
        g.anchors.push_back(a);
    }
    for (auto &[from, to] : _edges) {
        g.edges.push_back({from, to, "g." + from + "-" + to});
    }
    return g;
}

/// A frame that "is" anchor `_id`: the check below reads the id back.
cv::Mat shows(const std::string &_id) {
    static std::map<std::string, int> codes;
    const auto [it, _] = codes.emplace(_id, static_cast<int>(codes.size()) + 1);
    return cv::Mat(1, 1, CV_8UC1, cv::Scalar(it->second));
}

anchor_check_fn reads_frame() {
    return [](const anchor_def &_a, const cv::Mat &_frame) {
        return anchor_check{_frame.at<uchar>(0, 0) == shows(_a.id).at<uchar>(0, 0),
                            {}};
    };
}

/// Frames come from `_frames` in turn, the last one repeating.
nav_io io_of(std::vector<cv::Mat> _frames, std::vector<std::string> *_ran,
             std::function<bundle_run(const std::string &)> _run = {}) {
    auto at = std::make_shared<std::size_t>(0);
    nav_io io;
    io.capture = [_frames, at](cv::Mat *_out) {
        *_out = _frames[std::min(*at, _frames.size() - 1)];
        ++*at;
        return true;
    };
    io.should_stop = [] { return false; };
    io.check = reads_frame();
    io.run_bundle = [_ran, _run](const std::string &_b) {
        _ran->push_back(_b);
        return _run ? _run(_b) : bundle_run{};
    };
    io.log = [](const std::string &) {};
    return io;
}

nav_limits quick() {
    nav_limits l;
    l.unknown_ms = 0;
    l.poll_ms = 0;
    l.edge_timeout_ms = 0;
    return l;
}

const auto page = anchor_kind::page;

TEST(CcatGraph, ScanFindsAnchorsAndEdges) {
    toy g("scan");
    write_file(g.dir / "g.plain" / "main.ccat", "log(\"task\")\n");
    const auto scanned = scan_graph(g.dir, "g");
    EXPECT_TRUE(scanned.exists);
    EXPECT_TRUE(scanned.problems.empty())
        << (scanned.problems.empty() ? "" : scanned.problems[0]);
    ASSERT_EQ(scanned.graph.anchors.size(), 3u);
    EXPECT_EQ(scanned.graph.anchors[0].id, "a");
    EXPECT_EQ(scanned.graph.anchors[2].id, "entry");
    EXPECT_EQ(scanned.graph.anchors[0].description, "screen");
    ASSERT_EQ(scanned.graph.edges.size(), 2u);
    EXPECT_EQ(scanned.graph.edges[0].script, "g.a-b");
    EXPECT_TRUE(load_graph(g.dir, "g").ok);
    EXPECT_EQ(graph_names(g.dir), std::vector<std::string>{"g"});
}

TEST(CcatGraph, EdgeNeedsBothEndsAnchors) {
    toy g("ends");
    write_file(g.dir / "g.a-zzz" / "main.ccat", "goto(\"g:a\")\n");
    const auto scanned = scan_graph(g.dir, "g");
    EXPECT_EQ(scanned.graph.edges.size(), 2u);
    bool named = false;
    for (const std::string &p : scanned.problems) {
        if (p.find("g.a-zzz") != std::string::npos &&
            p.find("g.zzz has no golden.md") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named);
    EXPECT_TRUE(bundle_tags(g.dir, "g.a-zzz").edge_of.empty());
    EXPECT_EQ(bundle_tags(g.dir, "g.a-b").edge_of, "g");
}

TEST(CcatGraph, AnchorScriptWithoutGoldenIsReported) {
    toy g("nogolden");
    write_file(g.dir / "g.kucang" / "main.ccat",
               "if (has(\"t.png\")) { return true }\nreturn false\n");
    write_file(g.dir / "g.entry-kucang" / "main.ccat", "home\n");
    const auto scanned = scan_graph(g.dir, "g");
    for (const anchor_def &a : scanned.graph.anchors) {
        EXPECT_NE(a.id, "kucang");
    }
    for (const graph_edge &e : scanned.graph.edges) {
        EXPECT_NE(e.script, "g.entry-kucang");
    }
    bool anchor = false;
    bool edge = false;
    for (const std::string &p : scanned.problems) {
        if (p.find("g.kucang: anchor script with no golden.md") !=
            std::string::npos) {
            anchor = true;
        }
        if (p.find("g.entry-kucang") != std::string::npos &&
            p.find("g.kucang has no golden.md") != std::string::npos) {
            edge = true;
        }
    }
    EXPECT_TRUE(anchor);
    EXPECT_TRUE(edge);
}

TEST(CcatGraph, ScanReportsAll) {
    toy g("problems");
    anchor(g.dir, "g.pop", "pop.png", tex(4), "---\nkind: popup\n---\n");
    write_file(g.dir / "g.b" / "main.ccat", "tap(\"b.png\")\nreturn true\n");
    anchor(g.dir, "g.lost", "lost.png", tex(5));
    std::filesystem::remove(g.dir / "g.a" / "a.png");
    write_file(g.dir / "g.entry-a" / "main.ccat", "goto(\"g:b\")\n");
    const auto scanned = scan_graph(g.dir, "g");
    const auto has = [&](const std::string &_text) {
        for (const std::string &p : scanned.problems) {
            if (p.find(_text) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(has("g.pop: golden.md: a popup needs close"));
    EXPECT_TRUE(has("g.b: tap is not allowed in an anchor"));
    EXPECT_TRUE(has("g.lost is not reachable from entry"));
    EXPECT_TRUE(has("g.a: missing template a.png"));
    EXPECT_TRUE(has("g.entry-a: goto is not allowed in an edge"));
    const auto loaded = load_graph(g.dir, "g");
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error.find("g.lost"), std::string::npos) << loaded.error;
    EXPECT_NE(loaded.error.find("g.pop"), std::string::npos) << loaded.error;

    std::filesystem::remove_all(g.dir / "g.entry");
    const auto headless = scan_graph(g.dir, "g");
    EXPECT_FALSE(headless.exists);
    EXPECT_FALSE(load_graph(g.dir, "g").ok);
    EXPECT_NE(load_graph(g.dir, "g").error.find("does not exist"),
              std::string::npos);
    EXPECT_TRUE(graph_names(g.dir).empty());
}

TEST(CcatGraph, BundleTagsAndSummary) {
    toy g("tags");
    write_file(g.dir / "g.plain" / "main.ccat", "log(\"x\")\n");
    const auto entry = bundle_tags(g.dir, "g.entry");
    EXPECT_EQ(entry.anchor_of, "g");
    EXPECT_EQ(entry.anchors, 3);
    EXPECT_EQ(entry.edges, 2);
    const auto a = bundle_tags(g.dir, "g.a");
    EXPECT_EQ(a.anchor_of, "g");
    EXPECT_EQ(a.anchors, 0);
    EXPECT_TRUE(bundle_tags(g.dir, "g.plain").anchor_of.empty());
    EXPECT_TRUE(bundle_tags(g.dir, "g.plain").edge_of.empty());

    const std::string text = graph_summary(scan_graph(g.dir, "g"));
    EXPECT_NE(text.find("anchors: 3"), std::string::npos) << text;
    EXPECT_NE(text.find("  a [page] screen"), std::string::npos) << text;
    EXPECT_NE(text.find("a -> b (g.a-b)"), std::string::npos) << text;
    EXPECT_NE(text.find("problems: none"), std::string::npos) << text;
}

TEST(CcatGraph, LocatePopupFirst) {
    const screen_graph graph =
        mem_graph({{"a", page}, {"pop", anchor_kind::popup}}, {});
    const auto both = [](const anchor_def &, const cv::Mat &) {
        return anchor_check{true, {}};
    };
    const auto hit = locate(graph, both, shows("a"));
    EXPECT_EQ(hit.kind, locate_kind::interrupt);
    EXPECT_EQ(hit.id, "pop");
}

TEST(CcatGraph, LocateAmbiguous) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}}, {});
    const auto both = [](const anchor_def &, const cv::Mat &) {
        return anchor_check{true, {}};
    };
    const auto hit = locate(graph, both, shows("a"));
    EXPECT_EQ(hit.kind, locate_kind::ambiguous);
    EXPECT_EQ(hit.detail, "ambiguous: a; b");
}

TEST(CcatGraph, LocateOneAndUnknown) {
    const screen_graph graph =
        mem_graph({{"a", page}, {"load", anchor_kind::transient}}, {});
    EXPECT_EQ(locate(graph, reads_frame(), shows("a")).id, "a");
    EXPECT_EQ(locate(graph, reads_frame(), shows("load")).kind,
              locate_kind::transient);
    EXPECT_EQ(locate(graph, reads_frame(), shows("zzz")).kind,
              locate_kind::unknown);
}

TEST(CcatGraph, LocateNamesBrokenAnchor) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}}, {});
    const auto broken = [](const anchor_def &_a, const cv::Mat &) {
        return _a.id == "b" ? anchor_check{false, "cannot read g.b/main.ccat"}
                            : anchor_check{false, {}};
    };
    const auto hit = locate(graph, broken, shows("a"));
    EXPECT_EQ(hit.kind, locate_kind::error);
    EXPECT_EQ(hit.detail, "b anchor failed: cannot read g.b/main.ccat");
}

TEST(CcatGraph, GotoAlreadyThereRunsNothing) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}}, {{"a", "b"}});
    std::vector<std::string> ran;
    EXPECT_TRUE(navigate(graph, "b", io_of({shows("b")}, &ran), quick()).ok);
    EXPECT_TRUE(ran.empty());
}

TEST(CcatGraph, GotoOneHopThenAReplan) {
    // The a->b hop lands on c; that is a replan, not a failure.
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}, {"c", page}},
                                         {{"a", "b"}, {"c", "b"}});
    std::vector<std::string> ran;
    const auto got = navigate(
        graph, "b", io_of({shows("a"), shows("c"), shows("c"), shows("b")}, &ran),
        quick());
    EXPECT_TRUE(got.ok) << got.message;
    EXPECT_EQ(ran, (std::vector<std::string>{"g.a-b", "g.c-b"}));
}

TEST(CcatGraph, GotoClosesAPopup) {
    const screen_graph graph =
        mem_graph({{"a", page}, {"pop", anchor_kind::popup}}, {});
    std::vector<std::string> ran;
    const auto got =
        navigate(graph, "a", io_of({shows("pop"), shows("a")}, &ran), quick());
    EXPECT_TRUE(got.ok) << got.message;
    EXPECT_EQ(ran, std::vector<std::string>{"g.close"});
}

TEST(CcatGraph, GotoThreeFailuresNoRecover) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}}, {{"a", "b"}});
    std::vector<std::string> ran;
    const auto got = navigate(graph, "b", io_of({shows("a")}, &ran), quick());
    EXPECT_FALSE(got.ok);
    EXPECT_EQ(ran.size(), 3u);
    EXPECT_NE(got.message.find("a -[g.a-b]-> timeout 0 ms (last: a)"),
              std::string::npos)
        << got.message;
}

TEST(CcatGraph, NoPathFailsWithTheTrace) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}}, {});
    std::vector<std::string> ran;
    nav_limits limits = quick();
    limits.max_failures = 1;
    const auto got = navigate(graph, "b", io_of({shows("a")}, &ran), limits);
    EXPECT_FALSE(got.ok);
    EXPECT_NE(got.message.find("no path from a"), std::string::npos)
        << got.message;
}

TEST(CcatGraph, DeadDeviceIsNotAMiss) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}}, {{"a", "b"}});
    std::vector<std::string> ran;
    nav_io io = io_of({shows("a")}, &ran);
    io.capture = [](cv::Mat *) { return false; };
    const auto got = navigate(graph, "b", io, quick());
    EXPECT_FALSE(got.ok);
    EXPECT_TRUE(got.device);
}

TEST(CcatGraph, RunEdgeNotAtFrom) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}}, {{"a", "b"}});
    std::vector<std::string> ran;
    const auto got =
        run_edge(graph, graph.edges[0], io_of({shows("b")}, &ran), quick());
    EXPECT_FALSE(got.ok);
    EXPECT_EQ(got.message, "run g.a-b: not at a (at b)");
    EXPECT_TRUE(ran.empty());
}

TEST(CcatGraph, RunEdgeArrives) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}}, {{"a", "b"}});
    std::vector<std::string> ran;
    const auto got = run_edge(graph, graph.edges[0],
                              io_of({shows("a"), shows("b")}, &ran), quick());
    EXPECT_TRUE(got.ok) << got.message;
    EXPECT_EQ(ran, std::vector<std::string>{"g.a-b"});
}

TEST(CcatGraph, RunEdgeTimeoutAndLandedElsewhere) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}, {"c", page}},
                                         {{"a", "b"}});
    std::vector<std::string> ran;
    const auto stuck =
        run_edge(graph, graph.edges[0], io_of({shows("a")}, &ran), quick());
    EXPECT_EQ(stuck.message, "run g.a-b: timeout 0 ms (last: a)");
    const auto off = run_edge(graph, graph.edges[0],
                              io_of({shows("a"), shows("c")}, &ran), quick());
    EXPECT_EQ(off.message, "run g.a-b: landed on c");
}

TEST(CcatGraph, RunActionNotAtAnchor) {
    screen_graph graph =
        mem_graph({{"kucang", page}, {"wujiang", page}}, {});
    std::vector<std::string> ran;
    const graph_action claim{"kucang", "claim", "g.kucang.claim"};
    const auto got =
        run_action(graph, claim, io_of({shows("wujiang")}, &ran), quick());
    EXPECT_FALSE(got.ok);
    EXPECT_EQ(got.message, "run g.kucang.claim: not at kucang (at wujiang)");
    EXPECT_TRUE(ran.empty());
}

TEST(CcatGraph, RunActionReturns) {
    screen_graph graph =
        mem_graph({{"kucang", page}, {"wujiang", page}}, {});
    std::vector<std::string> ran;
    const graph_action claim{"kucang", "claim", "g.kucang.claim"};
    const auto got = run_action(
        graph, claim, io_of({shows("kucang"), shows("kucang")}, &ran), quick());
    EXPECT_TRUE(got.ok) << got.message;
    EXPECT_EQ(ran, std::vector<std::string>{"g.kucang.claim"});
}

TEST(CcatGraph, RunActionLandsElsewhereOnce) {
    screen_graph graph =
        mem_graph({{"kucang", page}, {"wujiang", page}}, {});
    std::vector<std::string> ran;
    const graph_action claim{"kucang", "claim", "g.kucang.claim"};
    const auto got = run_action(
        graph, claim, io_of({shows("kucang"), shows("wujiang")}, &ran), quick());
    EXPECT_NE(got.message.find("landed on wujiang"), std::string::npos)
        << got.message;
    EXPECT_EQ(ran, std::vector<std::string>{"g.kucang.claim"});
}

TEST(CcatGraph, GotoDoesNotRunAnAction) {
    screen_graph graph =
        mem_graph({{"entry", page}, {"kucang", page}}, {});
    graph.actions.push_back({"kucang", "claim", "g.kucang.claim"});
    std::vector<std::string> ran;
    const auto got =
        navigate(graph, "kucang", io_of({shows("entry")}, &ran), quick());
    EXPECT_FALSE(got.ok);
    EXPECT_NE(got.message.find("no path"), std::string::npos) << got.message;
    EXPECT_TRUE(ran.empty());
}

TEST(CcatGraph, RunEdgeStopIsNotFailure) {
    const screen_graph graph = mem_graph({{"a", page}, {"b", page}}, {{"a", "b"}});
    std::vector<std::string> ran;
    const auto got = run_edge(
        graph, graph.edges[0], io_of({shows("a")}, &ran, [](const std::string &) {
            bundle_run r;
            r.stopped = true;
            return r;
        }),
        quick());
    EXPECT_TRUE(got.stopped);
    EXPECT_FALSE(got.ok);
    EXPECT_TRUE(got.message.empty()) << got.message;
}

class ScriptAdb final : public campcat::adb_api {
public:
    std::vector<cv::Mat> frames;
    std::size_t at = 0;
    int captures = 0;
    int homes = 0;
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
    bool home_and_kill_all(int) override {
        ++homes;
        return true;
    }
    bool screencap_bgr(cv::Mat *_bgr_out, int, std::string *) override {
        ++captures;
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

struct device {
    ScriptAdb adb;
    campcat::app_config cfg;
    std::unique_ptr<campcat::ccat_lang::CcatInterpreter> interp;

    explicit device(const toy &_g) {
        cfg.config_home = _g.dir.string();
        interp = std::make_unique<campcat::ccat_lang::CcatInterpreter>(
            &adb, &cfg, matcher(), _g.dir, std::filesystem::path{});
    }
    campcat::automation_cycle_result run(const std::string &_source) {
        return interp->run(*parse_program(_source), [] { return false; });
    }
};

TEST(CcatGraph, GotoRunsEdgeBodyOnce) {
    toy g("goto");
    device d(g);
    d.adb.frames = {g.frame_a, g.frame_b};
    const auto result = d.run("goto(\"g:b\")");
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_EQ(d.adb.homes, 1);
    EXPECT_EQ(d.adb.captures, 2) << "one locate before the hop, one after";
}

TEST(CcatGraph, RunOnAnEdgeChecksBothEnds) {
    toy g("run_edge");
    device d(g);
    d.adb.frames = {g.frame_a, g.frame_b};
    const auto ok = d.run("run(\"g.a-b/main.ccat\")");
    EXPECT_TRUE(ok.ok) << ok.message;
    EXPECT_EQ(d.adb.homes, 1);

    device off(g);
    off.adb.frames = {g.frame_b};
    const auto wrong = off.run("run(\"g.a-b/main.ccat\")");
    EXPECT_FALSE(wrong.ok);
    EXPECT_NE(wrong.message.find("run g.a-b: not at a (at b)"),
              std::string::npos)
        << wrong.message;
    EXPECT_EQ(off.adb.homes, 0) << "not one action of the edge ran";
}

TEST(CcatGraph, RunOnAnActionChecksBothEnds) {
    toy g("run_action");
    write_file(g.dir / "g.a.claim" / "main.ccat", "home\n");
    device there(g);
    there.adb.frames = {g.frame_a, g.frame_a};
    const auto ok = there.run("run(\"g.a.claim/main.ccat\")");
    EXPECT_TRUE(ok.ok) << ok.message;
    EXPECT_EQ(there.adb.homes, 1);

    device off(g);
    off.adb.frames = {g.frame_b};
    const auto wrong = off.run("run(\"g.a.claim/main.ccat\")");
    EXPECT_FALSE(wrong.ok);
    EXPECT_NE(wrong.message.find("not at a"), std::string::npos)
        << wrong.message;
    EXPECT_EQ(off.adb.homes, 0);
}

TEST(CcatGraph, RunPublishesTheBundleWhileItRuns) {
    toy g("publish");
    write_file(g.dir / "g.a.claim" / "main.ccat", "home\n");
    std::string seen;
    const auto sink = campcat::automation_log::add_sink([&](const std::string &) {
        const std::string cur = running_graph_bundle();
        if (!cur.empty()) {
            seen = cur;
        }
    });
    (void)sink;
    device d(g);

    d.adb.frames = {g.frame_a, g.frame_a};
    EXPECT_TRUE(d.run("run(\"g.a.claim/main.ccat\")").ok);
    EXPECT_EQ(seen, "g.a.claim");
    EXPECT_TRUE(running_graph_bundle().empty());

    seen.clear();
    d.adb.frames = {g.frame_a, g.frame_b};
    d.adb.at = 0;
    EXPECT_TRUE(d.run("run(\"g.a-b/main.ccat\")").ok);
    EXPECT_EQ(seen, "g.a-b");
    EXPECT_TRUE(running_graph_bundle().empty());

    seen.clear();
    d.adb.frames = {g.frame_a, g.frame_b};
    d.adb.at = 0;
    EXPECT_TRUE(d.run("goto(\"g:b\")").ok);
    EXPECT_EQ(seen, "g.a-b");
    EXPECT_TRUE(running_graph_bundle().empty());
}

TEST(CcatGraph, RunOnPlainBundleUnchanged) {
    toy g("plain");
    save_png(g.dir / "g.plain" / "x.png", g.mark_b);
    write_file(g.dir / "g.plain" / "main.ccat",
               "if (has(\"x.png\")) { log(\"x\") }\n");
    write_file(g.dir / "g.a-zzz" / "main.ccat", "home\n");
    device d(g);
    d.adb.frames = {g.frame_a};
    EXPECT_TRUE(d.run("run(\"g.plain/main.ccat\")").ok);
    EXPECT_TRUE(d.run("run(\"g.a-zzz/main.ccat\")").ok);
    EXPECT_EQ(d.adb.captures, 1) << "the has, and no locate";
    EXPECT_EQ(d.adb.homes, 1);
}

TEST(CcatGraph, AtIsAConditionAndGotoInsideAnEdgeIsAnError) {
    toy g("at");
    write_file(g.dir / "g.a-b" / "main.ccat", "goto(\"g:b\")\n");
    // An invalid graph is an error, not "not there"; `if` reads any error in
    // its condition as false, so the error shows on goto.
    device d(g);
    d.adb.frames = {g.frame_b};
    const auto broken = d.run("goto(\"g:b\")");
    EXPECT_FALSE(broken.ok);
    EXPECT_NE(broken.message.find("goto is not allowed in an edge"),
              std::string::npos)
        << broken.message;

    write_file(g.dir / "g.a-b" / "main.ccat", "home\n");
    device fresh(g);
    fresh.adb.frames = {g.frame_b};
    EXPECT_TRUE(fresh.run("if (at(\"g:b\")) { return true }\nreturn false").ok);
    fresh.adb.frames = {g.frame_a};
    fresh.adb.at = 0;
    EXPECT_FALSE(fresh.run("if (at(\"g:b\")) { return true }\nreturn false").ok);
}

TEST(CcatGraph, CheckAnchorUsesOneFrame) {
    toy g("one_frame");
    write_file(g.dir / "g.a" / "main.ccat",
               "if (has(\"a.png\")) { return true }\n"
               "if (has(\"a.png\")) { return true }\nreturn false\n");
    write_file(g.dir / "g.bad" / "main.ccat", "tap(\"nope.png\")\n");
    device d(g);
    const auto never = [] { return false; };

    EXPECT_TRUE(d.interp->check_anchor("g.a", g.frame_a, never).here);
    const auto off = d.interp->check_anchor("g.a", g.frame_b, never);
    EXPECT_FALSE(off.here);
    EXPECT_TRUE(off.error.empty()) << off.error;
    EXPECT_FALSE(d.interp->check_anchor("g.bad", g.frame_a, never).error.empty());
    write_file(g.dir / "g.loop" / "main.ccat", "goto(\"g:b\")\n");
    EXPECT_EQ(d.interp->check_anchor("g.loop", g.frame_a, never).error,
              "goto inside a graph script");
    EXPECT_EQ(d.adb.captures, 0);
}

TEST(CcatGraph, LocateFrameReadsAGraphMidBuild) {
    toy g("mid_build");
    anchor(g.dir, "g.lost", "lost.png", tex(5));
    device d(g);
    std::string err;
    const auto hit =
        d.interp->locate_frame("g", g.frame_b, [] { return false; }, &err);
    EXPECT_EQ(hit.id, "b") << hit.detail;
    EXPECT_EQ(d.interp->locate_frame("none", g.frame_b, [] { return false; }, &err)
                  .kind,
              locate_kind::error);
}

TEST(CcatGraph, DeleteAndRenameGuardTheGraph) {
    toy g("guards");
    using campcat::ccat_lang::check_bundle_deletable;
    using campcat::ccat_lang::check_bundle_renamable;
    using campcat::ccat_lang::delete_status;
    using campcat::ccat_lang::rename_status;
    std::string err;
    EXPECT_EQ(check_bundle_deletable(g.dir, "g.a", &err),
              delete_status::referenced);
    EXPECT_NE(err.find("g.a-b"), std::string::npos) << err;
    EXPECT_NE(err.find("g.entry-a"), std::string::npos) << err;
    EXPECT_EQ(check_bundle_deletable(g.dir, "g.a-b", &err), delete_status::ok)
        << err;

    EXPECT_EQ(check_bundle_renamable(g.dir, "g.a", "g.c", nullptr, &err),
              rename_status::bad_name);
    EXPECT_EQ(check_bundle_renamable(g.dir, "g.a-b", "g.x", nullptr, &err),
              rename_status::bad_name);
    write_file(g.dir / "task" / "main.ccat", "log(\"t\")\n");
    EXPECT_EQ(check_bundle_renamable(g.dir, "task", "g.b-a", nullptr, &err),
              rename_status::bad_name);
    EXPECT_EQ(check_bundle_renamable(g.dir, "task", "g.task", nullptr, &err),
              rename_status::ok)
        << err;
}

TEST(CcatGraph, ABadReferenceDoesNotParse) {
    EXPECT_THROW(parse_program("goto(g:b)"), parse_error);
    EXPECT_THROW(parse_program("goto(\"b\")"), parse_error);
    EXPECT_THROW(parse_program("at(\"g:b\")"), parse_error);
}

TEST(CcatGraph, AnActionIsThreeNameParts) {
    toy g("action_name");
    write_file(g.dir / "g.a.claim" / "main.ccat", "log(\"x\")\n");
    write_file(g.dir / "g.a.claim.extra" / "main.ccat", "log(\"x\")\n");
    const auto claim = bundle_tags(g.dir, "g.a.claim");
    EXPECT_EQ(claim.action_of, "g");
    EXPECT_EQ(claim.action_on, "a");
    EXPECT_TRUE(bundle_tags(g.dir, "g.a.claim.extra").action_of.empty());
    const auto scanned = scan_graph(g.dir, "g");
    ASSERT_EQ(scanned.graph.actions.size(), 1u);
    EXPECT_EQ(scanned.graph.actions[0].script, "g.a.claim");
    EXPECT_EQ(scanned.graph.actions[0].name, "claim");
    EXPECT_EQ(scanned.graph.actions[0].anchor, "a");
    const std::string text = graph_summary(scanned);
    EXPECT_NE(text.find("actions: 1"), std::string::npos) << text;
    EXPECT_NE(text.find("  a.claim"), std::string::npos) << text;
    EXPECT_EQ(bundle_tags(g.dir, "g.entry").actions, 1);
}

TEST(CcatGraph, AnActionOnAMissingAnchorIsAProblem) {
    toy g("action_missing");
    write_file(g.dir / "g.nope.claim" / "main.ccat", "log(\"x\")\n");
    const auto scanned = scan_graph(g.dir, "g");
    EXPECT_TRUE(scanned.graph.actions.empty());
    bool named = false;
    for (const std::string &p : scanned.problems) {
        if (p.find("g.nope.claim: anchor nope does not exist") !=
            std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named);
}

TEST(CcatGraph, AnActionOnAPopupIsNotAPage) {
    toy g("action_popup");
    write_file(g.dir / "g.close" / "main.ccat", "log(\"close\")\n");
    anchor(g.dir, "g.pop", "pop.png", tex(4),
           "---\nkind: popup\nclose: g.close\n---\nscreen\n");
    write_file(g.dir / "g.pop.claim" / "main.ccat", "log(\"x\")\n");
    const auto scanned = scan_graph(g.dir, "g");
    EXPECT_TRUE(scanned.graph.actions.empty());
    bool named = false;
    for (const std::string &p : scanned.problems) {
        if (p.find("g.pop.claim: anchor pop is not a page") !=
            std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named);
}

TEST(CcatGraph, GotoInAnActionIsAProblem) {
    toy g("action_goto");
    write_file(g.dir / "g.a.claim" / "main.ccat", "goto(\"g:b\")\n");
    const auto scanned = scan_graph(g.dir, "g");
    bool named = false;
    for (const std::string &p : scanned.problems) {
        if (p.find("g.a.claim: goto is not allowed in an action") !=
            std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named);
}

} // namespace
