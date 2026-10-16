#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/llm/llm_fuse.h"
#include "core/llm/llm_memory.h"
#include "core/llm/llm_protocol.h"
#include "core/llm/llm_tools.h"
#include "core/process.h"

namespace campcat {

/**
 * @brief One-shot outcome of a turn request.
 */
struct llm_result {
    bool ok = false;
    bool interrupted = false; ///< user sent a new message; the text stays
    std::string text;         ///< model output when `ok`
    std::string error;        ///< human-readable failure detail
    int turns = -1;           ///< turns the sidecar retains; 0 after a reset
};

/// @brief One step of the turn in flight, in the order it happened.
struct llm_event {
    enum class kind { text, tool, image };
    kind type = kind::text;
    /// `text`: the model's words so far. `tool`: its Markdown note, which is
    /// also where the host puts a line of its own that belongs in the timeline.
    std::string text;
    bool ok = true;    ///< `tool`: whether the call succeeded
    long image_id = 0; ///< `image`: key for `event_image_png`
};

/**
 * @brief Append one streamed delta to a timeline.
 *
 * Grows the trailing `text` step, or starts a new one when the step before it
 * was a tool or an image: that boundary is the whole point of the timeline. An
 * empty delta adds nothing, so a thinking-only chunk leaves no empty step.
 */
void llm_events_append_text(std::vector<llm_event> &_events,
                            std::string_view _delta);

/// One question waiting on the human, as the UI needs to see it. Deliberately
/// without the screenshot: the UI polls this every frame and the image is a
/// megabyte, so it fetches that separately, once per `id`.
struct llm_approval {
    bool pending = false;
    long id = 0; ///< Bumped per request; the UI caches its texture against it
    std::string tool;
    std::string summary;
    std::vector<llm::region> highlight;
    bool editable = false; ///< Whether the human may redraw this region
    /// The region the model gave, when the snap moved it: a second, thinner
    /// frame so the human can compare the two. Empty when they are the same.
    std::optional<llm::region> given_region;
};

/// How long an idle consolidation may take. One model call and a file write, so
/// minutes are already generous; deliberately not the per-turn timeout, which
/// exists only because a turn chains many tool rounds.
constexpr int k_memorize_timeout_ms = 120000;

/// How long the process may stay alive behind an already-closed window to
/// finish the last consolidation. Only the tail of the session is at stake: the
/// idle pass has usually folded everything else already.
constexpr int k_shutdown_memorize_timeout_ms = 5000;

/**
 * @brief Owns the persistent Python sidecar and speaks NDJSON to it.
 *
 * The C++ side knows nothing about the model, endpoint or prompts: it hands
 * over the user's instruction and reads back text. Those settings live in
 * `campcat/sources/py/` (see `campcat/sources/py/models.py`), where the child process reads them itself.
 *
 * The child is started lazily on the first `run_turn` and reused afterwards.
 * `run_turn` never throws; every failure is reported through `llm_result`.
 */
class llm_host {
public:
    llm_host() = default;
    ~llm_host();

    llm_host(const llm_host &) = delete;
    llm_host &operator=(const llm_host &) = delete;

    /**
     * @brief Ask the sidecar for one turn on `_prompt`.
     * @param[in] _config_home The config directory; it holds the memory file
     * this turn carries, and its parent anchors `.venv`/`llm/`.
     * @param[in] _provider Which model provider the sidecar should build, from
     * the settings. Taken from a snapshot, so a change mid-turn waits its turn.
     * @param[in] _prompt The user's instruction. Blank is rejected.
     * @param[in] _max_dispatches How many tools the host may run this turn.
     * Values below 1 use the default. A change mid-turn waits for the next one.
     * @param[in] _max_model_rounds How many model rounds this turn may take,
     * sent to the sidecar. Values below 1 use the default.
     */
    llm_result run_turn(const std::filesystem::path &_config_home,
                        const std::string &_provider,
                        const std::string &_prompt, int _max_dispatches,
                        int _max_model_rounds);

    /// The latest usage the sidecar reported. Safe to call from the UI thread.
    llm_protocol::usage_stats usage() const;

    /**
     * @brief Fold this session into the long-term notes on disk.
     *
     * Reads the notes, has the sidecar rewrite them from the conversation it
     * already holds, and writes the result back. A failed read is fatal to this
     * call: notes that cannot be read are never overwritten.
     *
     * @param[in] _config_home The config directory, holding the memory file.
     * @param[in] _provider Which model provider to consolidate on. Must be the
     * same one turns use, or a Qwen-only setup fails here.
     * @param[in] _timeout_ms Hard limit on the whole request. Consolidation can
     * be slow, but the shutdown call must not be.
     */
    llm_result memorize(const std::filesystem::path &_config_home,
                        const std::string &_provider, llm::memory_kind _kind,
                        int _timeout_ms);

    /**
     * @brief Clear the sidecar's conversation history.
     *
     * Succeeds without spawning anything when no child is running, since there
     * is then no history to clear.
     *
     * Does not touch the memory file: callers that want the session kept run
     * `memorize` first, because this is what forgets the conversation it reads.
     */
    llm_result reset(const std::filesystem::path &_repo_root);

    /**
     * @brief Supply the state tools read, or a default context to disable them.
     *
     * Called by the worker that runs the exchange, before `run_turn`; that same
     * thread performs every tool call, so no lock guards the context. The adb
     * pointer must stay alive until the context is cleared.
     */
    void set_tool_context(const llm::tool_context &_ctx);

    /**
     * @brief The turn in flight as an ordered timeline; empty when idle.
     *
     * Polled by the UI every frame for what the model said and did, in the
     * order it happened. Carries no image bytes: those come from
     * `event_image_png`, once per image event.
     */
    std::vector<llm_event> events() const;

    /**
     * @brief Raw PNG of one image event, or empty once the host dropped it.
     *
     * Only the newest screenshots of a turn keep their bytes; a turn can take
     * hundreds, and a user may be looking at another page for all of them.
     *
     * @param[in] _image_id `llm_event::image_id` of the event to fetch.
     */
    std::string event_image_png(long _image_id) const;

    /**
     * @brief A copy of the turn's timeline, images included.
     *
     * The worker takes this as it finishes a turn, then clears the live
     * stream for the next one. The UI fills the bubble from the copy.
     */
    struct timeline {
        std::vector<llm_event> events;
        std::deque<std::pair<long, std::string>> images;
    };
    timeline copy_timeline() const;

    /// True once the sidecar reported the model is reasoning.
    bool is_thinking() const;

    /**
     * @brief Drop the stream buffer and the thinking flag.
     *
     * Call before starting a request: the worker clears it too, but only after
     * `ensure_running`, which may take a minute while it starts the
     * interpreter.
     */
    void clear_stream();

    /// Terminate the child if running. Safe to call repeatedly.
    void stop();

    /**
     * @brief Ask the turn in flight to stop at its next tool.
     *
     * The tool already running finishes. The next `tool_result` is replaced
     * with `interrupt`. A model call already on the wire is not cancelled.
     * Cleared when the queued message is taken, so the next turn is not born
     * stopped.
     */
    void request_interrupt();

    /// Drop a pending interrupt. The worker calls this as it takes the queued
    /// message.
    void clear_interrupt();

    /// True from `request_interrupt` until `clear_interrupt`.
    bool interrupt_pending() const;

    /**
     * @brief Reject the question the worker is blocked on, once.
     *
     * Unlike `cancel_approval`, this does not refuse later questions: the
     * session is continuing, and the next turn may ask again.
     */
    void release_approval();

    /**
     * @brief The question the worker is blocked on, if any.
     *
     * Polled by the UI every frame; `pending == false` means nothing to answer
     * and the other fields are stale. The screenshot is fetched separately, via
     * `approval_screen_png()`, only when `id` changes.
     */
    llm_approval pending_approval() const;

    /// Raw PNG bytes of the pending question's screenshot; empty if there is
    /// none.
    std::string approval_screen_png() const;

    /// Answer the pending question. A no-op when nothing is pending.
    /// `_guidance` and `_box` are corrections for the model and are kept only
    /// on a rejection.
    void answer_approval(bool _approved, std::string _guidance = {},
                         std::optional<llm::region> _redrawn = std::nullopt);

    /**
     * @brief Resolve the hanging question as rejected, and refuse later ones,
     * so the worker returns.
     *
     * Must be called before joining the worker thread at shutdown: otherwise a
     * question nobody can answer keeps `service_approval` blocked forever. It
     * latches instead of cancelling once, because a request published after
     * this call would still have nobody to answer it and wedge the join just
     * the same.
     */
    void cancel_approval();

private:
    bool ensure_running(const std::filesystem::path &_repo_root,
                        std::string *_error_out);
    /// Poll one child pipe until EOF, feeding lines to the stdout/stderr sink.
    void pump(int _fd, bool _is_stderr);
    void handle_line(const std::string &_line);
    bool write_all(const std::string &_data, std::string *_error_out);
    /// Claim the next request id, clearing the result slot and the stream.
    long next_request_id();
    /// Write one request line and block until its result arrives, serving any
    /// tool calls the sidecar raises in between. `_timeout_ms` bounds the whole
    /// exchange, tool calls included -- not just the model's first reply.
    llm_result exchange(const std::string &_request_line, int _timeout_ms);
    /// Run one tool call and build its `tool_result` line.
    std::string run_tool(const llm_protocol::message &_call);
    void record_note(const std::string &_note);
    void teardown();
    /// Entered through the tool context's approval callback; blocks until the
    /// human answers or the question is cancelled. The return value carries the
    /// permission and, on a rejection, the correction the human typed.
    llm::approval_decision service_approval(const llm::approval_request &_req);
    /// Take (and reset) the time spent waiting on humans, for the deadline
    /// top-up.
    std::chrono::nanoseconds take_approval_wait();

    /// Owns the sidecar pid and its stdio pipes; destroyed on restart/shutdown.
    process::child m_child;

    std::thread m_reader;
    std::thread m_stderr_reader;

    /// Mutable so the const stream accessors can lock it.
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_ready = false;
    bool m_broken = false;
    bool m_stopping = false;

    long m_next_id = 1;
    long m_pending_id = 0;
    bool m_has_result = false;
    llm_result m_result;

    /// Set when the sidecar announces a protocol revision this host cannot
    /// speak.
    std::string m_protocol_mismatch;

    /// Supplied by the worker; read when a turn starts. Not touched by readers.
    llm::tool_context m_tool_ctx;
    /// The session's last screenshot. Deliberately not a field of `m_tool_ctx`:
    /// `set_tool_context` is called every turn (the worker rebuilds it per turn
    /// and clears it after), so storing it there would be wiped by the next
    /// injection. Written and read only by the worker thread inside `run_turn`;
    /// `reset()` clears it after joining that thread.
    llm::screen_state m_screen;
    /// A pending tool call the sidecar is blocked on, guarded by `m_mu`.
    llm_protocol::message m_tool_call;
    bool m_has_tool = false;

    /// Tool activity for the turn in flight, read by the UI thread.
    int m_tool_calls = 0;
    /// How many `view_crop` calls landed in a row. Worker thread only, same
    /// lifetime as `m_tool_calls`.
    int m_view_crop_streak = 0;
    /// Cap for `m_tool_calls`, taken from the settings when the turn starts.
    int m_turn_max_dispatches = llm::k_default_max_dispatches;
    /// Session counters the status bar polls. Guarded by `m_mu`.
    llm_protocol::usage_stats m_usage;
    /// Steps of the turn in flight, in order, guarded by `m_mu`.
    std::vector<llm_event> m_events;
    /// PNG bytes of the newest image events, oldest first. Bounded: a turn can
    /// take hundreds of screenshots and nobody may be watching the page.
    std::deque<std::pair<long, std::string>> m_event_images;
    /// Bumped once per image a tool returns; the UI uses it as the image's key.
    long m_tool_image_id = 0;

    bool m_thinking = false;

    /// Set by the UI when the user sends a message during a turn. The worker
    /// reads it in `exchange` and clears it when that message is taken.
    std::atomic<bool> m_interrupt{false};

    // Human approval. A separate mutex/cv from `m_mu`/`m_cv`: that pair is the
    // sidecar reader's predicate, and sharing it would have both sides waking
    // each other's waits for nothing.
    mutable std::mutex m_approval_mu;
    std::condition_variable m_approval_cv;
    bool m_approval_pending = false;
    bool m_approval_answered = false;
    bool m_approval_approved = false;
    bool m_approval_cancelled = false;
    /// Correction typed by the human; only read when the question is rejected.
    std::string m_approval_guidance;
    /// Region the human redrew; same lifetime and same rule as the guidance.
    std::optional<llm::region> m_approval_region;
    /// Set once the host is going away; no question is answered after that.
    bool m_approval_abort = false;
    long m_next_approval_id = 0;
    llm::approval_request m_approval_req;
    /// Total time spent waiting on humans. Worker-thread only, hence no lock.
    std::chrono::nanoseconds m_approval_wait{0};
};

/**
 * @brief One entry of the turn's timeline that is not the model's own words,
 * as Markdown.
 *
 * The status is a bold line and `_body` a paragraph of its own below it; the
 * blank line between them is what keeps a multi-line payload (`list_scripts`
 * returns one `name -- description` per line) from being soft wrapped into one
 * paragraph with the status fused onto its first line. Runs of blank lines in
 * the input collapse to one, since the renderer spends a full empty text row on
 * each.
 *
 * Display only. What the model receives is the tool reply itself, and nothing
 * here touches the wire.
 *
 * @param[in] _name Tool name.
 * @param[in] _status "ok" or "error".
 * @param[in] _body Result text; may be empty.
 */
std::string tool_note(const std::string &_name, const char *_status,
                      const std::string &_body);

} // namespace campcat
