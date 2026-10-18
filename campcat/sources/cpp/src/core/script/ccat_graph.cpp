#include "core/script/ccat_graph.h"

#include "core/script/ccat_bundle.h"
#include "core/script/ccat_parser.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <utility>

namespace campcat::ccat_lang {

namespace {

std::mutex g_running_mu;
std::string g_running_bundle;

struct running_bundle_guard {
    std::string prev;
    explicit running_bundle_guard(std::string _bundle)
        : prev(running_graph_bundle()) {
        set_running_graph_bundle(std::move(_bundle));
    }
    ~running_bundle_guard() { set_running_graph_bundle(std::move(prev)); }
    running_bundle_guard(const running_bundle_guard &) = delete;
    running_bundle_guard &operator=(const running_bundle_guard &) = delete;
};

const anchor_def *find_anchor(const screen_graph &_graph,
                              std::string_view _id) {
    for (const anchor_def &a : _graph.anchors) {
        if (a.id == _id) {
            return &a;
        }
    }
    return nullptr;
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

/// Reads a file whole; false when it is not there. `*_out` is cleared first.
bool read_text_file(const std::filesystem::path &_path, std::string *_out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(_path, ec)) {
        return false;
    }
    std::ifstream in(_path, std::ios::binary);
    if (!in) {
        return false;
    }
    *_out = std::string{std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>()};
    return true;
}

std::string trimmed(std::string_view _s) {
    const auto first = _s.find_first_not_of(" \t\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = _s.find_last_not_of(" \t\n");
    return std::string(_s.substr(first, last - first + 1));
}

const char *kind_word(anchor_kind _k) {
    switch (_k) {
    case anchor_kind::transient:
        return "transient";
    case anchor_kind::popup:
        return "popup";
    case anchor_kind::page:
        break;
    }
    return "page";
}

/// The language's own word for a statement an anchor may not hold.
const char *statement_word(const Stmt &_s) {
    if (dynamic_cast<const TapStmt *>(&_s) != nullptr) {
        return "tap";
    }
    if (dynamic_cast<const TapAtStmt *>(&_s) != nullptr) {
        return "tap_at";
    }
    if (dynamic_cast<const TapOffsetStmt *>(&_s) != nullptr) {
        return "tap_offset";
    }
    if (dynamic_cast<const SwipeTemplatesStmt *>(&_s) != nullptr) {
        return "swipe";
    }
    if (dynamic_cast<const SwipeAtStmt *>(&_s) != nullptr) {
        return "swipe_at";
    }
    if (dynamic_cast<const WaitUntilStmt *>(&_s) != nullptr) {
        return "wait_until";
    }
    if (dynamic_cast<const WaitStmt *>(&_s) != nullptr) {
        return "wait";
    }
    if (dynamic_cast<const RunStmt *>(&_s) != nullptr) {
        return "run";
    }
    if (dynamic_cast<const HomeStmt *>(&_s) != nullptr) {
        return "home";
    }
    if (dynamic_cast<const LoopStmt *>(&_s) != nullptr) {
        return "loop";
    }
    if (dynamic_cast<const DoWhileStmt *>(&_s) != nullptr) {
        return "do-while";
    }
    if (dynamic_cast<const GotoStmt *>(&_s) != nullptr) {
        return "goto";
    }
    if (dynamic_cast<const AtStmt *>(&_s) != nullptr) {
        return "at";
    }
    if (dynamic_cast<const BreakStmt *>(&_s) != nullptr) {
        return "break";
    }
    return "this statement";
}

/// `_has`, when set, receives the template of every `has` condition.
void walk_anchor(const Stmt &_s, bool *_returns, std::vector<std::string> *_out,
                 std::vector<std::string> *_has) {
    if (const auto *b = dynamic_cast<const BlockStmt *>(&_s)) {
        for (const auto &child : b->body) {
            walk_anchor(*child, _returns, _out, _has);
        }
    } else if (const auto *i = dynamic_cast<const IfStmt *>(&_s)) {
        const auto *has = dynamic_cast<const HasStmt *>(i->action.get());
        if (i->action && has == nullptr) {
            _out->push_back(std::string(statement_word(*i->action)) +
                            " is not allowed in an anchor condition; use has");
        }
        if (has != nullptr && _has != nullptr) {
            _has->push_back(has->image_path);
        }
        if (i->then_branch) {
            walk_anchor(*i->then_branch, _returns, _out, _has);
        }
        if (i->else_branch) {
            walk_anchor(*i->else_branch, _returns, _out, _has);
        }
    } else if (dynamic_cast<const ReturnStmt *>(&_s) != nullptr) {
        *_returns = true;
    } else if (dynamic_cast<const HasStmt *>(&_s) == nullptr &&
               dynamic_cast<const LogStmt *>(&_s) == nullptr &&
               dynamic_cast<const DebugStmt *>(&_s) == nullptr) {
        _out->push_back(std::string(statement_word(_s)) +
                        " is not allowed in an anchor");
    }
}

std::vector<std::string> anchor_problems(const Program &_program,
                                         std::vector<std::string> *_has) {
    std::vector<std::string> out;
    bool returns = false;
    for (const auto &st : _program.stmts) {
        walk_anchor(*st, &returns, &out, _has);
    }
    if (!returns) {
        out.push_back("no return: an anchor must answer true or false");
    }
    return out;
}

bool contains_goto(const Stmt &_s) {
    if (dynamic_cast<const GotoStmt *>(&_s) != nullptr) {
        return true;
    }
    if (const auto *b = dynamic_cast<const BlockStmt *>(&_s)) {
        return std::any_of(b->body.begin(), b->body.end(),
                           [](const auto &_c) { return contains_goto(*_c); });
    }
    if (const auto *i = dynamic_cast<const IfStmt *>(&_s)) {
        return (i->then_branch && contains_goto(*i->then_branch)) ||
               (i->else_branch && contains_goto(*i->else_branch));
    }
    if (const auto *l = dynamic_cast<const LoopStmt *>(&_s)) {
        return l->body && contains_goto(*l->body);
    }
    if (const auto *d = dynamic_cast<const DoWhileStmt *>(&_s)) {
        return d->body && contains_goto(*d->body);
    }
    return false;
}

/// A bundle's main.ccat, parsed; null with `*_why` when unreadable or invalid.
std::unique_ptr<Program> parse_bundle(const std::filesystem::path &_library,
                                      const std::string &_bundle,
                                      std::string *_why) {
    if (!safe_leaf_name(_bundle)) {
        *_why = "\"" + _bundle + "\" is not a bundle name";
        return nullptr;
    }
    std::string text;
    if (!read_text_file(bundle_main(_library, _bundle), &text)) {
        *_why = "cannot read " + _bundle + "/main.ccat";
        return nullptr;
    }
    try {
        return parse_program(text);
    } catch (const parse_error &_e) {
        *_why = _bundle + "/main.ccat does not parse (line " +
                std::to_string(_e.line) + ", col " + std::to_string(_e.col) +
                ")";
        return nullptr;
    }
}

bool golden_there(const std::filesystem::path &_library,
                  const std::string &_bundle) {
    std::error_code ec;
    return std::filesystem::is_regular_file(_library / _bundle / "golden.md",
                                            ec);
}

/// `a-b` split at its only `-`.
bool split_edge_name(std::string_view _rest, std::string *_from,
                     std::string *_to) {
    const std::size_t dash = _rest.find('-');
    if (dash == std::string_view::npos ||
        _rest.find('-', dash + 1) != std::string_view::npos) {
        return false;
    }
    *_from = std::string(_rest.substr(0, dash));
    *_to = std::string(_rest.substr(dash + 1));
    return graph_part_name(*_from) && graph_part_name(*_to);
}

/// Ends of an edge that are not anchors, as one clause. Empty when both are.
std::string ends_without_golden(const std::filesystem::path &_library,
                                const std::string &_graph,
                                const std::string &_from,
                                const std::string &_to) {
    std::string missing;
    const auto add = [&](const std::string &_id) {
        if (golden_there(_library, _graph + "." + _id)) {
            return;
        }
        if (!missing.empty()) {
            missing += " and ";
        }
        missing += _graph + "." + _id;
    };
    add(_from);
    if (_to != _from) {
        add(_to);
    }
    if (missing.empty()) {
        return "";
    }
    const bool one = missing.find(" and ") == std::string::npos;
    return missing + (one ? " has no golden.md" : " have no golden.md");
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

sample take(const screen_graph &_graph, const nav_io &_io) {
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
    s.hit = locate(_graph, _io.check, frame);
    // A stop inside an anchor script surfaces as that anchor failing.
    if (s.hit.kind == locate_kind::error && _io.should_stop &&
        _io.should_stop()) {
        s.stopped = true;
    }
    return s;
}

/// Wait out a loading screen or an unrecognized one. Returns the sample that
/// ended the wait: a page, an interrupt, an error, or still unknown/transient
/// when the budget ran out. `_limits.unknown_ms == 0` does not wait.
sample wait_unknown(const screen_graph &_graph, const nav_io &_io,
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
        cur = take(_graph, _io);
        if (!cur.captured || cur.stopped) {
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

/// Run popup `_id`'s `close`; a failure's message names the hop.
bundle_run close_popup(const screen_graph &_graph, const std::string &_id,
                       const nav_io &_io) {
    const anchor_def *a = find_anchor(_graph, _id);
    bundle_run ran = _io.run_bundle(a->close);
    if (ran.failed) {
        ran.message = _id + " -[" + a->close + "]-> " + ran.message;
    }
    return ran;
}

enum class arrival { arrived, elsewhere, timeout, stopped, device, broken,
                     popup_failed };

struct arrival_out {
    arrival how = arrival::timeout;
    std::string why;
    std::string last = "unknown";
};

/// After `_edge` ran: poll until its `to` shows, closing popups on the way.
arrival_out wait_arrival(const screen_graph &_graph, const graph_edge &_edge,
                         const nav_io &_io, const nav_limits &_limits,
                         int *_interrupts, const char *_verb) {
    const auto started = std::chrono::steady_clock::now();
    const auto deadline =
        started + std::chrono::milliseconds(_limits.edge_timeout_ms);
    arrival_out out;
    for (;;) {
        const sample w = take(_graph, _io);
        if (w.stopped) {
            out.how = arrival::stopped;
            return out;
        }
        if (w.device) {
            out.how = arrival::device;
            return out;
        }
        out.last = w.hit.id.empty() ? "unknown" : w.hit.id;
        if (w.hit.kind == locate_kind::error ||
            w.hit.kind == locate_kind::ambiguous) {
            out.how = arrival::broken;
            out.why = w.hit.detail;
            return out;
        }
        if (w.hit.kind == locate_kind::interrupt) {
            if (*_interrupts >= _limits.max_interrupts) {
                out.how = arrival::popup_failed;
                out.why = "interrupt limit";
                return out;
            }
            ++*_interrupts;
            const bundle_run closed = close_popup(_graph, w.hit.id, _io);
            if (closed.stopped) {
                out.how = arrival::stopped;
                return out;
            }
            if (closed.failed) {
                out.how = arrival::popup_failed;
                out.why = closed.message;
                return out;
            }
            continue;
        }
        if ((w.hit.kind == locate_kind::page ||
             w.hit.kind == locate_kind::transient) &&
            w.hit.id == _edge.to) {
            const double sec = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
            std::ostringstream line;
            line.setf(std::ios::fixed);
            line.precision(1);
            line << _verb << ": " << _edge.from << " -> " << _edge.to
                 << " via " << _edge.script << " " << sec << " s";
            if (_io.log) {
                _io.log(line.str());
            }
            out.how = arrival::arrived;
            return out;
        }
        if (w.hit.kind == locate_kind::page && w.hit.id != _edge.from) {
            out.how = arrival::elsewhere;
            return out;
        }
        if (_limits.edge_timeout_ms <= 0 ||
            std::chrono::steady_clock::now() >= deadline) {
            out.how = arrival::timeout;
            out.why = "timeout " + std::to_string(_limits.edge_timeout_ms) +
                      " ms (last: " + out.last + ")";
            return out;
        }
        pause_ms(_io, _limits.poll_ms);
    }
}

} // namespace

bool split_action_name(std::string_view _rest, std::string *_anchor,
                       std::string *_action) {
    const std::size_t dot = _rest.find('.');
    if (dot == std::string_view::npos ||
        _rest.find('.', dot + 1) != std::string_view::npos) {
        return false;
    }
    *_anchor = std::string(_rest.substr(0, dot));
    *_action = std::string(_rest.substr(dot + 1));
    return graph_part_name(*_anchor) && graph_part_name(*_action);
}

std::string script_graph_block(const std::filesystem::path &_library,
                               const std::string &_name,
                               const Program &_program) {
    const std::size_t dot = _name.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= _name.size()) {
        return "";
    }
    const std::string graph = _name.substr(0, dot);
    const std::string rest = _name.substr(dot + 1);
    if (!graph_part_name(graph)) {
        return "";
    }
    std::string from;
    std::string to;
    if (split_edge_name(rest, &from, &to)) {
        const std::string missing =
            ends_without_golden(_library, graph, from, to);
        if (missing.empty()) {
            return "";
        }
        return _name + ": " + missing;
    }
    std::string anchor_id;
    std::string action;
    if (split_action_name(rest, &anchor_id, &action)) {
        const std::string bundle = graph + "." + anchor_id;
        if (!golden_there(_library, bundle)) {
            return _name + ": anchor " + anchor_id + " does not exist";
        }
        std::string text;
        std::string err;
        read_text_file(_library / bundle / "golden.md", &text);
        const auto doc = parse_golden_md(text, &err);
        if (!doc || doc->kind != anchor_kind::page) {
            return _name + ": anchor " + anchor_id + " is not a page";
        }
        if (std::any_of(_program.stmts.begin(), _program.stmts.end(),
                        [](const auto &_s) { return contains_goto(*_s); })) {
            return _name + ": goto is not allowed in an action";
        }
        return "";
    }
    if (!graph_part_name(rest) || golden_there(_library, _name)) {
        return "";
    }
    if (!anchor_problems(_program, nullptr).empty()) {
        return "";
    }
    return _name + ": anchor script with no golden.md; save_anchor writes it";
}

namespace {
/// Why `_part` cannot be one segment of a bundle name.
std::string bad_part(std::string_view _part) {
    if (_part.empty()) {
        return "the name is empty";
    }
    return std::string(_part) +
           " is not a name: lowercase letters, digits and _ only";
}
} // namespace

std::string create_action_bundle(const std::filesystem::path &_library,
                                 std::string_view _graph,
                                 std::string_view _anchor,
                                 std::string_view _action) {
    if (!graph_part_name(_graph)) {
        return bad_part(_graph);
    }
    if (!graph_part_name(_anchor)) {
        return bad_part(_anchor);
    }
    if (!graph_part_name(_action)) {
        return bad_part(_action);
    }
    const std::string on = std::string(_graph) + "." + std::string(_anchor);
    if (!golden_there(_library, on)) {
        return "anchor " + on + " does not exist";
    }
    std::string golden;
    std::string err;
    read_text_file(_library / on / "golden.md", &golden);
    const std::optional<golden_doc> doc = parse_golden_md(golden, &err);
    if (!doc.has_value()) {
        return on + ": " + err;
    }
    if (doc->kind != anchor_kind::page) {
        return on + " is not a page; an action runs on a page only";
    }
    const std::string bundle = on + "." + std::string(_action);
    const std::filesystem::path main = bundle_main(_library, bundle);
    std::error_code ec;
    if (std::filesystem::exists(main, ec)) {
        return bundle + " already exists";
    }
    std::filesystem::create_directories(main.parent_path(), ec);
    std::ofstream out(main, std::ios::binary | std::ios::trunc);
    out << "// " << _action << " on " << _graph << ":" << _anchor << "\n"
        << "// An action starts and ends on " << _anchor
        << "; goto is not allowed in an action.\n"
        << "return false\n";
    out.close();
    if (!out) {
        return "could not write " + bundle + "/main.ccat";
    }
    return "";
}

std::optional<golden_doc> parse_golden_md(std::string_view _text,
                                          std::string *_err) {
    std::string text(_text);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
        text.erase(0, 3);
    }
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    const auto fail = [&](const std::string &_why) {
        if (_err != nullptr) {
            *_err = "golden.md: " + _why;
        }
        return std::optional<golden_doc>{};
    };
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || trimmed(line) != "---") {
        return fail("must start with a --- line");
    }
    golden_doc doc;
    bool closed = false;
    while (std::getline(in, line)) {
        const std::string t = trimmed(line);
        if (t == "---") {
            closed = true;
            break;
        }
        if (t.empty()) {
            continue;
        }
        const auto colon = t.find(':');
        if (colon == std::string::npos) {
            return fail("\"" + t + "\" is not key: value");
        }
        const std::string key = trimmed(std::string_view(t).substr(0, colon));
        const std::string value =
            trimmed(std::string_view(t).substr(colon + 1));
        if (key == "kind") {
            if (value == "page") {
                doc.kind = anchor_kind::page;
            } else if (value == "transient") {
                doc.kind = anchor_kind::transient;
            } else if (value == "popup") {
                doc.kind = anchor_kind::popup;
            } else {
                return fail("kind must be page, transient or popup");
            }
        } else if (key == "close") {
            doc.close = value;
        } else {
            return fail("unknown key \"" + key + "\"");
        }
    }
    if (!closed) {
        return fail("front matter has no closing --- line");
    }
    if (doc.kind == anchor_kind::popup && doc.close.empty()) {
        return fail("a popup needs close: <bundle>");
    }
    doc.description = trimmed(std::string{std::istreambuf_iterator<char>(in),
                                          std::istreambuf_iterator<char>()});
    return doc;
}

std::vector<std::string> anchor_script_problems(const Program &_program) {
    return anchor_problems(_program, nullptr);
}

bool graph_part_name(std::string_view _s) {
    return !_s.empty() &&
           std::all_of(_s.begin(), _s.end(), [](char _c) {
               return (_c >= 'a' && _c <= 'z') || (_c >= '0' && _c <= '9') ||
                      _c == '_';
           });
}

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

scanned_graph scan_graph(const std::filesystem::path &_library,
                         std::string_view _name) {
    scanned_graph out;
    const std::string name(_name);
    out.graph.name = name;
    std::vector<std::string> &problems = out.problems;
    if (!graph_part_name(name)) {
        problems.push_back("graph name \"" + name + "\" is not [a-z0-9_]+");
        return out;
    }
    const std::string prefix = name + ".";
    std::vector<std::string> rests;
    for (const std::string &b : bundle_names(_library)) {
        if (b.rfind(prefix, 0) == 0) {
            rests.push_back(b.substr(prefix.size()));
        }
    }
    std::sort(rests.begin(), rests.end());

    for (const std::string &rest : rests) {
        const std::string bundle = prefix + rest;
        if (!graph_part_name(rest)) {
            continue;
        }
        if (!golden_there(_library, bundle)) {
            std::string why;
            if (const auto prog = parse_bundle(_library, bundle, &why)) {
                if (anchor_problems(*prog, nullptr).empty()) {
                    problems.push_back(
                        bundle +
                        ": anchor script with no golden.md; save_anchor writes it");
                }
            }
            continue;
        }
        anchor_def a;
        a.id = rest;
        std::string text;
        std::string err;
        read_text_file(_library / bundle / "golden.md", &text);
        if (const auto doc = parse_golden_md(text, &err)) {
            a.kind = doc->kind;
            a.close = doc->close;
            a.description = doc->description;
        } else {
            problems.push_back(bundle + ": " + err);
        }
        std::string why;
        if (const auto prog = parse_bundle(_library, bundle, &why)) {
            std::vector<std::string> has;
            for (const std::string &p : anchor_problems(*prog, &has)) {
                problems.push_back(bundle + ": " + p);
            }
            for (const std::string &png : has) {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(_library / bundle / png,
                                                      ec)) {
                    problems.push_back(bundle + ": missing template " + png);
                }
            }
        } else {
            problems.push_back(why);
        }
        if (a.kind == anchor_kind::popup && !a.close.empty() &&
            !parse_bundle(_library, a.close, &why)) {
            problems.push_back(bundle + ": close " + why);
        }
        out.graph.anchors.push_back(std::move(a));
    }

    for (const std::string &rest : rests) {
        graph_edge e;
        if (!split_edge_name(rest, &e.from, &e.to)) {
            continue;
        }
        e.script = prefix + rest;
        const anchor_def *from = find_anchor(out.graph, e.from);
        const anchor_def *to = find_anchor(out.graph, e.to);
        if (from == nullptr || to == nullptr) {
            const std::string missing =
                ends_without_golden(_library, name, e.from, e.to);
            if (!missing.empty()) {
                problems.push_back(e.script + ": " + missing);
            }
            continue;
        }
        if (from->kind != anchor_kind::page || to->kind != anchor_kind::page) {
            problems.push_back(e.script + ": an edge must join two pages");
        }
        std::string why;
        if (const auto prog = parse_bundle(_library, e.script, &why)) {
            if (std::any_of(prog->stmts.begin(), prog->stmts.end(),
                            [](const auto &_s) { return contains_goto(*_s); })) {
                problems.push_back(e.script + ": goto is not allowed in an edge");
            }
        } else {
            problems.push_back(why);
        }
        out.graph.edges.push_back(std::move(e));
    }

    for (const std::string &rest : rests) {
        std::string anchor_id;
        std::string action;
        if (!split_action_name(rest, &anchor_id, &action)) {
            continue;
        }
        const std::string script = prefix + rest;
        const anchor_def *on = find_anchor(out.graph, anchor_id);
        if (on == nullptr) {
            problems.push_back(script + ": anchor " + anchor_id +
                               " does not exist");
            continue;
        }
        if (on->kind != anchor_kind::page) {
            problems.push_back(script + ": anchor " + anchor_id +
                               " is not a page");
            continue;
        }
        std::string why;
        if (const auto prog = parse_bundle(_library, script, &why)) {
            if (std::any_of(prog->stmts.begin(), prog->stmts.end(),
                            [](const auto &_s) { return contains_goto(*_s); })) {
                problems.push_back(script +
                                   ": goto is not allowed in an action");
                continue;
            }
        } else {
            problems.push_back(why);
            continue;
        }
        out.graph.actions.push_back({anchor_id, action, script});
    }

    const anchor_def *entry = find_anchor(out.graph, "entry");
    out.exists = entry != nullptr;
    if (entry == nullptr) {
        problems.push_back(prefix + "entry is missing: every graph starts there");
    } else if (entry->kind != anchor_kind::page) {
        problems.push_back(prefix + "entry must be a page");
    }

    std::map<std::string, bool> reach;
    std::queue<std::string> q;
    if (entry != nullptr) {
        reach["entry"] = true;
        q.push("entry");
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
    if (entry != nullptr) {
        for (const anchor_def &a : out.graph.anchors) {
            if (a.kind == anchor_kind::page && !reach[a.id]) {
                problems.push_back(prefix + a.id +
                                   " is not reachable from entry");
            }
        }
    }
    return out;
}

loaded_graph load_graph(const std::filesystem::path &_library,
                        std::string_view _name) {
    scanned_graph scanned = scan_graph(_library, _name);
    loaded_graph out;
    if (!scanned.exists && graph_part_name(_name)) {
        out.error = "graph \"" + std::string(_name) + "\" does not exist: no " +
                    std::string(_name) + ".entry with a golden.md";
        return out;
    }
    if (!scanned.problems.empty()) {
        out.error = join_lines(scanned.problems);
        return out;
    }
    out.ok = true;
    out.graph = std::move(scanned.graph);
    return out;
}

std::vector<std::string> graph_names(const std::filesystem::path &_library) {
    constexpr std::string_view suffix = ".entry";
    std::vector<std::string> out;
    for (const std::string &b : bundle_names(_library)) {
        if (b.size() <= suffix.size() ||
            b.compare(b.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        const std::string g = b.substr(0, b.size() - suffix.size());
        if (graph_part_name(g) && golden_there(_library, b)) {
            out.push_back(g);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

locate_hit locate(const screen_graph &_graph, const anchor_check_fn &_check,
                  const cv::Mat &_frame) {
    locate_hit hit;
    if (_frame.empty()) {
        hit.kind = locate_kind::error;
        hit.detail = "empty frame";
        return hit;
    }
    /// @return false, with `hit` set to the error, when the check failed.
    const auto ask = [&](const anchor_def &_a, bool *_here) {
        const anchor_check r = _check(_a, _frame);
        if (!r.error.empty()) {
            hit.kind = locate_kind::error;
            hit.detail = _a.id + " anchor failed: " + r.error;
            return false;
        }
        *_here = r.here;
        return true;
    };
    for (const anchor_def &a : _graph.anchors) {
        if (a.kind != anchor_kind::popup) {
            continue;
        }
        bool here = false;
        if (!ask(a, &here)) {
            return hit;
        }
        if (here) {
            hit.kind = locate_kind::interrupt;
            hit.id = a.id;
            return hit;
        }
    }
    std::vector<const anchor_def *> hits;
    for (const anchor_def &a : _graph.anchors) {
        if (a.kind == anchor_kind::popup) {
            continue;
        }
        bool here = false;
        if (!ask(a, &here)) {
            return hit;
        }
        if (here) {
            hits.push_back(&a);
        }
    }
    if (hits.empty()) {
        hit.kind = locate_kind::unknown;
        return hit;
    }
    if (hits.size() > 1) {
        std::vector<std::string> ids;
        for (const anchor_def *a : hits) {
            ids.push_back(a->id);
        }
        hit.kind = locate_kind::ambiguous;
        hit.detail = "ambiguous: " + join_semi(ids);
        return hit;
    }
    hit.kind = hits[0]->kind == anchor_kind::transient ? locate_kind::transient
                                                        : locate_kind::page;
    hit.id = hits[0]->id;
    return hit;
}

nav_result navigate(const screen_graph &_graph, const std::string &_target,
                    const nav_io &_io, nav_limits _limits) {
    const std::string where = "goto " + _graph.name + ":" + _target;
    nav_result done;
    const anchor_def *target = find_anchor(_graph, _target);
    if (target == nullptr) {
        done.message = where + " unknown anchor";
        return done;
    }
    if (target->kind != anchor_kind::page) {
        done.message = where + " is not a place to stay";
        return done;
    }

    int failures = 0;
    int interrupts = 0;
    std::vector<std::string> trace;
    const auto failed = [&]() {
        nav_result r;
        r.message = where + " failed: " + join_semi(trace);
        return r;
    };
    /// @return true once `max_failures` is reached.
    const auto give_up = [&](std::string _why) {
        trace.push_back(std::move(_why));
        return ++failures >= _limits.max_failures;
    };
    const auto stopped = [] {
        nav_result r;
        r.stopped = true;
        return r;
    };
    const auto device = [&] {
        nav_result r;
        r.device = true;
        r.message = where + " screencap failed";
        return r;
    };
    const auto broken = [&](const std::string &_detail) {
        nav_result r;
        r.message = where + " " + _detail;
        return r;
    };

    for (;;) {
        sample s = take(_graph, _io);
        if (s.stopped) {
            return stopped();
        }
        if (s.device) {
            return device();
        }
        if (s.hit.kind == locate_kind::error ||
            s.hit.kind == locate_kind::ambiguous) {
            return broken(s.hit.detail);
        }

        if (s.hit.kind == locate_kind::interrupt) {
            if (interrupts >= _limits.max_interrupts) {
                if (give_up("interrupt limit")) {
                    return failed();
                }
                continue;
            }
            ++interrupts;
            const bundle_run ran = close_popup(_graph, s.hit.id, _io);
            if (ran.stopped) {
                return stopped();
            }
            if (ran.failed && give_up(ran.message)) {
                return failed();
            }
            continue;
        }

        if (s.hit.kind == locate_kind::unknown ||
            s.hit.kind == locate_kind::transient) {
            s = wait_unknown(_graph, _io, _limits, std::move(s));
            if (s.stopped) {
                return stopped();
            }
            if (s.device) {
                return device();
            }
            if (s.hit.kind == locate_kind::error ||
                s.hit.kind == locate_kind::ambiguous) {
                return broken(s.hit.detail);
            }
            if (s.hit.kind == locate_kind::interrupt) {
                continue; // the next loop handles it, keeping the cap
            }
            if (s.hit.kind != locate_kind::page) {
                const std::string last =
                    s.hit.kind == locate_kind::transient ? s.hit.id : "unknown";
                if (give_up("unknown (" + last + ")")) {
                    return failed();
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
                return failed();
            }
            continue;
        }

        // `goto` runs the edge here, not through `run_edge`. Held across the
        // script and the wait to land, then dropped before the next hop.
        const running_bundle_guard hold(hop->script);
        const bundle_run ran = _io.run_bundle(hop->script);
        if (ran.stopped) {
            return stopped();
        }
        if (ran.failed) {
            if (give_up(hop->from + " -[" + hop->script + "]-> " +
                        ran.message)) {
                return failed();
            }
            continue;
        }

        const arrival_out w =
            wait_arrival(_graph, *hop, _io, _limits, &interrupts, "goto");
        switch (w.how) {
        case arrival::arrived:
            if (hop->to == _target) {
                done.ok = true;
                return done;
            }
            break;
        case arrival::elsewhere: // somewhere else: plan again, no failure
            break;
        case arrival::stopped:
            return stopped();
        case arrival::device:
            return device();
        case arrival::broken:
            return broken(w.why);
        case arrival::timeout:
            if (give_up(hop->from + " -[" + hop->script + "]-> " + w.why)) {
                return failed();
            }
            break;
        case arrival::popup_failed:
            if (give_up(w.why)) {
                return failed();
            }
            break;
        }
    }
}

nav_result run_action(const screen_graph &_graph, const graph_action &_action,
                      const nav_io &_io, nav_limits _limits) {
    const graph_edge as_edge{_action.anchor, _action.anchor, _action.script};
    return run_edge(_graph, as_edge, _io, _limits);
}

nav_result run_edge(const screen_graph &_graph, const graph_edge &_edge,
                    const nav_io &_io, nav_limits _limits) {
    // The whole hop, including the wait to land. `goto` reaches an edge only
    // through here, so this is what the graph page highlights.
    const running_bundle_guard hold(_edge.script);
    const std::string where = "run " + _edge.script + ": ";
    nav_result r;
    const auto fail = [&](const std::string &_why) {
        r.message = where + _why;
        return r;
    };
    const auto stopped = [&] {
        r.stopped = true;
        return r;
    };
    const auto device = [&] {
        r.device = true;
        return fail("screencap failed");
    };

    int interrupts = 0;
    sample s;
    for (;;) {
        s = take(_graph, _io);
        if (!s.stopped && !s.device &&
            (s.hit.kind == locate_kind::unknown ||
             s.hit.kind == locate_kind::transient)) {
            s = wait_unknown(_graph, _io, _limits, std::move(s));
        }
        if (s.stopped) {
            return stopped();
        }
        if (s.device) {
            return device();
        }
        if (s.hit.kind == locate_kind::error ||
            s.hit.kind == locate_kind::ambiguous) {
            return fail(s.hit.detail);
        }
        if (s.hit.kind != locate_kind::interrupt) {
            break;
        }
        if (interrupts >= _limits.max_interrupts) {
            return fail("interrupt limit");
        }
        ++interrupts;
        const bundle_run closed = close_popup(_graph, s.hit.id, _io);
        if (closed.stopped) {
            return stopped();
        }
        if (closed.failed) {
            return fail(closed.message);
        }
    }
    if (s.hit.kind != locate_kind::page || s.hit.id != _edge.from) {
        return fail("not at " + _edge.from + " (at " +
                    (s.hit.id.empty() ? std::string("unknown") : s.hit.id) +
                    ")");
    }

    const bundle_run ran = _io.run_bundle(_edge.script);
    if (ran.stopped) {
        return stopped();
    }
    if (ran.failed) {
        return fail(ran.message);
    }

    const arrival_out w =
        wait_arrival(_graph, _edge, _io, _limits, &interrupts, "run");
    switch (w.how) {
    case arrival::arrived:
        r.ok = true;
        return r;
    case arrival::stopped:
        return stopped();
    case arrival::device:
        return device();
    case arrival::elsewhere:
        return fail("landed on " + w.last);
    case arrival::broken:
    case arrival::timeout:
    case arrival::popup_failed:
        break;
    }
    return fail(w.why);
}

bundle_tag bundle_tags(const std::filesystem::path &_library,
                       std::string_view _name) {
    bundle_tag tag;
    const std::size_t dot = _name.find('.');
    if (dot == std::string_view::npos) {
        return tag;
    }
    const std::string g(_name.substr(0, dot));
    const std::string rest(_name.substr(dot + 1));
    if (!graph_part_name(g)) {
        return tag;
    }
    const auto is_anchor = [&](const std::string &_id) {
        return graph_part_name(_id) && golden_there(_library, g + "." + _id);
    };
    if (is_anchor(rest)) {
        tag.anchor_of = g;
        std::string text;
        std::string err;
        read_text_file(_library / std::string(_name) / "golden.md", &text);
        if (const auto doc = parse_golden_md(text, &err)) {
            tag.kind = doc->kind;
        }
        if (rest == "entry") {
            const scanned_graph s = scan_graph(_library, g);
            tag.anchors = static_cast<int>(s.graph.anchors.size());
            tag.edges = static_cast<int>(s.graph.edges.size());
            tag.actions = static_cast<int>(s.graph.actions.size());
        }
        return tag;
    }
    std::string from;
    std::string to;
    if (split_edge_name(rest, &from, &to) && is_anchor(from) && is_anchor(to)) {
        tag.edge_of = g;
        return tag;
    }
    std::string anchor_id;
    std::string action;
    if (split_action_name(rest, &anchor_id, &action)) {
        tag.action_of = g;
        tag.action_on = anchor_id;
    }
    return tag;
}

std::string graph_summary(const scanned_graph &_scanned) {
    const screen_graph &g = _scanned.graph;
    std::string out = "graph: " + g.name + "\nanchors: " +
                      std::to_string(g.anchors.size()) + "\n";
    for (const anchor_def &a : g.anchors) {
        out += "  " + a.id + " [" + kind_word(a.kind);
        if (a.kind == anchor_kind::popup) {
            out += ", close " + a.close;
        }
        out += "] " + a.description.substr(0, a.description.find('\n')) + "\n";
    }
    out += "edges: " + std::to_string(g.edges.size()) + "\n";
    for (const graph_edge &e : g.edges) {
        out += "  " + e.from + " -> " + e.to + " (" + e.script + ")\n";
    }
    out += "actions: " + std::to_string(g.actions.size()) + "\n";
    for (const graph_action &a : g.actions) {
        out += "  " + a.anchor + "." + a.name + "\n";
    }
    if (_scanned.problems.empty()) {
        out += "problems: none";
    } else {
        out += "problems:";
        for (const std::string &p : _scanned.problems) {
            out += "\n  - " + p;
        }
    }
    return out;
}

std::string running_graph_bundle() {
    std::lock_guard<std::mutex> lock(g_running_mu);
    return g_running_bundle;
}

void set_running_graph_bundle(std::string _bundle) {
    std::lock_guard<std::mutex> lock(g_running_mu);
    g_running_bundle = std::move(_bundle);
}

} // namespace campcat::ccat_lang
