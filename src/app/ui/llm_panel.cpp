#include "app/ui/llm_panel.h"

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/mumu_launcher.h"
#include "core/automation_log.h"
#include "core/llm/llm_host.h"
#include "core/llm/llm_memory.h"

#include "app/shell_state.h"

#include "app/ui/activity_status.h"
#include "app/ui/fonts.h"
#include "app/ui/gl_texture.h"
#include "app/ui/icon_texture.h"
#include "app/ui/prompt_input.h"
#include "app/ui/script_editor_panel.h"

#include <imgui.h>
#include <imgui_markdown.h>
#include <misc/cpp/imgui_stdlib.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

/// The four UI faces come from one place (`app/ui/fonts.cpp`); the aliases keep
/// the call sites below to one line.
using campcat::app::font_role;
using campcat::app::fonts_get;

/// Oldest turns are dropped past this.
constexpr int k_turn_cap = 30;

/// How wide the gear is drawn inside its square button, in pixels. Smaller than
/// the button frame so the icon does not touch the edges; the source is 32px,
/// so this also halves it, which is what the 8 teeth need to stay separate.
constexpr float k_gear_side = 20.0F;

/// How far the status line under the prompt box is inset, so it reads as a
/// footnote on the conversation rather than another control.
constexpr float k_stat_indent = 12.0F;

/// One exchange in the conversation: what was sent and what came back.
struct llm_turn {
    std::string user_text;
    std::string reply;
    std::vector<std::string> tool_notes; ///< what the model did, in call order
    /// Last tool image of the turn, raw PNG, copied from the host once. The
    /// model already saw this image through the tool result; this copy is for
    /// the human.
    std::string image_png;
    long image_id =
        0;          ///< which host image `image_png` holds; see tool_image_id()
    GLuint tex = 0; ///< decoded lazily on the UI thread, needs the GL context
    int tex_w = 0;
    int tex_h = 0;
    bool tex_tried =
        false; ///< decode attempted; a 0 `tex` after this means failed
    bool pending = false; ///< sent but unanswered; drives the waiting bubble
    bool ok = false;
    std::string error;
};

/// Result of a finished background job, handed to the UI thread.
struct handoff {
    bool has = false;
    bool is_reset = false;
    bool is_memory = false;
    campcat::llm_result res;
};

// UI-thread state: never touched by the worker.
std::vector<llm_turn> g_turns;
std::string g_input;
bool g_pin_bottom = true;
bool g_thinking = false; ///< sidecar reports the model is reasoning

/// True while an automation cycle or the scheduler owns adb. Tools must refuse
/// then rather than contend for the device.
std::atomic<bool> g_adb_busy{false};

/// Wraps the library default, fixing two of its choices that read badly in a
/// chat bubble.
void md_format(const ImGui::MarkdownFormatInfo &_info, bool _start) {
    if (_info.type == ImGui::MarkdownFormatType::EMPHASIS) {
        // The default greys out `*italic*` (TextDisabled), which inside a
        // bubble reads as disabled text; level 1 therefore stays untouched.
        if (_info.level <= 1) {
            return;
        }
        if (_start) {
            ImGui::PushFont(fonts_get(font_role::bold));
        } else {
            ImGui::PopFont();
        }
        return;
    }
    ImGui::defaultMarkdownFormatCallback(_info, _start);
}

/// Built once on the first reply, which is a frame, so `fonts_init` has run.
ImGui::MarkdownConfig &md_config() {
    static ImGui::MarkdownConfig cfg = [] {
        ImGui::MarkdownConfig c;
        c.linkCallback = nullptr; // a reply must not open a browser
        c.imageCallback = nullptr;
        c.formatCallback = md_format;
        // `**strong**` is drawn with the last heading slot rather than with a
        // bold font of its own, so that slot is what holds the bold face.
        for (auto &fmt : c.headingFormats) {
            fmt.font = fonts_get(font_role::bold);
        }
        return c;
    }();
    return cfg;
}

// Worker state.
std::atomic<bool> g_running{false};
/// True while the worker is consolidating memory rather than answering a turn.
/// Only changes what the Send button's tooltip says; the disable itself is
/// `g_running`.
/// Set and cleared inside the worker that does the consolidating, so there is
/// no path that can leave it stuck on.
std::atomic<bool> g_memorizing{false};
std::thread g_thread;
/// The last consolidation, run at shutdown. A thread of its own because it is
/// deliberately not joined where `g_thread` is: the window has to close first.
std::thread g_shutdown_thread;
campcat::llm_host g_host;
std::mutex g_done_mu;
handoff g_done;

/// UI-thread only. Time of the last LLM request, which is what "idle" is
/// measured from: an agent nobody is talking to is an agent that can tidy up.
double g_last_llm = 0.0;
/// How long the conversation must have been quiet before consolidating.
constexpr double k_idle_seconds = 600.0; // 10 minutes
/// Remembered every frame so the shutdown pass knows where the memory file is,
/// since `llm_panel_shutdown_gl` has no config of its own.
std::filesystem::path g_config_home;

/// The Long-term memory window, and the copy it displays.
bool g_mem_open = false;
std::string g_mem_buf;

/// The sidecar's own count of the turns it still holds verbatim, taken from
/// each successful reply. Deliberately not `g_turns.size()`: that counts
/// bubbles and stops at `k_turn_cap`, while the sidecar drops turns on a token
/// budget and folds them into a summary.
int g_sidecar_turns = 0;
/// Characters in the memory file, for the status line. The window that shows
/// the notes is opened on demand, so this is kept even while it is closed.
int g_memory_chars = 0;
/// When `g_memory_chars` was last read, so the file is not re-read per frame.
double g_memory_read_at = 0.0;

/// (Re)read the memory file into the window's buffer.
void reload_memory() {
    std::string text;
    if (!campcat::llm::read_memory(g_config_home, &text)) {
        text = "(the memory file could not be read)";
    } else if (text.empty()) {
        text = "(no memory yet)";
    }
    g_mem_buf = std::move(text);
}

/// Refresh the memory size, at most once a second: it changes on an idle pass,
/// not on a frame. A read that fails keeps the last known number rather than
/// claiming an empty memory.
void refresh_memory_chars() {
    const double now = ImGui::GetTime();
    if (now - g_memory_read_at < 1.0) {
        return;
    }
    g_memory_read_at = now;
    std::string text;
    if (campcat::llm::read_memory(g_config_home, &text)) {
        g_memory_chars = static_cast<int>(text.size());
    }
}

/// Read-only view of the notes, with the one destructive action behind a
/// confirmation: the app never edits them here, so there is nothing to save.
void draw_memory_window() {
    if (!g_mem_open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(620.0F, 440.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Long-term memory", &g_mem_open)) {
        ImGui::End();
        return;
    }

    // Shown selectable on purpose: the point of it is that the user can open
    // the file in an editor of their own instead of fighting this box.
    ImGui::TextUnformatted(
        campcat::llm::memory_path(g_config_home).string().c_str());
    ImGui::TextDisabled("Kept in this file. Written when the conversation goes "
                        "idle, and before "
                        "it is cleared.");
    if (ImGui::Button("Reload")) {
        reload_memory();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear memory")) {
        ImGui::OpenPopup("clear_memory_confirm");
    }
    if (ImGui::BeginPopupModal("clear_memory_confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Delete the long-term memory file?");
        if (ImGui::Button("Delete", ImVec2(120.0F, 0.0F))) {
            if (!campcat::llm::clear_memory(g_config_home)) {
                campcat::automation_log::emit("[llm] memory file not deleted");
            }
            reload_memory();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0F, 0.0F))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    ImGui::InputTextMultiline("##memory", &g_mem_buf,
                              ImGui::GetContentRegionAvail(),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::End();
}

/// Bubble backgrounds. The colours also tell the two speakers apart, which is
/// why the "You" / "CampCat" labels are gone.
const ImVec4 k_user_bg{0.19F, 0.31F, 0.45F, 1.0F};
const ImVec4 k_bot_bg{0.16F, 0.18F, 0.20F, 1.0F};
const ImVec4 k_err_col{0.95F, 0.45F, 0.35F, 1.0F};
const ImVec4 k_err_bg{0.30F, 0.16F, 0.16F, 1.0F}; ///< failed-reply bubble
/// Send is the one primary action here; plain buttons are surfaces (main.cpp).
const ImVec4 k_primary{0.243F, 0.400F, 0.529F, 1.0F};
const ImVec4 k_primary_hi{0.302F, 0.478F, 0.616F, 1.0F};
const ImVec4 k_primary_lo{0.200F, 0.341F, 0.463F, 1.0F};

/// A bubble takes at most this share of the log width before it wraps.
constexpr float k_bubble_frac = 0.72F;
constexpr float k_bubble_rounding = 6.0F;

/// Width cap for the approval preview. Wider than a plain screenshot thumbnail
/// because the user has to judge a highlighted box before allowing a tap.
constexpr float k_approval_max_w = 420.0F;

/// Bounding side of a staged box's thumbnail in the send bar. Small on purpose:
/// it confirms "this is what I am about to send", it is not for judging the
/// element.
constexpr float k_stage_thumb_side = 96.0F;

/// A tool image is decoded at no more than this width. A full frame is ~10 MB
/// of texture, and turns are kept, so a whole conversation of them would not
/// fit in VRAM. The bubble draws it far smaller than this anyway.
constexpr int k_turn_img_max_w = 640;

/// Upload `_bgr` as a texture. Returns 0 on failure.
/// Take the host's latest tool image into the turn, if a new one appeared.
/// Compares ids, not pixels, so the per-frame cost does not include a megabyte
/// of PNG. A turn that arrives after the image was produced still gets it.
void adopt_tool_image(llm_turn &_t) {
    const long id = g_host.tool_image_id();
    if (_t.image_id == id) {
        return;
    }
    _t.image_id = id;
    _t.image_png = g_host.tool_image_png();
    campcat::app::delete_tex(&_t.tex, &_t.tex_w, &_t.tex_h);
    _t.tex_tried = false; // the next draw decodes the new bytes
}

/// Decode the turn's image once and upload it. UI thread, and the caller must
/// hold the GL context.
void ensure_turn_texture(llm_turn &_t) {
    if (_t.tex_tried || _t.image_png.empty()) {
        return;
    }
    _t.tex_tried = true;
    const cv::Mat buf(1, static_cast<int>(_t.image_png.size()), CV_8UC1,
                      const_cast<char *>(_t.image_png.data()));
    const cv::Mat bgr = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (bgr.empty()) {
        return;
    }
    if (bgr.cols > k_turn_img_max_w) {
        const double scale = static_cast<double>(k_turn_img_max_w) / bgr.cols;
        cv::Mat small;
        cv::resize(bgr, small, cv::Size(), scale, scale, cv::INTER_AREA);
        _t.tex = campcat::app::upload_bgr(small, &_t.tex_w, &_t.tex_h);
        return;
    }
    _t.tex = campcat::app::upload_bgr(bgr, &_t.tex_w, &_t.tex_h);
}

/// Drop every turn, releasing the textures they own. The GL context must be
/// live.
void drop_all_turns() {
    for (llm_turn &t : g_turns) {
        campcat::app::delete_tex(&t.tex, &t.tex_w, &t.tex_h);
    }
    g_turns.clear();
}

/// Fit `_w x _h` into `_max_w`, preserving aspect.
ImVec2 fit_image(int _w, int _h, float _max_w) {
    float w = static_cast<float>(_w);
    float h = static_cast<float>(_h);
    if (w > _max_w && w > 0.0F) {
        h *= _max_w / w;
        w = _max_w;
    }
    return ImVec2(w, h);
}

bool is_blank(std::string_view _s) {
    for (const char c : _s) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

/// Space reserved below the log. Measured from the previous frame rather than
/// computed: guessing the frame height and spacing got it wrong by a few
/// pixels, which pushed the controls over the log's border. Seeded generously
/// so the very first frame under-fills instead of overlapping.
float g_footer_h = 160.0F;

/// Queue the outgoing turn the moment it is sent, so the log shows the user's
/// message while the reply is still in flight.
void append_user_turn(std::string _prompt) {
    llm_turn t;
    t.user_text = std::move(_prompt);
    t.pending = true;
    // Start level with the host, so the previous turn's image is not mistaken
    // for a new one on the first frame.
    t.image_id = g_host.tool_image_id();

    g_turns.push_back(std::move(t));
    while (static_cast<int>(g_turns.size()) > k_turn_cap) {
        campcat::app::delete_tex(&g_turns.front().tex, &g_turns.front().tex_w,
                                 &g_turns.front().tex_h);
        g_turns.erase(g_turns.begin());
    }
    g_pin_bottom = true;
}

/// The turn still waiting for its reply, if any.
llm_turn *pending_turn() {
    for (auto it = g_turns.rbegin(); it != g_turns.rend(); ++it) {
        if (it->pending) {
            return &*it;
        }
    }
    return nullptr;
}

/// Fill the waiting turn with the reply. UI thread only.
void fill_reply(const campcat::llm_result &_res) {
    llm_turn *t = pending_turn();
    if (t != nullptr) {
        t->pending = false;
        t->ok = _res.ok;
        if (_res.ok) {
            t->reply = _res.text;
        } else {
            t->error = _res.error;
        }
        // A tool image may have arrived in the same frame the turn finished,
        // which pump_stream would have missed.
        adopt_tool_image(*t);
    }
    g_pin_bottom = true;
}

/// Mirror the in-flight stream into the waiting turn so its bubble grows.
void pump_stream() {
    llm_turn *t = pending_turn();
    if (t == nullptr) {
        return;
    }
    t->reply = g_host.streaming_text();
    t->tool_notes = g_host.tool_notes();
    adopt_tool_image(*t);
    g_thinking = g_host.is_thinking();
}

/// Run one turn on the worker.
void start_turn(campcat::shell_state &_st, const campcat::app_config &_cfg,
                std::string _prompt) {
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_running.store(true);
    // The whole snapshot, by value: the script tools need the interpreter's
    // config
    // (`tool_context::cfg`), and `_cfg` is a per-frame copy in the caller's
    // frame, so a pointer to it would dangle the moment the frame ends. `_st`
    // outlives the thread instead (it is a `main` local, and this thread is
    // joined by `llm_panel_shutdown_gl`), so the shell is held by pointer: it
    // is where the emulator launch the model may ask for lives.
    g_thread = std::thread([st = &_st, cfg_snapshot = _cfg,
                            prompt = std::move(_prompt)]() {
        // A per-turn client, built the way the automation cycle builds one.
        // Every tool runs on this thread, so nothing here races the UI.
        const std::string addr = campcat::mumu_adb_address(
            cfg_snapshot.mumu.tool_path, cfg_snapshot.mumu.device_index);
        campcat::adb_client adb(cfg_snapshot.adb.path,
                                addr.empty() ? cfg_snapshot.adb.serial : addr);
        adb.connect_remote(addr);
        campcat::llm::tool_context ctx;
        ctx.adb = &adb;
        ctx.adb_busy = [] { return g_adb_busy.load(); };
        // Folded here, in one place: the master switch and the tool's own gate.
        // The names are the ones the tools report in their approval requests,
        // so a tool asks about itself and there is no second table to keep in
        // step.
        const bool master = cfg_snapshot.approval.require;
        const campcat::approval_gates gates = cfg_snapshot.approval.gates;
        ctx.needs_approval = [master, gates](std::string_view _tool) {
            if (!master) {
                return false;
            }
            if (_tool == "tap") {
                return gates.tap;
            }
            if (_tool == "run_script") {
                return gates.run_script;
            }
            if (_tool == "save_script") {
                return gates.save_script;
            }
            if (_tool == "save_template") {
                return gates.save_template;
            }
            if (_tool == "update_script") {
                return gates.update_script;
            }
            if (_tool == "delete_script") {
                return gates.delete_script;
            }
            if (_tool == "rename_script") {
                return gates.rename_script;
            }
            // A gated tool this table has not heard of asks rather than skips:
            // an unguarded surprise is the one failure worth avoiding here.
            return true;
        };
        ctx.cfg = &cfg_snapshot;
        ctx.script_dir =
            std::filesystem::path(cfg_snapshot.config_home) / "scripts/llm";
        ctx.start_emulator = [st](const std::function<bool()> &_should_stop) {
            return st->start_mumu_blocking(_should_stop);
        };
        g_host.set_tool_context(ctx);
        campcat::llm_result res =
            g_host.run_turn(cfg_snapshot.config_home, prompt);
        g_host.set_tool_context({}); // `adb` dies with this lambda
        {
            std::lock_guard<std::mutex> lk(g_done_mu);
            g_done = handoff{};
            g_done.has = true;
            g_done.res = std::move(res);
        }
        g_running.store(false);
    });
}

/// Consolidate the session into the long-term notes, on the worker.
void start_memorize(std::filesystem::path _config_home) {
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_running.store(true);
    g_memorizing.store(true);
    g_thread = std::thread([config_home = std::move(_config_home)]() {
        campcat::llm_result res =
            g_host.memorize(config_home, campcat::k_memorize_timeout_ms);
        g_memorizing.store(false);
        {
            std::lock_guard<std::mutex> lk(g_done_mu);
            g_done = handoff{};
            g_done.has = true;
            g_done.is_memory = true;
            g_done.res = std::move(res);
        }
        g_running.store(false);
    });
}

void start_reset(std::filesystem::path _config_home) {
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_running.store(true);
    g_thread = std::thread([config_home = std::move(_config_home)]() {
        // Keep the session before forgetting it: `reset` is what drops the
        // history the consolidation reads, so the order here is the whole
        // point. A failed consolidation is not a reason to refuse the clear the
        // user asked for.
        g_memorizing.store(true);
        campcat::llm_result kept =
            g_host.memorize(config_home, campcat::k_memorize_timeout_ms);
        g_memorizing.store(false);
        if (!kept.ok) {
            campcat::automation_log::emit("[llm] memory not saved: " +
                                          kept.error);
        }
        campcat::llm_result res = g_host.reset(config_home.parent_path());
        {
            std::lock_guard<std::mutex> lk(g_done_mu);
            g_done = handoff{};
            g_done.has = true;
            g_done.is_reset = true;
            g_done.res = std::move(res);
        }
        g_running.store(false);
    });
}

/// Take the finished job, if any, and fold it into the conversation.
void consume_handoff() {
    handoff h;
    {
        std::lock_guard<std::mutex> lk(g_done_mu);
        if (!g_done.has) {
            return;
        }
        h = std::move(g_done);
        g_done = handoff{};
    }

    // Every finished request restarts the idle clock, consolidation included:
    // without this, finishing a memory pass would immediately satisfy the idle
    // condition again and it would loop.
    g_last_llm = ImGui::GetTime();

    if (h.is_memory) {
        if (h.res.ok) {
            // Exactly the notes `memorize` wrote back, so no re-read.
            g_memory_chars = static_cast<int>(h.res.text.size());
        }
        campcat::automation_log::emit(
            h.res.ok ? "[llm] long-term memory updated"
                     : "[llm] memory not saved: " + h.res.error);
        return;
    }

    if (!h.is_reset) {
        fill_reply(h.res);
        if (h.res.ok) {
            // Reported by the sidecar on a successful turn only: a result
            // without the field parses as 0, so a failure must not zero this.
            g_sidecar_turns = h.res.turns;
            campcat::automation_log::emit("[llm] ok");
        } else {
            campcat::automation_log::emit("[llm] failed: " + h.res.error);
        }
        return;
    }
    if (!h.res.ok) {
        campcat::automation_log::emit("[llm] reset failed: " + h.res.error);
        return;
    }
    drop_all_turns();
    g_sidecar_turns = 0;
    g_pin_bottom = true;
    campcat::automation_log::emit("[llm] conversation cleared");
}

/// Send whatever is staged: clear the input and hand the prompt to the worker.
void send_current(campcat::shell_state &_st, campcat::app_config &_cfg) {
    const std::string prompt = g_input;
    if (is_blank(g_input)) {
        return;
    }

    g_input.clear();

    append_user_turn(prompt);
    // Cleared from the UI thread, not just on the worker: the worker clears it
    // inside next_request_id, which runs after ensure_running may have spent a
    // minute starting the interpreter, and until then the new bubble would show
    // the previous request's leftovers.
    g_host.clear_stream();
    start_turn(_st, _cfg, prompt);
}

/// "Thinking" (when the sidecar says so) plus 1..3 dots on the frame clock, so
/// a waiting bubble visibly ticks instead of sitting on a static "...".
std::string waiting_label() {
    const int dots = 1 + static_cast<int>(ImGui::GetTime() / 0.4) % 3;
    return std::string(g_thinking ? "Thinking" : "") + std::string(dots, '.');
}

/// Centred two-line hint for an empty conversation.
void draw_empty_state() {
    const char *title = "Tell CampCat what to do";
    const char *hint = "It looks at the device screen itself, then taps";
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.4F);
    WITH_FONT(bold, {
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(0.0F, (avail.x - ImGui::CalcTextSize(title).x) * 0.5F));
        ImGui::TextUnformatted(title);
    });
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() +
        std::max(0.0F, (avail.x - ImGui::CalcTextSize(hint).x) * 0.5F));
    ImGui::TextDisabled("%s", hint);
}

/// Width of a bubble holding `_text`. Measured with a narrower wrap width than
/// the bubble ends up with, so the text never wraps a line earlier than needed.
float bubble_width(const char *_text, float _max_w, float _extra, float _pad) {
    float inner = _extra;
    if (_text != nullptr && *_text != '\0') {
        const ImVec2 sz = ImGui::CalcTextSize(_text, nullptr, false,
                                              _max_w - 2.0F * _pad - 4.0F);
        inner = std::max(inner, sz.x);
    }
    return std::min(inner + 2.0F * _pad, _max_w);
}

/// One exchange as two bubbles: the user's hugging the right edge, the reply on
/// the left. A pending turn shows a waiting bubble in place of the reply.
void draw_turn(llm_turn &_t, int _index) {
    ImGui::PushID(_index);
    const float pad = ImGui::GetStyle().WindowPadding.x;
    const float max_w = ImGui::GetContentRegionAvail().x * k_bubble_frac;

    const float uw = bubble_width(_t.user_text.c_str(), max_w, 0.0F, pad);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         ImGui::GetContentRegionAvail().x - uw);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, k_user_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_bubble_rounding);
    ImGui::BeginChild("u", ImVec2(uw, 0.0F),
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextWrapped("%s", _t.user_text.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // A markdown reply cannot be measured from the raw text (`**` would be
    // counted), so the bubble goes full width once there is real text; the
    // placeholder stays tight because a wide box holding "..." looks broken. It
    // is measured at its widest so the ticking dots do not resize it. A tool
    // image needs the full width too, or it would be clipped into a narrow
    // strip.
    const bool placeholder = _t.pending && _t.reply.empty();
    const bool has_image = !_t.image_png.empty();
    const float aw =
        (placeholder && !has_image)
            ? bubble_width(g_thinking ? "Thinking..." : "...", max_w, 0.0F, pad)
            : max_w;
    const bool failed = !_t.pending && !_t.ok;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, failed ? k_err_bg : k_bot_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_bubble_rounding);
    ImGui::BeginChild("a", ImVec2(aw, 0.0F),
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    // Shown above the reply so the model's device actions are visible even
    // while the answer is still streaming. Without them a tap that changed the
    // screen would look like nothing happened.
    for (const std::string &note : _t.tool_notes) {
        ImGui::TextDisabled("%s", note.c_str());
    }
    // What the model was looking at when it wrote the reply below: a tool image
    // (a screenshot, or a box `mark` drew) that would otherwise be invisible
    // here.
    ensure_turn_texture(_t);
    if (_t.tex != 0 && _t.tex_w > 0) {
        const ImVec2 img =
            fit_image(_t.tex_w, _t.tex_h, ImGui::GetContentRegionAvail().x);
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(_t.tex)),
                     img, ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
    }
    if (_t.pending && !_t.reply.empty()) {
        ImGui::Markdown(_t.reply.c_str(), _t.reply.size(), md_config());
    } else if (_t.pending) {
        ImGui::TextDisabled("%s", waiting_label().c_str());
    } else if (_t.ok) {
        ImGui::Markdown(_t.reply.c_str(), _t.reply.size(), md_config());
    } else {
        WITH_FONT(bold, { ImGui::TextColored(k_err_col, "Request failed"); });
        ImGui::TextWrapped("%s", _t.error.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::PopID();
}

/// Approval preview texture. Rebuilt only when the question changes: the
/// screenshot is a megabyte or two, far too much to re-upload every frame.
GLuint g_appr_tex = 0;
int g_appr_w = 0;
int g_appr_h = 0;
long g_appr_id = -1;

/// Pull the pending question and keep its texture in step. Returns whether a
/// question is waiting.
bool sync_approval_texture(const campcat::llm_approval &_a) {
    if (!_a.pending) {
        campcat::app::delete_tex(&g_appr_tex, &g_appr_w, &g_appr_h);
        g_appr_id = -1;
        return false;
    }
    if (_a.id == g_appr_id) {
        return true;
    }
    campcat::app::delete_tex(&g_appr_tex, &g_appr_w, &g_appr_h);
    g_appr_id = _a.id;
    const std::string png = g_host.approval_screen_png();
    if (png.empty()) {
        return true; // nothing to show, but the buttons must stay reachable
    }
    const cv::Mat buf(1, static_cast<int>(png.size()), CV_8UC1,
                      const_cast<char *>(png.data()));
    const cv::Mat bgr = cv::imdecode(buf, cv::IMREAD_COLOR);
    g_appr_tex = campcat::app::upload_bgr(bgr, &g_appr_w, &g_appr_h);
    return true;
}

/// The gear next to Send, and the approval gates behind it.
///
/// The gear, relative to the working directory the app is started from (the
/// same base `fonts.cpp` resolves `assets/fonts/` against). Drawn rather than
/// designed: a white disc at radius 0.63 of the half-size, eight teeth out to
/// 0.875, a hub punched back out at 0.22, all 8x supersampled and INTER_AREA'd
/// down. White on an alpha channel is the shape `load_icon` uploads.
constexpr char k_gear_path[] = "assets/icons/gear.png";

/// The paper plane on Send. White on purpose, like the gear: `load_icon` hands
/// back a glyph the draw call tints, so an enabled button shows it in the text
/// colour and a disabled one in that colour faded -- `BeginDisabled` scales the
/// alpha that `GetColorU32` hands back, so the icon fades with the button
/// instead of staying lit.
///
/// Drawn rather than designed, like the gear: two triangles sharing an apex at
/// (29,16), the wings swept back to (4,4) and (4,28), notched at (12,16), all
/// 8x supersampled and INTER_AREA'd down.
constexpr char k_send_path[] = "assets/icons/send.png";

/// How wide the plane is drawn inside its square button, in pixels. The same
/// box the gear uses, so the two square buttons carry equally sized icons.
constexpr float k_send_icon_side = 20.0F;

/// Which tools ask the human before acting. The master switch keeps its old
/// meaning -- off means nothing asks -- and the seven under it only matter
/// while it is on, hence the greying rather than hiding them.
void draw_approval_settings(campcat::app_config &_c) {
    const float side = ImGui::GetFrameHeight();
    if (ImGui::Button("##approval_settings", ImVec2(side, side))) {
        ImGui::OpenPopup("llm_approval_settings");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Which tools ask before acting");
    }
    // Drawn after the button, into the same window list, so the icon lands on
    // top of the frame. Tinted with the text colour so it follows the theme.
    const GLuint tex = campcat::app::load_icon(k_gear_path);
    if (tex != 0) {
        const ImVec2 mn = ImGui::GetItemRectMin();
        const float pad = (side - k_gear_side) * 0.5F;
        ImGui::GetWindowDrawList()->AddImage(
            static_cast<ImTextureID>(static_cast<uintptr_t>(tex)),
            ImVec2(mn.x + pad, mn.y + pad),
            ImVec2(mn.x + side - pad, mn.y + side - pad), ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F), ImGui::GetColorU32(ImGuiCol_Text));
    }

    if (!ImGui::BeginPopup("llm_approval_settings")) {
        return;
    }
    ImGui::Checkbox("Require approval", &_c.approval.require);
    ImGui::Separator();
    const bool master_off = !_c.approval.require;
    if (master_off) {
        ImGui::BeginDisabled();
    }
    ImGui::Checkbox("Device tap", &_c.approval.gates.tap);
    ImGui::Checkbox("Run script", &_c.approval.gates.run_script);
    ImGui::Checkbox("Save script", &_c.approval.gates.save_script);
    ImGui::Checkbox("Save template", &_c.approval.gates.save_template);
    ImGui::Checkbox("Update script", &_c.approval.gates.update_script);
    ImGui::Checkbox("Delete script", &_c.approval.gates.delete_script);
    ImGui::Checkbox("Rename script", &_c.approval.gates.rename_script);
    if (master_off) {
        ImGui::EndDisabled();
    }
    ImGui::EndPopup();
}

/// The box-annotation window, opened by double-clicking the preview. The box is
/// the user's redraw, kept apart from the model's so Cancel costs nothing.
bool g_annot_open = false;
long g_annot_id = -1; ///< which question this edit belongs to (`_a.id`)
bool g_annot_dragging = false;
bool g_annot_has_box = false;
int g_annot_ax = 0; ///< drag anchor, image px
int g_annot_ay = 0;
int g_annot_cx = 0; ///< drag current point, image px
int g_annot_cy = 0;
int g_annot_x0 = 0; ///< committed box, inclusive image px
int g_annot_y0 = 0;
int g_annot_x1 = 0;
int g_annot_y1 = 0;

/// A redrawn box waiting to go with the reply. The send bar shows it, and Send
/// and Reject carry it; it belongs to exactly one question.
std::optional<campcat::llm::rect> g_staged_box;
long g_staged_id = -1;

/// Drop the staged redraw, because the question it belonged to is gone.
void clear_staged() {
    g_staged_box.reset();
    g_staged_id = -1;
}

/// Open the annotation window for `_a`. The box is seeded from a staged redraw
/// when there is one (reopening must not lose the last edit), else from the
/// model's.
void open_annotation(const campcat::llm_approval &_a) {
    g_annot_open = true;
    g_annot_id = _a.id;
    g_annot_dragging = false;
    campcat::llm::rect seed;
    bool has = false;
    if (g_staged_box && g_staged_id == _a.id) {
        seed = *g_staged_box;
        has = true;
    } else if (!_a.highlight.empty()) {
        seed = _a.highlight[0];
        has = true;
    }
    g_annot_has_box = has;
    if (has) {
        g_annot_x0 = seed.x;
        g_annot_y0 = seed.y;
        g_annot_x1 = seed.x + seed.w - 1;
        g_annot_y1 = seed.y + seed.h - 1;
    }
}

/// The screenshot at 1:1, the model's box under the user's redraw, and the
/// button that hands the redraw to the send bar. Closes itself when the
/// question goes.
void draw_annotation_window() {
    if (!g_annot_open) {
        return;
    }
    const campcat::llm_approval a = g_host.pending_approval();
    if (!a.pending || a.id != g_annot_id || g_appr_tex == 0 || g_appr_w <= 0) {
        g_annot_open = false;
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(560.0F, 520.0F), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Correct the box", &open)) {
        ImGui::TextDisabled(
            "Drag a new box on the image. The thin one is the model's.");
        // Two rows below the canvas: the box readout and the button row.
        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0F;
        ImGui::BeginChild("annot_scroll", ImVec2(0.0F, -footer),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        const ImVec2 canvas(static_cast<float>(g_appr_w),
                            static_cast<float>(g_appr_h));
        ImGui::Image(
            static_cast<ImTextureID>(static_cast<uintptr_t>(g_appr_tex)),
            canvas, ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
        const ImVec2 mn = ImGui::GetItemRectMin();
        // A plain Image carries no id, so ImGui reads a drag on it as "move the
        // window" (see UpdateMouseMovingWindowEndFrame). An invisible button
        // over it claims the mouse, which is what makes the drag ours. Same
        // rect, so the layout below is untouched.
        ImGui::SetCursorScreenPos(mn);
        ImGui::InvisibleButton("annot_canvas", canvas,
                               ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImGuiIO &io = ImGui::GetIO();
        // 1:1, so image px is just the offset from the image's top-left: no
        // scale term to get wrong, unlike the shrunk preview in the bubble.
        const auto to_px = [&](const ImVec2 &_s, int *_x, int *_y) {
            *_x = std::clamp(static_cast<int>(_s.x - mn.x), 0, g_appr_w - 1);
            *_y = std::clamp(static_cast<int>(_s.y - mn.y), 0, g_appr_h - 1);
        };
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_annot_dragging = true;
            to_px(io.MousePos, &g_annot_ax, &g_annot_ay);
            g_annot_cx = g_annot_ax;
            g_annot_cy = g_annot_ay;
        }
        if (g_annot_dragging) {
            to_px(io.MousePos, &g_annot_cx, &g_annot_cy);
            if (!held) {
                g_annot_dragging = false;
                // A click that never moved is a stray click, not a zero-size
                // box.
                if (g_annot_cx != g_annot_ax || g_annot_cy != g_annot_ay) {
                    g_annot_x0 = std::min(g_annot_ax, g_annot_cx);
                    g_annot_y0 = std::min(g_annot_ay, g_annot_cy);
                    g_annot_x1 = std::max(g_annot_ax, g_annot_cx);
                    g_annot_y1 = std::max(g_annot_ay, g_annot_cy);
                    g_annot_has_box = true;
                }
            }
        }
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // The model's box stays visible under the redraw: how much the user
        // moved it is the whole point of this window.
        for (const campcat::llm::rect &r : a.highlight) {
            dl->AddRect(ImVec2(mn.x + static_cast<float>(r.x),
                               mn.y + static_cast<float>(r.y)),
                        ImVec2(mn.x + static_cast<float>(r.x + r.w),
                               mn.y + static_cast<float>(r.y + r.h)),
                        IM_COL32(255, 200, 60, 130), 0.0F, 0, 1.0F);
        }
        if (g_annot_dragging || g_annot_has_box) {
            const int x0 = g_annot_dragging ? g_annot_ax : g_annot_x0;
            const int y0 = g_annot_dragging ? g_annot_ay : g_annot_y0;
            const int x1 = g_annot_dragging ? g_annot_cx : g_annot_x1;
            const int y1 = g_annot_dragging ? g_annot_cy : g_annot_y1;
            const ImVec2 s0(mn.x + static_cast<float>(std::min(x0, x1)),
                            mn.y + static_cast<float>(std::min(y0, y1)));
            const ImVec2 s1(mn.x + static_cast<float>(std::max(x0, x1)),
                            mn.y + static_cast<float>(std::max(y0, y1)));
            dl->AddRectFilled(s0, s1, IM_COL32(80, 200, 255, 55));
            dl->AddRect(s0, s1, IM_COL32(255, 220, 80, 255), 0.0F, 0, 2.0F);
        }
        ImGui::EndChild();
        if (g_annot_has_box) {
            ImGui::TextDisabled("box %d,%d %dx%d px", g_annot_x0, g_annot_y0,
                                g_annot_x1 - g_annot_x0 + 1,
                                g_annot_y1 - g_annot_y0 + 1);
        } else {
            ImGui::TextDisabled("drag on the image to pick a box");
        }
        if (!g_annot_has_box) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Use this box")) {
            g_staged_box = campcat::llm::rect{g_annot_x0, g_annot_y0,
                                              g_annot_x1 - g_annot_x0 + 1,
                                              g_annot_y1 - g_annot_y0 + 1};
            g_staged_id = g_annot_id;
            g_annot_open = false;
        }
        if (!g_annot_has_box) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            g_annot_open = false; // the staged box, if any, is left alone
        }
    }
    ImGui::End();
    if (!open) {
        g_annot_open = false;
    }
}

/// The staged redraw in the send bar: the screenshot with the box on it, so
/// what is about to be sent is visible before it goes. Reuses the approval's
/// own texture; nothing new is uploaded.
void draw_staged_thumb(const std::string &_tool) {
    // Fitted to a square, not just to a width: a phone frame is portrait, so
    // capping width alone would turn the send bar's one row into a 200px-tall
    // band.
    const float s = std::min(k_stage_thumb_side / static_cast<float>(g_appr_w),
                             k_stage_thumb_side / static_cast<float>(g_appr_h));
    const ImVec2 size(static_cast<float>(g_appr_w) * s,
                      static_cast<float>(g_appr_h) * s);
    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(g_appr_tex)),
                 size, ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
    const ImVec2 img_min = ImGui::GetItemRectMin();
    const ImVec2 img_max = ImGui::GetItemRectMax();
    // Where the next row starts; the overlay below is put back here so it costs
    // no layout.
    const ImVec2 below = ImGui::GetCursorScreenPos();
    const campcat::llm::rect &r = *g_staged_box;
    const ImVec2 p0(img_min.x + static_cast<float>(r.x) * s,
                    img_min.y + static_cast<float>(r.y) * s);
    const ImVec2 p1(img_min.x + static_cast<float>(r.x + r.w) * s,
                    img_min.y + static_cast<float>(r.y + r.h) * s);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(80, 200, 255, 55));
    dl->AddRect(p0, p1, IM_COL32(255, 220, 80, 255), 0.0F, 0, 2.0F);
    ImGui::SameLine();
    ImGui::TextDisabled("Corrected %s box will be sent with your reply.",
                        _tool.c_str());
    ImGui::SetCursorScreenPos(ImVec2(img_max.x - 20.0F, img_min.y + 1.0F));
    if (ImGui::SmallButton("x##staged_clear")) {
        clear_staged();
    }
    ImGui::SetCursorScreenPos(below);
}

/// The question waiting on the user: the proposed box drawn over the
/// screenshot, and the two buttons that decide whether the tap runs at all.
void draw_approval_bubble(const campcat::llm_approval &_a) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, k_bot_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_bubble_rounding);
    ImGui::BeginChild("approval", ImVec2(0.0F, 0.0F),
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    WITH_FONT(bold, { ImGui::TextUnformatted("Waiting for your approval"); });
    ImGui::TextDisabled("%s", _a.summary.c_str());
    if (g_appr_tex != 0 && g_appr_w > 0) {
        const ImVec2 size = fit_image(g_appr_w, g_appr_h, k_approval_max_w);
        ImGui::Image(
            static_cast<ImTextureID>(static_cast<uintptr_t>(g_appr_tex)), size,
            ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
        // Read off the Image before anything else touches the "last item"
        // state.
        const bool dbl = _a.box_editable && ImGui::IsItemHovered() &&
                         ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        // The boxes are in screenshot pixels; the image is drawn scaled.
        const ImVec2 origin = ImGui::GetItemRectMin();
        const float scale = size.x / static_cast<float>(g_appr_w);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (const campcat::llm::rect &r : _a.highlight) {
            const ImVec2 p0(origin.x + static_cast<float>(r.x) * scale,
                            origin.y + static_cast<float>(r.y) * scale);
            const ImVec2 p1(origin.x + static_cast<float>(r.x + r.w) * scale,
                            origin.y + static_cast<float>(r.y + r.h) * scale);
            dl->AddRectFilled(p0, p1, IM_COL32(255, 200, 60, 48));
            dl->AddRect(p0, p1, IM_COL32(255, 200, 60, 230), 0.0F, 0, 2.0F);
        }
        if (dbl) {
            open_annotation(_a);
        }
    }
    if (_a.box_editable) {
        ImGui::TextDisabled("Double-click the image to correct the box.");
    }
    if (ImGui::Button("Approve")) {
        g_host.answer_approval(true);
        clear_staged(); // an approval means the box is right; a redraw is moot
    }
    ImGui::SameLine();
    // Rejection with a typed correction goes through the prompt box, which
    // stays live while this bubble is up; these buttons are the no-typing
    // shortcut. Plain words, not ✓/✗: the UI font carries no glyphs for those
    // two and draws them as "?" (see 2026-10-09-llm-tap-approval-design.md
    // §4.5).
    if (ImGui::Button("Reject")) {
        // A staged redraw rides along: the user staged it to be sent, and
        // dropping it silently here would surprise them.
        g_host.answer_approval(false, {}, g_staged_box);
        clear_staged();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/// While a script the tools are running is in flight: one line and a Stop, so a
/// loop that never ends can be stopped without quitting the app.
void draw_script_bubble() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, k_bot_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_bubble_rounding);
    ImGui::BeginChild("script_run", ImVec2(0.0F, 0.0F),
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("Running script...");
    if (ImGui::Button("Stop")) {
        campcat::llm::request_script_stop();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/// "Starting the emulator... 12s" plus a Stop, mirroring `draw_script_bubble`.
/// The emulator wait can run for minutes and nothing else in the app can cut it
/// short. The clock is this panel's own, started on the first frame the tool's
/// flag is seen, so the number is the wait as the human watched it.
void draw_emulator_bubble() {
    static double shown_at = 0.0;
    if (!campcat::llm::emulator_starting()) {
        shown_at = 0.0; // the next wait starts its own clock
        return;
    }
    if (shown_at == 0.0) {
        shown_at = ImGui::GetTime();
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, k_bot_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_bubble_rounding);
    ImGui::BeginChild("emulator_start", ImVec2(0.0F, 0.0F),
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::Text("Starting the emulator... %ds",
                static_cast<int>(ImGui::GetTime() - shown_at));
    if (ImGui::Button("Stop")) {
        campcat::llm::request_emulator_stop();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/// Draws the conversation. Returns true when the user asked to clear it via the
/// context menu, so the caller can start the reset on the worker.
bool draw_log(const ImVec2 &_size) {
    bool clear_requested = false;
    ImGui::BeginChild("llm_log", _size, ImGuiChildFlags_Borders);
    if (ImGui::BeginPopupContextWindow("llm_log_ctx",
                                       ImGuiPopupFlags_MouseButtonRight)) {
        // Greyed out while a turn is in flight, because clearing would have to
        // wait for it anyway.
        if (ImGui::MenuItem("Clear conversation", nullptr, false,
                            !g_running.load() && !g_turns.empty())) {
            clear_requested = true;
        }
        ImGui::Separator();
        // Readable at any time, running turn or not: it is a file, not
        // conversation state, so there is nothing to wait for.
        if (ImGui::MenuItem("Long-term memory...")) {
            g_mem_open = true;
            reload_memory();
        }
        ImGui::EndPopup();
    }
    // Placed after the context menu so right-clicking the log does not pop a
    // bigger menu, and popped below so nothing outside the conversation
    // inherits it. The empty state and the trailing bubbles are conversation
    // too. A real face, not `SetWindowFontScale`: the scale would leave
    // `io.FontGlobalScale` at 1.0, and `imgui_markdown` wraps replies to that,
    // clipping the tail of every line.
    ImGui::PushFont(fonts_get(font_role::chat));
    if (g_turns.empty()) {
        draw_empty_state();
    }
    for (size_t i = 0; i < g_turns.size(); ++i) {
        if (i > 0) {
            ImGui::Spacing(); // turns read as groups: twice the in-turn bubble
                              // gap
        }
        draw_turn(g_turns[i], static_cast<int>(i));
    }

    // The approval gate, at the end of the conversation: the user reads it as
    // the model's latest action, and it scrolls away with the rest once
    // answered.
    const campcat::llm_approval approval = g_host.pending_approval();
    if (sync_approval_texture(approval)) {
        ImGui::Spacing();
        draw_approval_bubble(approval);
    }

    // A script in flight, next to where the approval bubble was: the same place
    // the user is already looking. Only shown when there is really a script to
    // stop, so it never becomes a button that does nothing.
    if (campcat::llm::script_running()) {
        ImGui::Spacing();
        draw_script_bubble();
    }

    // The same place, for the same reason: a wait nothing else can cut short.
    if (campcat::llm::emulator_starting()) {
        ImGui::Spacing();
        draw_emulator_bubble();
    }
    ImGui::PopFont();

    // Follow new content only while the user is already at the bottom.
    if (g_pin_bottom) {
        ImGui::SetScrollHereY(1.0F);
    }
    const float max_scroll = ImGui::GetScrollMaxY();
    g_pin_bottom =
        (max_scroll <= 0.0F) || (ImGui::GetScrollY() >= max_scroll - 4.0F);
    ImGui::EndChild();
    return clear_requested;
}

} // namespace

void llm_panel_shutdown_gl() {
    // Unblock a question nobody can answer any more: without this the worker
    // stays inside service_approval and the join below never returns.
    g_host.cancel_approval();
    // A long script keeps the worker inside run_script, where cancel_approval
    // means nothing; without this the join below waits for the script to finish
    // on its own.
    campcat::llm::request_script_stop();
    // Same for a boot in flight: the wait polls this, so the join does not sit
    // through the rest of a VM coming up.
    campcat::llm::request_emulator_stop();
    if (g_thread.joinable()) {
        g_thread.join();
    }

    // The last consolidation is started, not awaited: this runs with the window
    // still on screen, so blocking here would look like a freeze. It finishes
    // in `llm_panel_shutdown_finish`, once there is nothing left to freeze.
    if (!g_config_home.empty()) {
        g_shutdown_thread = std::thread([]() {
            const campcat::llm_result res = g_host.memorize(
                g_config_home, campcat::k_shutdown_memorize_timeout_ms);
            if (!res.ok) {
                campcat::automation_log::emit("[llm] memory not saved: " +
                                              res.error);
            }
        });
    }

    campcat::app::delete_tex(&g_appr_tex, &g_appr_w, &g_appr_h);
    // The gear, Send and the log's chevrons go with them. Nothing here owns
    // them -- `load_icon` does -- but the context is still current at this
    // point and this is where the panel's own textures are already released.
    campcat::app::release_icons();
    g_appr_id = -1;
    drop_all_turns();
    g_thinking = false;
}

void llm_panel_shutdown_finish() {
    // Nothing visible is left to stall: the window is gone, so this wait is
    // only the process outliving it. Bounded by the timeout handed to
    // `memorize`.
    if (g_shutdown_thread.joinable()) {
        g_shutdown_thread.join();
    }
    g_host.stop();
}

campcat::activity_status llm_panel_activity() {
    // Most specific first: a boot and a script both run inside a turn, so
    // reading `g_running` first would report "Thinking" through all of them.
    // The memory pass sets `g_running` too, so it has to be read before it.
    if (g_host.pending_approval().pending) {
        return {campcat::activity_level::waiting, "Awaiting"};
    }
    if (campcat::llm::emulator_starting()) {
        return {campcat::activity_level::waiting, "Booting"};
    }
    if (campcat::llm::script_running()) {
        return {campcat::activity_level::busy, "Script"};
    }
    if (g_memorizing.load()) {
        return {campcat::activity_level::busy, "Memory"};
    }
    if (g_running.load()) {
        return {campcat::activity_level::busy, "Thinking"};
    }
    // The newest turn is "pending" from the moment the prompt is sent, so a new
    // turn clears this by itself; an empty conversation has nothing to report.
    const llm_turn *last = g_turns.empty() ? nullptr : &g_turns.back();
    if (last != nullptr && !last->pending && !last->ok) {
        return {campcat::activity_level::failed, "Failed"};
    }
    return {campcat::activity_level::idle, "Idle"};
}

void llm_panel_draw_panel(campcat::shell_state &_st, campcat::app_config &_cfg,
                          bool _adb_busy) {
    // Finished work is folded in on the UI thread, which owns g_turns.
    consume_handoff();

    // An automation cycle or the scheduler is using adb, so tools must refuse
    // rather than contend with it.
    g_adb_busy.store(_adb_busy);

    // Kept for the shutdown pass and the memory window, neither of which is
    // handed the config: this panel is the only place that sees it.
    g_config_home = _cfg.config_home;

    const bool busy = g_running.load();

    pump_stream();
    // Saved skills in a fixed column on the left, conversation on the right,
    // the prompt box below both. Side by side because the list is an index, not
    // a second conversation: stacked, it took height off every reply.
    script_editor_panel_draw(std::filesystem::path(_cfg.config_home) /
                                 "scripts/llm",
                             k_script_list_w, -g_footer_h);
    ImGui::SameLine();
    // Measured, not another negative width: a negative size is resolved against
    // the window's content region, not against what is left past the cursor, so
    // "-list_w" here would leave the list's width blank at the right edge too.
    const bool clear_requested =
        draw_log(ImVec2(ImGui::GetContentRegionAvail().x, -g_footer_h));
    if (clear_requested) {
        start_reset(_cfg.config_home);
    }

    draw_memory_window();

    // Top of whatever comes below the log: the reference for the footer height.
    const float footer_top = ImGui::GetCursorScreenPos().y;

    // While a question waits on the user, this box and its Send are the
    // rejection form: the text becomes the correction the model gets back. One
    // input, one habit, instead of a second box inside the bubble.
    const campcat::llm_approval approval = g_host.pending_approval();
    const bool awaiting = approval.pending;
    // A staged redraw belongs to exactly one question. Drop it as soon as that
    // question is gone, so it cannot ride the next one.
    if (g_staged_box && (!awaiting || g_staged_id != approval.id)) {
        clear_staged();
    }
    if (awaiting && g_staged_box && g_appr_tex != 0 && g_appr_w > 0) {
        draw_staged_thumb(approval.tool);
    }

    // Idle consolidation. Measured from the last LLM request rather than from
    // mouse or keyboard activity: an agent nobody is talking to is an agent
    // that can tidy up, and a turn in flight or a half-typed prompt means
    // somebody is still here. Runs on the same worker as a turn, so the two
    // never interleave on the sidecar's stdin.
    if (!g_running.load() && !awaiting && !g_turns.empty() &&
        is_blank(g_input) && !g_config_home.empty() &&
        ImGui::GetTime() - g_last_llm > k_idle_seconds) {
        start_memorize(g_config_home);
    }

    // The button is square like the gear: the icon carries the meaning, so no
    // label has to fit, and the 104px the text reserved goes back to the prompt
    // box. The width still reserves exactly what follows the input.
    const bool memorizing = g_memorizing.load();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float send_w = ImGui::GetFrameHeight();
    const float gear_w = ImGui::GetFrameHeight() + gap;
    ImGui::SetNextItemWidth(-(send_w + gear_w + gap * 2.0F + 2.0F));
    const bool submitted =
        prompt_input_text("##llm_prompt",
                          awaiting ? "Optional: tell it what to do instead"
                                   : "Tell CampCat what to do...",
                          &g_input);

    ImGui::SameLine();
    const bool can_send = awaiting || (!busy && !is_blank(g_input));
    if (!can_send) {
        ImGui::BeginDisabled();
    }
    ImGui::PushStyleColor(ImGuiCol_Button, k_primary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, k_primary_hi);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, k_primary_lo);
    const bool clicked = ImGui::Button("##llm_send", ImVec2(send_w, 0.0F));
    ImGui::PopStyleColor(3);
    // Read while disabled is still in effect: the icon wants the faded colour,
    // the tooltip must not get it, so the tint is taken here and the tooltip
    // goes out after EndDisabled.
    const ImU32 send_tint = ImGui::GetColorU32(ImGuiCol_Text);
    const bool send_hovered =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const GLuint send_tex = campcat::app::load_icon(k_send_path);
    if (send_tex != 0) {
        const ImVec2 mn = ImGui::GetItemRectMin();
        const float pad = (send_w - k_send_icon_side) * 0.5F;
        ImGui::GetWindowDrawList()->AddImage(
            static_cast<ImTextureID>(static_cast<uintptr_t>(send_tex)),
            ImVec2(mn.x + pad, mn.y + pad),
            ImVec2(mn.x + send_w - pad, mn.y + send_w - pad),
            ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F), send_tint);
    }
    if (!can_send) {
        ImGui::EndDisabled();
    }
    // The label used to say why a disabled Send was disabled; an icon cannot,
    // so that moved here. A disabled item never reports as hovered without the
    // flag. An empty prompt needs no explanation, and a turn in flight already
    // shows as its own header chip, so only the memory pass speaks up.
    if (send_hovered) {
        if (memorizing) {
            ImGui::SetTooltip("Summarizing...");
        } else if (can_send) {
            ImGui::SetTooltip("Send");
        }
    }
    ImGui::SameLine();
    draw_approval_settings(_cfg);

    // What the conversation currently holds, as a three-column grid so the
    // numbers line up instead of running together. Two turn counts, because
    // they answer different questions and drift apart: `turns` is what the
    // model still holds verbatim, `shown` is what this panel has kept, capped
    // at `k_turn_cap`.
    refresh_memory_chars();
    ImGui::Indent(k_stat_indent);
    WITH_FONT(small, {
        // Zero vertical cell padding: a row's height is its content plus
        // `CellPadding.y * 2` (imgui_tables.cpp:1886), and this line is
        // context, so the padding is the only slack worth removing. The
        // horizontal half stays, so the columns keep their inset.
        const float pad_x = ImGui::GetStyle().CellPadding.x;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(pad_x, 0.0F));
        if (ImGui::BeginTable("##llm_stats", 3,
                              ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Turns");
            ImGui::SameLine();
            ImGui::Text("%d", g_sidecar_turns);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Shown");
            ImGui::SameLine();
            ImGui::Text("%d/%d", static_cast<int>(g_turns.size()), k_turn_cap);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Memory (chars)");
            ImGui::SameLine();
            ImGui::Text("%d/%d", g_memory_chars,
                        static_cast<int>(campcat::llm::k_memory_max_chars));
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    });
    ImGui::Unindent(k_stat_indent);

    if ((clicked || submitted) && can_send) {
        if (awaiting) {
            const bool blank = is_blank(g_input);
            g_host.answer_approval(false, blank ? std::string() : g_input,
                                   g_staged_box);
            clear_staged();
            g_input.clear(); // consumed as the answer, not as a prompt
        } else {
            send_current(_st, _cfg);
        }
    }

    // Reserve exactly what the controls actually used, so they never creep over
    // the log on the next frame.
    g_footer_h = ImGui::GetItemRectMax().y - footer_top +
                 ImGui::GetStyle().ItemSpacing.y;
}

void llm_panel_draw_overlay() { draw_annotation_window(); }
