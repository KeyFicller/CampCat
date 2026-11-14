#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace campcat::llm {

/// How many characters of notes may ride in the prompt on every turn.
///
/// A budget, not a file limit: the ledgers grow without bound and only what
/// fits here is sent. Anything over it stays on disk and shows up in the UI as
/// "not in the prompt", which is the lever the user has.
constexpr std::size_t k_memory_budget_chars = 8000;

/// One fact's length cap. A longer one is refused whole rather than truncated:
/// half a sentence reads as an instruction.
/// Mirrored by `ENTRY_MAX_CHARS` in `campcat/sources/py/agent/memorier.py`.
constexpr std::size_t k_entry_max_chars = 200;

/// Records one `memorize` pass may write. Bounds the blast radius of a bad
/// pass.
constexpr int k_max_ops_per_pass = 40;

/// Which agent's notebook: the operator's preferences and names, or the
/// scripter's pitfalls and style.
enum class memory_kind { operator_notes, scripter_notes };

/// One fact after replay. `id` is stable for the life of the ledger and is what
/// an op addresses; it is never reused.
struct memory_entry {
    std::string id;
    std::string section;
    std::string text;
    /// Turn marks the fact came from, filled by the host, never by the model.
    std::vector<std::string> src;
    double at = 0.0; ///< when it was added, for ordering
    bool pinned = false;
    bool visible =
        true; ///< retired facts stay in the ledger, out of the prompt
    bool by_user = false; ///< user edited it: a pass may not touch it again
};

/// One proposed change. Parsed from a pass's reply and validated before it is
/// written; `kind` covers the records in the ledger one for one.
struct memory_op {
    enum class kind { add, edit, pin, unpin, retire, restore, remove };
    kind k = kind::add;
    std::string id;
    std::string section;
    std::string text;
    std::string why;
};

struct memory_proposal {
    std::vector<memory_op> ops;
    std::string from, to;
};
/// Parse the complete model reply before any operation or cursor is persisted.
bool parse_memory_proposal(std::string_view _json, memory_proposal *_out,
                           std::string *_error);

/// `<config_home>/memory/<kind>.jsonl`, or
/// `<config_home>/<bundle>/memory/<kind>.jsonl` when `_bundle` is not empty.
/// Graph scope is where a bundle's own names live; the library scope holds what
/// holds across bundles.
std::filesystem::path ledger_path(const std::filesystem::path &_config_home,
                                  memory_kind _kind, std::string_view _bundle);

/// Replay the library and graph ledgers into one projection.
///
/// A missing file is an empty ledger, not an error. Returns false only when a
/// file exists and cannot be read, which is what stops `apply_ops` from writing
/// onto something it could not see.
bool read_ledger(const std::filesystem::path &_config_home, memory_kind _kind,
                 std::string_view _bundle, std::vector<memory_entry> *_out);

/// The markdown that goes into the prompt: `## 偏好` sections, pinned first,
/// then newest. Stops at `_budget` characters; how many entries that left out
/// is written to `*_dropped` (which may be null). Pinned entries are never
/// dropped, so a budget below the pinned size overshoots instead of losing
/// them.
std::string render_memory(const std::vector<memory_entry> &_entries,
                          std::size_t _budget, int *_dropped);

/// The same facts as an index for the memory model: `[f3a1] (叫法, 钉) text`,
/// one per line, retired ones omitted. Every visible entry, never trimmed --
/// the model has to see an id to propose an edit against it.
std::string render_index(const std::vector<memory_entry> &_entries);

/// Validate `_ops` and append the accepted ones as records.
///
/// `_src` fills `add`'s provenance. `_by_user` decides which entries may be
/// touched: a pass cannot edit or retire a pinned entry or one the user edited,
/// and only a user may pin, unpin or delete. Returns how many records were
/// written and appends a line per rejected op to `*_notes` (which may be null).
/// `*_storage_ok`, when supplied, distinguishes an I/O failure from a rejected
/// proposal; it is true even when every proposal was rejected.
int apply_ops(const std::filesystem::path &_config_home, memory_kind _kind,
              std::string_view _bundle, const std::vector<memory_op> &_ops,
              const std::vector<std::string> &_src, bool _by_user,
              std::vector<std::string> *_notes, bool *_storage_ok = nullptr);

struct memory_commit_result {
    /// Operations committed by this call; zero for an already committed pass.
    int written = 0;
    int rejected = 0;
    bool storage_ok = true;
};

/// Persist accepted operations and their cursor as one recoverable pass.
/// Repeating the same bundle/from/to is idempotent. Rejected proposals do not
/// fail a pass, but a storage failure never writes its completion receipt.
memory_commit_result
commit_pass(const std::filesystem::path &_config_home, memory_kind _kind,
            std::string_view _bundle, const std::vector<memory_op> &_ops,
            const std::vector<std::string> &_src, const std::string &_from,
            const std::string &_to, std::vector<std::string> *_notes);

/// Record where a pass folded up to. Written even when the pass proposed
/// nothing: without it those turns are folded again on the next pass, at cost.
bool append_pass(const std::filesystem::path &_config_home, memory_kind _kind,
                 std::string_view _bundle, const std::string &_from,
                 const std::string &_to);

/// The turn mark the last pass reached, or empty when there has been none. The
/// sidecar uses it to skip history the notes already cover.
std::string last_pass_to(const std::filesystem::path &_config_home,
                         memory_kind _kind, std::string_view _bundle);

/// `read_ledger` + `render_memory` at the default budget. A read failure clears
/// `*_out` and returns false.
bool read_memory(const std::filesystem::path &_config_home, memory_kind _kind,
                 std::string_view _bundle, std::string *_out);

/// Delete both ledgers for a kind. Already absent counts as success.
bool clear_memory(const std::filesystem::path &_config_home, memory_kind _kind,
                  std::string_view _bundle);

/// Wire name for a `memorize` request: `operator` or `scripter`. Also the file
/// stem of that kind's ledger.
const char *memory_agent_name(memory_kind _kind);

/// `<config_home>/turns.jsonl`, this conversation's event log.
///
/// Unlike the notes, the host never writes this: the sidecar appends to it. The
/// host only needs the path to delete when the user clears the conversation --
/// which, unlike a reset request, still works when no sidecar is running.
std::filesystem::path
transcript_path(const std::filesystem::path &_config_home);

/// Delete the transcript. Already absent counts as success, the same way
/// `clear_memory` treats it.
bool clear_transcript(const std::filesystem::path &_config_home);

} // namespace campcat::llm
