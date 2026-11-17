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
#include "app/llm_controller.h"

#include "app/ui/activity_status.h"
#include "app/ui/buttons.h"
#include "app/ui/chat_text.h"
#include "app/ui/gl_texture.h"
#include "app/ui/icons.h"
#include "app/ui/prompt_input.h"
#include "app/ui/script_editor_panel.h"
#include "app/ui/theme.h"
#include "app/ui/widgets.h"

#include <imgui.h>
#include <imgui_internal.h> // ImTextCountCharsFromUtf8
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
#include <cctype>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// The four UI faces come from one place (`app/ui/fonts.cpp`); the aliases keep
/// the call sites below to one line.
using campcat::app::font_role;
using campcat::app::fonts_get;

/// Oldest turns are dropped past this.
constexpr int k_turn_cap = 30;

/// One step of a turn's timeline, mirrored from the host while the turn runs.
struct llm_item {
    campcat::llm_event::kind type = campcat::llm_event::kind::text;
    std::string text;  ///< text/thinking: the words so far; tool: the note
    bool ok = true;    ///< tool
    long image_id = 0; ///< image: host key, and this item's identity
    /// thinking, tool: the reader opened the folded step to read it again.
    bool expanded = false;
    /// text: `text` split for drawing, redone when the size moves -- text
    /// only ever grows, so its size is enough to tell it changed.
    std::vector<campcat::app::chat_block> blocks;
    size_t blocks_for = std::string::npos;
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
    bool steps_open = false; ///< the reader unfolded the finished turn's steps
};

// UI-thread state: never touched by the worker.
std::vector<llm_turn> g_turns;
std::string g_input;
bool g_pin_bottom = true;
/// Where the conversation sat at the end of the last frame. Its own snap lands
/// after this is read, so a smaller value now means the user moved up.
float g_last_scroll = 0.0F;
bool g_thinking = false; ///< sidecar reports the model is reasoning

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
    if (_info.type == ImGui::MarkdownFormatType::LINK) {
        // Code spans arrive here as links, and no link opens anything, so the
        // default underline would promise a click that never happens.
        if (_start) {
            ImGui::PushStyleColor(ImGuiCol_Text, campcat::app::theme::k_accent_fg);
        } else {
            ImGui::PopStyleColor();
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

campcat::llm_host &g_host = campcat::consult_host();
campcat::llm_controller g_controller(g_host);

/// UI-thread only. Time of the last LLM request, which is what "idle" is
/// measured from: an agent nobody is talking to is an agent that can tidy up.
double g_last_llm = 0.0;
bool g_memory_pending = false;
/// How long the conversation must have been quiet before consolidating.
constexpr double k_idle_seconds = 600.0; // 10 minutes
/// Remembered every frame so the shutdown pass knows where the memory file is,
/// since `llm_panel_shutdown_gl` has no config of its own.
std::filesystem::path g_config_home;
/// Same reason as `g_config_home`: the shutdown pass needs the selected
/// provider, and only the draw call sees the config.
std::string g_provider = "deepseek";
/// And the selected graph, whose ledger holds that graph's own names.
std::string g_memory_graph;

/// The page the memory window is showing.
campcat::llm::memory_kind g_mem_kind =
    campcat::llm::memory_kind::operator_notes;
/// The Long-term memory window, and the notebook it shows.
bool g_mem_open = false;
std::vector<campcat::llm::memory_entry> g_mem_entries;
/// Ledger read failure, shown instead of a misleading empty list.
bool g_mem_unreadable = false;
/// Entries the budget keeps out of the prompt, from the last render.
int g_mem_dropped = 0;
/// The entry being edited in place, and the text under the cursor. Empty id
/// means no row is open for editing.
std::string g_mem_edit_id;
std::string g_mem_edit_buf;
/// Characters in the page being shown, for the window's own budget line.
int g_mem_page_chars = 0;

/// The sidecar's own count of the turns it still holds verbatim, taken from
/// each successful reply. Deliberately not `g_turns.size()`: that counts
/// bubbles and stops at `k_turn_cap`, while the sidecar compacts once the
/// conversation reaches its token trigger line.
int g_sidecar_turns = 0;

/// (Re)read the shown notebook's ledger into the window's list.
void reload_memory() {
    g_mem_entries.clear();
    g_mem_edit_id.clear();
    g_mem_unreadable =
        !campcat::llm::read_ledger(g_config_home, g_mem_kind, g_memory_graph,
                                   &g_mem_entries);
    if (g_mem_unreadable) {
        g_mem_page_chars = 0;
        g_mem_dropped = 0;
        return;
    }
    const std::string rendered = campcat::llm::render_memory(
        g_mem_entries, campcat::llm::k_memory_budget_chars, &g_mem_dropped);
    g_mem_page_chars = ImTextCountCharsFromUtf8(
        rendered.c_str(), rendered.c_str() + rendered.size());
}

/// Apply one user edit and reload. The same entry point every row uses, so
/// there is one place where "the user changed this" is decided.
void edit_memory(const campcat::llm::memory_op &_op) {
    std::vector<std::string> notes;
    campcat::llm::apply_ops(g_config_home, g_mem_kind, g_memory_graph, {_op}, {},
                            /*_by_user=*/true, &notes);
    for (const std::string &n : notes) {
        campcat::automation_log::emit("[llm] memory op refused: " + n);
    }
    reload_memory();
}

/// One notebook as a list of facts. Each row carries the three verbs the user
/// has: edit the text, retire it from the prompt, delete it for good.
void draw_memory_page(campcat::llm::memory_kind _kind) {
    if (g_mem_kind != _kind) {
        g_mem_kind = _kind;
        reload_memory();
    }
    const bool scripter = _kind == campcat::llm::memory_kind::scripter_notes;
    const char *popup =
        scripter ? "clear_scripter_memory" : "clear_operator_memory";
    const char *ask = scripter ? "Delete the scripter long-term memory ledger?"
                               : "Delete the operator long-term memory ledger?";

    const auto library_ledger = campcat::llm::ledger_path(g_config_home, _kind, "");
    ImGui::Text("Library: %s", library_ledger.string().c_str());
    if (!g_memory_graph.empty()) {
        const auto graph_ledger =
            campcat::llm::ledger_path(g_config_home, _kind, g_memory_graph);
        ImGui::Text("Graph (%s): %s", g_memory_graph.c_str(),
                    graph_ledger.string().c_str());
    }
    ImGui::TextDisabled("Written when the conversation goes idle, and before "
                        "it is cleared. A pass may not touch a fact you edited "
                        "or pinned.");
    if (g_mem_unreadable) {
        ImGui::TextColored(campcat::app::theme::k_danger,
                           "The ledger could not be read.");
    }
    int live = 0;
    for (const auto &e : g_mem_entries) {
        live += e.visible ? 1 : 0;
    }
    ImGui::Text("%d active / %d over the %d-character prompt budget", live,
                g_mem_dropped,
                static_cast<int>(campcat::llm::k_memory_budget_chars));
    ImGui::TextDisabled("%d of %d characters reach the prompt", g_mem_page_chars,
                        static_cast<int>(campcat::llm::k_memory_budget_chars));
    if (campcat::app::button("Reload")) {
        reload_memory();
    }
    ImGui::SameLine();
    if (campcat::app::button("Clear memory", ImVec2(), campcat::app::button_kind::danger)) {
        ImGui::OpenPopup(popup);
    }
    if (ImGui::BeginPopupModal(popup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(ask);
        if (campcat::app::button("Delete", ImVec2(120.0F, 0.0F),
                                 campcat::app::button_kind::danger)) {
            if (!campcat::llm::clear_memory(g_config_home, _kind, g_memory_graph)) {
                campcat::automation_log::emit("[llm] memory ledger not deleted");
            }
            reload_memory();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (campcat::app::button("Cancel", ImVec2(120.0F, 0.0F))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    if (ImGui::BeginChild("##memory_rows", ImVec2(0.0F, 0.0F))) {
        for (const auto &e : g_mem_entries) {
            ImGui::PushID(e.id.c_str());
            bool pinned = e.pinned;
            if (ImGui::Checkbox("##pin", &pinned)) {
                campcat::llm::memory_op op;
                op.k = pinned ? campcat::llm::memory_op::kind::pin
                              : campcat::llm::memory_op::kind::unpin;
                op.id = e.id;
                edit_memory(op);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Pinned: kept when the prompt budget trims, "
                                  "and a pass may not change it.");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", e.section.c_str());
            ImGui::SameLine();
            if (g_mem_edit_id == e.id) {
                ImGui::SetNextItemWidth(-120.0F);
                char buf[260] = {};
                std::snprintf(buf, sizeof buf, "%s", g_mem_edit_buf.c_str());
                const bool done = ImGui::InputText(
                    "##edit", buf, sizeof buf,
                    ImGuiInputTextFlags_EnterReturnsTrue);
                g_mem_edit_buf = buf;
                if (done) {
                    campcat::llm::memory_op op;
                    op.k = campcat::llm::memory_op::kind::edit;
                    op.id = e.id;
                    op.text = g_mem_edit_buf;
                    g_mem_edit_id.clear();
                    edit_memory(op);
                }
                ImGui::SameLine();
                if (campcat::app::small_button("Cancel")) {
                    g_mem_edit_id.clear();
                }
                ImGui::PopID();
                continue;
            }
            if (e.visible) {
                ImGui::TextUnformatted(e.text.c_str());
            } else {
                ImGui::TextDisabled("%s (retired)", e.text.c_str());
            }
            ImGui::SameLine();
            if (campcat::app::small_button("Edit")) {
                g_mem_edit_id = e.id;
                g_mem_edit_buf = e.text;
            }
            ImGui::SameLine();
            if (campcat::app::small_button(e.visible ? "Retire" : "Restore")) {
                campcat::llm::memory_op op;
                op.k = e.visible ? campcat::llm::memory_op::kind::retire
                                 : campcat::llm::memory_op::kind::restore;
                op.id = e.id;
                edit_memory(op);
            }
            ImGui::SameLine();
            if (campcat::app::small_button("Delete")) {
                campcat::llm::memory_op op;
                op.k = campcat::llm::memory_op::kind::remove;
                op.id = e.id;
                edit_memory(op);
            }
            if (!e.src.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", e.src.front().c_str());
            }
            ImGui::PopID();
        }
        if (g_mem_entries.empty() && !g_mem_unreadable) {
            ImGui::TextDisabled("(no memory yet)");
        }
    }
    // Unconditional: BeginChild returns false when the list is clipped, and
    // EndChild is still required.
    ImGui::EndChild();
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

/// Name for the subagent card's heading. Unknown names are shown as sent.
const char *agent_label(const std::string &_agent) {
    if (_agent == "scripter") {
        return "Scripter";
    }
    if (_agent == "diagnoser") {
        return "Diagnoser";
    }
    if (_agent == "region") {
        return "Region check";
    }
    return _agent.c_str();
}

/// Defined with the bubble's rendering below: the card shows the same four
/// kinds of step the conversation does.
void draw_item(llm_item &_it, bool _growing, bool _subagent = false);
bool grows_in_place(campcat::llm_event::kind _kind);

/// How many pictures in the subagent card keep a texture.
constexpr size_t k_sub_tex_cap = 4;

/// The card owns its own textures; these items are not in a chat turn.
std::vector<llm_item> g_sub_items;
std::deque<long> g_sub_tex;
unsigned long g_sub_run = 0;
std::optional<unsigned long> g_sub_cleared_run;

void release_sub_items() {
    for (llm_item &it : g_sub_items) {
        if (it.image_id != 0) {
            campcat::app::delete_tex(&it.tex, &it.tex_w, &it.tex_h);
        }
    }
    g_sub_items.clear();
    g_sub_tex.clear();
}

/// Same shape as `sync_items`, and for the same reason: a growing step is
/// updated in place, a new one appended, an image fetched once.
void sync_sub_items(const campcat::llm_host::subagent_state &_st) {
    if (_st.runs != g_sub_run) {
        release_sub_items();
        g_sub_run = _st.runs;
    }
    for (size_t i = 0; i < _st.lines.size(); ++i) {
        const campcat::llm_event &e = _st.lines[i];
        if (i < g_sub_items.size()) {
            if (grows_in_place(e.type) && g_sub_items[i].text != e.text) {
                g_sub_items[i].text = e.text;
            }
            continue;
        }
        llm_item it;
        it.type = e.type;
        it.text = e.text;
        it.ok = e.ok;
        it.image_id = e.image_id;
        if (e.type == campcat::llm_event::kind::image) {
            it.png = g_host.subagent_image_png(e.image_id);
            it.tex_dropped = it.png.empty();
        }
        g_sub_items.push_back(std::move(it));
    }
    // An older image keeps its row and reports that the screenshot was dropped.
    for (llm_item &it : g_sub_items) {
        if (it.image_id != 0 && it.tex != 0 &&
            std::find(g_sub_tex.begin(), g_sub_tex.end(), it.image_id) ==
                g_sub_tex.end()) {
            g_sub_tex.push_back(it.image_id);
        }
    }
    while (g_sub_tex.size() > k_sub_tex_cap) {
        const long id = g_sub_tex.front();
        g_sub_tex.pop_front();
        for (llm_item &it : g_sub_items) {
            if (it.image_id == id) {
                campcat::app::delete_tex(&it.tex, &it.tex_w, &it.tex_h);
                it.tex_dropped = true;
            }
        }
    }
}

/// Right-click on the block just drawn: a menu that copies `_text` out.
///
/// `_top` is where the block started, taken before drawing it: a bubble is
/// markdown, an image or a folded step, so the last item is only ever its final
/// widget and cannot say where the block began.
void copy_menu(const char *_id, std::string_view _text, ImVec2 _top) {
    // The window check keeps a covered bubble quiet: the rect is in screen space.
    if (!_text.empty() && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(
            _top, ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                         ImGui::GetItemRectMax().y)) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(_id);
    }
    if (!ImGui::BeginPopup(_id)) {
        return;
    }
    if (ImGui::MenuItem("Copy")) {
        ImGui::SetClipboardText(std::string(_text).c_str());
    }
    ImGui::EndPopup();
}

/// A reply bubble as plain text: every step in the order it happened, and the
/// error when the turn failed.
std::string reply_text(const llm_turn &_t) {
    std::string out;
    for (const llm_item &it : _t.items) {
        if (it.text.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += "\n\n";
        }
        out += it.text;
    }
    if (!_t.error.empty()) {
        if (!out.empty()) {
            out += "\n\n";
        }
        out += _t.error;
    }
    return out;
}

/// The latest subagent run at the end of the chat, with the same scroll as
/// replies, in the message column that starts at `_x` and is `_col` wide.
void draw_subagent_card(float _x, float _col) {
    const campcat::llm_host::subagent_state st = g_host.subagent();
    if (!st.seen || g_sub_cleared_run == st.runs) {
        return;
    }
    const bool new_run = st.runs != g_sub_run;
    sync_sub_items(st);
    ImGui::Spacing();
    // The card keeps its open flag in this window's storage under its id; a
    // new run opens it again even if the reader folded the last one.
    if (new_run) {
        ImGui::GetStateStorage()->SetBool(ImGui::GetID("subagent_card"), true);
    }
    const bool failed = !st.active && !st.ok;
    const campcat::activity_status status =
        st.active ? campcat::activity_status{campcat::activity_level::busy, "Working"}
        : failed  ? campcat::activity_status{campcat::activity_level::failed, "Failed"}
                  : campcat::activity_status{campcat::activity_level::ok, "Done"};
    const std::string title = std::string("Sub-agent \xC2\xB7 ") + agent_label(st.agent);
    ImGui::SetCursorPosX(_x);
    if (campcat::app::card_begin("subagent_card", title.c_str(),
                                 {.glyph = campcat::app::icon::k_bot,
                                  .width = _col,
                                  .status = &status})) {
        if (!st.goal.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, campcat::app::theme::k_text_2);
            ImGui::TextWrapped("%s", st.goal.c_str());
            ImGui::PopStyleColor();
        }
        for (size_t i = 0; i < g_sub_items.size(); ++i) {
            const bool growing = st.active && i + 1 == g_sub_items.size() &&
                                 grows_in_place(g_sub_items[i].type);
            const ImVec2 top = ImGui::GetCursorScreenPos();
            ImGui::PushID(static_cast<int>(i));
            draw_item(g_sub_items[i], growing, true);
            copy_menu("copy", g_sub_items[i].text, top);
            ImGui::PopID();
        }
    }
    campcat::app::card_end();
}

/// A user bubble takes at most this share of the message column before it wraps.
constexpr float k_bubble_frac = 0.72F;

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
void ensure_texture(llm_item &_it, bool _subagent) {
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
    if (!_subagent) {
        g_tex_order.push_back(_it.image_id);
        evict_over_cap();
    }
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

/// Steps whose text grows while they stream; the rest are complete on arrival.
bool grows_in_place(campcat::llm_event::kind _kind) {
    return _kind == campcat::llm_event::kind::text ||
           _kind == campcat::llm_event::kind::thinking;
}

/// Mirror the host's timeline into the waiting turn: a text step grows in
/// place, a new step appends, and an image's bytes are taken the moment it
/// appears -- the host drops them when the next request starts.
void sync_items(llm_turn &_t) {
    const std::vector<campcat::llm_event> events = g_host.events();
    for (size_t i = 0; i < events.size(); ++i) {
        const campcat::llm_event &e = events[i];
        if (i < _t.items.size()) {
            if (grows_in_place(e.type) && _t.items[i].text != e.text) {
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

/// Fit `_w x _h` into `_box`, preserving aspect; never enlarged, and never
/// below a pixel, which `InvisibleButton` would assert on.
ImVec2 fit_image(int _w, int _h, ImVec2 _box) {
    const float w = static_cast<float>(_w);
    const float h = static_cast<float>(_h);
    const float scale = std::min({1.0F, _box.x / w, _box.y / h});
    return ImVec2(std::max(1.0F, w * scale), std::max(1.0F, h * scale));
}

/// A screenshot in the timeline is at most this tall; a click opens it whole.
constexpr float k_thumb_h = 160.0F;

/// Reply line gap. imgui_markdown lays out every wrapped line as its own item,
/// so the item spacing is what sits between two lines of one paragraph.
constexpr float k_line_gap = 5.0F;
/// Extra space between paragraphs, on top of the line gap either side.
constexpr float k_para_gap = 6.0F;

/// A reasoning step: open while it is the live one, one clickable line after.
/// Plain wrapped text rather than Markdown -- it can run to thousands of
/// characters, and re-parsing it every frame while it grows is the cost this
/// avoids.
void draw_thinking(llm_item &_it, bool _streaming) {
    namespace icon = campcat::app::icon;
    ImGui::PushStyleColor(ImGuiCol_Text, campcat::app::theme::k_text_3);
    if (_streaming) {
        ImGui::TextUnformatted("Thinking");
        ImGui::TextWrapped("%s", _it.text.c_str());
    } else {
        // Several folded steps carry the same label; the item is what makes
        // each one a different widget.
        ImGui::PushID(&_it);
        const std::string label =
            std::string(_it.expanded ? icon::k_chevron_down : icon::k_chevron_right) +
            " Thinking";
        if (ImGui::Selectable(label.c_str())) {
            _it.expanded = !_it.expanded;
        }
        ImGui::PopID();
        if (_it.expanded) {
            ImGui::TextWrapped("%s", _it.text.c_str());
        }
    }
    ImGui::PopStyleColor();
}

/// Verbatim text in the mono face on an inset box the width of the column.
/// Drawn on the draw list, so a box needs no window or id of its own.
void draw_code(std::string_view _code) {
    using namespace campcat::app;
    constexpr ImVec2 k_pad(10.0F, 8.0F);
    ImGui::PushFont(fonts_get(font_role::mono));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = std::max(1.0F, ImGui::GetContentRegionAvail().x);
    const float wrap = std::max(1.0F, w - 2.0F * k_pad.x);
    const char *end = _code.data() + _code.size();
    const ImVec2 text = ImGui::CalcTextSize(_code.data(), end, false, wrap);
    const ImVec2 box(w, text.y + 2.0F * k_pad.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + box.x, at.y + box.y),
                      ImGui::GetColorU32(theme::k_surface_inset), theme::k_radius_popup);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(at.x + k_pad.x, at.y + k_pad.y), ImGui::GetColorU32(theme::k_text_2),
                _code.data(), end, wrap);
    ImGui::Dummy(box);
    ImGui::PopFont();
}

/// Prose through the Markdown renderer with the tighter line gap, fenced code
/// in a box, and one fixed gap between paragraphs however many blank lines
/// the model wrote.
void draw_blocks(llm_item &_it) {
    if (_it.blocks_for != _it.text.size()) {
        _it.blocks = campcat::app::chat_blocks(_it.text);
        _it.blocks_for = _it.text.size();
    }
    if (_it.blocks.empty()) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, k_line_gap));
    for (size_t i = 0; i < _it.blocks.size(); ++i) {
        if (i > 0) {
            ImGui::Dummy(ImVec2(0.0F, k_para_gap));
        }
        const campcat::app::chat_block &b = _it.blocks[i];
        if (b.code) {
            draw_code(b.text);
        } else {
            ImGui::Markdown(b.text.c_str(), b.text.size(), md_config());
        }
    }
    ImGui::PopStyleVar();
    // The last line kept the tight gap; the next step gets the usual one.
    ImGui::Spacing();
}

/// A tool step went wrong. Host notes say so in their status word and leave
/// `ok` at its default; a subagent's events carry `ok` and no such word.
bool tool_failed(const llm_item &_it) {
    const std::optional<campcat::app::tool_note_parts> note =
        campcat::app::split_tool_note(_it.text);
    return !_it.ok || (note && note->status == "error");
}

/// A tool call on one line -- outcome, name, the first line of what came back
/// -- and the rest on a click. Host notes are `**name** ok|error` plus a body;
/// a subagent's are `**name** {args}`, whose args are then the detail.
void draw_tool(llm_item &_it) {
    using namespace campcat::app;
    const std::optional<tool_note_parts> note = split_tool_note(_it.text);
    if (!note) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::k_text_2);
        draw_blocks(_it);
        ImGui::PopStyleColor();
        return;
    }
    const bool failed = tool_failed(_it);
    const bool known = note->status == "ok" || note->status == "error";
    const std::string_view detail = known ? note->body : note->status;
    const std::string_view summary = first_line(detail);
    const char *glyph = failed ? icon::k_failed : known ? icon::k_ok : icon::k_tool;
    const ImVec4 glyph_col = failed ? theme::k_danger : known ? theme::k_success : theme::k_text_3;

    ImGui::PushID(&_it);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeight();
    if (detail.empty()) {
        ImGui::Dummy(ImVec2(w, h));
    } else if (ImGui::Selectable("##tool", false, ImGuiSelectableFlags_None, ImVec2(w, h))) {
        _it.expanded = !_it.expanded;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    float x = at.x;
    dl->AddText(ImVec2(x, at.y), ImGui::GetColorU32(glyph_col), glyph);
    x += ImGui::CalcTextSize(glyph).x + gap;
    const char *name_end = note->name.data() + note->name.size();
    dl->AddText(ImVec2(x, at.y), ImGui::GetColorU32(theme::k_text_2), note->name.data(),
                name_end);
    x += ImGui::CalcTextSize(note->name.data(), name_end).x + gap * 2.0F;
    if (!summary.empty() && x < at.x + w) {
        ImGui::PushStyleColor(ImGuiCol_Text, failed ? theme::k_danger : theme::k_text_3);
        ImGui::RenderTextEllipsis(dl, ImVec2(x, at.y), ImVec2(at.x + w, at.y + h), at.x + w,
                                  at.x + w, summary.data(), summary.data() + summary.size(),
                                  nullptr);
        ImGui::PopStyleColor();
    }
    if (_it.expanded && !detail.empty()) {
        draw_code(detail);
    }
    ImGui::PopID();
}

/// A screenshot as a thumbnail, opened whole in a popup on a click.
void draw_thumbnail(llm_item &_it) {
    using namespace campcat::app;
    const auto tex = static_cast<ImTextureID>(static_cast<uintptr_t>(_it.tex));
    // The id the host gave the image, not the item's address: a turn's items
    // move when it grows, which would close a popup someone is looking at.
    ImGui::PushID(static_cast<int>(_it.image_id));
    const ImVec2 thumb = fit_image(_it.tex_w, _it.tex_h,
                                   ImVec2(ImGui::GetContentRegionAvail().x, k_thumb_h));
    if (ImGui::InvisibleButton("thumb", thumb)) {
        ImGui::OpenPopup("view");
    }
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    dl->AddImageRounded(tex, mn, mx, ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F), IM_COL32_WHITE,
                        theme::k_radius_popup);
    dl->AddRect(mn, mx, ImGui::GetColorU32(hovered ? theme::k_border_strong : theme::k_border),
                theme::k_radius_popup);
    // Centred rather than at the cursor, which would push a large view off
    // the window when the thumbnail sits low or to the right.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5F, 0.5F));
    if (ImGui::BeginPopup("view")) {
        // ponytail: shows the timeline's own texture, decoded at most
        // `k_turn_img_max_w` wide. Sharper needs the host to keep each turn's
        // PNG past the next turn, which it clears today.
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const ImVec2 full = fit_image(_it.tex_w, _it.tex_h,
                                      ImVec2(disp.x * 0.85F, disp.y * 0.85F));
        if (ImGui::InvisibleButton("full", full)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::GetWindowDrawList()->AddImageRounded(
            tex, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F), IM_COL32_WHITE, theme::k_radius_popup);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

/// One step of a timeline: an image, what the agent did, its reasoning, or its
/// words. Shared by replies and the subagent card.
void draw_item(llm_item &_it, bool _growing, bool _subagent) {
    if (_it.type == campcat::llm_event::kind::image) {
        ensure_texture(_it, _subagent);
        if (_it.tex != 0 && _it.tex_w > 0) {
            draw_thumbnail(_it);
        } else if (_it.tex_dropped) {
            ImGui::TextDisabled("(screenshot dropped)");
        }
        return;
    }
    if (_it.type == campcat::llm_event::kind::tool) {
        draw_tool(_it);
        return;
    }
    if (_it.type == campcat::llm_event::kind::thinking) {
        draw_thinking(_it, _growing);
        return;
    }
    draw_blocks(_it);
}

bool is_blank(std::string_view _s) {    for (const char c : _s) {
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

/// The skill index beside the chat is folded away, and how wide it is when it
/// shows. Both kept in imgui.ini by `llm_panel_init_settings`.
bool g_index_hidden = false;
float g_index_w = campcat::app::theme::k_index_w;
constexpr float k_index_min_w = 160.0F;
/// A drag stops here, so the index cannot take the replies with it.
constexpr float k_chat_min_w = 280.0F;

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
    for (auto &turn : g_turns) {
        if (turn.pending && !turn.queued) {
            return &turn;
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
            if (grows_in_place(e.type) && _t.items[i].text != e.text) {
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

/// Take the finished job, if any, and fold it into the conversation.
bool consume_handoff() {
    auto done = g_controller.take_completion();
    if (!done) {
        return false;
    }
    auto &h = *done;

    if (h.kind == campcat::llm_controller::completion_kind::promotion) {
        for (auto &turn : g_turns) {
            if (turn.queued) {
                turn.queued = false;
                break;
            }
        }
        return true;
    }

    // Every finished request restarts the idle clock, consolidation included:
    // without this, finishing a memory pass would immediately satisfy the idle
    // condition again and it would loop.
    g_last_llm = ImGui::GetTime();

    if (h.kind == campcat::llm_controller::completion_kind::memory) {
        if (h.result.ok) {
            g_memory_pending = false;
            if (g_mem_open) {
                reload_memory();
            }
        }
        campcat::automation_log::emit(
            h.result.ok ? "[llm] long-term memory updated"
                     : "[llm] memory not saved: " + h.result.error);
        return true;
    }

    if (h.kind == campcat::llm_controller::completion_kind::turn) {
        g_memory_pending = true;
        fill_reply(h.result, h.timeline);
        if (h.result.ok) {
            // Reported by the sidecar on a successful turn only: a result
            // without the field parses as 0, so a failure must not zero this.
            g_sidecar_turns = h.result.turns;
            campcat::automation_log::emit("[llm] ok");
        } else {
            campcat::automation_log::emit("[llm] failed: " + h.result.error);
        }
        return true;
    }
    if (!h.result.ok) {
        campcat::automation_log::emit("[llm] reset failed: " + h.result.error);
        return true;
    }
    drop_all_turns();
    g_memory_pending = false;
    g_sub_cleared_run = g_host.subagent().runs;
    release_sub_items();
    g_sidecar_turns = 0;
    g_pin_bottom = true;
    campcat::automation_log::emit("[llm] conversation cleared");
    return true;
}

/// Send whatever is staged: clear the input and hand the prompt to the worker.
void send_current(campcat::shell_state &_st, campcat::app_config &_cfg) {
    const std::string prompt = g_input;
    if (is_blank(g_input)) {
        return;
    }

    if (!g_controller.start_turn(_st, _cfg, prompt)) {
        campcat::automation_log::emit("[llm] another device task is running");
        return;
    }
    g_input.clear();
    append_user_turn(prompt);
}

/// Show `_prompt` now and stop the turn in flight at its next tool. A second
/// send before that tool replaces the message; the earlier one is not sent.
bool queue_prompt(std::string _prompt) {
    bool replaced = false;
    if (!g_controller.queue_prompt(_prompt, &replaced)) {
        return false;
    }
    if (llm_turn *live = in_flight_turn()) {
        live->frozen = true;
    }
    if (replaced) {
        if (llm_turn *queued = queued_turn()) {
            queued->user_text = _prompt;
            return true;
        }
    }
    append_user_turn(std::move(_prompt), true);
    return true;
}

void request_task_stop() {
    if (g_controller.request_stop()) {
        if (llm_turn *queued = queued_turn()) {
            queued->queued = false;
            queued->pending = false;
            queued->interrupted = true;
            queued->ok = true;
        }
    }
}

/// "Thinking" (when the sidecar says so) plus 1..3 dots on the frame clock, so
/// a waiting bubble visibly ticks instead of sitting on a static "...".
std::string waiting_label() {
    const int dots = 1 + static_cast<int>(ImGui::GetTime() / 0.4) % 3;
    return std::string(g_thinking ? "Thinking" : "") + std::string(dots, '.');
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

/// One exchange in the message column that starts at `_x` and is `_col` wide:
/// the user's bubble hugging its right edge, the reply under an avatar on the
/// left. A pending turn shows a waiting line in place of the reply.
void draw_turn(llm_turn &_t, int _index, float _x, float _col) {
    using namespace campcat::app;
    constexpr float k_avatar = 24.0F;
    constexpr float k_avatar_gap = 8.0F;
    ImGui::PushID(_index);
    const ImVec2 bubble_pad(12.0F, 8.0F);
    const float max_w = _col * k_bubble_frac;

    const float uw = bubble_width(_t.user_text.c_str(), max_w, 0.0F, bubble_pad.x);
    ImGui::SetCursorPosX(_x + _col - uw);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::k_surface_3);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::k_radius_bubble);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, bubble_pad);
    ImGui::BeginChild("u", ImVec2(uw, 0.0F),
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    const ImVec2 user_top = ImGui::GetCursorScreenPos();
    ImGui::TextWrapped("%s", _t.user_text.c_str());
    copy_menu("copy", _t.user_text, user_top);
    ImGui::EndChild();

    ImGui::SetCursorPosX(_x);
    const ImVec2 avatar = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 centre(avatar.x + k_avatar * 0.5F, avatar.y + k_avatar * 0.5F);
    dl->AddCircleFilled(centre, k_avatar * 0.5F, ImGui::GetColorU32(theme::k_accent_wash));
    ImFont *small_font = fonts_get(font_role::small);
    dl->AddText(small_font, small_font->FontSize,
                icon_origin(small_font, icon::k_cat, avatar,
                            ImVec2(avatar.x + k_avatar, avatar.y + k_avatar)),
                ImGui::GetColorU32(theme::k_accent_fg), icon::k_cat);
    ImGui::Dummy(ImVec2(k_avatar, k_avatar));
    ImGui::SameLine(0.0F, k_avatar_gap);

    // No bubble around a reply: it reads as the page's own text, and only a
    // failure gets a box. Full width, because a markdown reply cannot be
    // measured from the raw text (`**` would be counted), and steps and images
    // need the room.
    const float aw = _col - k_avatar - k_avatar_gap;
    const bool failed = !_t.pending && !_t.ok;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, failed ? theme::k_danger_wash : ImVec4());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::k_radius_card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        failed ? ImGui::GetStyle().WindowPadding : ImVec2(0.0F, 2.0F));
    ImGui::BeginChild("a", ImVec2(aw, 0.0F),
                      ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    const ImVec2 reply_top = ImGui::GetCursorScreenPos();
    // The turn as it happened, step by step, while it runs. Once it is over,
    // everything before the closing words folds into one line, so the answer
    // is what the eye lands on; a turn that never called a tool has nothing
    // worth folding.
    size_t reply_start = _t.items.size();
    while (reply_start > 0 &&
           _t.items[reply_start - 1].type == campcat::llm_event::kind::text) {
        --reply_start;
    }
    int tools = 0;
    int shots = 0;
    int tool_failures = 0;
    for (size_t i = 0; i < reply_start; ++i) {
        const llm_item &it = _t.items[i];
        tools += it.type == campcat::llm_event::kind::tool ? 1 : 0;
        tool_failures += it.type == campcat::llm_event::kind::tool && tool_failed(it) ? 1 : 0;
        shots += it.type == campcat::llm_event::kind::image ? 1 : 0;
    }
    const bool fold = !_t.pending && tools > 0;
    size_t first = 0;
    if (fold) {
        char label[128];
        int n = std::snprintf(label, sizeof(label), "%s  %d step%s",
                              _t.steps_open ? icon::k_chevron_down : icon::k_chevron_right,
                              tools, tools == 1 ? "" : "s");
        if (shots > 0) {
            n += std::snprintf(label + n, sizeof(label) - n, " \xC2\xB7 %d screenshot%s", shots,
                               shots == 1 ? "" : "s");
        }
        if (tool_failures > 0) {
            std::snprintf(label + n, sizeof(label) - n, " \xC2\xB7 %d failed", tool_failures);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme::k_text_3);
        if (ImGui::Selectable(label)) {
            _t.steps_open = !_t.steps_open;
        }
        ImGui::PopStyleColor();
        first = _t.steps_open ? 0 : reply_start;
    }
    // Unfolded, the steps sit behind a rule so they stay apart from the answer.
    const bool ruled = fold && _t.steps_open;
    const ImVec2 steps_top = ImGui::GetCursorScreenPos();
    auto end_steps = [&] {
        ImGui::Unindent(12.0F);
        const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
        ImGui::GetWindowDrawList()->AddRectFilled(steps_top,
                                                  ImVec2(steps_top.x + 2.0F, bottom),
                                                  ImGui::GetColorU32(theme::k_border_strong));
    };
    if (ruled) {
        ImGui::Indent(12.0F);
    }
    for (size_t i = first; i < _t.items.size(); ++i) {
        if (ruled && i == reply_start) {
            end_steps();
        }
        draw_item(_t.items[i], _t.pending && i + 1 == _t.items.size());
    }
    if (ruled && reply_start == _t.items.size()) {
        end_steps();
    }
    // Only while the reply is still to come: a growing text step is its own
    // sign that the model is working, and a label under it would just be a
    // second one. A reasoning step counts for the same reason.
    const bool has_text =
        !_t.items.empty() && (grows_in_place(_t.items.back().type));
    if (_t.pending && !has_text) {
        ImGui::TextDisabled("%s", waiting_label().c_str());
    } else if (_t.interrupted) {
        ImGui::TextDisabled("Interrupted");
    } else if (failed) {
        WITH_FONT(bold, {
            ImGui::TextColored(theme::k_danger, "%s  Request failed", icon::k_failed);
        });
        ImGui::TextWrapped("%s", _t.error.c_str());
    }
    copy_menu("copy", reply_text(_t), reply_top);
    ImGui::EndChild();

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
    const bool rounds_ok = parse_cap(
        rounds_buf, campcat::llm::k_default_max_model_rounds, &rounds);
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
    if (campcat::app::icon_button("##approval_settings", campcat::app::icon::k_settings,
                                  "Which tools ask before acting")) {
        ImGui::OpenPopup("llm_approval_settings");
    }

    if (!ImGui::BeginPopup("llm_approval_settings")) {
        return;
    }
    // Opened from the composer, which draws in the chat face.
    ImGui::PushFont(fonts_get(font_role::body));
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
    ImGui::PopFont();
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
        using campcat::app::theme::rgb;
        const ImU32 model = ImGui::GetColorU32(
            rgb(0xE8A94B, a.given_region ? 230.0F / 255.0F : 130.0F / 255.0F));
        for (const campcat::llm::region &r : a.highlight) {
            draw_region(dl, mn, 1.0F, r, model, model, a.given_region ? 2.0F : 1.0F);
        }
        if (a.given_region) {
            draw_region(dl, mn, 1.0F, *a.given_region, 0,
                        ImGui::GetColorU32(rgb(0xC9A2F5, 220.0F / 255.0F)), 1.0F);
        }
        if (g_annot_dragging || g_annot_has_region) {
            draw_region(dl, mn, 1.0F, annot_draft(),
                        ImGui::GetColorU32(rgb(0x7EA6FF, 0.20F)),
                        ImGui::GetColorU32(campcat::app::theme::k_accent_fg), 2.0F);
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
            ImGui::TextDisabled(g_annot_shape == edit_shape::circle
                                    ? "drag from the centre to set the radius"
                                : g_annot_shape == edit_shape::diamond
                                    ? "drag from the centre; horizontal sets "
                                      "rx, vertical sets ry"
                                    : "drag on the image to pick a box");
        }
        // Switching shape drops the committed region: a box must not ride along
        // as a circle (or the other way round) just because it was drawn first.
        // Re-clicking the active option is not a switch (`segmented` reports
        // only a change), so what was just drawn survives it.
        ImGui::SameLine();
        static const char *const k_shapes[] = {"Box", "Circle", "Diamond"};
        int shape_index = static_cast<int>(g_annot_shape);
        if (campcat::app::segmented("##annot_shape", k_shapes, 3, &shape_index)) {
            g_annot_shape = static_cast<edit_shape>(shape_index);
            g_annot_has_region = false;
        }
        if (!g_annot_has_region) {
            ImGui::BeginDisabled();
        }
        if (campcat::app::button("Use this region", ImVec2(),
                                 campcat::app::button_kind::primary)) {
            g_staged_region = annot_draft();
            g_staged_id = g_annot_id;
            g_annot_open = false;
        }
        if (!g_annot_has_region) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (campcat::app::button("Cancel", ImVec2(), campcat::app::button_kind::ghost)) {
            g_annot_open = false; // the staged box, if any, is left alone
        }
    }
    ImGui::End();
    if (!open) {
        g_annot_open = false;
    }
}

/// Shared by the pinned preview and its full-size inspection popup.
void draw_approval_image(const campcat::llm_approval &_a, float _max_h) {
    if (g_appr_tex == 0 || g_appr_w <= 0 || g_appr_h <= 0) {
        return;
    }
    const float max_w = ImGui::GetContentRegionAvail().x;
    const float scale = std::min(
        {1.0F, max_w / static_cast<float>(g_appr_w),
         _max_h / static_cast<float>(g_appr_h)});
    using campcat::app::theme::rgb;
    const ImVec2 size(g_appr_w * scale, g_appr_h * scale);
    ImGui::Dummy(size);
    const ImVec2 origin = ImGui::GetItemRectMin();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddImageRounded(static_cast<ImTextureID>(static_cast<uintptr_t>(g_appr_tex)),
                        origin, ImGui::GetItemRectMax(), ImVec2(0.0F, 0.0F),
                        ImVec2(1.0F, 1.0F), IM_COL32_WHITE, 8.0F);
    dl->AddRect(origin, ImGui::GetItemRectMax(),
                ImGui::GetColorU32(campcat::app::theme::k_border), 8.0F);
    for (const campcat::llm::region &r : _a.highlight) {
        draw_region(dl, origin, scale, r, ImGui::GetColorU32(rgb(0xE8A94B, 48.0F / 255.0F)),
                    ImGui::GetColorU32(rgb(0xE8A94B, 230.0F / 255.0F)), 2.0F);
    }
    if (_a.given_region) {
        draw_region(dl, origin, scale, *_a.given_region, 0,
                    ImGui::GetColorU32(rgb(0xC9A2F5, 220.0F / 255.0F)), 1.0F);
    }
    if (g_staged_region && g_staged_id == _a.id) {
        draw_region(dl, origin, scale, *g_staged_region,
                    ImGui::GetColorU32(rgb(0x7EA6FF, 0.20F)),
                    ImGui::GetColorU32(campcat::app::theme::k_accent_fg), 2.0F);
    }
    if (_a.editable && ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        open_annotation(_a);
    }
}

/// Outside the scrollback: the question and its decisions stay by the input.
/// True when a decision was sent, so Enter cannot also submit this frame.
bool draw_approval_panel(const campcat::llm_approval &_a) {
    using campcat::app::button_kind;
    bool answered = false;
    campcat::app::card_begin("approval", "Needs your approval",
                             {.glyph = campcat::app::icon::k_approval,
                              .collapsible = false,
                              .bar = campcat::app::theme::k_warning});
    if (ImGui::BeginTable("approval_details", 2,
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("evidence", ImGuiTableColumnFlags_WidthStretch,
                                0.4F);
        ImGui::TableSetupColumn("decision", ImGuiTableColumnFlags_WidthStretch,
                                0.6F);
        ImGui::TableNextColumn();
        if (g_appr_tex != 0) {
            draw_approval_image(_a, 120.0F);
            if (campcat::app::button("View image")) {
                ImGui::OpenPopup("approval_screenshot");
            }
            ImGui::SetNextWindowSize(
                ImVec2(ImGui::GetIO().DisplaySize.x * 0.7F,
                       ImGui::GetIO().DisplaySize.y * 0.75F),
                ImGuiCond_Appearing);
            if (ImGui::BeginPopup("approval_screenshot")) {
                draw_approval_image(_a, ImGui::GetContentRegionAvail().y);
                ImGui::EndPopup();
            }
        } else {
            ImGui::TextWrapped("No screenshot attached to this request.");
        }
        ImGui::TableNextColumn();
        ImGui::BeginChild("approval_summary", ImVec2(0.0F, 48.0F));
        ImGui::TextWrapped("%s", _a.summary.c_str());
        ImGui::EndChild();
        if (campcat::app::button("Allow once", ImVec2(), button_kind::primary)) {
            g_host.answer_approval(true);
            clear_staged();
            answered = true;
        }
        const float reject_w = campcat::app::button_width("Reject");
        const float right = ImGui::GetCursorScreenPos().x +
                            ImGui::GetContentRegionAvail().x;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x +
                reject_w <= right) {
            ImGui::SameLine();
        }
        if (campcat::app::button("Reject", ImVec2(), button_kind::danger)) {
            g_host.answer_approval(false, {}, g_staged_region);
            clear_staged();
            answered = true;
        }
        if (_a.editable && g_appr_tex != 0 &&
            campcat::app::button("Edit region", ImVec2(), button_kind::ghost)) {
            open_annotation(_a);
        }
        if (g_staged_region) {
            ImGui::TextWrapped("Corrected region ready. Submit below to send it.");
            if (campcat::app::button("Discard correction", ImVec2(), button_kind::ghost)) {
                clear_staged();
            }
        }
        ImGui::EndTable();
    }
    campcat::app::card_end();
    return answered;
}

/// Draws the conversation. Returns true when the user asked to clear it via the
/// context menu, so the caller can start the reset on the worker.
bool draw_log(const ImVec2 &_size) {
    bool clear_requested = false;
    ImGui::BeginChild("llm_log", _size);
    // Any upward move is the reader asking for history, and it outranks
    // following the tail. Checked before the snap below sets its target: the
    // snap lands on the next frame and would otherwise undo a notch at a time,
    // then read the bottom it restored as "the user is still following".
    if (ImGui::GetScrollY() < g_last_scroll - 0.5F) {
        g_pin_bottom = false;
    }
    if (ImGui::BeginPopupContextWindow("llm_log_ctx",
                                       ImGuiPopupFlags_MouseButtonRight)) {
        // Greyed out while a turn is in flight, because clearing would have to
        // wait for it anyway.
        if (ImGui::MenuItem("Clear conversation", nullptr, false,
                            !g_controller.running() && !g_turns.empty())) {
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
        campcat::app::empty_state(campcat::app::icon::k_cat, "Tell CampCat what to do",
                                  "It looks at the device screen itself, then taps");
    }
    // One centred column: a line much wider than this is hard to read back.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float col = std::min(avail, campcat::app::theme::k_chat_max_w);
    const float x = ImGui::GetCursorPosX() + (avail - col) * 0.5F;
    for (size_t i = 0; i < g_turns.size(); ++i) {
        if (i > 0) {
            ImGui::Spacing(); // turns read as groups: twice the in-turn bubble
                              // gap
        }
        draw_turn(g_turns[i], static_cast<int>(i), x, col);
    }
    draw_subagent_card(x, col);

    ImGui::PopFont();

    // Follow new content only while the user is already at the bottom. The
    // comparison has no slack: a tolerance would keep re-arming inside the
    // trackpad's per-frame distance, which is the jitter this used to have.
    if (g_pin_bottom) {
        ImGui::SetScrollHereY(1.0F);
    }
    const float max_scroll = ImGui::GetScrollMaxY();
    g_pin_bottom = (max_scroll <= 0.0F) || (ImGui::GetScrollY() >= max_scroll);
    g_last_scroll = ImGui::GetScrollY();
    ImGui::EndChild();
    return clear_requested;
}

} // namespace

void llm_panel_shutdown_gl() {
    campcat::app_config shutdown_cfg;
    shutdown_cfg.config_home = g_config_home.string();
    shutdown_cfg.llm.provider = g_provider;
    shutdown_cfg.memory_graph = g_memory_graph;
    g_controller.begin_shutdown(shutdown_cfg);

    campcat::app::delete_tex(&g_appr_tex, &g_appr_w, &g_appr_h);
    g_appr_id = -1;
    drop_all_turns();
    release_sub_items();
    g_thinking = false;
}

void llm_panel_shutdown_finish() {
    g_controller.finish_shutdown();
}

campcat::activity_status llm_panel_activity() {
    if (g_controller.stopping()) {
        return {g_controller.running() ? campcat::activity_level::waiting
                                 : campcat::activity_level::idle,
                g_controller.running() ? "Stopping" : "Stopped"};
    }
    if (g_controller.resetting()) {
        return {campcat::activity_level::busy, "Clearing"};
    }
    // Most specific first: a boot and a script both run inside a turn, so
    // Reading the general running flag first would hide nested tool activity.
    // The memory pass also runs on the worker, so it is checked before that.
    if (g_host.pending_approval().pending) {
        return {campcat::activity_level::waiting, "Awaiting"};
    }
    if (campcat::llm::emulator_starting()) {
        return {campcat::activity_level::waiting, "Booting"};
    }
    if (campcat::llm::script_running()) {
        return {campcat::activity_level::busy, "Script"};
    }
    if (g_controller.memorizing()) {
        return {campcat::activity_level::busy, "Memory"};
    }
    if (g_controller.running()) {
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

void llm_panel_tick(campcat::shell_state &_st, const campcat::app_config &_cfg,
                    bool _on_llm_page) {
    while (consume_handoff()) {
    }
    if (g_controller.stopping() && g_controller.running()) {
        // A tool can publish its approval or reset its stop flag after the
        // first click. Keep cancelling until the worker acknowledges Stop.
        request_task_stop();
    }
    pump_stream();
    if (g_memory_pending && !g_controller.running() &&
        !g_host.pending_approval().pending &&
        (!_on_llm_page || is_blank(g_input)) && !_cfg.config_home.empty() &&
        ImGui::GetTime() - g_last_llm >= k_idle_seconds) {
        auto cfg = _cfg;
        cfg.memory_graph = _st.workspace.graph;
        g_controller.start_memorize(_st, cfg);
    }
}

bool llm_panel_draw_sidebar_task(bool _on_llm_page) {
    const campcat::activity_status status = llm_panel_activity();
    if (!_on_llm_page && !g_controller.running() && !g_controller.stopping() &&
        status.level != campcat::activity_level::failed) {
        return false;
    }
    using campcat::app::button_kind;
    bool view_task = false;
    ImGui::Spacing();
    campcat::app::card_begin("agent_task", "Agent task",
                             {.glyph = campcat::app::icon::k_bot,
                              .collapsible = false,
                              .status = &status});
    if (g_controller.running()) {
        ImGui::BeginDisabled(g_controller.stopping() || g_controller.resetting());
        if (campcat::app::button("Stop agent", ImVec2(-1.0F, 0.0F), button_kind::danger)) {
            request_task_stop();
        }
        ImGui::EndDisabled();
    }
    if (!_on_llm_page) {
        view_task = campcat::app::button("Open task in LLM", ImVec2(-1.0F, 0.0F),
                                         button_kind::ghost);
    }
    if (g_controller.stopping() && g_controller.running()) {
        ImGui::TextWrapped("Stop requested. Waiting for the current operation to finish.");
    } else if (status.level == campcat::activity_level::waiting &&
               g_host.pending_approval().pending) {
        ImGui::TextWrapped(_on_llm_page ? "Review the pending approval in chat."
                                        : "Open LLM to review the pending approval.");
    }
    campcat::app::card_end();
    return view_task;
}

void llm_panel_init_settings() {
    ImGuiSettingsHandler h;
    h.TypeName = "CampCat";
    h.TypeHash = ImHashStr(h.TypeName);
    h.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *, const char *_name) -> void * {
        return std::strcmp(_name, "Llm") == 0 ? &g_index_hidden : nullptr;
    };
    h.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *, void *, const char *_line) {
        int hidden = 0;
        float width = 0.0F;
        if (std::sscanf(_line, "IndexHidden=%d", &hidden) == 1) {
            g_index_hidden = hidden != 0;
        } else if (std::sscanf(_line, "IndexWidth=%f", &width) == 1 && width >= k_index_min_w) {
            g_index_w = width;
        }
    };
    h.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *_h, ImGuiTextBuffer *_buf) {
        _buf->appendf("[%s][Llm]\nIndexHidden=%d\nIndexWidth=%.0f\n\n", _h->TypeName,
                      g_index_hidden ? 1 : 0, g_index_w);
    };
    ImGui::AddSettingsHandler(&h);
}

void llm_panel_draw_index_toggle() {
    namespace icon = campcat::app::icon;
    if (campcat::app::icon_button("##index",
                                  g_index_hidden ? icon::k_index_show : icon::k_index_hide,
                                  g_index_hidden ? "Show index" : "Hide index")) {
        g_index_hidden = !g_index_hidden;
        ImGui::MarkIniSettingsDirty();
    }
}

/// Width of the index for this frame. The grab is the gap beside the column:
/// the list fills the column, so a handle on the rule itself would lose the
/// click to a row. While held, the width follows the pointer.
static float index_column_width(float _row_h) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float max_w = avail - gap - k_chat_min_w;
    float width = g_index_w;
    if (max_w >= k_index_min_w) {
        width = std::min(width, max_w);
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float h = std::max(1.0F, _row_h);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width, origin.y));
    ImGui::InvisibleButton("index_split", ImVec2(std::max(1.0F, gap), h));
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (active) {
        const float next = std::clamp(g_index_w + ImGui::GetIO().MouseDelta.x, k_index_min_w,
                                      std::max(k_index_min_w, max_w));
        if (next != g_index_w) {
            g_index_w = next;
            width = max_w >= k_index_min_w ? std::min(next, max_w) : next;
            ImGui::MarkIniSettingsDirty();
        }
    }
    if (ImGui::IsItemHovered() || active) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(origin.x + width, origin.y), ImVec2(origin.x + width + 2.0F, origin.y + h),
            ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered));
    }
    ImGui::SetCursorScreenPos(origin);
    return width;
}

void llm_panel_draw_panel(campcat::shell_state &_st, campcat::app_config &_cfg) {
    // Kept for the shutdown pass and the memory window, neither of which is
    // handed the config: this panel is the only place that sees it.
    const bool scope_changed = g_config_home != _cfg.config_home ||
                               g_memory_graph != _st.workspace.graph;
    _cfg.memory_graph = _st.workspace.graph;
    g_config_home = _cfg.config_home;
    g_provider = _cfg.llm.provider;
    g_memory_graph = _cfg.memory_graph;
    if (scope_changed && g_mem_open) {
        reload_memory();
    }

    // Saved skills in a column on the left, conversation on the right, the
    // prompt box below both. Side by side because the list is an index, not a
    // second conversation: stacked, it took height off every reply.
    if (!g_index_hidden) {
        const float index_w = index_column_width(ImGui::GetContentRegionAvail().y - g_footer_h);
        script_editor_panel_draw(_cfg.library_root(), index_w, -g_footer_h, _st.workspace,
                                 _st.device_tasks.busy() || _st.sched.running());
        ImGui::SameLine();
    }
    // Measured, not another negative width: a negative size is resolved against
    // the window's content region, not against what is left past the cursor, so
    // "-list_w" here would leave the list's width blank at the right edge too.
    const bool clear_requested =
        draw_log(ImVec2(ImGui::GetContentRegionAvail().x, -g_footer_h));
    if (clear_requested) {
        g_controller.start_reset(_st, _cfg);
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
    const bool have_approval = sync_approval_texture(approval);
    ImGui::BeginDisabled(g_controller.stopping());
    const bool answered = have_approval && draw_approval_panel(approval);
    ImGui::EndDisabled();
    namespace theme = campcat::app::theme;
    if (awaiting) {
        ImGui::TextColored(theme::k_warning, "Correction for this operation");
    }

    // The action label makes an approval correction distinct from a new message.
    const bool memorizing = g_controller.memorizing();
    const char *send_label = awaiting ? "Submit correction" : "Send";
    // The frame lights up while the box inside it has focus; that is only
    // known after the box is drawn, so it is last frame's answer.
    static bool s_prompt_active = false;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::k_surface_inset);
    ImGui::PushStyleColor(ImGuiCol_Border,
                          s_prompt_active ? theme::k_accent_fg : theme::k_border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::k_radius_bubble);
    ImGui::BeginChild("composer", ImVec2(0.0F, 0.0F),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    // The conversation's face, which carries the wide fill: the body face
    // turns symbols typed here into "?".
    ImGui::PushFont(fonts_get(font_role::chat));
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float frame_h = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 2.0F * frame_h - 2.0F * gap);
    // The frame around it is the box; a second box inside would double the edge.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4());
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4());
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    const bool submitted =
        prompt_input_text("##llm_prompt",
                          awaiting ? "Describe the correction for this operation..."
                                   : "Tell CampCat what to do...",
                          &g_input);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    s_prompt_active = ImGui::IsItemActive();
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
    draw_approval_settings(_cfg);
    ImGui::SameLine();
    // A turn in flight still accepts a message: that message is how the user
    // stops it. Clear conversation does not, because the reset drops the log.
    const bool can_send = !answered &&
                          !(g_controller.stopping() && g_controller.running()) &&
                          !g_controller.resetting() &&
                          (!is_blank(g_input) || (awaiting && g_staged_region));
    ImGui::BeginDisabled(!can_send);
    const bool clicked =
        campcat::app::icon_button("##send", campcat::app::icon::k_send,
                                  memorizing ? "Summarizing..." : send_label,
                                  campcat::app::button_kind::primary);
    ImGui::EndDisabled();
    ImGui::PopFont();
    ImGui::EndChild();

    // Four counts. Dispatches is the only one with a fuse, and that fuse is
    // per turn while the number is the conversation total.
    const campcat::llm_protocol::usage_stats usage = g_host.usage();
    ImGui::Indent(ImGui::GetStyle().WindowPadding.x);
    WITH_FONT(small, {
        bool open_fuse = false;
        bool first = true;
        const auto stat = [&](const char *_label, const std::string &_value) {
            const auto double_clicked = [] {
                return ImGui::IsItemHovered() &&
                       ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            };
            if (!first) {
                ImGui::SameLine(0.0F, 8.0F);
                ImGui::TextColored(theme::k_text_3, "\xC2\xB7");
                ImGui::SameLine(0.0F, 8.0F);
            }
            first = false;
            ImGui::TextColored(theme::k_text_3, "%s", _label);
            open_fuse = double_clicked() || open_fuse;
            ImGui::SameLine(0.0F, 4.0F);
            ImGui::TextColored(theme::k_text_2, "%s", _value.c_str());
            open_fuse = double_clicked() || open_fuse;
        };
        stat("Turns", std::to_string(g_sidecar_turns));
        stat("LLM calls", std::to_string(usage.llm_calls));
        stat("Dispatches", std::to_string(usage.tool_calls) + "/" +
                               campcat::llm::format_fuse(_cfg.llm.max_dispatches));
        stat("Context", usage.context_tokens < 0 ? std::string("\xE2\x80\x94")
                                                 : std::to_string(usage.context_tokens));
        if (open_fuse) {
            ImGui::OpenPopup("llm_fuse_settings");
        }
        draw_fuse_settings(_cfg);
    });
    ImGui::Unindent(ImGui::GetStyle().WindowPadding.x);

    if ((clicked || submitted) && can_send) {
        if (awaiting) {
            const bool blank = is_blank(g_input);
            g_host.answer_approval(false, blank ? std::string() : g_input,
                                   g_staged_region);
            clear_staged();
            g_input.clear(); // consumed as the answer, not as a prompt
        } else if (g_controller.running()) {
            if (queue_prompt(g_input)) {
                g_input.clear();
            } else {
                send_current(_st, _cfg);
            }
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
