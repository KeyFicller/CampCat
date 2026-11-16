#include "core/llm/llm_memory.h"
#include "core/file_io.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <llm/reflection_json.h>
#include <nlohmann/json.hpp>

namespace campcat::llm {

namespace {

using json = nlohmann::json;

enum class memory_add_op { add };
enum class memory_edit_op { edit };
enum class memory_retire_op { retire };
enum class memory_section { preference, alias, pitfall, ui_style };

constexpr std::string_view ledger_section(memory_section _section) {
    switch (_section) {
    case memory_section::preference: return "偏好";
    case memory_section::alias: return "叫法";
    case memory_section::pitfall: return "坑";
    case memory_section::ui_style: return "风格";
    }
    return {};
}

struct memory_add {
    memory_add_op op;
    memory_section section;
    std::string text;
};

struct memory_edit {
    memory_edit_op op;
    std::string id;
    std::string text;
};

struct memory_retire {
    memory_retire_op op;
    std::string id;
    std::string why;
};

struct memory_model_proposal {
    std::vector<std::variant<memory_add, memory_edit, memory_retire>> ops;
};

constexpr std::string_view k_memory_proposal_schema =
    std::define_static_string(::js::param_schema<memory_model_proposal>());

/// The UI thread (the memory window) and the LLM worker (a pass) both touch
/// these files, so every path through them takes this lock.
std::recursive_mutex &file_mutex() { return library_io_mutex(); }

/// One entry plus the ledger it came from. An op must be written back to the
/// file that holds the id, not to whichever file the caller named: a `叫法`
/// lives in the graph ledger even while the caller is looking at the library.
struct located_entry {
    memory_entry e;
    std::filesystem::path file;
};

struct replay {
    std::vector<located_entry> entries;
    std::unordered_set<std::uint64_t> used_ids;
    std::unordered_set<std::string> applied_steps;
    std::unordered_set<std::string> committed_passes;
    double last_pass_at = -1.0;
    std::string last_pass_to;
};

constexpr char k_ledgers_dir[] = "memory";
constexpr char k_root_scope[] = "library";

bool parse_hex_id(std::string_view _s, std::uint64_t *_out) {
    if (_s.size() < 4 || _s.size() > 16) {
        return false;
    }
    std::uint64_t v = 0;
    for (const char c : _s) {
        unsigned d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<unsigned>(c - 'a') + 10;
        } else {
            return false;
        }
        v = v * 16 + d;
    }
    *_out = v;
    return true;
}

std::string format_id(std::uint64_t _v) {
    char buf[17] = {};
    std::snprintf(buf, sizeof buf, "%04llx",
                  static_cast<unsigned long long>(_v));
    return std::string(buf);
}

std::size_t utf8_chars(std::string_view _text) {
    return static_cast<std::size_t>(
        std::count_if(_text.begin(), _text.end(),
                      [](unsigned char c) { return (c & 0xc0) != 0x80; }));
}

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

/// A missing file is an empty ledger. Returns false only when it cannot be read
/// -- including the case where a parent is a regular file, which `exists`
/// reports as "not found" and would otherwise look like an empty ledger.
bool read_file(const std::filesystem::path &_path, std::string *_out) {
    std::error_code ec;
    const std::filesystem::file_status st = std::filesystem::status(_path, ec);
    if (ec) {
        // A parent that is a regular file has to read as unreadable, not as
        // empty: "empty" would let a write land somewhere we cannot see.
        return ec == std::errc::no_such_file_or_directory;
    }
    if (!std::filesystem::exists(st)) {
        return true;
    }
    std::ifstream in(_path, std::ios::binary);
    if (!in) {
        return false;
    }
    _out->assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
    return !in.bad();
}

located_entry *find_entry(replay &_r, const std::string &_id) {
    for (auto &le : _r.entries) {
        if (le.e.id == _id) {
            return &le;
        }
    }
    return nullptr;
}

void drop_entry(replay &_r, const std::string &_id) {
    std::erase_if(_r.entries,
                  [&](const located_entry &le) { return le.e.id == _id; });
}

bool valid_record(const json &_j) {
    if (!_j.is_object() || !_j.contains("v") || !_j["v"].is_number_integer() ||
        _j["v"] != 1) {
        return false;
    }
    // Validate before applying anything: a bad optional field must not leave
    // half an edit in the projection.
    for (const char *field : {"kind", "id", "section", "text", "by", "from",
                              "to", "why", "commit"}) {
        if (_j.contains(field) && !_j[field].is_string())
            return false;
    }
    if (_j.contains("at") && !_j["at"].is_number())
        return false;
    if (_j.contains("src") &&
        (!_j["src"].is_array() ||
         !std::all_of(_j["src"].begin(), _j["src"].end(),
                      [](const json &s) { return s.is_string(); })))
        return false;
    if (_j.contains("step") &&
        (!_j["step"].is_number_integer() || _j["step"] < 0))
        return false;
    return true;
}

std::string step_key(const json &_j) {
    return _j.contains("commit") && _j.contains("step")
               ? _j["commit"].get<std::string>() + ":" + _j["step"].dump()
               : std::string();
}

/// Malformed records are skipped before they can alter the projection.
void apply_record(const json &_j, const std::filesystem::path &_file,
                  replay &_r) {
    if (!valid_record(_j))
        return;
    const std::string step = step_key(_j);
    if (!step.empty() && !_r.applied_steps.insert(step).second)
        return;
    const std::string kind = _j.value("kind", std::string());
    if (kind == "pass") {
        if (_j.contains("commit"))
            _r.committed_passes.insert(_j["commit"].get<std::string>());
        const double at = _j.value("at", 0.0);
        if (at >= _r.last_pass_at) {
            _r.last_pass_at = at;
            _r.last_pass_to = _j.value("to", std::string());
        }
        return;
    }
    const std::string id = _j.value("id", std::string());
    std::uint64_t ignored = 0;
    if (!parse_hex_id(id, &ignored)) {
        return;
    }
    if (kind == "add") {
        if (_r.used_ids.count(ignored) != 0) {
            return; // ids are unique; a repeat is a corrupt line, not a merge
        }
        memory_entry e;
        e.id = id;
        e.section = _j.value("section", std::string());
        e.text = _j.value("text", std::string());
        e.at = _j.value("at", 0.0);
        // A fact the user typed is theirs from the start, so a pass may not
        // rewrite it later.
        e.by_user = _j.value("by", std::string()) == "user";
        if (const auto it = _j.find("src"); it != _j.end() && it->is_array()) {
            for (const auto &s : *it) {
                if (s.is_string()) {
                    e.src.push_back(s.get<std::string>());
                }
            }
        }
        if (e.section.empty() || e.text.empty()) {
            return;
        }
        _r.used_ids.insert(ignored);
        _r.entries.push_back(located_entry{std::move(e), _file});
        return;
    }
    if (kind == "delete") {
        _r.used_ids.insert(ignored);
        drop_entry(_r, id);
        return;
    }
    located_entry *le = find_entry(_r, id);
    if (le == nullptr) {
        return;
    }
    if (kind == "edit") {
        const std::string text = _j.value("text", std::string());
        if (text.empty()) {
            return;
        }
        le->e.text = text;
        le->e.at = _j.value("at", le->e.at);
        le->e.by_user = _j.value("by", std::string()) == "user";
    } else if (kind == "pin") {
        le->e.pinned = true;
    } else if (kind == "unpin") {
        le->e.pinned = false;
    } else if (kind == "retire") {
        le->e.visible = false;
    } else if (kind == "restore") {
        le->e.visible = true;
    }
}

bool replay_file(const std::filesystem::path &_path, replay &_r) {
    std::string data;
    if (!read_file(_path, &data)) {
        return false;
    }
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t nl = data.find('\n', pos);
        const std::size_t stop = nl == std::string::npos ? data.size() : nl;
        const std::string line = data.substr(pos, stop - pos);
        pos = stop + 1;
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        const json j = json::parse(line, nullptr, false);
        if (j.is_discarded()) {
            continue;
        }
        apply_record(j, _path, _r);
    }
    return true;
}

bool section_allowed(memory_kind _kind, const std::string &_section) {
    if (_kind == memory_kind::operator_notes) {
        return _section == "偏好" || _section == "叫法";
    }
    return _section == "坑" || _section == "风格";
}

/// Append one line. Opened in append mode so a failure cannot reach the records
/// already there; flushed so a crash right after leaves the line whole.
bool append_line(const std::filesystem::path &_path, const json &_j) {
    std::error_code ec;
    std::filesystem::create_directories(_path.parent_path(), ec);
    if (ec) {
        return false;
    }
    // A killed writer may have left an unterminated tail. Separate it before
    // appending so it cannot consume the next complete record.
    bool separator = false;
    const auto size = std::filesystem::file_size(_path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return false;
    if (!ec && size != 0) {
        std::ifstream tail(_path, std::ios::binary);
        tail.seekg(-1, std::ios::end);
        char last = '\0';
        if (!tail.get(last))
            return false;
        separator = last != '\n';
    }
    std::string line;
    try {
        line = _j.dump();
    } catch (const json::exception &) {
        return false;
    }
    std::ofstream out(_path, std::ios::binary | std::ios::app);
    if (!out) {
        return false;
    }
    if (separator)
        out << '\n';
    out << line << '\n';
    out.flush();
    return static_cast<bool>(out);
}

void note(std::vector<std::string> *_notes, const std::string &_text) {
    if (_notes != nullptr) {
        _notes->push_back(_text);
    }
}

/// IDs increase across every historical add/delete, including other graphs.
///
/// Every ledger of this kind under the config home counts, not just the two
/// being merged: an id handed out while no graph was selected still has to
/// differ from one living under a graph, or selecting that graph later would
/// merge two facts onto one id.
std::string next_id(const std::filesystem::path &_config_home,
                    memory_kind _kind, const replay &_r) {
    std::unordered_set<std::uint64_t> used;
    auto collect = [&used](const replay &_from) {
        used.insert(_from.used_ids.begin(), _from.used_ids.end());
    };
    collect(_r);
    std::error_code ec;
    std::filesystem::directory_iterator it(_config_home, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto &entry = *it;
        std::error_code not_dir;
        const bool directory = entry.is_directory(not_dir);
        if (not_dir)
            return std::string();
        if (!directory) {
            continue;
        }
        replay other;
        if (!replay_file(entry.path() / k_ledgers_dir /
                             (std::string(memory_agent_name(_kind)) + ".jsonl"),
                         other))
            return std::string();
        collect(other);
    }

    if (ec)
        return std::string();
    std::uint64_t highest = 0;
    for (const std::uint64_t v : used) {
        highest = std::max(highest, v);
    }
    if (highest == std::numeric_limits<std::uint64_t>::max())
        return std::string();
    return format_id(highest + 1);
}

bool load_scope(const std::filesystem::path &_home, memory_kind _kind,
                std::string_view _graph, replay &_r) {
    if (!replay_file(ledger_path(_home, _kind, ""), _r)) {
        return false;
    }
    // Older versions wrote graph notes at the root without a graph selected.
    // Keep their IDs and receipts, but never expose those facts across graphs.
    std::erase_if(_r.entries, [_kind](const located_entry &_entry) {
        return _kind != memory_kind::operator_notes || _entry.e.section != "偏好";
    });
    return _graph.empty() ||
           replay_file(ledger_path(_home, _kind, _graph), _r);
}

bool recover_pass(const std::filesystem::path &_home, memory_kind _kind);

} // namespace

std::string_view memory_proposal_schema() {
    return k_memory_proposal_schema;
}

bool parse_memory_proposal(std::string_view _json, memory_proposal *_out,
                           std::string *_error) {
    if (!_out || !_error) return false;
    *_out = {}; _error->clear();
    const auto fail = [&] {
        *_error = "memory reply has malformed operations or cursor fields; nothing was committed";
        return false;
    };
    const auto j = json::parse(_json, nullptr, false);
    if (!j.is_object() || !j.contains("ops") || !j["ops"].is_array()) return fail();
    for (const char *field : {"from", "to"}) {
        const auto it = j.find(field);
        if (it != j.end() && !it->is_string()) return fail();
    }
    if (j.size() != 1 + j.contains("from") + j.contains("to")) return fail();
    memory_proposal parsed;
    try {
        auto reply = ::js::from_json<memory_model_proposal>({{"ops", j["ops"]}});
        parsed.ops.reserve(reply.ops.size());
        for (auto &op : reply.ops) {
            if (auto *add = std::get_if<memory_add>(&op)) {
                parsed.ops.push_back({.k = memory_op::kind::add,
                                      .section = std::string(ledger_section(add->section)),
                                      .text = std::move(add->text)});
            } else if (auto *edit = std::get_if<memory_edit>(&op)) {
                parsed.ops.push_back({.k = memory_op::kind::edit,
                                      .id = std::move(edit->id),
                                      .text = std::move(edit->text)});
            } else {
                auto &retire = std::get<memory_retire>(op);
                parsed.ops.push_back({.k = memory_op::kind::retire,
                                      .id = std::move(retire.id),
                                      .why = std::move(retire.why)});
            }
        }
    } catch (const ::js::bind_error &) {
        return fail();
    }
    parsed.from = j.value("from", std::string());
    parsed.to = j.value("to", std::string());
    *_out = std::move(parsed);
    return true;
}

std::filesystem::path ledger_path(const std::filesystem::path &_config_home,
                                  memory_kind _kind, std::string_view _graph) {
    const std::string stem = memory_agent_name(_kind);
    if (_graph.empty()) {
        return _config_home / k_ledgers_dir / (stem + ".jsonl");
    }
    return _config_home / std::string(_graph) / k_ledgers_dir /
           (stem + ".jsonl");
}

const char *memory_agent_name(memory_kind _kind) {
    return _kind == memory_kind::scripter_notes ? "scripter" : "operator";
}

bool read_ledger(const std::filesystem::path &_config_home, memory_kind _kind,
                 std::string_view _graph, std::vector<memory_entry> *_out) {
    if (_out == nullptr) {
        return false;
    }
    _out->clear();
    if (_config_home.empty()) {
        return true; // no config, so no notes to have
    }
    std::lock_guard lk(file_mutex());
    if (!recover_pass(_config_home, _kind))
        return false;
    replay r;
    if (!load_scope(_config_home, _kind, _graph, r)) {
        return false;
    }
    _out->reserve(r.entries.size());
    for (auto &le : r.entries) {
        _out->push_back(std::move(le.e));
    }
    return true;
}

std::string render_memory(const std::vector<memory_entry> &_entries,
                          std::size_t _budget, int *_dropped) {
    if (_dropped != nullptr) {
        *_dropped = 0;
    }
    std::vector<const memory_entry *> ranked;
    for (const auto &e : _entries) {
        if (e.visible) {
            ranked.push_back(&e);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const memory_entry *_a, const memory_entry *_b) {
                         if (_a->pinned != _b->pinned) {
                             return _a->pinned;
                         }
                         if (_a->by_user != _b->by_user) {
                             return _a->by_user;
                         }
                         return _a->at > _b->at;
                     });

    std::vector<std::string> order;
    std::unordered_map<std::string, std::string> books;
    std::size_t chars = 0;
    for (const memory_entry *e : ranked) {
        const auto it = books.find(e->section);
        const std::string heading = it == books.end() ? "## " + e->section + "\n" : "";
        const std::string line = "- " + e->text + "\n";
        const std::size_t added_chars = utf8_chars(heading) + utf8_chars(line);
        if (!e->pinned && chars + added_chars > _budget) {
            if (_dropped != nullptr) {
                ++*_dropped;
            }
            continue;
        }
        if (it == books.end()) {
            order.push_back(e->section);
            books.emplace(e->section, heading + line);
        } else {
            it->second += line;
        }
        chars += added_chars;
    }

    std::string out;
    for (const auto &section : order) {
        out += books.at(section);
    }
    return out;
}

std::string render_index(const std::vector<memory_entry> &_entries) {
    std::string out;
    for (const auto &e : _entries) {
        if (!e.visible) {
            continue;
        }
        out += "[" + e.id + "] (" + e.section;
        if (e.pinned) {
            out += ", 钉";
        }
        if (e.by_user) {
            out += ", 用户";
        }
        out += ") " + e.text + "\n";
    }
    return out;
}

namespace {

struct staged_record {
    std::filesystem::path file;
    json record;
};

memory_commit_result prepare_ops(const std::filesystem::path &_config_home,
                                 memory_kind _kind, std::string_view _graph,
                                 const std::vector<memory_op> &_ops,
                                 const std::vector<std::string> &_src,
                                 bool _by_user, replay &_r,
                                 std::vector<std::string> *_notes,
                                 std::vector<staged_record> &_records) {
    memory_commit_result result;
    int seen = 0;
    for (const memory_op &op : _ops) {
        if (seen++ >= k_max_ops_per_pass) {
            note(_notes, "ops past the per-pass limit were dropped");
            result.rejected +=
                static_cast<int>(_ops.size()) - k_max_ops_per_pass;
            break;
        }
        const bool is_add = op.k == memory_op::kind::add;
        const bool text_op = is_add || op.k == memory_op::kind::edit;
        if (text_op &&
            (op.text.empty() || utf8_chars(op.text) > k_entry_max_chars)) {
            note(_notes, "entry text is empty or over " +
                             std::to_string(k_entry_max_chars) + " characters");
            ++result.rejected;
            continue;
        }

        std::filesystem::path target;
        located_entry *existing = nullptr;
        if (is_add) {
            if (!section_allowed(_kind, op.section)) {
                note(_notes, "unknown section \"" + op.section + "\"");
                ++result.rejected;
                continue;
            }
            const bool graph_scoped = _kind == memory_kind::scripter_notes ||
                                      op.section == "叫法";
            if (graph_scoped && _graph.empty()) {
                note(_notes, "graph notes require an active graph");
                ++result.rejected;
                continue;
            }
            target = ledger_path(_config_home, _kind,
                                 graph_scoped ? _graph : std::string_view{});
        } else {
            if (!op.id.empty()) {
                existing = find_entry(_r, op.id);
            }
            if (existing == nullptr) {
                note(_notes, "unknown entry id \"" + op.id + "\"");
                ++result.rejected;
                continue;
            }
            const bool user_only = op.k == memory_op::kind::pin ||
                                   op.k == memory_op::kind::unpin ||
                                   op.k == memory_op::kind::remove;
            if (user_only && !_by_user) {
                note(_notes, "only the user may pin, unpin or delete");
                ++result.rejected;
                continue;
            }
            // The user's own words are authoritative: a pass may never rewrite
            // or hide what the user wrote, nor anything they pinned.
            if (!_by_user && (existing->e.pinned || existing->e.by_user)) {
                note(_notes, "entry " + op.id + " is pinned or user-edited");
                ++result.rejected;
                continue;
            }
            target = existing->file;
        }

        json rec;
        rec["v"] = 1;
        rec["id"] = is_add ? next_id(_config_home, _kind, _r) : op.id;
        if (rec["id"] == "") {
            note(_notes, "could not allocate a unique memory id");
            result.storage_ok = false;
            break;
        }
        switch (op.k) {
        case memory_op::kind::add:
            rec["kind"] = "add";
            rec["section"] = op.section;
            rec["text"] = op.text;
            rec["at"] = now_seconds();
            rec["src"] = _src;
            rec["by"] = _by_user ? "user" : "pass";
            break;
        case memory_op::kind::edit:
            rec["kind"] = "edit";
            rec["text"] = op.text;
            rec["by"] = _by_user ? "user" : "pass";
            break;
        case memory_op::kind::pin:
            rec["kind"] = "pin";
            break;
        case memory_op::kind::unpin:
            rec["kind"] = "unpin";
            break;
        case memory_op::kind::retire:
            rec["kind"] = "retire";
            rec["why"] = op.why;
            break;
        case memory_op::kind::restore:
            rec["kind"] = "restore";
            break;
        case memory_op::kind::remove:
            rec["kind"] = "delete";
            break;
        }
        _records.push_back({target, rec});
        ++result.written;

        // Keep the in-memory replay in step so a batch may add then edit what
        // it just added.
        apply_record(rec, target, _r);
    }
    return result;
}

std::filesystem::path pending_path(const std::filesystem::path &_home,
                                   memory_kind _kind) {
    return _home / k_ledgers_dir /
           (std::string(memory_agent_name(_kind)) + ".pending.json");
}

bool recover_pass(const std::filesystem::path &_home, memory_kind _kind) {
    const auto pending = pending_path(_home, _kind);
    std::string data;
    if (!read_file(pending, &data))
        return false;
    if (data.empty())
        return true;
    const json journal = json::parse(data, nullptr, false);
    if (!journal.is_object() || !journal.contains("bundle") ||
        !journal["bundle"].is_string() || !journal.contains("records") ||
        !journal["records"].is_array() || !journal.contains("receipt") ||
        !valid_record(journal["receipt"]) ||
        journal["receipt"].value("kind", "") != "pass" ||
        !journal["receipt"].contains("commit"))
        return false;
    for (const auto &item : journal["records"]) {
        if (!item.is_object() || !item.contains("graph") ||
            !item["graph"].is_boolean() || !item.contains("record") ||
            !valid_record(item["record"]))
            return false;
    }
    const std::string bundle = journal["bundle"].get<std::string>();
    replay r;
    if (!load_scope(_home, _kind, bundle, r))
        return false;
    const std::string commit = journal["receipt"]["commit"].get<std::string>();
    if (r.committed_passes.count(commit) == 0) {
        for (const auto &item : journal["records"]) {
            const json &record = item["record"];
            if (r.applied_steps.count(step_key(record)) != 0)
                continue;
            const auto path = ledger_path(
                _home, _kind, item["graph"].get<bool>() ? bundle : "");
            if (!append_line(path, record))
                return false;
            apply_record(record, path, r);
        }
        if (!append_line(ledger_path(_home, _kind, ""), journal["receipt"]))
            return false;
    }
    std::error_code ec;
    std::filesystem::remove(pending, ec);
    // A completed journal is harmless if cleanup fails: the receipt makes its
    // next recovery a no-op, and persistence already succeeded.
    return true;
}

bool write_pending(const std::filesystem::path &_path, const json &_journal) {
    std::error_code ec;
    std::filesystem::create_directories(_path.parent_path(), ec);
    if (ec)
        return false;
    auto temporary = _path;
    temporary += ".tmp";
    std::string data;
    try {
        data = _journal.dump();
    } catch (const json::exception &) {
        return false;
    }
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << data << '\n';
        out.flush();
        if (!out)
            return false;
    }
    std::filesystem::rename(temporary, _path, ec);
    return !ec;
}

} // namespace

int apply_ops(const std::filesystem::path &_config_home, memory_kind _kind,
              std::string_view _graph, const std::vector<memory_op> &_ops,
              const std::vector<std::string> &_src, bool _by_user,
              std::vector<std::string> *_notes, bool *_storage_ok) {
    if (_storage_ok != nullptr)
        *_storage_ok = false;
    if (_config_home.empty())
        return 0;
    std::lock_guard lk(file_mutex());
    replay r;
    if (!recover_pass(_config_home, _kind) ||
        !load_scope(_config_home, _kind, _graph, r)) {
        note(_notes, "memory ledger is unreadable; nothing was written");
        return 0;
    }
    std::vector<staged_record> records;
    const auto prepared = prepare_ops(_config_home, _kind, _graph, _ops, _src,
                                      _by_user, r, _notes, records);
    if (!prepared.storage_ok)
        return 0;
    int written = 0;
    for (const auto &item : records) {
        if (!append_line(item.file, item.record)) {
            note(_notes, "could not write " + item.file.string());
            return written;
        }
        ++written;
    }
    if (_storage_ok != nullptr)
        *_storage_ok = true;
    return written;
}

memory_commit_result
commit_pass(const std::filesystem::path &_config_home, memory_kind _kind,
            std::string_view _graph, const std::vector<memory_op> &_ops,
            const std::vector<std::string> &_src, const std::string &_from,
            const std::string &_to, std::vector<std::string> *_notes) {
    memory_commit_result result;
    result.storage_ok = false;
    if (_config_home.empty())
        return result;
    if (_ops.empty() && _from.empty() && _to.empty()) {
        result.storage_ok = true;
        return result;
    }
    std::lock_guard lk(file_mutex());
    replay r;
    if (!recover_pass(_config_home, _kind) ||
        !load_scope(_config_home, _kind, _graph, r)) {
        note(_notes, "could not read or recover the memory ledger");
        return result;
    }
    const std::string commit =
        json::array({std::string(_graph), _from, _to}).dump();
    if (r.committed_passes.count(commit) != 0) {
        result.storage_ok = true;
        return result;
    }
    std::vector<staged_record> records;
    result = prepare_ops(_config_home, _kind, _graph, _ops, _src, false, r,
                         _notes, records);
    if (!result.storage_ok) {
        result.written = 0;
        return result;
    }
    json journal = {{"bundle", std::string(_graph)},
                    {"records", json::array()}};
    for (std::size_t i = 0; i < records.size(); ++i) {
        auto &item = records[i];
        item.record["commit"] = commit;
        item.record["step"] = i;
        journal["records"].push_back(
            {{"graph", item.file != ledger_path(_config_home, _kind, "")},
             {"record", item.record}});
    }
    journal["receipt"] = {
        {"v", 1},        {"kind", "pass"}, {"at", now_seconds()},
        {"from", _from}, {"to", _to},      {"commit", commit}};
    result.storage_ok =
        write_pending(pending_path(_config_home, _kind), journal) &&
        recover_pass(_config_home, _kind);
    if (!result.storage_ok) {
        result.written = 0;
        note(_notes,
             "could not persist the memory pass; its cursor was not committed");
    }
    return result;
}

bool append_pass(const std::filesystem::path &_config_home, memory_kind _kind,
                 std::string_view _graph, const std::string &_from,
                 const std::string &_to) {
    return commit_pass(_config_home, _kind, _graph, {}, {}, _from, _to,
                       nullptr)
        .storage_ok;
}

std::string last_pass_to(const std::filesystem::path &_config_home,
                         memory_kind _kind, std::string_view _graph) {
    if (_config_home.empty()) {
        return std::string();
    }
    std::lock_guard lk(file_mutex());
    (void)recover_pass(_config_home, _kind);
    replay r;
    if (!replay_file(ledger_path(_config_home, _kind, ""), r)) {
        return std::string();
    }
    if (!_graph.empty() &&
        !replay_file(ledger_path(_config_home, _kind, _graph), r)) {
        return std::string();
    }
    return r.last_pass_to;
}

bool read_memory(const std::filesystem::path &_config_home, memory_kind _kind,
                 std::string_view _graph, std::string *_out) {
    if (_out == nullptr) {
        return false;
    }
    _out->clear();
    std::vector<memory_entry> entries;
    if (!read_ledger(_config_home, _kind, _graph, &entries)) {
        return false;
    }
    *_out = render_memory(entries, k_memory_budget_chars, nullptr);
    return true;
}

bool clear_memory(const std::filesystem::path &_config_home, memory_kind _kind,
                  std::string_view _graph) {
    if (_config_home.empty()) {
        return false;
    }
    std::lock_guard lk(file_mutex());
    std::error_code ec;
    std::filesystem::remove(pending_path(_config_home, _kind), ec);
    if (ec)
        return false;
    std::filesystem::remove(ledger_path(_config_home, _kind, ""), ec);
    if (ec) {
        return false;
    }
    if (!_graph.empty()) {
        std::error_code graph_ec;
        std::filesystem::remove(ledger_path(_config_home, _kind, _graph),
                                graph_ec);
        if (graph_ec) {
            return false;
        }
    }
    return true;
}

std::filesystem::path
transcript_path(const std::filesystem::path &_config_home) {
    return _config_home / "turns.jsonl";
}

bool clear_transcript(const std::filesystem::path &_config_home) {
    if (_config_home.empty()) {
        return false;
    }
    std::lock_guard lk(file_mutex());
    std::error_code ec;
    std::filesystem::remove(transcript_path(_config_home), ec);
    return !ec;
}

} // namespace campcat::llm
