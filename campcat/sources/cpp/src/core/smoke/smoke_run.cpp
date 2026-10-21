#include "core/smoke/smoke_run.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/llm/llm_host.h"
#include "core/mumu_launch.h"
#include "core/script/ccat_graph.h"

namespace campcat {
namespace {

using run_clock = std::chrono::steady_clock;

/// One line of the run, flushed as it is written: a smoke run is watched live,
/// and a crash must not swallow the lines that came before it.
void say(const std::string &_line) {
    std::printf("[smoke] %s\n", _line.c_str());
    std::fflush(stdout);
}

std::string seconds_since(run_clock::time_point _from) {
    const double elapsed =
        std::chrono::duration<double>(run_clock::now() - _from).count();
    char out[32];
    std::snprintf(out, sizeof(out), "%.1fs", elapsed);
    return out;
}

/// `YYYYMMDD-HHMMSS`, local time: one directory per run, named in the order the
/// runs happened.
std::string stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char out[32];
    std::strftime(out, sizeof(out), "%Y%m%d-%H%M%S", &tm);
    return out;
}

/// `_to` gets `_from`'s bytes when there are any: a run needs the checkout's
/// `CCAT.md` and the notes the real home had, but must not write back into it.
void copy_if_present(const std::filesystem::path &_from,
                     const std::filesystem::path &_to) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(_from, ec)) {
        std::filesystem::copy_file(
            _from, _to, std::filesystem::copy_options::overwrite_existing, ec);
    }
}

/// What the run built, one line per graph. Reported rather than judged: a graph
/// with problems is still the thing whose cost was measured.
void report_graphs(const std::filesystem::path &_library) {
    const std::vector<std::string> names = ccat_lang::graph_names(_library);
    if (names.empty()) {
        say("graph: none (no <name>.entry was written)");
        return;
    }
    for (const std::string &name : names) {
        const ccat_lang::scanned_graph scanned =
            ccat_lang::scan_graph(_library, name);
        say("graph " + name + ": anchors " +
            std::to_string(scanned.graph.anchors.size()) + ", edges " +
            std::to_string(scanned.graph.edges.size()) + ", actions " +
            std::to_string(scanned.graph.actions.size()) + ", problems " +
            (scanned.problems.empty() ? "none"
                                      : std::to_string(scanned.problems.size())));
    }
}

} // namespace

std::vector<std::string> smoke_prompts(std::string_view _text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= _text.size()) {
        const std::size_t newline = _text.find('\n', pos);
        std::string line(_text.substr(
            pos, newline == std::string_view::npos ? _text.size() - pos
                                                   : newline - pos));
        // A list saved on Windows carries the CR, and it would reach the model.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t first = line.find_first_not_of(" \t");
        const std::size_t last = line.find_last_not_of(" \t");
        if (first != std::string::npos && line[first] != '#') {
            out.push_back(line.substr(first, last - first + 1));
        }
        if (newline == std::string_view::npos) {
            break;
        }
        pos = newline + 1;
    }
    return out;
}

int run_smoke(const std::filesystem::path &_prompts,
              const std::filesystem::path &_config) {
    const run_clock::time_point began = run_clock::now();

    std::vector<std::string> prompts;
    {
        std::ifstream in(_prompts);
        if (!in) {
            say("cannot read " + _prompts.string());
            return 1;
        }
        std::ostringstream text;
        text << in.rdbuf();
        prompts = smoke_prompts(text.str());
    }
    if (prompts.empty()) {
        say("no prompts in " + _prompts.string());
        return 1;
    }

    app_config cfg = app_config::load_from_file(_config);
    // Nobody redraws a region on this path, but this config would still file a
    // sample if anything ever did: keep the smoke run's screens out of them.
    cfg.region_score.record = false;

    const run_clock::time_point booting = run_clock::now();
    const mumu_status launched = launch_and_wait(
        cfg, {}, launch_sink{.log = [](std::string_view _line) {
                     say(std::string(_line));
                 }});
    if (launched.state != mumu_state::running) {
        say("emulator: " + launched.message + "; " + seconds_since(booting));
        return 1;
    }
    say("emulator " + seconds_since(booting) +
        (launched.message.empty() ? "" : " (" + launched.message + ")"));

    const std::string address =
        mumu_adb_address(cfg.mumu.tool_path, cfg.mumu.device_index);
    adb_client adb(cfg.adb.path, address.empty() ? cfg.adb.serial : address);
    if (!adb.connect_remote(address)) {
        say("adb could not connect to " +
            (address.empty() ? std::string("the default device") : address));
        return 1;
    }

    // A fresh directory per run: nothing written here can reach the real
    // library, and no run inherits a screen from the last one.
    const std::filesystem::path real_home = cfg.config_home;
    const std::filesystem::path library =
        real_home.parent_path() / "smoke" / stamp();
    std::error_code ec;
    std::filesystem::create_directories(library, ec);
    if (ec) {
        say("cannot create " + library.string() + ": " + ec.message());
        return 1;
    }
    // The fresh directory is also `config_home` for the turn: `locate`, `goto`
    // and the interpreter's graph loads read that field directly instead of
    // going through the tool context, so a real home would answer every new
    // screen with one of its own anchors and refuse the save as a duplicate.
    cfg.config_home = library.string();
    copy_if_present(real_home.parent_path() / "CCAT.md",
                    library.parent_path() / "CCAT.md");
    for (const char *notes : {"operator.md", "scripter.md"}) {
        copy_if_present(real_home / notes, library / notes);
    }
    say("library " + library.string());

    llm::tool_context ctx;
    ctx.adb = &adb;
    ctx.box_snap = [&cfg] { return cfg.box_snap; };
    ctx.cfg = &cfg;
    ctx.script_dir = library;
    // No `needs_approval` and no `request_approval`: a smoke run has nobody to
    // answer, and a gate that waits forever is worse than no gate at all.

    llm_host host;
    host.set_tool_context(ctx);

    int done = 0;
    bool failed = false;
    for (std::size_t i = 0; i < prompts.size(); ++i) {
        const run_clock::time_point started = run_clock::now();
        const llm_result res =
            host.run_turn(cfg.config_home, cfg.llm.provider, prompts[i],
                          cfg.llm.max_dispatches, cfg.llm.max_model_rounds,
                          false, real_home.parent_path());
        std::string line = "turn " + std::to_string(i + 1) + "/" +
                           std::to_string(prompts.size()) + " " +
                           seconds_since(started) + (res.ok ? " ok" : " FAILED");
        if (!res.ok && !res.error.empty()) {
            line += ": " + res.error;
        }
        say(line);
        if (!res.ok) {
            failed = true;
            break;
        }
        ++done;
    }

    host.set_tool_context({});
    report_graphs(library);
    say("turns " + std::to_string(done) + ", failed " + (failed ? "1" : "0"));
    say("total " + seconds_since(began));
    return failed ? 1 : 0;
}

} // namespace campcat
