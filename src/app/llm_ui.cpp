#include "app/llm_ui.h"

#include "app/native_file_dialog.h"
#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "core/llm/llm_host.h"

#include <imgui.h>
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
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/// Oldest turns are dropped past this, each releasing its texture.
constexpr int k_turn_cap = 30;
/// Thumbnails are downscaled to this long side before upload: a raw 1080x2400
/// screenshot is ~10 MB of texture, which 30 turns would blow up.
constexpr int k_thumb_max_side = 320;
constexpr float k_image_max_width = 260.0f;
/// Cap on the pending-screenshot preview. It bounds the height of the controls
/// below the log, which is what keeps the panel itself from ever scrolling.
constexpr float k_preview_max_height = 140.0F;

/// One exchange in the conversation: what was sent and what came back.
struct llm_turn {
  GLuint tex = 0; ///< 0 when the turn carried no image
  int tex_w = 0;
  int tex_h = 0;
  std::string label; ///< screenshot source, e.g. "emulator screenshot"
  std::string user_text;
  std::string reply;
  bool pending = false; ///< sent but unanswered; drives the waiting bubble
  bool ok = false;
  std::string error;
};

/// Result of a finished background job, handed to the UI thread. The outgoing
/// image and prompt are consumed when the turn is queued, so they are not
/// carried back.
struct handoff {
  bool has = false;
  bool is_reset = false;
  campcat::llm_result res;
};

// UI-thread state: never touched by the worker.
std::vector<llm_turn> g_turns;
cv::Mat g_pending;
std::string g_pending_label;
char g_input[1024] = {};
bool g_pin_bottom = true;
bool g_thinking = false; ///< sidecar reports the model is reasoning
ImFont *g_bold = nullptr; ///< set by llm_ui_set_bold_font

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
GLuint g_tex = 0; ///< pending preview texture
int g_tex_w = 0;
int g_tex_h = 0;

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

bool is_blank(const char *_s) {
  for (const char *p = _s; *p != '\0'; ++p) {
    if (!std::isspace(static_cast<unsigned char>(*p))) {
      return false;
    }
  }
  return true;
}

/// Downscale to the thumbnail budget; leaves small images untouched.
cv::Mat make_thumb(const cv::Mat &_bgr) {
  if (_bgr.empty()) {
    return {};
  }
  const int long_side = std::max(_bgr.cols, _bgr.rows);
  if (long_side <= k_thumb_max_side) {
    return _bgr.clone();
  }
  const double scale =
      static_cast<double>(k_thumb_max_side) / static_cast<double>(long_side);
  cv::Mat out;
  cv::resize(_bgr, out, cv::Size(), scale, scale, cv::INTER_AREA);
  return out;
}

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

/// Fit `_w x _h` into `k_image_max_width`, preserving aspect.
ImVec2 fit_image(int _w, int _h) {
  float w = static_cast<float>(_w);
  float h = static_cast<float>(_h);
  if (w > k_image_max_width && w > 0.0F) {
    h *= k_image_max_width / w;
    w = k_image_max_width;
  }
  return ImVec2(w, h);
}

/// Preview box for the pending screenshot: `fit_image` plus a height cap.
ImVec2 preview_size() {
  const ImVec2 fitted = fit_image(g_tex_w, g_tex_h);
  if (fitted.y > k_preview_max_height && fitted.y > 0.0F) {
    return ImVec2(fitted.x * k_preview_max_height / fitted.y,
                  k_preview_max_height);
  }
  return fitted;
}

/// Space reserved below the log. Measured from the previous frame rather than
/// computed: guessing the frame height and spacing got it wrong by a few pixels,
/// which pushed the controls over the log's border. Seeded generously so the
/// very first frame under-fills instead of overlapping.
float g_footer_h = 160.0F;

/// Replace the pending screenshot and refresh its preview texture (UI thread).
void set_pending(cv::Mat &&_bgr, std::string _label) {
  g_pending = std::move(_bgr);
  g_pending_label = std::move(_label);
  delete_tex(&g_tex, &g_tex_w, &g_tex_h);
  if (g_pending.empty()) {
    return;
  }
  const cv::Mat thumb = make_thumb(g_pending);
  g_tex = upload_bgr(thumb, &g_tex_w, &g_tex_h);
  if (g_tex == 0) {
    campcat::automation_log::emit("[llm] texture upload failed");
  }
}

/// Pending screenshot as a strip: thumbnail, source, native size and an "x"
/// that drops it. Callers skip this entirely when nothing is attached.
void draw_attachment_bar() {
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_bubble_rounding);
  ImGui::BeginChild("llm_attach", ImVec2(0.0F, 0.0F),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders |
                        ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(g_tex)),
               preview_size(), ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted(g_pending_label.c_str());
  ImGui::TextDisabled("%d x %d", g_pending.cols, g_pending.rows);
  ImGui::EndGroup();
  ImGui::SameLine(ImGui::GetWindowContentRegionMax().x -
                  ImGui::GetFrameHeight());
  const bool remove = ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0F));
  ImGui::EndChild();
  ImGui::PopStyleVar();
  if (remove) {
    set_pending({}, "");
  }
}

void capture_from_adb(campcat::app_config &_cfg) {
  campcat::adb_client adb(_cfg.adb_path, _cfg.adb_serial);
  const bool connected = adb.connect_remote(_cfg.adb_connect_address);
  cv::Mat cap;
  std::string diag;
  if (!adb.screencap_png(&cap, 45000, &diag)) {
    if (!connected) {
      diag += " (adb connect " + _cfg.adb_connect_address + " failed)";
    }
    campcat::automation_log::emit("[llm] screencap failed " + diag);
    return;
  }
  campcat::automation_log::emit("[llm] captured " + std::to_string(cap.cols) +
                                "x" + std::to_string(cap.rows));
  set_pending(std::move(cap), "emulator screenshot");
}

void pick_local_image() {
  auto picked = campcat::pick_open_file(std::filesystem::current_path(), "png",
                                        "Select a screenshot");
  if (!picked) {
    return;
  }
  cv::Mat img = cv::imread(picked->string(), cv::IMREAD_COLOR);
  if (img.empty()) {
    campcat::automation_log::emit("[llm] cannot read image: " +
                                  picked->string());
    return;
  }
  campcat::automation_log::emit("[llm] loaded " + picked->string());
  set_pending(std::move(img), picked->filename().string());
}

/// Queue the outgoing turn the moment it is sent, so the log shows the user's
/// message while the reply is still in flight. UI thread only, for the GL call.
void append_user_turn(const cv::Mat &_image, std::string _label,
                      std::string _prompt) {
  llm_turn t;
  t.label = std::move(_label);
  t.user_text = std::move(_prompt);
  t.pending = true;
  if (!_image.empty()) {
    const cv::Mat thumb = make_thumb(_image);
    t.tex = upload_bgr(thumb, &t.tex_w, &t.tex_h);
  }

  g_turns.push_back(std::move(t));
  while (static_cast<int>(g_turns.size()) > k_turn_cap) {
    delete_tex(&g_turns.front().tex, &g_turns.front().tex_w,
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
  g_thinking = g_host.is_thinking();
}

/// Run one turn on the worker. `_image` may be empty for a text-only turn.
void start_turn(std::filesystem::path _repo_root, cv::Mat _image,
                std::string _prompt) {
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_running.store(true);
  g_thread = std::thread([root = std::move(_repo_root), img = std::move(_image),
                          prompt = std::move(_prompt)]() {
    campcat::llm_result res = g_host.describe(root, img, prompt);
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
      campcat::automation_log::emit("[llm] ok: " + h.res.text);
    } else {
      campcat::automation_log::emit("[llm] failed: " + h.res.error);
    }
    return;
  }
  if (!h.res.ok) {
    campcat::automation_log::emit("[llm] reset failed: " + h.res.error);
    return;
  }
  for (llm_turn &t : g_turns) {
    delete_tex(&t.tex, &t.tex_w, &t.tex_h);
  }
  g_turns.clear();
  g_pin_bottom = true;
  campcat::automation_log::emit("[llm] conversation cleared");
}

/// Send whatever is staged: clear the input and hand the image to the worker.
void send_current(campcat::app_config &_cfg) {
  const std::string prompt = g_input;
  if (is_blank(g_input) && g_pending.empty()) {
    return;
  }
  cv::Mat image = g_pending;
  const std::string label = g_pending_label;

  g_input[0] = '\0';
  g_pending.release();
  g_pending_label.clear();
  delete_tex(&g_tex, &g_tex_w, &g_tex_h);

  append_user_turn(image, label, prompt);
  // Cleared from the UI thread, not just on the worker: the worker clears it
  // inside next_request_id, which runs after ensure_running may have spent a
  // minute starting the interpreter, and until then the new bubble would show
  // the previous request's leftovers.
  g_host.clear_stream();
  start_turn(_cfg.config_home.parent_path(), std::move(image), prompt);
}

/// "Thinking" (when the sidecar says so) plus 1..3 dots on the frame clock, so
/// a waiting bubble visibly ticks instead of sitting on a static "...".
std::string waiting_label() {
  const int dots = 1 + static_cast<int>(ImGui::GetTime() / 0.4) % 3;
  return std::string(g_thinking ? "Thinking" : "") + std::string(dots, '.');
}

/// Centred two-line hint for an empty conversation.
void draw_empty_state() {
  const char *title = "Ask about a screenshot";
  const char *hint = "Attach one with \"+\" or just type a question";
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
void draw_turn(const llm_turn &_t, int _index) {
  ImGui::PushID(_index);
  const float pad = ImGui::GetStyle().WindowPadding.x;
  const float max_w = ImGui::GetContentRegionAvail().x * k_bubble_frac;
  const ImVec2 fitted =
      _t.tex != 0 ? fit_image(_t.tex_w, _t.tex_h) : ImVec2(0.0F, 0.0F);

  const float uw = bubble_width(_t.user_text.c_str(), max_w, fitted.x, pad);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                       ImGui::GetContentRegionAvail().x - uw);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, k_user_bg);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, k_bubble_rounding);
  ImGui::BeginChild("u", ImVec2(uw, 0.0F),
                    ImGuiChildFlags_AutoResizeY |
                        ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar);
  if (_t.tex != 0) {
    const float room = ImGui::GetContentRegionAvail().x;
    ImVec2 shown = fitted;
    if (shown.x > room && shown.x > 0.0F) {
      shown.y *= room / shown.x;
      shown.x = room;
    }
    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(_t.tex)), shown,
                 ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F));
    if (!_t.label.empty()) {
      ImGui::TextDisabled("%s", _t.label.c_str());
    }
  }
  if (!_t.user_text.empty()) {
    ImGui::TextWrapped("%s", _t.user_text.c_str());
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // A markdown reply cannot be measured from the raw text (`**` would be
  // counted), so the bubble goes full width once there is real text; the
  // placeholder stays tight because a wide box holding "..." looks broken. It
  // is measured at its widest so the ticking dots do not resize it.
  const bool placeholder = _t.pending && _t.reply.empty();
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
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_host.stop();
  for (llm_turn &t : g_turns) {
    delete_tex(&t.tex, &t.tex_w, &t.tex_h);
  }
  g_turns.clear();
  g_thinking = false;
  delete_tex(&g_tex, &g_tex_w, &g_tex_h);
  g_pending.release();
}

void llm_ui_draw_panel(campcat::app_config &_cfg, bool _disable_capture) {
  // GL calls belong to the UI thread, so finished work is folded in here.
  consume_handoff();

  const bool busy = g_running.load();

  pump_stream();
  if (draw_log()) {
    start_reset(_cfg.config_home.parent_path());
  }

  // Top of whatever comes below the log: the reference for the footer height.
  const float footer_top = ImGui::GetCursorScreenPos().y;
  if (g_tex != 0) {
    draw_attachment_bar();
  }

  // Leaves room for the "+" button, Send, and the spacing between the three.
  ImGui::SetNextItemWidth(-156.0F);
  const bool submitted = ImGui::InputTextWithHint(
      "##llm_prompt", "Type an instruction (optional)...", g_input,
      sizeof(g_input), ImGuiInputTextFlags_EnterReturnsTrue);

  ImGui::SameLine();
  if (busy) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("+", ImVec2(32.0F, 0.0F))) {
    ImGui::OpenPopup("llm_attach_menu");
  }
  if (busy) {
    ImGui::EndDisabled();
  }
  if (ImGui::BeginPopup("llm_attach_menu")) {
    if (ImGui::MenuItem("Capture from emulator (ADB)", nullptr, false,
                        !_disable_capture)) {
      capture_from_adb(_cfg);
    }
    if (ImGui::MenuItem("Open image file...")) {
      pick_local_image();
    }
    ImGui::EndPopup();
  }

  ImGui::SameLine();
  const bool can_send = !busy && (!g_pending.empty() || !is_blank(g_input));
  if (!can_send) {
    ImGui::BeginDisabled();
  }
  const bool clicked = ImGui::Button("Send", ImVec2(104.0F, 0.0F));
  if (!can_send) {
    ImGui::EndDisabled();
  }

  if ((clicked || submitted) && can_send) {
    send_current(_cfg);
  }

  // Reserve exactly what the controls actually used, so they never creep over
  // the log on the next frame.
  g_footer_h = ImGui::GetItemRectMax().y - footer_top +
               ImGui::GetStyle().ItemSpacing.y;
}
