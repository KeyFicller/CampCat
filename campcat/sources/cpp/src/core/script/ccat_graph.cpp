#include "core/script/ccat_graph.h"

#include "core/script/ccat_bundle.h"
#include "core/script/ccat_parser.h"

#include <nlohmann/json.hpp>

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <queue>
#include <sstream>
#include <thread>
#include <utility>

namespace campcat::ccat_lang {

namespace {

using json = nlohmann::json;

constexpr int k_default_edge_timeout_ms = 20000;
constexpr int k_max_edge_timeout_ms = 600000;

const anchor_def *find_anchor(const screen_graph &_graph,
                              std::string_view _id) {
    for (const anchor_def &a : _graph.anchors) {
        if (a.id == _id) {
            return &a;
        }
    }
    return nullptr;
}

bool is_interrupt(const screen_graph &_graph, std::string_view _id) {
    for (const graph_interrupt &i : _graph.interrupts) {
        if (i.anchor == _id) {
            return true;
        }
    }
    return false;
}

void add(std::vector<std::string> *_errs, const std::string &_msg) {
    _errs->push_back(_msg);
}

bool known_keys(const json &_obj, std::initializer_list<const char *> _keys,
                const std::string &_where, std::vector<std::string> *_errs) {
    bool ok = true;
    if (!_obj.is_object()) {
        add(_errs, _where + " must be an object");
        return false;
    }
    for (auto it = _obj.begin(); it != _obj.end(); ++it) {
        const bool known =
            std::any_of(_keys.begin(), _keys.end(),
                        [&](const char *_k) { return it.key() == _k; });
        if (!known) {
            add(_errs, _where + " unknown field \"" + it.key() + "\"");
            ok = false;
        }
    }
    return ok;
}

bool read_roi(const json &_el, anchor_template *_out, const std::string &_where,
              std::vector<std::string> *_errs) {
    if (!_el.is_array() || _el.size() != 4) {
        add(_errs, _where + " roi must be [x, y, w, h]");
        return false;
    }
    double v[4];
    for (int i = 0; i < 4; ++i) {
        if (!_el[i].is_number()) {
            add(_errs, _where + " roi must be [x, y, w, h]");
            return false;
        }
        v[i] = _el[i].get<double>();
        if (!std::isfinite(v[i]) || v[i] < 0 || v[i] > 1) {
            add(_errs, _where + " roi is outside [0,1]");
            return false;
        }
    }
    if (v[2] <= 0 || v[3] <= 0 || v[0] + v[2] > 1.000001 ||
        v[1] + v[3] > 1.000001) {
        add(_errs, _where + " roi is outside [0,1]");
        return false;
    }
    _out->has_roi = true;
    _out->x = v[0];
    _out->y = v[1];
    _out->w = v[2];
    _out->h = v[3];
    return true;
}

bool read_template(const json &_el, anchor_template *_out,
                   const std::string &_where, std::vector<std::string> *_errs) {
    if (_el.is_string()) {
        _out->png = _el.get<std::string>();
    } else if (_el.is_object()) {
        if (!known_keys(_el, {"png", "roi"}, _where, _errs)) {
            return false;
        }
        if (!_el.contains("png") || !_el["png"].is_string()) {
            add(_errs, _where + " needs a png name");
            return false;
        }
        _out->png = _el["png"].get<std::string>();
        if (_el.contains("roi") &&
            !read_roi(_el["roi"], _out, _where, _errs)) {
            return false;
        }
    } else {
        add(_errs, _where + " must be a png name or {png, roi}");
        return false;
    }
    if (!safe_leaf_name(_out->png) ||
        _out->png.find('/') != std::string::npos) {
        add(_errs, _where + " png \"" + _out->png + "\" is not a file name");
        return false;
    }
    return true;
}

void read_template_list(const json &_arr, std::vector<anchor_template> *_out,
                        const std::string &_where,
                        std::vector<std::string> *_errs) {
    if (!_arr.is_array() || _arr.empty()) {
        add(_errs, _where + " must be a non-empty list");
        return;
    }
    for (std::size_t i = 0; i < _arr.size(); ++i) {
        anchor_template t;
        if (read_template(_arr[i], &t, _where + "[" + std::to_string(i) + "]",
                          _errs)) {
            _out->push_back(std::move(t));
        }
    }
}

bool png_readable(const std::filesystem::path &_path) {
    const cv::Mat img = cv::imread(_path.string(), cv::IMREAD_UNCHANGED);
    return !img.empty();
}

bool script_parses(const std::filesystem::path &_library,
                   const std::string &_bundle, std::string *_why) {
    if (!safe_leaf_name(_bundle)) {
        *_why = "\"" + _bundle + "\" is not a bundle name";
        return false;
    }
    const std::filesystem::path path = bundle_main(_library, _bundle);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *_why = "cannot read " + path.string();
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    try {
        parse_program(text);
    } catch (const parse_error &_e) {
        *_why = _bundle + "/main.ccat does not parse (line " +
                std::to_string(_e.line) + ", col " + std::to_string(_e.col) +
                ")";
        return false;
    }
    return true;
}

std::string join_lines(const std::vector<std::string> &_parts) {
    std::string out;
    for (const std::string &p : _parts) {
        if (!out.empty()) {
            out += "\n";
        }
        out += p;
    }
    return out;
}

std::string join_semi(const std::vector<std::string> &_parts) {
    std::string out;
    for (const std::string &p : _parts) {
        if (!out.empty()) {
            out += "; ";
        }
        out += p;
    }
    return out;
}

cv::Rect roi_px(const anchor_template &_t, const cv::Size &_sz) {
    if (!_t.has_roi || _sz.width < 1 || _sz.height < 1) {
        return {};
    }
    int x = static_cast<int>(std::lround(_t.x * _sz.width));
    int y = static_cast<int>(std::lround(_t.y * _sz.height));
    int w = static_cast<int>(std::lround(_t.w * _sz.width));
    int h = static_cast<int>(std::lround(_t.h * _sz.height));
    x = std::clamp(x, 0, _sz.width);
    y = std::clamp(y, 0, _sz.height);
    w = std::min(w, _sz.width - x);
    h = std::min(h, _sz.height - y);
    if (w < 1 || h < 1) {
        return {};
    }
    return {x, y, w, h};
}

/// `nullopt` when the file will not load. An empty rect means "not this roi":
/// the matcher treats an empty rect as the whole frame, which is the opposite
/// of a roi that rounded away to nothing.
std::optional<bool> template_hits(const screen_graph &_graph,
                                  const template_matcher &_matcher,
                                  const anchor_template &_t,
                                  const cv::Mat &_frame) {
    const cv::Rect roi = roi_px(_t, _frame.size());
    if (_t.has_roi && roi.area() <= 0) {
        return false;
    }
    const std::optional<match_result> hit = _matcher.match_file(
        _frame, (_graph.dir / _t.png).string(), 0, roi);
    if (!hit) {
        return std::nullopt;
    }
    return hit->found;
}

bool signature_holds(const screen_graph &_graph,
                     const template_matcher &_matcher, const anchor_def &_a,
                     const cv::Mat &_frame, std::string *_broken) {
    for (const anchor_template &t : _a.must) {
        const std::optional<bool> hit =
            template_hits(_graph, _matcher, t, _frame);
        if (!hit) {
            *_broken = _a.id + " template unreadable " + t.png;
            return false;
        }
        if (!*hit) {
            return false;
        }
    }
    for (const anchor_template &t : _a.must_not) {
        const std::optional<bool> hit =
            template_hits(_graph, _matcher, t, _frame);
        if (!hit) {
            *_broken = _a.id + " template unreadable " + t.png;
            return false;
        }
        if (*hit) {
            return false;
        }
    }
    return true;
}

void pause_ms(const nav_io &_io, int _ms) {
    if (_ms <= 0) {
        return;
    }
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (_io.should_stop && _io.should_stop()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct sample {
    bool captured = false;
    bool device = false;
    bool stopped = false;
    locate_hit hit;
};

sample take(const screen_graph &_graph, const template_matcher &_matcher,
            const nav_io &_io) {
    sample s;
    if (_io.should_stop && _io.should_stop()) {
        s.stopped = true;
        return s;
    }
    cv::Mat frame;
    if (!_io.capture || !_io.capture(&frame)) {
        if (_io.should_stop && _io.should_stop()) {
            s.stopped = true;
        } else {
            s.device = true;
        }
        return s;
    }
    s.captured = true;
    s.hit = locate(_graph, _matcher, frame);
    return s;
}

/// Wait out a loading screen or an unrecognized one. Returns the sample that
/// ended the wait: a page, an interrupt, an error, or still unknown/transient
/// when the budget ran out. `_limits.unknown_ms == 0` does not wait.
sample wait_unknown(const screen_graph &_graph,
                    const template_matcher &_matcher, const nav_io &_io,
                    const nav_limits &_limits, sample _first) {
    if (_limits.unknown_ms <= 0) {
        return _first;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(_limits.unknown_ms);
    sample cur = std::move(_first);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cur.hit.kind != locate_kind::unknown &&
            cur.hit.kind != locate_kind::transient) {
            return cur;
        }
        pause_ms(_io, std::min(_limits.poll_ms,
                               static_cast<int>(
                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count())));
        if (_io.should_stop && _io.should_stop()) {
            cur.stopped = true;
            return cur;
        }
        cur = take(_graph, _matcher, _io);
        if (!cur.captured) {
            return cur;
        }
    }
    return cur;
}

const graph_edge *first_hop(const screen_graph &_graph,
                            const std::string &_from, const std::string &_to) {
    std::map<std::string, const graph_edge *> prev;
    std::queue<std::string> q;
    q.push(_from);
    prev.emplace(_from, nullptr);
    while (!q.empty()) {
        const std::string at = q.front();
        q.pop();
        if (at == _to) {
            break;
        }
        for (const graph_edge &e : _graph.edges) {
            if (e.from == at && prev.find(e.to) == prev.end()) {
                prev.emplace(e.to, &e);
                q.push(e.to);
            }
        }
    }
    if (prev.find(_to) == prev.end()) {
        return nullptr;
    }
    const graph_edge *hop = nullptr;
    for (std::string at = _to; at != _from;) {
        hop = prev[at];
        at = hop->from;
    }
    return hop;
}

bool is_page(locate_kind _k) { return _k == locate_kind::page; }

void collect_script_fields(const json &_j, std::vector<std::string> *_out) {
    if (_j.is_object()) {
        for (auto it = _j.begin(); it != _j.end(); ++it) {
            if ((it.key() == "script" || it.key() == "recover") &&
                it->is_string()) {
                _out->push_back(it->get<std::string>());
            } else {
                collect_script_fields(*it, _out);
            }
        }
    } else if (_j.is_array()) {
        for (const json &el : _j) {
            collect_script_fields(el, _out);
        }
    }
}

json replace_script_fields(json _j, std::string_view _from,
                           std::string_view _to, int *_n) {
    if (_j.is_object()) {
        for (auto it = _j.begin(); it != _j.end(); ++it) {
            if ((it.key() == "script" || it.key() == "recover") &&
                it->is_string() && it->get<std::string>() == _from) {
                *it = std::string(_to);
                ++(*_n);
            } else {
                *it = replace_script_fields(std::move(*it), _from, _to, _n);
            }
        }
    } else if (_j.is_array()) {
        for (json &el : _j) {
            el = replace_script_fields(std::move(el), _from, _to, _n);
        }
    }
    return _j;
}

} // namespace

bool split_anchor_ref(std::string_view _ref, std::string *_graph,
                      std::string *_anchor) {
    const std::size_t colon = _ref.find(':');
    if (colon == std::string_view::npos ||
        _ref.find(':', colon + 1) != std::string_view::npos) {
        return false;
    }
    const std::string_view g = _ref.substr(0, colon);
    const std::string_view a = _ref.substr(colon + 1);
    if (!safe_leaf_name(g) || !safe_leaf_name(a)) {
        return false;
    }
    *_graph = std::string(g);
    *_anchor = std::string(a);
    return true;
}

loaded_graph load_graph(const std::filesystem::path &_library,
                        std::string_view _name) {
    loaded_graph out;
    const std::string name(_name);
    std::vector<std::string> errs;
    if (!safe_leaf_name(name) || name.find(':') != std::string::npos) {
        out.error = "graph name \"" + name + "\" is not usable";
        return out;
    }
    out.graph.name = name;
    out.graph.dir = _library / name;
    const std::filesystem::path path = out.graph.dir / "graph.json";

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.error = "cannot read " + path.string();
        return out;
    }
    const std::string text{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::exception &_e) {
        out.error = path.string() + ": " + _e.what();
        return out;
    }
    if (!known_keys(doc, {"root", "recover", "anchors", "edges", "interrupts"},
                    name, &errs)) {
        // still walk what we can, so one file reports every problem
    }
    if (!doc.is_object()) {
        out.error = join_lines(errs);
        return out;
    }

    if (!doc.contains("root") || !doc["root"].is_string()) {
        add(&errs, name + ": root is required");
    } else {
        out.graph.root = doc["root"].get<std::string>();
    }
    if (doc.contains("recover") && !doc["recover"].is_null()) {
        if (!doc["recover"].is_string()) {
            add(&errs, name + ": recover must be a bundle name");
        } else {
            out.graph.recover = doc["recover"].get<std::string>();
        }
    }

    if (!doc.contains("anchors") || !doc["anchors"].is_object()) {
        add(&errs, name + ": anchors must be an object");
    } else {
        for (auto it = doc["anchors"].begin(); it != doc["anchors"].end();
             ++it) {
            anchor_def a;
            a.id = it.key();
            const std::string where = name + " anchor \"" + a.id + "\"";
            if (!safe_leaf_name(a.id) || a.id.find(':') != std::string::npos) {
                add(&errs, where + " is not a usable name");
                continue;
            }
            if (!it->is_object()) {
                add(&errs, where + " must be an object");
                continue;
            }
            // Unknown fields are recorded and the rest is still checked, so one
            // anchor reports every problem it has.
            known_keys(*it, {"must", "must_not", "transient"}, where, &errs);
            if (!it->contains("must")) {
                add(&errs, where + " needs must");
            } else {
                read_template_list((*it)["must"], &a.must, where + " must",
                                   &errs);
            }
            if (it->contains("must_not")) {
                read_template_list((*it)["must_not"], &a.must_not,
                                   where + " must_not", &errs);
            }
            if (it->contains("transient")) {
                if (!(*it)["transient"].is_boolean()) {
                    add(&errs, where + " transient must be true or false");
                } else {
                    a.transient = (*it)["transient"].get<bool>();
                }
            }
            for (const anchor_template &t : a.must) {
                if (!png_readable(out.graph.dir / t.png)) {
                    add(&errs, where + " missing template " + t.png);
                }
            }
            for (const anchor_template &t : a.must_not) {
                if (!png_readable(out.graph.dir / t.png)) {
                    add(&errs, where + " missing template " + t.png);
                }
            }
            out.graph.anchors.push_back(std::move(a));
        }
    }

    if (!doc.contains("edges") || !doc["edges"].is_array()) {
        add(&errs, name + ": edges must be a list");
    } else {
        for (std::size_t i = 0; i < doc["edges"].size(); ++i) {
            const json &el = doc["edges"][i];
            const std::string where =
                name + " edge[" + std::to_string(i) + "]";
            graph_edge e;
            if (!el.is_object() ||
                !known_keys(el, {"from", "to", "script", "timeout_ms"}, where,
                            &errs)) {
                continue;
            }
            if (!el.contains("from") || !el["from"].is_string() ||
                !el.contains("to") || !el["to"].is_string() ||
                !el.contains("script") || !el["script"].is_string()) {
                add(&errs, where + " needs from, to and script");
                continue;
            }
            e.from = el["from"].get<std::string>();
            e.to = el["to"].get<std::string>();
            e.script = el["script"].get<std::string>();
            e.timeout_ms = k_default_edge_timeout_ms;
            if (el.contains("timeout_ms")) {
                if (!el["timeout_ms"].is_number_integer()) {
                    add(&errs, where + " timeout_ms must be an integer");
                } else {
                    e.timeout_ms = el["timeout_ms"].get<int>();
                    if (e.timeout_ms < 1 ||
                        e.timeout_ms > k_max_edge_timeout_ms) {
                        add(&errs, where + " timeout_ms out of range");
                    }
                }
            }
            out.graph.edges.push_back(std::move(e));
        }
    }

    if (doc.contains("interrupts")) {
        if (!doc["interrupts"].is_array()) {
            add(&errs, name + ": interrupts must be a list");
        } else {
            for (std::size_t i = 0; i < doc["interrupts"].size(); ++i) {
                const json &el = doc["interrupts"][i];
                const std::string where =
                    name + " interrupt[" + std::to_string(i) + "]";
                if (!el.is_object() ||
                    !known_keys(el, {"anchor", "script"}, where, &errs)) {
                    continue;
                }
                if (!el.contains("anchor") || !el["anchor"].is_string() ||
                    !el.contains("script") || !el["script"].is_string()) {
                    add(&errs, where + " needs anchor and script");
                    continue;
                }
                graph_interrupt intr;
                intr.anchor = el["anchor"].get<std::string>();
                intr.script = el["script"].get<std::string>();
                out.graph.interrupts.push_back(std::move(intr));
            }
        }
    }

    if (find_anchor(out.graph, out.graph.root) == nullptr) {
        add(&errs, name + ": root \"" + out.graph.root + "\" is not an anchor");
    }
    for (const graph_edge &e : out.graph.edges) {
        const anchor_def *from = find_anchor(out.graph, e.from);
        const anchor_def *to = find_anchor(out.graph, e.to);
        if (from == nullptr || to == nullptr) {
            add(&errs, name + ": edge " + e.from + " -> " + e.to +
                           " names an unknown anchor");
            continue;
        }
        if (from->transient || to->transient || is_interrupt(out.graph, e.from) ||
            is_interrupt(out.graph, e.to)) {
            add(&errs, name + ": edge " + e.from + " -> " + e.to +
                           " cannot start or end on a transient or popup");
        }
        std::string why;
        if (!script_parses(_library, e.script, &why)) {
            add(&errs, name + ": edge " + e.script + ": " + why);
        }
    }
    for (std::size_t i = 0; i < out.graph.edges.size(); ++i) {
        for (std::size_t j = i + 1; j < out.graph.edges.size(); ++j) {
            if (out.graph.edges[i].from == out.graph.edges[j].from &&
                out.graph.edges[i].to == out.graph.edges[j].to) {
                add(&errs, name + ": duplicate edge " + out.graph.edges[i].from +
                               " -> " + out.graph.edges[i].to);
            }
        }
    }
    for (const graph_interrupt &intr : out.graph.interrupts) {
        if (find_anchor(out.graph, intr.anchor) == nullptr) {
            add(&errs, name + ": interrupt \"" + intr.anchor +
                           "\" is not an anchor");
        }
        std::string why;
        if (!script_parses(_library, intr.script, &why)) {
            add(&errs, name + ": interrupt " + intr.script + ": " + why);
        }
    }
    if (!out.graph.recover.empty()) {
        std::string why;
        if (!script_parses(_library, out.graph.recover, &why)) {
            add(&errs, name + ": recover: " + why);
        }
    }

    std::map<std::string, bool> reach;
    std::queue<std::string> q;
    if (find_anchor(out.graph, out.graph.root) != nullptr) {
        reach[out.graph.root] = true;
        q.push(out.graph.root);
    }
    while (!q.empty()) {
        const std::string at = q.front();
        q.pop();
        for (const graph_edge &e : out.graph.edges) {
            if (e.from == at && !reach[e.to]) {
                reach[e.to] = true;
                q.push(e.to);
            }
        }
    }
    for (const anchor_def &a : out.graph.anchors) {
        if (a.transient || is_interrupt(out.graph, a.id)) {
            continue;
        }
        if (!reach[a.id]) {
            add(&errs, name + ": \"" + a.id + "\" is not reachable from root");
        }
    }

    if (!errs.empty()) {
        out.error = join_lines(errs);
        out.graph = {};
        out.ok = false;
        return out;
    }
    out.ok = true;
    return out;
}

locate_hit locate(const screen_graph &_graph, const template_matcher &_matcher,
                  const cv::Mat &_frame) {
    locate_hit hit;
    if (_frame.empty()) {
        hit.kind = locate_kind::error;
        hit.detail = "empty frame";
        return hit;
    }
    for (const graph_interrupt &intr : _graph.interrupts) {
        const anchor_def *a = find_anchor(_graph, intr.anchor);
        if (a == nullptr) {
            continue;
        }
        std::string broken;
        if (!signature_holds(_graph, _matcher, *a, _frame, &broken)) {
            if (!broken.empty()) {
                hit.kind = locate_kind::error;
                hit.detail = broken;
                return hit;
            }
            continue;
        }
        hit.kind = locate_kind::interrupt;
        hit.id = a->id;
        return hit;
    }

    std::vector<std::string> pages;
    for (const anchor_def &a : _graph.anchors) {
        if (is_interrupt(_graph, a.id)) {
            continue;
        }
        std::string broken;
        if (!signature_holds(_graph, _matcher, a, _frame, &broken)) {
            if (!broken.empty()) {
                hit.kind = locate_kind::error;
                hit.detail = broken;
                return hit;
            }
            continue;
        }
        pages.push_back(a.id);
    }
    if (pages.empty()) {
        hit.kind = locate_kind::unknown;
        return hit;
    }
    if (pages.size() > 1) {
        std::sort(pages.begin(), pages.end());
        hit.kind = locate_kind::ambiguous;
        hit.detail = "ambiguous: " + join_semi(pages);
        return hit;
    }
    const anchor_def *a = find_anchor(_graph, pages[0]);
    hit.kind = a->transient ? locate_kind::transient : locate_kind::page;
    hit.id = pages[0];
    return hit;
}

nav_result navigate(const screen_graph &_graph, const std::string &_target,
                    const template_matcher &_matcher, const nav_io &_io,
                    nav_limits _limits) {
    nav_result done;
    const anchor_def *target = find_anchor(_graph, _target);
    if (target == nullptr) {
        done.message =
            "goto " + _graph.name + ":" + _target + " unknown anchor";
        return done;
    }
    if (target->transient || is_interrupt(_graph, _target)) {
        done.message = "goto " + _graph.name + ":" + _target +
                       " is not a place to stay";
        return done;
    }

    int failures = 0;
    bool recovered = false;
    int interrupts = 0;
    std::vector<std::string> trace;
    bool halt = false;
    nav_result halted;

    auto finish_fail = [&]() {
        nav_result r;
        r.message = "goto " + _graph.name + ":" + _target + " failed: " +
                    join_semi(trace);
        return r;
    };
    /// @return true when this attempt is over (caller returns `finish_fail`,
    /// or `halted` when a stop happened inside recover).
    auto give_up = [&](std::string _why) {
        trace.push_back(std::move(_why));
        if (++failures < _limits.max_failures) {
            return false;
        }
        if (!_graph.recover.empty() && !recovered) {
            recovered = true;
            failures = 0;
            const bundle_run ran = _io.run_bundle(_graph.recover);
            trace.push_back("recover ran");
            if (ran.stopped || (_io.should_stop && _io.should_stop())) {
                halted.stopped = true;
                halt = true;
                return true;
            }
            if (ran.failed) {
                trace.push_back("recover: " + ran.message);
            }
            return false;
        }
        return true;
    };

    while (!halt) {
        sample s = take(_graph, _matcher, _io);
        if (s.stopped) {
            done.stopped = true;
            return done;
        }
        if (s.device) {
            done.device = true;
            done.message = "goto " + _graph.name + ":" + _target +
                           " screencap failed";
            return done;
        }
        if (s.hit.kind == locate_kind::error) {
            done.message = "goto " + _graph.name + ":" + _target + " " +
                           s.hit.detail;
            return done;
        }
        if (s.hit.kind == locate_kind::ambiguous) {
            done.message =
                "goto " + _graph.name + ":" + _target + " " + s.hit.detail;
            return done;
        }

        if (s.hit.kind == locate_kind::interrupt) {
            if (interrupts >= _limits.max_interrupts) {
                if (give_up("interrupt limit")) {
                    return halt ? halted : finish_fail();
                }
                continue;
            }
            ++interrupts;
            const graph_interrupt *intr = nullptr;
            for (const graph_interrupt &i : _graph.interrupts) {
                if (i.anchor == s.hit.id) {
                    intr = &i;
                    break;
                }
            }
            const bundle_run ran = _io.run_bundle(intr->script);
            if (ran.stopped) {
                done.stopped = true;
                return done;
            }
            if (ran.failed && give_up(s.hit.id + " -[" + intr->script + "]-> " +
                                      ran.message)) {
                return halt ? halted : finish_fail();
            }
            continue;
        }

        if (s.hit.kind == locate_kind::unknown ||
            s.hit.kind == locate_kind::transient) {
            s = wait_unknown(_graph, _matcher, _io, _limits, std::move(s));
            if (s.stopped) {
                done.stopped = true;
                return done;
            }
            if (s.device) {
                done.device = true;
                done.message = "goto " + _graph.name + ":" + _target +
                               " screencap failed";
                return done;
            }
            if (s.hit.kind == locate_kind::error ||
                s.hit.kind == locate_kind::ambiguous) {
                done.message = "goto " + _graph.name + ":" + _target + " " +
                               s.hit.detail;
                return done;
            }
            if (s.hit.kind == locate_kind::interrupt) {
                continue; // the next loop handles it, keeping the cap
            }
            if (s.hit.kind != locate_kind::page) {
                const std::string last =
                    s.hit.kind == locate_kind::transient ? s.hit.id : "unknown";
                if (give_up("unknown (" + last + ")")) {
                    return halt ? halted : finish_fail();
                }
                continue;
            }
        }

        if (s.hit.id == _target) {
            done.ok = true;
            return done;
        }

        const graph_edge *hop = first_hop(_graph, s.hit.id, _target);
        if (hop == nullptr) {
            if (give_up("no path from " + s.hit.id)) {
                return halt ? halted : finish_fail();
            }
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        const bundle_run ran = _io.run_bundle(hop->script);
        if (ran.stopped) {
            done.stopped = true;
            return done;
        }
        if (ran.failed) {
            if (give_up(hop->from + " -[" + hop->script + "]-> " +
                        ran.message)) {
                return halt ? halted : finish_fail();
            }
            continue;
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(hop->timeout_ms);
        bool replanned = false;
        locate_hit last = s.hit;
        while (!replanned) {
            sample w = take(_graph, _matcher, _io);
            if (w.stopped) {
                done.stopped = true;
                return done;
            }
            if (w.device) {
                done.device = true;
                done.message = "goto " + _graph.name + ":" + _target +
                               " screencap failed";
                return done;
            }
            last = w.hit;
            if (w.hit.kind == locate_kind::error ||
                w.hit.kind == locate_kind::ambiguous) {
                done.message = "goto " + _graph.name + ":" + _target + " " +
                               w.hit.detail;
                return done;
            }
            if (w.hit.kind == locate_kind::interrupt) {
                if (interrupts >= _limits.max_interrupts) {
                    if (give_up("interrupt limit")) {
                        return halt ? halted : finish_fail();
                    }
                    replanned = true;
                    break;
                }
                ++interrupts;
                const graph_interrupt *intr = nullptr;
                for (const graph_interrupt &i : _graph.interrupts) {
                    if (i.anchor == w.hit.id) {
                        intr = &i;
                        break;
                    }
                }
                const bundle_run closed = _io.run_bundle(intr->script);
                if (closed.stopped) {
                    done.stopped = true;
                    return done;
                }
                if (closed.failed &&
                    give_up(w.hit.id + " -[" + intr->script + "]-> " +
                            closed.message)) {
                    return halt ? halted : finish_fail();
                }
                continue;
            }
            if ((w.hit.kind == locate_kind::page ||
                 w.hit.kind == locate_kind::transient) &&
                w.hit.id == hop->to) {
                const double sec =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - started)
                        .count();
                std::ostringstream line;
                line.setf(std::ios::fixed);
                line.precision(1);
                line << "goto: " << hop->from << " -> " << hop->to << " via "
                     << hop->script << " " << sec << " s";
                if (_io.log) {
                    _io.log(line.str());
                }
                replanned = true;
                break;
            }
            if (w.hit.kind == locate_kind::page && w.hit.id != hop->from) {
                replanned = true; // somewhere else: plan again, no failure
                break;
            }
            if (hop->timeout_ms <= 0 ||
                std::chrono::steady_clock::now() >= deadline) {
                const std::string where =
                    w.hit.id.empty() ? "unknown" : w.hit.id;
                if (give_up(hop->from + " -[" + hop->script + "]-> timeout " +
                            std::to_string(hop->timeout_ms) + " ms (last: " +
                            where + ")")) {
                    return halt ? halted : finish_fail();
                }
                replanned = true;
                break;
            }
            pause_ms(_io, _limits.poll_ms);
        }
        (void)last;
    }
    return halted.stopped ? halted : finish_fail();
}

std::optional<std::vector<std::string>>
graph_script_names(const std::filesystem::path &_graph_json,
                   std::string *_err) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(_graph_json, ec)) {
        return std::vector<std::string>{};
    }
    std::ifstream in(_graph_json, std::ios::binary);
    if (!in) {
        if (_err != nullptr) {
            *_err = "cannot read " + _graph_json.string();
        }
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::exception &_e) {
        if (_err != nullptr) {
            *_err = _graph_json.string() + ": " + _e.what();
        }
        return std::nullopt;
    }
    std::vector<std::string> names;
    collect_script_fields(doc, &names);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::optional<std::string> rewrite_graph_scripts(std::string_view _text,
                                                 std::string_view _from,
                                                 std::string_view _to,
                                                 std::string *_err) {
    json doc;
    try {
        doc = json::parse(_text);
    } catch (const json::exception &_e) {
        if (_err != nullptr) {
            *_err = _e.what();
        }
        return std::nullopt;
    }
    int n = 0;
    doc = replace_script_fields(std::move(doc), _from, _to, &n);
    if (n == 0) {
        return std::nullopt;
    }
    return doc.dump(2);
}

std::vector<std::string> golden_mismatches(const screen_graph &_graph,
                                           const template_matcher &_matcher) {
    std::vector<std::string> out;
    const std::filesystem::path root = _graph.dir / "golden";
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return out;
    }
    for (const auto &anchor_dir :
         std::filesystem::directory_iterator(root, ec)) {
        if (!anchor_dir.is_directory(ec)) {
            continue;
        }
        const std::string id = anchor_dir.path().filename().string();
        for (const auto &shot :
             std::filesystem::directory_iterator(anchor_dir.path(), ec)) {
            if (!shot.is_regular_file(ec)) {
                continue;
            }
            const cv::Mat frame =
                cv::imread(shot.path().string(), cv::IMREAD_COLOR);
            if (frame.empty()) {
                out.push_back(id + " unreadable " + shot.path().string());
                continue;
            }
            const locate_hit hit = locate(_graph, _matcher, frame);
            const bool same =
                (hit.kind == locate_kind::page ||
                 hit.kind == locate_kind::transient ||
                 hit.kind == locate_kind::interrupt) &&
                hit.id == id;
            if (!same) {
                out.push_back(id + " located as " +
                              (hit.id.empty() ? hit.detail : hit.id));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace campcat::ccat_lang
