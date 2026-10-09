#pragma once

#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/llm/llm_protocol.h"
#include "core/llm/llm_tools.h"
#include "core/process.h"

namespace campcat {

/**
 * @brief One-shot outcome of a turn request.
 */
struct llm_result {
  bool ok = false;
  std::string text; ///< model output when `ok`
  std::string error; ///< human-readable failure detail
  int turns = -1;   ///< turns the sidecar retains; 0 after a reset
};

/// One question waiting on the human, as the UI needs to see it. Deliberately
/// without the screenshot: the UI polls this every frame and the image is a
/// megabyte, so it fetches that separately, once per `id`.
struct llm_approval {
  bool pending = false;
  long id = 0; ///< 每次新请求自增；UI 用它对贴图换缓存
  std::string tool;
  std::string summary;
  std::vector<llm::rect> highlight;
};

/**
 * @brief Owns the persistent Python sidecar and speaks NDJSON to it.
 *
 * The C++ side knows nothing about the model, endpoint or prompts: it hands
 * over the user's instruction and reads back text. Those settings live in `llm/`
 * (see `llm/models.py`), where the child process reads them itself.
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
   * @param[in] _repo_root Directory holding `config/`; anchors `.venv` and `llm/`.
   * @param[in] _prompt The user's instruction. Blank is rejected.
   */
  llm_result run_turn(const std::filesystem::path &_repo_root,
                      const std::string &_prompt);

  /**
   * @brief Clear the sidecar's conversation history.
   *
   * Succeeds without spawning anything when no child is running, since there
   * is then no history to clear.
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
   * @brief One line per tool call in the turn in flight, in call order.
   *
   * Polled by the UI to show what the model actually did. Cleared when the next
   * request starts.
   */
  std::vector<std::string> tool_notes() const;

  /**
   * @brief Answer text streamed so far for the in-flight request.
   *
   * Empty when idle. The UI polls this each frame to make the reply grow.
   */
  std::string streaming_text() const;

  /// True once the sidecar reported the model is reasoning.
  bool is_thinking() const;

  /**
   * @brief Drop the stream buffer and the thinking flag.
   *
   * Call before starting a request: the worker clears it too, but only after
   * `ensure_running`, which may take a minute while it starts the interpreter.
   */
  void clear_stream();

  /// Terminate the child if running. Safe to call repeatedly.
  void stop();

  /**
   * @brief The question the worker is blocked on, if any.
   *
   * Polled by the UI every frame; `pending == false` means nothing to answer and
   * the other fields are stale. The screenshot is fetched separately, via
   * `approval_screen_png()`, only when `id` changes.
   */
  llm_approval pending_approval() const;

  /**
   * @brief Raw PNG bytes of the last image a tool returned this turn.
   *
   * The UI draws it in the reply bubble; the model already got its own copy
   * through the tool result. Empty when no tool returned an image. Cleared when
   * the next turn starts, which is also when `tool_image_id()` moves on.
   */
  std::string tool_image_png() const;

  /// Bumped once per image a tool returns. The UI uses it to notice a new image
  /// without comparing a megabyte of pixels every frame.
  long tool_image_id() const;

  /// Raw PNG bytes of the pending question's screenshot; empty if there is none.
  std::string approval_screen_png() const;

  /// Answer the pending question. A no-op when nothing is pending. `_guidance`
  /// is a correction for the model and is kept only on a rejection.
  void answer_approval(bool _approved, std::string _guidance = {});

  /**
   * @brief Resolve the hanging question as rejected, and refuse later ones, so
   * the worker returns.
   *
   * Must be called before joining the worker thread at shutdown: otherwise a
   * question nobody can answer keeps `service_approval` blocked forever. It
   * latches instead of cancelling once, because a request published after this
   * call would still have nobody to answer it and wedge the join just the same.
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
  /// tool calls the sidecar raises in between.
  llm_result exchange(const std::string &_request_line);
  /// Run one tool call and build its `tool_result` line.
  std::string run_tool(const llm_protocol::message &_call);
  void record_note(const std::string &_note);
  void teardown();
  /// Entered through the tool context's approval callback; blocks until the human
  /// answers or the question is cancelled. The return value carries the permission
  /// and, on a rejection, the correction the human typed.
  llm::approval_decision service_approval(const llm::approval_request &_req);
  /// Take (and reset) the time spent waiting on humans, for the deadline top-up.
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

  /// Set when the sidecar announces a protocol revision this host cannot speak.
  std::string m_protocol_mismatch;

  /// Supplied by the worker; read when a turn starts. Not touched by readers.
  llm::tool_context m_tool_ctx;
  /// The session's last screenshot. Deliberately not a field of `m_tool_ctx`:
  /// `set_tool_context` is called every turn (the worker rebuilds it per turn and
  /// clears it after), so storing it there would be wiped by the next injection.
  /// Written and read only by the worker thread inside `run_turn`; `reset()`
  /// clears it after joining that thread.
  llm::screen_state m_screen;
  /// A pending tool call the sidecar is blocked on, guarded by `m_mu`.
  llm_protocol::message m_tool_call;
  bool m_has_tool = false;

  /// Tool activity for the turn in flight; `m_notes` is read by the UI thread.
  int m_tool_calls = 0;
  std::vector<std::string> m_notes;
  /// Last tool image of the turn, raw PNG, kept for the UI to draw.
  std::string m_tool_png;
  long m_tool_image_id = 0;

  /// Streamed deltas for the in-flight request, read by the UI every frame.
  std::string m_stream;
  bool m_thinking = false;

  // Human approval. A separate mutex/cv from `m_mu`/`m_cv`: that pair is the
  // sidecar reader's predicate, and sharing it would have both sides waking each
  // other's waits for nothing.
  mutable std::mutex m_approval_mu;
  std::condition_variable m_approval_cv;
  bool m_approval_pending = false;
  bool m_approval_answered = false;
  bool m_approval_approved = false;
  bool m_approval_cancelled = false;
  /// Correction typed by the human; only read when the question is rejected.
  std::string m_approval_guidance;
  /// Set once the host is going away; no question is answered after that.
  bool m_approval_abort = false;
  long m_next_approval_id = 0;
  llm::approval_request m_approval_req;
  /// Total time spent waiting on humans. Worker-thread only, hence no lock.
  std::chrono::nanoseconds m_approval_wait{0};
};

} // namespace campcat
