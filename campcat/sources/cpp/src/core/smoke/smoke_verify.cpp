#include "core/smoke/smoke_run.h"

#include "core/script/ccat_graph.h"
#include "core/script/ccat_bundle.h"
#include "core/file_io.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace campcat {
namespace {
using nlohmann::json;
using path = std::filesystem::path;

std::string read(const path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

path resolve(const path &library, const std::string &script,
             const std::string &operand) {
    const path base = script == "<inline>" ? library : path(script).parent_path();
    return std::filesystem::weakly_canonical(base / operand);
}

struct invocation {
    path target;
    bool input = false, postcondition = false, current = true, saw_source = false;
    std::set<path> dependencies;
    std::set<path> templates;
    std::set<path> found_before, absent_before, found_after, observed;
};

using case_guards = std::map<path, std::pair<path, path>>;
using case_results = std::map<path, std::set<std::string>>;

// A receipt from an old script or template does not validate today's bundle.
bool unchanged(const invocation &call, const path &log) {
    const auto when = std::filesystem::last_write_time(log);
    for (const auto &png : call.templates) {
        if (!std::filesystem::is_regular_file(png) ||
            std::filesystem::last_write_time(png) > when) return false;
    }
    for (const auto &dir : call.dependencies) {
        for (const auto &file : std::filesystem::directory_iterator(dir)) {
            const auto name = file.path().filename();
            if ((name == "main.ccat" || name == "golden.md" ||
                 file.path().extension() == ".png") &&
                std::filesystem::last_write_time(file.path()) > when)
                return false;
        }
    }
    return true;
}

std::set<path> verified_replays(const path &library, const path &root,
                               const std::map<path, bool> &required,
                               const case_guards &guards, case_results &covered,
                               std::map<path, std::set<path>> &observed,
                               std::vector<std::string> &problems) {
    std::set<path> verified;
    if (!std::filesystem::exists(root)) return verified;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        const auto log = entry.path() / "actions.jsonl";
        if (!entry.is_directory() || !std::filesystem::exists(log)) continue;
        try {
            std::ifstream in(log);
            std::map<unsigned long long, invocation> active;
            std::map<unsigned long long, std::pair<path, bool>> sources;
            std::set<path> passed;
            case_results cases;
            std::map<path, std::set<path>> seen;
            const auto accept = [&](const invocation &call, bool success,
                                    const std::string &code) {
                if (!call.current || !call.saw_source || !unchanged(call, log)) return;
                if (success && (!required.at(call.target) || (call.input && call.postcondition)))
                    passed.insert(call.target);
                if (success && call.input)
                    seen[call.target].insert(call.observed.begin(), call.observed.end());
                if (const auto it = guards.find(call.target); it != guards.end()) {
                    const auto &[ready, done] = it->second;
                    if (success && call.input && call.postcondition && call.found_after.contains(done))
                        cases[call.target].insert("ready");
                    if (success && !call.input && call.found_before.contains(done))
                        cases[call.target].insert("done");
                    if (!success && !call.input && code == "script_failed" &&
                        call.absent_before.contains(ready) && call.absent_before.contains(done))
                        cases[call.target].insert("missing");
                }
            };
            bool started = false, ended = false, succeeded = false, expected_failure = false;
            for (std::string line; std::getline(in, line);) {
                const auto row = json::parse(line);
                const auto event = row.at("event").get<std::string>();
                if (ended || (!started && event != "run_start"))
                    throw std::runtime_error("invalid audit order");
                if (event == "run_start") {
                    if (started) throw std::runtime_error("duplicate run_start");
                    started = true;
                } else if (event == "source") {
                    const auto script = row.at("script").get<std::string>();
                    if (script == "<inline>") continue;
                    const auto file = std::filesystem::weakly_canonical(script);
                    sources[row.at("source_id").get<unsigned long long>()] =
                        {file, std::filesystem::is_regular_file(file) &&
                               row.at("source").get<std::string>() == read(file)};
                } else if (event == "step_start" && row.value("action", "") == "run") {
                    const auto operands = row.at("operands").get<std::vector<std::string>>();
                    if (operands.size() != 1) continue;
                    const auto target = resolve(library, row.at("script"), operands[0]);
                    if (required.contains(target))
                        active.emplace(row.at("step_id").get<unsigned long long>(),
                                       invocation{.target = target});
                } else if (event == "step") {
                    const auto script = row.at("script").get<std::string>();
                    for (auto &[id, call] : active) {
                        for (const auto &match : row.value("matches", json::array())) {
                            if (!match.contains("template") || !match.at("template").is_string() ||
                                match.at("template").get<std::string>().empty())
                                throw std::runtime_error("match receipt has no template path");
                            const auto png = std::filesystem::weakly_canonical(match.at("template").get<std::string>());
                            call.templates.insert(png);
                            const auto fingerprint = match.value("fingerprint", "");
                            if (!fingerprint.empty())
                                call.current &= std::filesystem::is_regular_file(png) && file_fingerprint(png) == fingerprint;
                        }
                        if (script != "<inline>") {
                            const auto file = std::filesystem::weakly_canonical(script);
                            const auto source = sources.find(row.value("source_id", 0ull));
                            call.current &= source != sources.end() && source->second.first == file && source->second.second;
                            call.saw_source |= file == call.target && source != sources.end();
                            call.dependencies.insert(file.parent_path());
                            if (file == call.target && !call.input && row.value("action", "") == "has") {
                                for (const auto &match : row.value("matches", json::array())) {
                                    if (!match.contains("template")) continue;
                                    const auto png = std::filesystem::weakly_canonical(match.at("template").get<std::string>());
                                    (match.value("found", false) ? call.found_before : call.absent_before).insert(png);
                                }
                            }
                        }
                        if (!row.value("ok", false) || !row.value("value", false)) continue;
                        const auto action = row.value("action", "");
                        if ((action == "tap" || action == "tap_at" || action == "tap_offset" ||
                             action == "swipe" || action == "swipe_at" || action == "home") &&
                            (action == "home" || !row.value("input_pixels", json::array()).empty())) {
                            call.input = true;
                            call.postcondition = false;
                            call.found_after.clear();
                        }
                        if (action == "wait_until" && call.input) {
                            for (const auto &match : row.value("matches", json::array())) {
                                call.postcondition |= match.value("found", false);
                                if (match.value("found", false) && match.contains("template"))
                                    call.found_after.insert(std::filesystem::weakly_canonical(
                                        match.at("template").get<std::string>()));
                            }
                        }
                        if ((action == "has" || action == "wait_until") && call.input) {
                            for (const auto &match : row.value("matches", json::array())) {
                                if (match.value("found", false) && match.contains("template"))
                                    call.observed.insert(std::filesystem::weakly_canonical(
                                        match.at("template").get<std::string>()));
                            }
                        }
                    }
                    const auto it = active.find(row.value("step_id", 0ull));
                    if (row.value("action", "") == "run" && it != active.end()) {
                        const auto &call = it->second;
                        accept(call, row.value("ok", false) && row.value("value", false),
                               row.value("error_code", ""));
                        active.erase(it);
                    }
                } else if (event == "run_end") {
                    ended = true;
                    const bool ok = row.value("ok", false);
                    const auto code = row.value("error_code", "");
                    succeeded = ok && active.empty();
                    expected_failure = !ok && code == "script_failed" && active.empty();
                }
            }
            if (!ended) problems.push_back("incomplete audit: " + log.string());
            else if (succeeded || expected_failure) {
                for (const auto &[file, outcomes] : cases) {
                    for (const auto &outcome : outcomes)
                        if (succeeded || outcome == "missing") covered[file].insert(outcome);
                }
                if (succeeded) {
                    verified.insert(passed.begin(), passed.end());
                    for (const auto &[file, templates] : seen)
                        observed[file].insert(templates.begin(), templates.end());
                }
            }
        } catch (const std::exception &error) {
            problems.push_back("invalid audit " + log.string() + ": " + error.what());
        }
    }
    return verified;
}
} // namespace

std::vector<std::string> smoke_problems(const path &library, const path &replays,
                                      const path &expectations) {
    std::vector<std::string> problems;
    try {
        std::map<std::string, std::set<std::string>> actual;
        std::map<path, bool> required;
        case_guards guards;
        std::map<path, std::set<path>> observations, observed;
        const auto graphs = ccat_lang::graph_names(library);
        if (graphs.empty()) problems.push_back("no graph was built");
        for (const auto &graph : graphs) {
            const auto scanned = ccat_lang::scan_graph(library, graph);
            for (const auto &problem : scanned.problems) problems.push_back(graph + ": " + problem);
            for (const auto &anchor : scanned.graph.anchors) actual["anchors"].insert(graph + "." + anchor.id);
            for (const auto &edge : scanned.graph.edges) {
                actual["edges"].insert(edge.script);
                required[std::filesystem::weakly_canonical(library / edge.script / "main.ccat")] = false;
            }
            for (const auto &action : scanned.graph.actions) {
                actual["actions"].insert(action.script);
                required[std::filesystem::weakly_canonical(library / action.script / "main.ccat")] = true;
            }
        }
        if (!expectations.empty()) {
            const auto fixture = json::parse(read(expectations));
            if (!fixture.is_object()) throw std::runtime_error("expectations must be an object");
            for (const auto &[graph, spec] : fixture.items()) {
                if (!spec.is_object()) throw std::runtime_error("graph expectations must be an object");
                for (const auto &[kind, names] : spec.items()) {
                    if (kind == "observations") {
                        if (!names.is_object()) throw std::runtime_error("observations must be an object");
                        for (const auto &[action, templates] : names.items()) {
                            const auto file = std::filesystem::weakly_canonical(library / (graph + "." + action) / "main.ccat");
                            if (!required.contains(file) || !required.at(file)) {
                                problems.push_back("missing expected action " + graph + "." + action);
                                continue;
                            }
                            for (const auto &name : templates.get<std::vector<std::string>>()) {
                                if (!ccat_lang::safe_leaf_name(name)) throw std::runtime_error("observation must name a template in the action bundle");
                                observations[file].insert(file.parent_path() / name);
                            }
                        }
                        continue;
                    }
                    if (kind == "cases") {
                        if (!names.is_object()) throw std::runtime_error("cases must be an object");
                        for (const auto &[action, templates] : names.items()) {
                            const auto file = std::filesystem::weakly_canonical(library / (graph + "." + action) / "main.ccat");
                            if (!required.contains(file) || !required.at(file)) {
                                problems.push_back("missing expected action " + graph + "." + action);
                                continue;
                            }
                            const auto ready = templates.at("ready").get<std::string>();
                            const auto done = templates.at("done").get<std::string>();
                            if (templates.size() != 2 || ready == done ||
                                !ccat_lang::safe_leaf_name(ready) || !ccat_lang::safe_leaf_name(done))
                                throw std::runtime_error("case guards require distinct ready and done template names");
                            guards[file] = {file.parent_path() / ready, file.parent_path() / done};
                        }
                        continue;
                    }
                    if (kind != "anchors" && kind != "edges" && kind != "actions")
                        throw std::runtime_error("unknown expectation: " + kind);
                    const std::string singular = kind == "anchors" ? "anchor" : kind == "edges" ? "edge" : "action";
                    for (const auto &name : names.get<std::vector<std::string>>()) {
                        const auto bundle = graph + "." + name;
                        if (!actual[kind].contains(bundle))
                            problems.push_back("missing expected " + singular + " " + bundle);
                    }
                }
            }
        }
        case_results covered;
        const auto verified = verified_replays(library, replays, required, guards, covered,
                                               observed, problems);
        for (const auto &[file, action] : required) {
            if (!verified.contains(file))
                problems.push_back(std::string("unverified ") + (action ? "action " : "edge ") +
                                   file.parent_path().filename().string() +
                                   (action ? ": need input, subsequent wait_until, and successful run" : ": need successful run"));
        }
        for (const auto &[file, _] : guards) {
            for (const auto *scenario : {"ready", "done", "missing"}) {
                if (!covered[file].contains(scenario))
                    problems.push_back("unverified case " + file.parent_path().filename().string() + "/" + scenario);
            }
        }
        for (const auto &[file, templates] : observations) {
            for (const auto &png : templates) {
                if (!observed[file].contains(png))
                    problems.push_back("unobserved " + file.parent_path().filename().string() + "/" + png.filename().string());
            }
        }
    } catch (const std::exception &error) {
        problems.push_back(error.what());
    }
    return problems;
}
} // namespace campcat
