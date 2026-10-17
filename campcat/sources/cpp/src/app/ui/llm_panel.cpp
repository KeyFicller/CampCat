#include "app/ui/llm_panel.h"

#include <cstdio>
#include <cstring>

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "core/llm/llm_host.h"
#include "core/llm/llm_memory.h"
#include "core/mumu_launcher.h"

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
#include <cmath>
#include <deque>
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

/// One step of a turn's timeline, mirrored from the host while the turn runs.
struct llm_item {
    campcat::llm_event::kind type = campcat::llm_event::kind::text;
    std::string text;  ///< text: the model's words; tool: the dimmed note
    bool ok = true;    ///< tool
    long image_id = 0; ///< image: host key, and this item's identity
    /// Bytes until the texture is uploaded, freed right after: a conversation
    /// of screenshots does not fit in memory, let alone in VRAM.
    std::string png;
    GLuint tex = 0;
    int tex_w = 0;
    int tex_h = 0;
    bool tex_tried = false;   ///< upload attempted; 0 `tex` after this failed
    bool tex_dropped = false; ///< bytes never arrived, or the cap released it
};

/// One exchange in the conversation: what was sent and what came back.
struct llm_turn {
    std::string user_text;
    std::vector<llm_item> items; ///< what happened, in the order it happened
    bool pending = false; ///< sent but unanswered; drives the waiting bubble
    bool queued = false;  ///< shown, not yet handed to the worker
    bool frozen = false;  ///< stop growing; the user already sent the next one
    bool interrupted = false; ///< the turn was cut by that next message
    bool ok = false;
    std::string error;
};

/// Result of a finished background job, handed to the UI thread.
struct handoff {
    bool has = false;
    bool is_reset = false;
    bool is_memory = false;
    campcat::llm_result res;
    /// Timeline at the moment the turn finished. The live stream is cleared
    /// for the next turn before the UI may read it.
    campcat::llm_host::timeline timeline;
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
/// True while Clear conversation is in flight. Send stays disabled: a message
/// queued under a reset would be wiped when the log is dropped.
std::atomic<bool> g_resetting{false};
/// The worker sets this after it clears the stream for a queued message. The
/// UI then marks that message as the turn in flight.
std::atomic<bool> g_promote{false};
/// Latest message the user sent while a turn was running. Empty when none.
std::mutex g_queue_mu;
std::string g_next_prompt;
/// True while the worker is consolidating memory rather than answering a turn.
/// Only changes the Send button's tooltip. A message sent during the pass
/// discards the notes.
/// Set and cleared inside the worker that does the consolidating, so there is
/// no path that can leave it stuck on.
std::atomic<bool> g_memorizing{false};
std::thread g_thread;
/// The last consolidation, run at shutdown. A thread of its own because it is
/// deliberately not joined where `g_thread` is: the window has to close first.
std::thread g_shutdown_thread;
campcat::llm_host g_host;
std::mutex g_done_mu;
/// More than one job can finish before the UI draws: a turn that ended on its
/// own, then the message that was queued behind it. A single slot would drop
/// the first result into the second bubble.
std::deque<handoff> g_done;

/// UI-thread only. Time of the last LLM request, which is what "idle" is
/// measured from: an agent nobody is talking to is an agent that can tidy up.
double g_last_llm = 0.0;
/// How long the conversation must have been quiet before consolidating.
constexpr double k_idle_seconds = 600.0; // 10 minutes
/// Remembered every frame so the shutdown pass knows where the memory file is,
/// since `llm_panel_shutdown_gl` has no config of its own.
std::filesystem::path g_config_home;
/// Same reason as `g_config_home`: the shutdown pass needs the selected
/// provider, and only the draw call sees the config.
std::string g_provider = "deepseek";

/// The page the memory window is showing.
campcat::llm::memory_kind g_mem_kind = campcat::llm::memory_kind::operator_notes;
/// The Long-term memory window, and the copy it displays.
bool g_mem_open = false;
std::string g_mem_buf;
/// Characters in the page being shown. The status line keeps its own count.
int g_mem_page_chars = 0;

/// The sidecar's own count of the turns it still holds verbatim, taken from
/// each successful reply. Deliberately not `g_turns.size()`: that counts
/// bubbles and stops at `k_turn_cap`, while the sidecar compacts once the
/// conversation reaches its token trigger line.
int g_sidecar_turns = 0;
/// Characters in the memory file, for the status line. The window that shows
/// the notes is opened on demand, so this is kept even while it is closed.
int g_memory_chars = 0;
/// When `g_memory_chars` was last read, so the file is not re-read per frame.
double g_memory_read_at = 0.0;

/// (Re)read the memory file into the window's buffer.
void reload_memory() {
    std::string text;
    if (!campcat::llm::read_memory(g_config_home, g_mem_kind, &text)) {
        text = "(the memory file could not be read)";
        g_mem_page_chars = 0;
    } else {
        g_mem_page_chars = static_cast<int>(text.size());
        if (text.empty()) {
            text = "(no memory yet)";
        }
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
    if (campcat::llm::read_memory(g_config_home,
                                  campcat::llm::memory_kind::operator_notes,
                                  &text)) {
        g_memory_chars = static_cast<int>(text.size());
    }
}

/// Read-only view of one notebook. Switching pages reloads that file.
void draw_memory_page(campcat::llm::memory_kind _kind) {
    if (g_mem_kind != _kind) {
        g_mem_kind = _kind;
        reload_memory();
    }
    const bool scripter = _kind == campcat::llm::memory_kind::scripter_notes;
    const char *popup =
        scripter ? "clear_scripter_memory" : "clear_operator_memory";
    const char *ask = scripter ? "Delete the scripter long-term memory file?"
                               : "Delete the operator long-term memory file?";

    ImGui::TextUnformatted(
        campcat::llm::memory_path(g_config_home, _kind).string().c_str());
    ImGui::TextDisabled("Kept in this file. Written when the conversation goes "
                        "idle, and before "
                        "it is cleared.");
    ImGui::Text("%d/%d", g_mem_page_chars,
                static_cast<int>(campcat::llm::k_memory_max_chars));
    if (ImGui::Button("Reload")) {
        reload_memory();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear memory")) {
        ImGui::OpenPopup(popup);
    }
    if (ImGui::BeginPopupModal(popup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(ask);
        if (ImGui::Button("Delete", ImVec2(120.0F, 0.0F))) {
            if (!campcat::llm::clear_memory(g_config_home, _kind)) {
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

    if (ImGui::BeginTabBar("##memory_books")) {
        if (ImGui::BeginTabItem("Operator")) {
            draw_memory_page(campcat::llm::memory_kind::operator_notes);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scripter")) {
            draw_memory_page(campcat::llm::memory_kind::scripter_notes);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
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

/// Image textures kept alive at once, oldest released first past this. The
/// conversation's steps stay, so a released image keeps its row and says so.
constexpr size_t k_tex_cap = 24;

/// Images whose texture is live, oldest first, by `llm_item::image_id`. Ids and
/// not pointers: turns are erased from the front as the cap is reached, so an
/// item can move while the queue still refers to it.
std::deque<long> g_tex_order;

/// The item holding `_id`'s texture, if the turn is still in the log.
llm_item *find_item(long _id) {
    for (llm_turn &t : g_turns) {
        for (llm_item &it : t.items) {
            if (it.image_id == _id) {
                return &it;
            }
        }
    }
    return nullptr;
}

/// Forget the queue's claim on `_id`, releasing its texture.
void release_item_tex(long _id) {
    const auto end = std::remove(g_tex_order.begin(), g_tex_order.end(), _id);
    g_tex_order.erase(end, g_tex_order.end());
    if (llm_item *it = find_item(_id)) {
        campcat::app::delete_tex(&it->tex, &it->tex_w, &it->tex_h);
    }
}

/// Release the oldest textures until the cap is met again.
void evict_over_cap() {
    while (g_tex_order.size() > k_tex_cap) {
        const long id = g_tex_order.front();
        if (llm_item *it = find_item(id)) {
            campcat::app::delete_tex(&it->tex, &it->tex_w, &it->tex_h);
            it->tex_dropped = true;
        }
        g_tex_order.pop_front();
    }
}

/// Upload an item's image once and release its bytes. UI thread, and the caller
/// must hold the GL context.
void ensure_texture(llm_item &_it) {
    if (_it.tex_tried || _it.png.empty()) {
        return;
    }
    _it.tex_tried = true;
    const cv::Mat buf(1, static_cast<int>(_it.png.size()), CV_8UC1,
                      _it.png.data());
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_UNCHANGED);
    _it.png.clear();
    _it.png.shrink_to_fit();
    if (img.empty()) {
        return;
    }
    if (img.channels() == 1) {
        cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);
    }
    if (img.cols > k_turn_img_max_w) {
        const double scale = static_cast<double>(k_turn_img_max_w) / img.cols;
        cv::Mat small;
        cv::resize(img, small, cv::Size(), scale, scale, cv::INTER_AREA);
        img = small;
    }
    _it.tex = img.channels() == 4
                  ? campcat::app::upload_bgra(img, &_it.tex_w, &_it.tex_h)
                  : campcat::app::upload_bgr(img, &_it.tex_w, &_it.tex_h);
    if (_it.tex == 0) {
        _it.tex_dropped = true;
        return;
    }
    g_tex_order.push_back(_it.image_id);
    evict_over_cap();
}

/// Release every texture `_t`'s steps own, and the queue's claim on them.
void release_turn(llm_turn &_t) {
    for (llm_item &it : _t.items) {
        if (it.image_id != 0) {
            release_item_tex(it.image_id);
        }
    }
}

/// Drop every turn, releasing the textures they own. The GL context must be
/// live.
void drop_all_turns() {
    for (llm_turn &t : g_turns) {
        release_turn(t);
    }
    g_turns.clear();
    g_tex_order.clear();
}

/// Mirror the host's timeline into the waiting turn: a text step grows in
/// place, a new step appends, and an image's bytes are taken the moment it
/// appears -- the host drops them when the next request starts.
void sync_items(llm_turn &_t) {
    const std::vector<campcat::llm_event> events = g_host.events();
    for (size_t i = 0; i < events.size(); ++i) {
        const campcat::llm_event &e = events[i];
        if (i < _t.items.size()) {
            if (e.type == campcat::llm_event::kind::text &&
                _t.items[i].text != e.text) {
                _t.items[i].text = e.text;
            }
            continue;
        }
        llm_item it;
        it.type = e.type;
        it.text = e.text;
        it.ok = e.ok;
        it.image_id = e.image_id;
        if (e.type == campcat::llm_event::kind::image) {
            it.png = g_host.event_image_png(e.image_id);
            it.tex_dropped = it.png.empty();
        }
        _t.items.push_back(std::move(it));
    }
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
void append_user_turn(std::string _prompt, bool _queued = false) {
    llm_turn t;
    t.user_text = std::move(_prompt);
    t.pending = true;
    t.queued = _queued;

    g_turns.push_back(std::move(t));
    while (static_cast<int>(g_turns.size()) > k_turn_cap) {
        release_turn(g_turns.front());
        g_turns.erase(g_turns.begin());
    }
    g_pin_bottom = true;
}

/// The turn the worker is answering. A queued message is on screen already
/// but is not this turn until the worker takes it.
llm_turn *in_flight_turn() {
    for (auto it = g_turns.rbegin(); it != g_turns.rend(); ++it) {
        if (it->pending && !it->queued) {
            return &*it;
        }
    }
    return nullptr;
}

/// The message waiting to be sent, if the user typed one during a turn.
llm_turn *queued_turn() {
    for (auto it = g_turns.rbegin(); it != g_turns.rend(); ++it) {
        if (it->queued) {
            return &*it;
        }
    }
    return nullptr;
}

/// Fill the waiting turn with the reply. UI thread only.
/// Same growth rules as `sync_items`, reading a finished copy rather than the
/// live stream. Image bytes come from the copy: the host may already have
/// dropped them for the next turn.
void apply_snap(llm_turn &_t, const campcat::llm_host::timeline &_tl) {
    for (size_t i = 0; i < _tl.events.size(); ++i) {
        const campcat::llm_event &e = _tl.events[i];
        if (i < _t.items.size()) {
            if (e.type == campcat::llm_event::kind::text &&
                _t.items[i].text != e.text) {
                _t.items[i].text = e.text;
            }
            continue;
        }
        llm_item it;
        it.type = e.type;
        it.text = e.text;
        it.ok = e.ok;
        it.image_id = e.image_id;
        if (e.type == campcat::llm_event::kind::image) {
            for (const auto &image : _tl.images) {
                if (image.first == e.image_id) {
                    it.png = image.second;
                    break;
                }
            }
            it.tex_dropped = it.png.empty();
        }
        _t.items.push_back(std::move(it));
    }
}

void fill_reply(const campcat::llm_result &_res,
                const campcat::llm_host::timeline &_tl) {
    llm_turn *t = in_flight_turn();
    if (t == nullptr) {
        return;
    }
    // Once more, from the copy taken as the turn finished. The live stream
    // may already belong to the next message.
    apply_snap(*t, _tl);
    t->pending = false;
    t->frozen = false;
    t->interrupted = _res.ok && _res.interrupted;
    t->ok = _res.ok;
    if (!_res.ok) {
        t->error = _res.error;
    } else if (!t->interrupted && !_res.text.empty()) {
        // The timeline is what was streamed, and it normally ends with the
        // reply. An empty one means the stream never reached us; keeping the
        // result here is what stops the answer going missing.
        const bool has_text =
            !t->items.empty() &&
            t->items.back().type == campcat::llm_event::kind::text;
        if (!has_text) {
            llm_item it;
            it.type = campcat::llm_event::kind::text;
            it.text = _res.text;
            t->items.push_back(std::move(it));
        }
    }
    g_pin_bottom = true;
}

/// Mirror the in-flight stream into the waiting turn so its bubble grows.
void pump_stream() {
    llm_turn *t = in_flight_turn();
    // Frozen once the user has sent the next message: the bubble stays on
    // what had already arrived. The finished copy lands in `fill_reply`.
    if (t == nullptr || t->frozen) {
        g_thinking = g_host.is_thinking();
        return;
    }
    sync_items(*t);
    g_thinking = g_host.is_thinking();
}

/// Hand a finished turn to the UI, timeline included, before the stream is
/// cleared for whatever runs next.
void post_turn(campcat::llm_result _res) {
    handoff done;
    done.has = true;
    done.res = std::move(_res);
    done.timeline = g_host.copy_timeline();
    std::lock_guard<std::mutex> lk(g_done_mu);
    g_done.push_back(std::move(done));
}

/// The message the user sent while a turn was running, if there is one.
/// Clears the interrupt flag: the message is about to become a turn of its own.
std::optional<std::string> take_queued() {
    std::lock_guard<std::mutex> lk(g_queue_mu);
    if (g_next_prompt.empty()) {
        return std::nullopt;
    }
    std::string prompt = std::move(g_next_prompt);
    g_next_prompt.clear();
    g_host.clear_interrupt();
    return prompt;
}

/// Run `_prompt`, then any message the user sends while it runs. One thread:
/// the next message does not start another. `adb` lives for the whole loop.
/// `_promote` is false for the turn the UI just started itself.
void run_prompt_loop(campcat::shell_state *_st, const campcat::app_config &_cfg,
                     std::string _prompt, bool _promote) {
    const std::string addr = campcat::mumu_adb_address(
        _cfg.mumu.tool_path, _cfg.mumu.device_index);
    campcat::adb_client adb(_cfg.adb.path,
                            addr.empty() ? _cfg.adb.serial : addr);
    adb.connect_remote(addr);
    campcat::llm::tool_context ctx;
    ctx.adb = &adb;
    ctx.adb_busy = [] { return g_adb_busy.load(); };
    // Both read live rather than folded here: the settings popup can change
    // them mid-turn, and a gate that keeps asking after it was switched off
    // is the bug this avoids. `snapshot_cfg` is the thread-safe read, and
    // `_st` outlives the thread (it is a `main` local). `_cfg` stays this
    // thread's snapshot: it is what the adb client was built from.
    ctx.needs_approval = [_st](std::string_view _tool) {
        return campcat::llm::approval_gate_required(
            _st->snapshot_cfg().approval, _tool);
    };
    ctx.box_snap = [_st] { return _st->snapshot_cfg().box_snap; };
    ctx.cfg = &_cfg;
    ctx.script_dir = std::filesystem::path(_cfg.config_home);
    ctx.start_emulator = [_st](const std::function<bool()> &_should_stop) {
        return _st->start_mumu_blocking(_should_stop);
    };
    g_host.set_tool_context(ctx);
    while (true) {
        if (_promote) {
            g_host.clear_stream();
            g_promote.store(true);
        }
        _promote = true;
        campcat::llm_result res = g_host.run_turn(
            _cfg.config_home, _cfg.llm.provider, _prompt,
            _cfg.llm.max_dispatches, _cfg.llm.max_model_rounds,
            // Sampling is on: the human is correcting this agent's regions, so
            // only it runs, and only with the two tools a sample needs.
            _cfg.region_score.record);
        post_turn(std::move(res));
        std::optional<std::string> next = take_queued();
        if (!next.has_value()) {
            break;
        }
        _prompt = std::move(*next);
    }
    g_host.set_tool_context({}); // `adb` dies with this function
    g_running.store(false);
}

/// Run one turn on the worker.
void start_turn(campcat::shell_state &_st, const campcat::app_config &_cfg,
                std::string _prompt) {
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_running.store(true);
    // The whole snapshot, by value: the script tools need the interpreter's
    // config (`tool_context::cfg`), and `_cfg` is a per-frame copy in the
    // caller's frame, so a pointer to it would dangle the moment the frame
    // ends. `_st` outlives the thread (it is a `main` local, and this thread
    // is joined by `llm_panel_shutdown_gl`).
    g_thread = std::thread([st = &_st, cfg_snapshot = _cfg,
                            prompt = std::move(_prompt)]() {
        run_prompt_loop(st, cfg_snapshot, prompt, false);
    });
}

/// Operator first, then scripter. A failed operator pass still writes scripter.
/// A message queued during the first pass skips the second. `*_operator_out`
/// is the operator pass, which is what the status line counts.
void memorize_both(const std::filesystem::path &_home, const std::string &_provider,
                   int _timeout_ms, campcat::llm_result *_operator_out) {
    *_operator_out = g_host.memorize(
        _home, _provider, campcat::llm::memory_kind::operator_notes, _timeout_ms);
    // operator 失败也要写 scripter。只有用户已经发来新消息才停。
    if (g_host.interrupt_pending()) {
        return;
    }
    campcat::llm_result scripter = g_host.memorize(
        _home, _provider, campcat::llm::memory_kind::scripter_notes, _timeout_ms);
    if (!scripter.ok) {
        campcat::automation_log::emit("[llm] scripter memory not saved: " +
                                      scripter.error);
    }
}

/// Consolidate the session into the long-term notes, on the worker.
/// A message sent while this runs discards the notes and becomes the next turn.
void start_memorize(campcat::shell_state &_st, const campcat::app_config &_cfg) {
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_running.store(true);
    g_memorizing.store(true);
    g_thread = std::thread([st = &_st, cfg_snapshot = _cfg]() {
        campcat::llm_result res;
        memorize_both(cfg_snapshot.config_home, cfg_snapshot.llm.provider,
                      campcat::k_memorize_timeout_ms, &res);
        g_memorizing.store(false);
        // Checked before the queue is taken: taking it clears the flag.
        const bool dropped = g_host.interrupt_pending();
        if (!dropped) {
            handoff done;
            done.has = true;
            done.is_memory = true;
            done.res = std::move(res);
            std::lock_guard<std::mutex> lk(g_done_mu);
            g_done.push_back(std::move(done));
        }
        std::optional<std::string> next = take_queued();
        if (!next.has_value()) {
            g_running.store(false);
            return;
        }
        run_prompt_loop(st, cfg_snapshot, std::move(*next), true);
    });
}

void start_reset(std::filesystem::path _config_home, std::string _provider) {
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_running.store(true);
    g_resetting.store(true);
    g_thread = std::thread([config_home = std::move(_config_home),
                            provider = std::move(_provider)]() {
        // Keep the session before forgetting it: `reset` is what drops the
        // history the consolidation reads, so the order here is the whole
        // point. A failed consolidation is not a reason to refuse the clear the
        // user asked for.
        g_memorizing.store(true);
        campcat::llm_result kept;
        memorize_both(config_home, provider, campcat::k_memorize_timeout_ms, &kept);
        g_memorizing.store(false);
        if (!kept.ok) {
            campcat::automation_log::emit("[llm] memory not saved: " +
                                          kept.error);
        }
        campcat::llm_result res = g_host.reset(config_home.parent_path());
        {
            handoff done;
            done.has = true;
            done.is_reset = true;
            done.res = std::move(res);
            std::lock_guard<std::mutex> lk(g_done_mu);
            g_done.push_back(std::move(done));
        }
        g_resetting.store(false);
        g_running.store(false);
    });
}

/// Take the finished job, if any, and fold it into the conversation.
void consume_handoff() {
    handoff h;
    {
        std::lock_guard<std::mutex> lk(g_done_mu);
        if (g_done.empty()) {
            return;
        }
        h = std::move(g_done.front());
        g_done.pop_front();
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
            h.res.ok ? "[llm] operator long-term memory updated"
                     : "[llm] memory not saved: " + h.res.error);
        return;
    }

    if (!h.is_reset) {
        fill_reply(h.res, h.timeline);
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

/// Show `_prompt` now and stop the turn in flight at its next tool. A second
/// send before that tool replaces the message; the earlier one is not sent.
void queue_prompt(std::string _prompt) {
    if (llm_turn *live = in_flight_turn()) {
        live->frozen = true;
    }
    if (llm_turn *queued = queued_turn()) {
        queued->user_text = _prompt;
    } else {
        append_user_turn(std::move(_prompt), true);
        // append moved from the parameter; the queue still needs the text.
        _prompt = g_turns.back().user_text;
    }
    {
        std::lock_guard<std::mutex> lk(g_queue_mu);
        g_next_prompt = std::move(_prompt);
    }
    g_host.request_interrupt();
    campcat::llm::request_script_stop();
    campcat::llm::request_emulator_stop();
    g_host.release_approval();
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
    // is measured at its widest so the ticking dots do not resize it. Steps and
    // images need the full width too, or a note would be wrapped into a narrow
    // column and an image clipped into a narrow strip.
    const bool placeholder = _t.pending && _t.items.empty();
    const float aw =
        placeholder
            ? bubble_width(g_thinking ? "Thinking..." : "...", max_w, 0.0F, pad)
            : max_w;
    const bool failed = !_t.pending && !_t.ok;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, failed ? k_err_bg : k_bot_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_bubble_rounding);
    ImGui::BeginChild("a", ImVec2(aw, 0.0F),
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    // The turn as it happened, step by step: the model's words in the normal
    // colour, what it did dimmed, and each image where it appeared. Markdown
    // rather than plain text for a note, because a note carries a whole tool
    // result and the renderer is what wraps it to the bubble width -- plain
    // text is simply clipped at the edge once the line runs long. The disabled
    // colour is pushed back by hand so the notes still read as quieter than the
    // replies between them.
    for (llm_item &it : _t.items) {
        if (it.type == campcat::llm_event::kind::image) {
            ensure_texture(it);
            if (it.tex != 0 && it.tex_w > 0) {
                const ImVec2 img = fit_image(it.tex_w, it.tex_h,
                                             ImGui::GetContentRegionAvail().x);
                ImGui::Image(
                    static_cast<ImTextureID>(static_cast<uintptr_t>(it.tex)),
                    img, ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
            } else if (it.tex_dropped) {
                ImGui::TextDisabled("(screenshot dropped)");
            }
            continue;
        }
        if (it.type == campcat::llm_event::kind::tool) {
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::Markdown(it.text.c_str(), it.text.size(), md_config());
            ImGui::PopStyleColor();
            continue;
        }
        ImGui::Markdown(it.text.c_str(), it.text.size(), md_config());
    }
    // Only while the reply is still to come: a growing text step is its own
    // sign that the model is working, and a label under it would just be a
    // second one.
    const bool has_text =
        !_t.items.empty() &&
        _t.items.back().type == campcat::llm_event::kind::text;
    if (_t.pending && !has_text) {
        ImGui::TextDisabled("%s", waiting_label().c_str());
    } else if (_t.interrupted) {
        ImGui::TextDisabled("Interrupted");
    } else if (failed) {
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
/// same base `fonts.cpp` resolves `campcat/sources/cpp/assets/fonts/` against).
/// Drawn rather than designed: a white disc at radius 0.63 of the half-size,
/// eight teeth out to 0.875, a hub punched back out at 0.22, all 8x
/// supersampled and INTER_AREA'd down. White on an alpha channel is the shape
/// `load_icon` uploads.
constexpr char k_gear_path[] = "campcat/sources/cpp/assets/icons/gear.png";

/// The paper plane on Send. White on purpose, like the gear: `load_icon` hands
/// back a glyph the draw call tints, so an enabled button shows it in the text
/// colour and a disabled one in that colour faded -- `BeginDisabled` scales the
/// alpha that `GetColorU32` hands back, so the icon fades with the button
/// instead of staying lit.
///
/// Drawn rather than designed, like the gear: two triangles sharing an apex at
/// (29,16), the wings swept back to (4,4) and (4,28), notched at (12,16), all
/// 8x supersampled and INTER_AREA'd down.
constexpr char k_send_path[] = "campcat/sources/cpp/assets/icons/send.png";

/// How wide the plane is drawn inside its square button, in pixels. The same
/// box the gear uses, so the two square buttons carry equally sized icons.
constexpr float k_send_icon_side = 20.0F;

/// Wide enough for the longest snap mode name ("Otsu blobs") plus the arrow.
constexpr float k_snap_combo_w = 130.0F;

/// Empty or the infinity mark means the built-in stand-in. Anything else must
/// be a positive integer of at most seven digits; a half-typed field is not
/// written.
bool parse_cap(const char *_text, int _fallback, int *_out) {
    if (_text[0] == '\0' || std::strcmp(_text, "\xE2\x88\x9E") == 0) {
        *_out = _fallback;
        return true;
    }
    int n = 0;
    int digits = 0;
    for (const char *p = _text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9' || digits == 7) {
            return false;
        }
        n = n * 10 + (*p - '0');
        ++digits;
    }
    if (n < 1) {
        return false;
    }
    *_out = n;
    return true;
}

void show_cap(char *_buf, std::size_t _n, int _value) {
    if (campcat::llm::fuse_is_unlimited(_value)) {
        std::snprintf(_buf, _n, "\xE2\x88\x9E");
    } else {
        std::snprintf(_buf, _n, "%d", _value);
    }
}

/// Per-turn fuse caps. Drawn inside the status font so infinity has a glyph.
void draw_fuse_settings(campcat::app_config &_c) {
    static char calls_buf[32];
    static char rounds_buf[32];
    static bool fresh = true;
    if (!ImGui::BeginPopup("llm_fuse_settings")) {
        fresh = true;
        return;
    }
    if (fresh) {
        show_cap(calls_buf, sizeof calls_buf, _c.llm.max_dispatches);
        show_cap(rounds_buf, sizeof rounds_buf, _c.llm.max_model_rounds);
        fresh = false;
    }
    ImGui::InputText("Dispatches per turn", calls_buf, sizeof calls_buf);
    ImGui::InputText("Model rounds per turn", rounds_buf, sizeof rounds_buf);
    int calls = 0;
    int rounds = 0;
    const bool calls_ok =
        parse_cap(calls_buf, campcat::llm::k_default_max_dispatches, &calls);
    const bool rounds_ok =
        parse_cap(rounds_buf, campcat::llm::k_default_max_model_rounds, &rounds);
    if (calls_ok && rounds_ok && campcat::llm::fuse_pair_ok(calls, rounds)) {
        _c.llm.max_dispatches = calls;
        _c.llm.max_model_rounds = rounds;
    } else if (calls_ok && rounds_ok) {
        ImGui::TextDisabled(
            "Model rounds per turn cannot exceed dispatches per turn.");
    }
    ImGui::EndPopup();
}

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
    // Not an approval gate either, but this is the panel's one settings popup,
    // and switching the model is the same kind of "change how it behaves" knob.
    ImGui::TextDisabled("Model provider");
    int provider = _c.llm.provider == "qwen" ? 1 : 0;
    ImGui::RadioButton("DeepSeek", &provider, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Qwen", &provider, 1);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Qwen goes through Aliyun's OpenAI-compatible endpoint and needs "
            "QWEN_API_KEY in .env. Applies from the next message, no "
            "restart.");
    }
    _c.llm.provider = provider == 1 ? "qwen" : "deepseek";
    ImGui::Separator();
    ImGui::Checkbox("Require approval", &_c.approval.require);
    ImGui::Separator();
    const bool master_off = !_c.approval.require;
    if (master_off) {
        ImGui::BeginDisabled();
    }
    ImGui::Checkbox("Device tap", &_c.approval.gates.tap);
    ImGui::Checkbox("Go home", &_c.approval.gates.go_home);
    ImGui::Checkbox("Run script", &_c.approval.gates.run_script);
    ImGui::Checkbox("Save script", &_c.approval.gates.save_script);
    ImGui::Checkbox("Save template", &_c.approval.gates.save_template);
    ImGui::Checkbox("Update script", &_c.approval.gates.update_script);
    ImGui::Checkbox("Delete script", &_c.approval.gates.delete_script);
    ImGui::Checkbox("Rename script", &_c.approval.gates.rename_script);
    if (master_off) {
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    // Not an approval gate: the snap is something the tool does on the way to
    // acting. It sits here because this is where a human comes when a tap
    // landed wrong, and it needs the same kind of kill switch. One control
    // rather than a checkbox over a pair of radios: "off" is just the third
    // mode, so the two answers cannot disagree. The list is the enum in
    // declaration order, `none` first, so a mode index and a strategy are the
    // same number.
    const char *const k_snap_modes[] = {"None", "Otsu blobs", "Edge step"};
    int snap_mode = static_cast<int>(_c.box_snap.strategy);
    ImGui::SetNextItemWidth(k_snap_combo_w);
    if (ImGui::Combo("Snap boxes", &snap_mode, k_snap_modes, 3)) {
        _c.box_snap.strategy =
            static_cast<campcat::box_snap_strategy>(snap_mode);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Pull the model's box onto the element before tapping or cutting "
            "it. The measured gain is small and one kind of element (a glowing "
            "or soft-edged button) comes out worse, so choose None to restore "
            "the old behaviour exactly.");
    }
    ImGui::EndPopup();
}

/// Draw `_region` on a canvas whose top-left is `_mn` at scale `_s` (1.0 for
/// the correction window, smaller for a thumbnail). A circle is drawn as a
/// disc and a diamond as its four vertices: the corners a box would add are
/// exactly what those shapes exist to exclude.
/// `_fill` of 0 means no fill.
void draw_region(ImDrawList *_dl, const ImVec2 &_mn, float _s,
                 const campcat::llm::region &_region, ImU32 _fill,
                 ImU32 _stroke, float _thickness) {
    const auto at = [&](int _x, int _y) {
        return ImVec2(_mn.x + static_cast<float>(_x) * _s,
                      _mn.y + static_cast<float>(_y) * _s);
    };
    if (const auto *b = std::get_if<campcat::llm::box>(&_region)) {
        const ImVec2 p0 = at(b->x, b->y);
        const ImVec2 p1 = at(b->x + b->w, b->y + b->h);
        if (_fill != 0) {
            _dl->AddRectFilled(p0, p1, _fill);
        }
        _dl->AddRect(p0, p1, _stroke, 0.0F, 0, _thickness);
        return;
    }
    if (const auto *c = std::get_if<campcat::llm::circle>(&_region)) {
        const ImVec2 centre = at(c->cx, c->cy);
        const float radius = static_cast<float>(c->r) * _s;
        if (_fill != 0) {
            _dl->AddCircleFilled(centre, radius, _fill);
        }
        _dl->AddCircle(centre, radius, _stroke, 0, _thickness);
        return;
    }
    const auto &d = std::get<campcat::llm::diamond>(_region);
    const ImVec2 top = at(d.cx, d.cy - d.ry);
    const ImVec2 right = at(d.cx + d.rx, d.cy);
    const ImVec2 bottom = at(d.cx, d.cy + d.ry);
    const ImVec2 left = at(d.cx - d.rx, d.cy);
    if (_fill != 0) {
        _dl->AddQuadFilled(top, right, bottom, left, _fill);
    }
    _dl->AddQuad(top, right, bottom, left, _stroke, _thickness);
}

/// A region's bounding box in image pixels: what the correction window's drag
/// handles and its readout work in.
campcat::llm::box region_bounds_ui(const campcat::llm::region &_region) {
    if (const auto *b = std::get_if<campcat::llm::box>(&_region)) {
        return *b;
    }
    if (const auto *c = std::get_if<campcat::llm::circle>(&_region)) {
        return campcat::llm::box{c->cx - c->r, c->cy - c->r, 2 * c->r + 1,
                                 2 * c->r + 1};
    }
    const auto &d = std::get<campcat::llm::diamond>(_region);
    return campcat::llm::box{d.cx - d.rx, d.cy - d.ry, 2 * d.rx + 1,
                             2 * d.ry + 1};
}

/// The region-annotation window, opened by double-clicking the preview. What it
/// holds is the user's redraw, kept apart from the model's so Cancel costs
/// nothing.
bool g_annot_open = false;
long g_annot_id = -1; ///< which question this edit belongs to (`_a.id`)
bool g_annot_dragging = false;
bool g_annot_has_region = false;
/// Which shape the drag draws. Seeded from the region under correction, and
/// switchable: a box the model got wrong may be a circle or a diamond.
enum class edit_shape { box, circle, diamond };
edit_shape g_annot_shape = edit_shape::box;
int g_annot_ax = 0; ///< drag anchor, image px
int g_annot_ay = 0;
int g_annot_cx = 0; ///< drag current point, image px
int g_annot_cy = 0;
int g_annot_x0 = 0; ///< committed box (x,y,w,h layout) or circle centre
int g_annot_y0 = 0;
int g_annot_x1 = 0; ///< box far corner, inclusive image px
int g_annot_y1 = 0;
int g_annot_r = 0;  ///< committed circle radius
int g_annot_rx = 0; ///< committed diamond half-diagonals
int g_annot_ry = 0;

/// A redrawn region waiting to go with the reply. The send bar shows it, and
/// Send and Reject carry it; it belongs to exactly one question.
std::optional<campcat::llm::region> g_staged_region;
long g_staged_id = -1;

/// Drop the staged redraw, because the question it belonged to is gone.
void clear_staged() {
    g_staged_region.reset();
    g_staged_id = -1;
}

/// What the annotation drag currently stands for: the live drag while the mouse
/// is down, the last committed one otherwise.
campcat::llm::region annot_draft() {
    if (g_annot_shape == edit_shape::circle) {
        const int ax = g_annot_dragging ? g_annot_ax : g_annot_x0;
        const int ay = g_annot_dragging ? g_annot_ay : g_annot_y0;
        const int r = g_annot_dragging
                          ? static_cast<int>(std::lround(std::hypot(
                                static_cast<double>(g_annot_cx - ax),
                                static_cast<double>(g_annot_cy - ay))))
                          : g_annot_r;
        return campcat::llm::circle{ax, ay, r};
    }
    if (g_annot_shape == edit_shape::diamond) {
        const int ax = g_annot_dragging ? g_annot_ax : g_annot_x0;
        const int ay = g_annot_dragging ? g_annot_ay : g_annot_y0;
        int rx = g_annot_rx;
        int ry = g_annot_ry;
        if (g_annot_dragging) {
            // Both directions, or a centre near an edge would write a diamond
            // whose vertices sit off the frame the tool then refuses.
            const int room_x = std::min(ax, std::max(0, g_appr_w - 1 - ax));
            const int room_y = std::min(ay, std::max(0, g_appr_h - 1 - ay));
            rx = std::min(std::abs(g_annot_cx - ax), room_x);
            ry = std::min(std::abs(g_annot_cy - ay), room_y);
        }
        return campcat::llm::diamond{ax, ay, rx, ry};
    }
    const int x0 = g_annot_dragging ? g_annot_ax : g_annot_x0;
    const int y0 = g_annot_dragging ? g_annot_ay : g_annot_y0;
    const int x1 = g_annot_dragging ? g_annot_cx : g_annot_x1;
    const int y1 = g_annot_dragging ? g_annot_cy : g_annot_y1;
    return campcat::llm::box{std::min(x0, x1), std::min(y0, y1),
                             std::abs(x1 - x0) + 1, std::abs(y1 - y0) + 1};
}

/// Open the annotation window for `_a`. The region is seeded from a staged
/// redraw when there is one (reopening must not lose the last edit), else from
/// the model's.
void open_annotation(const campcat::llm_approval &_a) {
    g_annot_open = true;
    g_annot_id = _a.id;
    g_annot_dragging = false;
    std::optional<campcat::llm::region> seed;
    if (g_staged_region && g_staged_id == _a.id) {
        seed = g_staged_region;
    } else if (!_a.highlight.empty()) {
        seed = _a.highlight[0];
    }
    g_annot_has_region = seed.has_value();
    if (seed && std::holds_alternative<campcat::llm::circle>(*seed)) {
        g_annot_shape = edit_shape::circle;
    } else if (seed && std::holds_alternative<campcat::llm::diamond>(*seed)) {
        g_annot_shape = edit_shape::diamond;
    } else {
        g_annot_shape = edit_shape::box;
    }
    if (seed) {
        if (const auto *c = std::get_if<campcat::llm::circle>(&*seed)) {
            g_annot_x0 = c->cx;
            g_annot_y0 = c->cy;
            g_annot_r = c->r;
        } else if (const auto *d = std::get_if<campcat::llm::diamond>(&*seed)) {
            g_annot_x0 = d->cx;
            g_annot_y0 = d->cy;
            g_annot_rx = d->rx;
            g_annot_ry = d->ry;
        } else {
            const campcat::llm::box b = region_bounds_ui(*seed);
            g_annot_x0 = b.x;
            g_annot_y0 = b.y;
            g_annot_x1 = b.x + b.w - 1;
            g_annot_y1 = b.y + b.h - 1;
        }
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
    if (ImGui::Begin("Correct the region", &open)) {
        ImGui::TextDisabled(
            a.given_region
                ? "Drag a new region. Thin: the model's. Solid: after snapping."
                : "Drag a new region on the image. The thin one is the "
                  "model's.");
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
                // Read the live drag before clearing the flag: annot_draft()
                // falls back to the committed region once dragging is false.
                const campcat::llm::region draft = annot_draft();
                g_annot_dragging = false;
                // A drag that never moved is a stray click, not a zero-size box
                // or a zero-radius circle.
                if (g_annot_shape == edit_shape::circle) {
                    const auto &c = std::get<campcat::llm::circle>(draft);
                    if (c.r > 0) {
                        g_annot_x0 = c.cx;
                        g_annot_y0 = c.cy;
                        g_annot_r = c.r;
                        g_annot_has_region = true;
                    }
                } else if (g_annot_shape == edit_shape::diamond) {
                    const auto &d = std::get<campcat::llm::diamond>(draft);
                    if (d.rx > 0 && d.ry > 0) {
                        g_annot_x0 = d.cx;
                        g_annot_y0 = d.cy;
                        g_annot_rx = d.rx;
                        g_annot_ry = d.ry;
                        g_annot_has_region = true;
                    }
                } else if (g_annot_cx != g_annot_ax ||
                           g_annot_cy != g_annot_ay) {
                    g_annot_x0 = std::min(g_annot_ax, g_annot_cx);
                    g_annot_y0 = std::min(g_annot_ay, g_annot_cy);
                    g_annot_x1 = std::max(g_annot_ax, g_annot_cx);
                    g_annot_y1 = std::max(g_annot_ay, g_annot_cy);
                    g_annot_has_region = true;
                }
            }
        }
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // What the user is correcting, plus the box the model gave when the
        // snap moved it: without the model's own box on screen there is nothing
        // to judge the snap against. The model's is a different colour and a
        // thinner line, the same encoding the approval bubble uses, so "which
        // is which" does not depend on telling two offsets apart.
        for (const campcat::llm::region &r : a.highlight) {
            draw_region(dl, mn, 1.0F, r,
                        a.given_region ? IM_COL32(255, 200, 60, 230)
                                       : IM_COL32(255, 200, 60, 130),
                        a.given_region ? IM_COL32(255, 200, 60, 230)
                                       : IM_COL32(255, 200, 60, 130),
                        a.given_region ? 2.0F : 1.0F);
        }
        if (a.given_region) {
            draw_region(dl, mn, 1.0F, *a.given_region, 0,
                        IM_COL32(200, 120, 255, 220), 1.0F);
        }
        if (g_annot_dragging || g_annot_has_region) {
            draw_region(dl, mn, 1.0F, annot_draft(), IM_COL32(80, 200, 255, 55),
                        IM_COL32(255, 220, 80, 255), 2.0F);
        }
        ImGui::EndChild();
        if (g_annot_has_region) {
            if (g_annot_shape == edit_shape::circle) {
                ImGui::TextDisabled("circle %d,%d r=%d px", g_annot_x0,
                                    g_annot_y0, g_annot_r);
            } else if (g_annot_shape == edit_shape::diamond) {
                ImGui::TextDisabled("diamond %d,%d rx=%d ry=%d px", g_annot_x0,
                                    g_annot_y0, g_annot_rx, g_annot_ry);
            } else {
                ImGui::TextDisabled("box %d,%d %dx%d px", g_annot_x0,
                                    g_annot_y0, g_annot_x1 - g_annot_x0 + 1,
                                    g_annot_y1 - g_annot_y0 + 1);
            }
        } else {
            ImGui::TextDisabled(
                g_annot_shape == edit_shape::circle
                    ? "drag from the centre to set the radius"
                : g_annot_shape == edit_shape::diamond
                    ? "drag from the centre; horizontal sets rx, vertical sets ry"
                    : "drag on the image to pick a box");
        }
        // Switching shape drops the committed region: a box must not ride along
        // as a circle (or the other way round) just because it was drawn first.
        // Re-clicking the active radio is not a switch -- RadioButton reports
        // the click either way, and dropping the region there would throw away
        // what was just drawn.
        ImGui::SameLine();
        if (ImGui::RadioButton("box", g_annot_shape == edit_shape::box) &&
            g_annot_shape != edit_shape::box) {
            g_annot_shape = edit_shape::box;
            g_annot_has_region = false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("circle", g_annot_shape == edit_shape::circle) &&
            g_annot_shape != edit_shape::circle) {
            g_annot_shape = edit_shape::circle;
            g_annot_has_region = false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("diamond",
                               g_annot_shape == edit_shape::diamond) &&
            g_annot_shape != edit_shape::diamond) {
            g_annot_shape = edit_shape::diamond;
            g_annot_has_region = false;
        }
        if (!g_annot_has_region) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Use this region")) {
            g_staged_region = annot_draft();
            g_staged_id = g_annot_id;
            g_annot_open = false;
        }
        if (!g_annot_has_region) {
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
    draw_region(ImGui::GetWindowDrawList(), img_min, s, *g_staged_region,
                IM_COL32(80, 200, 255, 55), IM_COL32(255, 220, 80, 255), 2.0F);
    ImGui::SameLine();
    ImGui::TextDisabled("Corrected %s region will be sent with your reply.",
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
        const bool dbl = _a.editable && ImGui::IsItemHovered() &&
                         ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        // The boxes are in screenshot pixels; the image is drawn scaled.
        const ImVec2 origin = ImGui::GetItemRectMin();
        const float scale = size.x / static_cast<float>(g_appr_w);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (const campcat::llm::region &r : _a.highlight) {
            draw_region(dl, origin, scale, r, IM_COL32(255, 200, 60, 48),
                        IM_COL32(255, 200, 60, 230), 2.0F);
        }
        // The region the model gave, when the snap moved it. A different
        // colour, not just a thinner line: the preview is drawn at ~0.16 scale
        // on a phone resolution, so a 20px snap is a 3px offset and the two
        // frames would read as one if they were both yellow.
        if (_a.given_region) {
            draw_region(dl, origin, scale, *_a.given_region, 0,
                        IM_COL32(200, 120, 255, 220), 1.0F);
        }
        if (dbl) {
            open_annotation(_a);
        }
        if (_a.given_region) {
            ImGui::TextDisabled("Thin frame: the region the model gave. Solid: "
                                "after snapping.");
        }
    }
    if (_a.editable) {
        ImGui::TextDisabled("Double-click the image to correct the region.");
    }
    if (ImGui::Button("Approve")) {
        g_host.answer_approval(true);
        clear_staged(); // an approval means the region is right; a redraw is
                        // moot
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
        g_host.answer_approval(false, {}, g_staged_region);
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
            campcat::llm_result res;
            memorize_both(g_config_home, g_provider,
                          campcat::k_shutdown_memorize_timeout_ms, &res);
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
    // The worker cleared the stream for a message that was only queued. It is
    // the turn in flight now, so the pump below writes into it and not into
    // the one that just finished.
    if (g_promote.exchange(false)) {
        if (llm_turn *queued = queued_turn()) {
            queued->queued = false;
        }
    }

    // An automation cycle or the scheduler is using adb, so tools must refuse
    // rather than contend with it.
    g_adb_busy.store(_adb_busy);

    // Kept for the shutdown pass and the memory window, neither of which is
    // handed the config: this panel is the only place that sees it.
    g_config_home = _cfg.config_home;
    g_provider = _cfg.llm.provider;

    const bool busy = g_running.load();

    pump_stream();
    // Saved skills in a fixed column on the left, conversation on the right,
    // the prompt box below both. Side by side because the list is an index, not
    // a second conversation: stacked, it took height off every reply.
    script_editor_panel_draw(std::filesystem::path(_cfg.config_home),
                             k_script_list_w, -g_footer_h);
    ImGui::SameLine();
    // Measured, not another negative width: a negative size is resolved against
    // the window's content region, not against what is left past the cursor, so
    // "-list_w" here would leave the list's width blank at the right edge too.
    const bool clear_requested =
        draw_log(ImVec2(ImGui::GetContentRegionAvail().x, -g_footer_h));
    if (clear_requested) {
        start_reset(_cfg.config_home, _cfg.llm.provider);
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
    if (g_staged_region && (!awaiting || g_staged_id != approval.id)) {
        clear_staged();
    }
    if (awaiting && g_staged_region && g_appr_tex != 0 && g_appr_w > 0) {
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
        start_memorize(_st, _cfg);
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
    // Attached here rather than inside prompt_input_text: that helper paints
    // the IME pre-edit without submitting an item, so the box is still the last
    // item at this point. The target would silently leave the box if the helper
    // ever submitted one.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload(k_script_drag_payload)) {
            // `@name` glued to the text before it would not read as a mention,
            // so a separator goes in unless one is already there.
            if (!g_input.empty() &&
                !std::isspace(static_cast<unsigned char>(g_input.back()))) {
                g_input.push_back(' ');
            }
            g_input += '@';
            g_input.append(static_cast<const char *>(payload->Data),
                           static_cast<size_t>(payload->DataSize));
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine();
    // A turn in flight still accepts a message: that message is how the user
    // stops it. Clear conversation does not, because the reset drops the log.
    const bool can_send =
        awaiting || (!g_resetting.load() && !is_blank(g_input));
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

    // Four counts. Dispatches is the only one with a fuse, and that fuse is
    // per turn while the number is the conversation total.
    refresh_memory_chars();
    const campcat::llm_protocol::usage_stats usage = g_host.usage();
    ImGui::Indent(k_stat_indent);
    WITH_FONT(small, {
        // Zero vertical cell padding: a row's height is its content plus
        // `CellPadding.y * 2` (imgui_tables.cpp:1886), and this line is
        // context, so the padding is the only slack worth removing. The
        // horizontal half stays, so the columns keep their inset.
        const float pad_x = ImGui::GetStyle().CellPadding.x;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(pad_x, 0.0F));
        // Opened after the table: a popup id pushed inside the table does not
        // match BeginPopup outside it.
        bool open_fuse = false;
        const auto cell = [&](const char *_label, const std::string &_value) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", _label);
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                open_fuse = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(_value.c_str());
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                open_fuse = true;
            }
        };
        if (ImGui::BeginTable("##llm_stats", 4,
                              ImGuiTableFlags_SizingStretchSame)) {
            cell("Turns", std::to_string(g_sidecar_turns));
            cell("LLM calls", std::to_string(usage.llm_calls));
            cell("Dispatches",
                 std::to_string(usage.tool_calls) + "/" +
                     campcat::llm::format_fuse(_cfg.llm.max_dispatches));
            cell("Context", usage.context_tokens < 0
                               ? std::string("\xE2\x80\x94")
                               : std::to_string(usage.context_tokens));
            ImGui::EndTable();
        }
        if (open_fuse) {
            ImGui::OpenPopup("llm_fuse_settings");
        }
        draw_fuse_settings(_cfg);
        ImGui::PopStyleVar();
    });
    ImGui::Unindent(k_stat_indent);

    if ((clicked || submitted) && can_send) {
        if (awaiting) {
            const bool blank = is_blank(g_input);
            g_host.answer_approval(false, blank ? std::string() : g_input,
                                   g_staged_region);
            clear_staged();
            g_input.clear(); // consumed as the answer, not as a prompt
        } else if (busy) {
            const std::string prompt = g_input;
            g_input.clear();
            queue_prompt(prompt);
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
