#include "app/llm_ui.h"

#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "core/llm/llm_host.h"

#if defined(__APPLE__)
#include "app/macos_ime.h"
#endif

#include <imgui.h>
#include <imgui_internal.h> // GetInputTextState: the IME pre-edit hangs off the caret
#include <imgui_markdown.h>

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
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/// Oldest turns are dropped past this.
constexpr int k_turn_cap = 30;

/// One exchange in the conversation: what was sent and what came back.
struct llm_turn {
  std::string user_text;
  std::string reply;
  std::vector<std::string> tool_notes; ///< what the model did, in call order
  /// Last tool image of the turn, raw PNG, copied from the host once. The model
  /// already saw this image through the tool result; this copy is for the human.
  std::string image_png;
  long image_id = 0; ///< which host image `image_png` holds; see tool_image_id()
  GLuint tex = 0;    ///< decoded lazily on the UI thread, needs the GL context
  int tex_w = 0;
  int tex_h = 0;
  bool tex_tried = false; ///< decode attempted; a 0 `tex` after this means failed
  bool pending = false; ///< sent but unanswered; drives the waiting bubble
  bool ok = false;
  std::string error;
};

/// Result of a finished background job, handed to the UI thread.
struct handoff {
  bool has = false;
  bool is_reset = false;
  campcat::llm_result res;
};

// UI-thread state: never touched by the worker.
std::vector<llm_turn> g_turns;
char g_input[1024] = {};
bool g_pin_bottom = true;
bool g_thinking = false; ///< sidecar reports the model is reasoning
ImFont *g_bold = nullptr; ///< set by llm_ui_set_bold_font

/// True while an automation cycle or the scheduler owns adb. Tools must refuse
/// then rather than contend for the device.
std::atomic<bool> g_adb_busy{false};

/// Wraps the library default, fixing two of its choices that read badly in a
/// chat bubble.
void md_format(const ImGui::MarkdownFormatInfo &_info, bool _start) {
  if (_info.type == ImGui::MarkdownFormatType::EMPHASIS) {
    // The default greys out `*italic*` (TextDisabled), which inside a bubble
    // reads as disabled text; level 1 therefore stays untouched.
    if (_info.level <= 1 || g_bold == nullptr) {
      return;
    }
    if (_start) {
      ImGui::PushFont(g_bold);
    } else {
      ImGui::PopFont();
    }
    return;
  }
  ImGui::defaultMarkdownFormatCallback(_info, _start);
}

/// Built once; the fonts are filled in by llm_ui_set_bold_font.
ImGui::MarkdownConfig &md_config() {
  static ImGui::MarkdownConfig cfg = [] {
    ImGui::MarkdownConfig c;
    c.linkCallback = nullptr; // a reply must not open a browser
    c.imageCallback = nullptr;
    c.formatCallback = md_format;
    return c;
  }();
  return cfg;
}

/// IME helper. Off Apple it is constant, so the input path needs no guard.
std::string ime_preedit() {
#if defined(__APPLE__)
  return macos_ime_preedit();
#else
  return {};
#endif
}

/// Paints the IME composition over the prompt box. While composing, ImGui's own
/// buffer is still empty, so nothing else in the box shows what is being typed.
///
/// Call it right after the prompt's InputText, before SameLine/Button: the caret
/// comes from the last item and the origin from its rect.
///
/// ponytail: no selection highlight, and the pre-edit is clipped to the box
/// instead of pushing the text after the caret along; upgrade if a mid-text
/// composition ever needs either.
void draw_ime_preedit(const ImVec2 &_box_min, const ImVec2 &_box_max,
                      const char *_buf, const std::string &_preedit) {
  ImGuiInputTextState *state = ImGui::GetInputTextState(ImGui::GetItemID());
  if (state == nullptr) {
    return; // not the live input right now
  }
  const int len = static_cast<int>(std::strlen(_buf));
  // Stb is only forward-declared outside imgui_widgets.cpp, so read the caret
  // through the accessor ImGui itself exposes.
  const int cursor = ImClamp(state->GetCursorPos(), 0, len); // byte index
  const ImGuiStyle &style = ImGui::GetStyle();
  const float prefix = ImGui::CalcTextSize(_buf, _buf + cursor).x;
  const ImVec2 pos(_box_min.x + style.FramePadding.x + prefix - state->Scroll.x,
                   _box_min.y + style.FramePadding.y);
  const ImVec2 size = ImGui::CalcTextSize(_preedit.c_str());
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(_box_min, _box_max, true);
  dl->AddRectFilled(ImVec2(pos.x, _box_min.y + 3.0F),
                    ImVec2(pos.x + size.x + 2.0F, _box_min.y + size.y + 3.0F),
                    IM_COL32(70, 70, 85, 255), 3.0F);
  dl->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), _preedit.c_str());
  dl->AddLine(ImVec2(pos.x, pos.y + size.y),
              ImVec2(pos.x + size.x, pos.y + size.y),
              ImGui::GetColorU32(ImGuiCol_Text), 1.0F);
  dl->PopClipRect();
}

// Worker state.
std::atomic<bool> g_running{false};
std::thread g_thread;
campcat::llm_host g_host;
std::mutex g_done_mu;
handoff g_done;

/// Bubble backgrounds. The colours also tell the two speakers apart, which is
/// why the "You" / "CampCat" labels are gone.
const ImVec4 k_user_bg{0.19F, 0.31F, 0.45F, 1.0F};
const ImVec4 k_bot_bg{0.16F, 0.18F, 0.20F, 1.0F};
const ImVec4 k_err_col{0.95F, 0.45F, 0.35F, 1.0F};
const ImVec4 k_err_bg{0.30F, 0.16F, 0.16F, 1.0F}; ///< failed-reply bubble

/// A bubble takes at most this share of the log width before it wraps.
constexpr float k_bubble_frac = 0.72F;
constexpr float k_bubble_rounding = 6.0F;

/// Width cap for the approval preview. Wider than a plain screenshot thumbnail
/// because the user has to judge a highlighted box before allowing a tap.
constexpr float k_approval_max_w = 420.0F;

/// A tool image is decoded at no more than this width. A full frame is ~10 MB of
/// texture, and turns are kept, so a whole conversation of them would not fit in
/// VRAM. The bubble draws it far smaller than this anyway.
constexpr int k_turn_img_max_w = 640;

/// Upload `_bgr` as a texture. Returns 0 on failure.
GLuint upload_bgr(const cv::Mat &_bgr, int *_w, int *_h) {
  if (_bgr.empty() || _bgr.type() != CV_8UC3) {
    return 0;
  }
  cv::Mat rgba;
  cv::cvtColor(_bgr, rgba, cv::COLOR_BGR2RGBA);

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.cols, rgba.rows, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba.ptr());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
  *_w = rgba.cols;
  *_h = rgba.rows;
  return tex;
}

void delete_tex(GLuint *_tex, int *_w, int *_h) {
  if (*_tex != 0) {
    glDeleteTextures(1, _tex);
    *_tex = 0;
  }
  *_w = 0;
  *_h = 0;
}

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
  delete_tex(&_t.tex, &_t.tex_w, &_t.tex_h);
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
    _t.tex = upload_bgr(small, &_t.tex_w, &_t.tex_h);
    return;
  }
  _t.tex = upload_bgr(bgr, &_t.tex_w, &_t.tex_h);
}

/// Drop every turn, releasing the textures they own. The GL context must be live.
void drop_all_turns() {
  for (llm_turn &t : g_turns) {
    delete_tex(&t.tex, &t.tex_w, &t.tex_h);
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

bool is_blank(const char *_s) {
  for (const char *p = _s; *p != '\0'; ++p) {
    if (!std::isspace(static_cast<unsigned char>(*p))) {
      return false;
    }
  }
  return true;
}

/// Space reserved below the log. Measured from the previous frame rather than
/// computed: guessing the frame height and spacing got it wrong by a few pixels,
/// which pushed the controls over the log's border. Seeded generously so the
/// very first frame under-fills instead of overlapping.
float g_footer_h = 160.0F;

/// Queue the outgoing turn the moment it is sent, so the log shows the user's
/// message while the reply is still in flight.
void append_user_turn(std::string _prompt) {
  llm_turn t;
  t.user_text = std::move(_prompt);
  t.pending = true;
  // Start level with the host, so the previous turn's image is not mistaken for
  // a new one on the first frame.
  t.image_id = g_host.tool_image_id();

  g_turns.push_back(std::move(t));
  while (static_cast<int>(g_turns.size()) > k_turn_cap) {
    delete_tex(&g_turns.front().tex, &g_turns.front().tex_w, &g_turns.front().tex_h);
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
    // A tool image may have arrived in the same frame the turn finished, which
    // pump_stream would have missed.
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
void start_turn(const campcat::app_config &_cfg, std::string _repo_root,
                std::string _prompt) {
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_running.store(true);
  // The whole snapshot, by value: the script tools need the interpreter's config
  // (`tool_context::cfg`), and `_cfg` is a per-frame copy in the caller's frame,
  // so a pointer to it would dangle the moment the frame ends.
  g_thread = std::thread([cfg_snapshot = _cfg, root = std::move(_repo_root),
                          prompt = std::move(_prompt)]() {
    // A per-turn client, built the way the automation cycle builds one. Every
    // tool runs on this thread, so nothing here races the UI.
    campcat::adb_client adb(cfg_snapshot.adb_path, cfg_snapshot.adb_serial);
    adb.connect_remote(cfg_snapshot.adb_connect_address);
    campcat::llm::tool_context ctx;
    ctx.adb = &adb;
    ctx.adb_busy = [] { return g_adb_busy.load(); };
    ctx.require_approval = cfg_snapshot.require_tool_approval;
    ctx.cfg = &cfg_snapshot;
    ctx.script_dir = cfg_snapshot.config_home / "scripts/llm";
    g_host.set_tool_context(ctx);
    campcat::llm_result res = g_host.run_turn(root, prompt);
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

void start_reset(std::filesystem::path _repo_root) {
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_running.store(true);
  g_thread = std::thread([root = std::move(_repo_root)]() {
    campcat::llm_result res = g_host.reset(root);
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

  if (!h.is_reset) {
    fill_reply(h.res);
    if (h.res.ok) {
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
  g_pin_bottom = true;
  campcat::automation_log::emit("[llm] conversation cleared");
}

/// Send whatever is staged: clear the input and hand the prompt to the worker.
void send_current(campcat::app_config &_cfg) {
  const std::string prompt = g_input;
  if (is_blank(g_input)) {
    return;
  }

  g_input[0] = '\0';

  append_user_turn(prompt);
  // Cleared from the UI thread, not just on the worker: the worker clears it
  // inside next_request_id, which runs after ensure_running may have spent a
  // minute starting the interpreter, and until then the new bubble would show
  // the previous request's leftovers.
  g_host.clear_stream();
  start_turn(_cfg, _cfg.config_home.parent_path(), prompt);
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
  if (g_bold != nullptr) {
    ImGui::PushFont(g_bold);
  }
  ImGui::SetCursorPosX(
      ImGui::GetCursorPosX() +
      std::max(0.0F, (avail.x - ImGui::CalcTextSize(title).x) * 0.5F));
  ImGui::TextUnformatted(title);
  if (g_bold != nullptr) {
    ImGui::PopFont();
  }
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
    const ImVec2 sz =
        ImGui::CalcTextSize(_text, nullptr, false, _max_w - 2.0F * _pad - 4.0F);
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
  // is measured at its widest so the ticking dots do not resize it. A tool image
  // needs the full width too, or it would be clipped into a narrow strip.
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
  // Shown above the reply so the model's device actions are visible even while
  // the answer is still streaming. Without them a tap that changed the screen
  // would look like nothing happened.
  for (const std::string &note : _t.tool_notes) {
    ImGui::TextDisabled("%s", note.c_str());
  }
  // What the model was looking at when it wrote the reply below: a tool image
  // (a screenshot, or a box `mark` drew) that would otherwise be invisible here.
  ensure_turn_texture(_t);
  if (_t.tex != 0 && _t.tex_w > 0) {
    const ImVec2 img =
        fit_image(_t.tex_w, _t.tex_h, ImGui::GetContentRegionAvail().x);
    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(_t.tex)), img,
                 ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
  }
  if (_t.pending && !_t.reply.empty()) {
    ImGui::Markdown(_t.reply.c_str(), _t.reply.size(), md_config());
  } else if (_t.pending) {
    ImGui::TextDisabled("%s", waiting_label().c_str());
  } else if (_t.ok) {
    ImGui::Markdown(_t.reply.c_str(), _t.reply.size(), md_config());
  } else {
    if (g_bold != nullptr) {
      ImGui::PushFont(g_bold);
    }
    ImGui::TextColored(k_err_col, "Request failed");
    if (g_bold != nullptr) {
      ImGui::PopFont();
    }
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
    delete_tex(&g_appr_tex, &g_appr_w, &g_appr_h);
    g_appr_id = -1;
    return false;
  }
  if (_a.id == g_appr_id) {
    return true;
  }
  delete_tex(&g_appr_tex, &g_appr_w, &g_appr_h);
  g_appr_id = _a.id;
  const std::string png = g_host.approval_screen_png();
  if (png.empty()) {
    return true; // nothing to show, but the buttons must stay reachable
  }
  const cv::Mat buf(1, static_cast<int>(png.size()), CV_8UC1,
                    const_cast<char *>(png.data()));
  const cv::Mat bgr = cv::imdecode(buf, cv::IMREAD_COLOR);
  g_appr_tex = upload_bgr(bgr, &g_appr_w, &g_appr_h);
  return true;
}

/// The question waiting on the user: the proposed box drawn over the screenshot,
/// and the two buttons that decide whether the tap runs at all.
void draw_approval_bubble(const campcat::llm_approval &_a) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, k_bot_bg);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_bubble_rounding);
  ImGui::BeginChild("approval", ImVec2(0.0F, 0.0F),
                    ImGuiChildFlags_AutoResizeY |
                        ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar);
  if (g_bold != nullptr) {
    ImGui::PushFont(g_bold);
  }
  ImGui::TextUnformatted("Waiting for your approval");
  if (g_bold != nullptr) {
    ImGui::PopFont();
  }
  ImGui::TextDisabled("%s", _a.summary.c_str());
  if (g_appr_tex != 0 && g_appr_w > 0) {
    const ImVec2 size = fit_image(g_appr_w, g_appr_h, k_approval_max_w);
    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(g_appr_tex)),
                 size, ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
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
  }
  if (ImGui::Button("Approve")) {
    g_host.answer_approval(true);
  }
  ImGui::SameLine();
  // Rejection with a typed correction goes through the prompt box, which stays
  // live while this bubble is up; these buttons are the no-typing shortcut.
  // Plain words, not ✓/✗: the UI font carries no glyphs for those two and draws
  // them as "?" (see 2026-10-09-llm-tap-approval-design.md §4.5).
  if (ImGui::Button("Reject")) {
    g_host.answer_approval(false);
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

/// Draws the conversation. Returns true when the user asked to clear it via the
/// context menu, so the caller can start the reset on the worker.
bool draw_log() {
  bool clear_requested = false;
  ImGui::BeginChild("llm_log", ImVec2(0.0F, -g_footer_h),
                    ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  if (ImGui::BeginPopupContextWindow("llm_log_ctx",
                                     ImGuiPopupFlags_MouseButtonRight)) {
    // Greyed out while a turn is in flight, because clearing would have to wait
    // for it anyway.
    if (ImGui::MenuItem("Clear conversation", nullptr, false,
                        !g_running.load() && !g_turns.empty())) {
      clear_requested = true;
    }
    ImGui::EndPopup();
  }
  if (g_turns.empty()) {
    draw_empty_state();
  }
  for (size_t i = 0; i < g_turns.size(); ++i) {
    if (i > 0) {
      ImGui::Spacing(); // turns read as groups: twice the in-turn bubble gap
    }
    draw_turn(g_turns[i], static_cast<int>(i));
  }

  // The approval gate, at the end of the conversation: the user reads it as the
  // model's latest action, and it scrolls away with the rest once answered.
  const campcat::llm_approval approval = g_host.pending_approval();
  if (sync_approval_texture(approval)) {
    ImGui::Spacing();
    draw_approval_bubble(approval);
  }

  // A script in flight, next to where the approval bubble was: the same place the
  // user is already looking. Only shown when there is really a script to stop, so
  // it never becomes a button that does nothing.
  if (campcat::llm::script_running()) {
    ImGui::Spacing();
    draw_script_bubble();
  }

  // Follow new content only while the user is already at the bottom.
  if (g_pin_bottom) {
    ImGui::SetScrollHereY(1.0F);
  }
  const float max_scroll = ImGui::GetScrollMaxY();
  g_pin_bottom = (max_scroll <= 0.0F) || (ImGui::GetScrollY() >= max_scroll - 4.0F);
  ImGui::EndChild();
  return clear_requested;
}

} // namespace

void llm_ui_set_bold_font(ImFont *_font) {
  g_bold = _font;
  // `**strong**` is drawn with the last heading slot rather than with a bold
  // font of its own, so that slot is what has to hold the bold face.
  for (auto &fmt : md_config().headingFormats) {
    fmt.font = _font;
  }
}

void llm_ui_shutdown_gl() {
  // Unblock a question nobody can answer any more: without this the worker stays
  // inside service_approval and the join below never returns.
  g_host.cancel_approval();
  // A long script keeps the worker inside run_script, where cancel_approval means
  // nothing; without this the join below waits for the script to finish on its own.
  campcat::llm::request_script_stop();
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_host.stop();
  delete_tex(&g_appr_tex, &g_appr_w, &g_appr_h);
  g_appr_id = -1;
  drop_all_turns();
  g_thinking = false;
}

void llm_ui_draw_panel(campcat::app_config &_cfg, bool _adb_busy) {
  // Finished work is folded in on the UI thread, which owns g_turns.
  consume_handoff();

  // An automation cycle or the scheduler is using adb, so tools must refuse
  // rather than contend with it.
  g_adb_busy.store(_adb_busy);

  const bool busy = g_running.load();

  pump_stream();
  if (draw_log()) {
    start_reset(_cfg.config_home.parent_path());
  }

  // Top of whatever comes below the log: the reference for the footer height.
  const float footer_top = ImGui::GetCursorScreenPos().y;

  // While a question waits on the user, this box and its Send are the rejection
  // form: the text becomes the correction the model gets back. One input, one
  // habit, instead of a second box inside the bubble.
  const bool awaiting = g_host.pending_approval().pending;

  // ImGui is kept out of the keys while a composition is live (see
  // macos_ime.mm), so an Enter here is a real Enter.
  const std::string preedit = ime_preedit();

  // Leaves room for the Send button and the spacing before it.
  ImGui::SetNextItemWidth(-116.0F);
  const bool submitted = ImGui::InputTextWithHint(
      "##llm_prompt",
      // While composing the hint is blanked: the composition is painted over the
      // box and the two would collide on the same line.
      preedit.empty() ? (awaiting ? "Optional: tell it what to do instead"
                                  : "Tell CampCat what to do...")
                      : "",
      g_input, sizeof(g_input), ImGuiInputTextFlags_EnterReturnsTrue);
  // Both need the prompt to still be the last item.
  const ImVec2 prompt_min = ImGui::GetItemRectMin();
  const ImVec2 prompt_max = ImGui::GetItemRectMax();
  if (!preedit.empty()) {
    draw_ime_preedit(prompt_min, prompt_max, g_input, preedit);
  }

  ImGui::SameLine();
  const bool can_send = awaiting || (!busy && !is_blank(g_input));
  if (!can_send) {
    ImGui::BeginDisabled();
  }
  const bool clicked = ImGui::Button("Send", ImVec2(104.0F, 0.0F));
  if (!can_send) {
    ImGui::EndDisabled();
  }

  if ((clicked || submitted) && can_send) {
    if (awaiting) {
      const bool blank = is_blank(g_input);
      g_host.answer_approval(false, blank ? std::string() : std::string(g_input));
      g_input[0] = '\0'; // consumed as the answer, not as a prompt
    } else {
      send_current(_cfg);
    }
  }

  // Reserve exactly what the controls actually used, so they never creep over
  // the log on the next frame.
  g_footer_h = ImGui::GetItemRectMax().y - footer_top +
               ImGui::GetStyle().ItemSpacing.y;
}
