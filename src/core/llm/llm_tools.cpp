#include "core/llm/llm_tools.h"

#include "ccat_script/ccat_program_runner.h" // run_ccat_program
#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/script/ccat_bundle.h"  // bundle_main / bundle_names / ...
#include "core/script/ccat_parser.h" // parse_program / parse_error

// Private: <meta> and the consteval cost stay out of include/.
#include "llm_tool_schema.h"

#include <opencv2/freetype.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace campcat::llm {

namespace {

/// Set by set_context() on the worker thread; read by every tool below.
tool_context g_ctx;

/// The numbered screenshots this session can still cut a template out of: number
/// -> that frame's raw PNG. The sidecar used to keep them and attach one to a
/// call, which capped a call at one picture; here any tool can reach any frame by
/// number, so one call can cut from several.
///
/// `g_ctx.screen` stays as well, keeping the latest frame as its own copy: tap
/// and mark check coordinates against it, and the approval bubble draws on it.
/// Two copies of one picture buy leaving set_context()/m_screen() alone.
///
/// Same thread rule as g_ctx: only the worker running the sidecar exchange reads
/// this, and forget_frames() runs at session reset, where no turn is in flight.
std::map<int, screen_state> g_frames;
/// Only ever goes up, so a number the model remembers from an earlier request
/// keeps pointing at the same picture. Reset with the table.
int g_next_shot = 0;
/// The number this call produced, if it produced one. Set by `screenshot`, read
/// by dispatch() on the way out, and cleared at the top of every dispatch so one
/// call's number can never be reported for the next.
int g_last_shot = 0;

/// A script is executing (the UI polls this) and it has been asked to stop (the UI
/// writes it, the interpreter reads it). Both cross threads, hence atomics.
std::atomic<bool> g_script_running{false};
std::atomic<bool> g_script_stop{false};

/// Clears a stale stop on entry and marks the script as running until it returns.
/// The clear is not decoration: a turn can run a second script, and the stop the
/// user pressed for the first one would otherwise kill it on entry.
struct script_run_guard {
  script_run_guard() {
    g_script_stop.store(false);
    g_script_running.store(true);
  }
  ~script_run_guard() { g_script_running.store(false); }
};

/// Parses `_source` or turns the syntax error into the bind_error the model reads.
std::unique_ptr<ccat_lang::Program> parse_or_throw(const std::string &_source) {
  try {
    return ccat_lang::parse_program(_source);
  } catch (const ccat_lang::parse_error &ex) {
    throw js::bind_error("parse error: " + std::string(ex.what()) + " (line " +
                         std::to_string(ex.line) + ", col " +
                         std::to_string(ex.col) + ")");
  }
}

/// Rejects a coordinate outside the screenshot the model last saw. Rejection,
/// not clamping: a clamped tap would look successful to the model.
bool in_screen(int _x, int _y, std::string *_error) {
  if (g_ctx.screen.w <= 0 || g_ctx.screen.h <= 0) {
    *_error = "no screenshot yet; call screenshot first";
    return false;
  }
  if (_x < 0 || _y < 0 || _x >= g_ctx.screen.w || _y >= g_ctx.screen.h) {
    *_error = "(" + std::to_string(_x) + "," + std::to_string(_y) +
              ") out of range (screen " + std::to_string(g_ctx.screen.w) + "x" +
              std::to_string(g_ctx.screen.h) + ")";
    return false;
  }
  return true;
}

/// Rejects a box that is not wholly inside a picture of `_img_w` x `_img_h`.
/// The size is a parameter, not read from any single global: the box may be cut
/// out of an older screenshot, whose size need not match the screen's current
/// one. Same rejection-not-clamping rule as in_screen().
bool box_on_screen(int _img_w, int _img_h, int _x, int _y, int _w, int _h,
                   std::string *_error) {
  if (_img_w <= 0 || _img_h <= 0) {
    *_error = "no screenshot yet; call screenshot first";
    return false;
  }
  if (_w <= 0 || _h <= 0 || _x < 0 || _y < 0 || _x + _w > _img_w ||
      _y + _h > _img_h) {
    *_error = "box (" + std::to_string(_x) + "," + std::to_string(_y) + " " +
              std::to_string(_w) + "x" + std::to_string(_h) +
              ") out of range (screen " + std::to_string(_img_w) + "x" +
              std::to_string(_img_h) + ")";
    return false;
  }
  return true;
}

/// A name that can only land inside the directory it is meant for: non-empty,
/// not `.` or `..` (walking up), no separator (walking out or into a sibling),
/// and no control byte (a NUL would truncate the path in the OS call).
bool safe_leaf_name(const std::string &_s) {
  if (_s.empty() || _s == "." || _s == "..") {
    return false;
  }
  for (const unsigned char c : _s) {
    if (c == '/' || c == '\\' || c < 0x20U || c == 0x7FU) {
      return false;
    }
  }
  return true;
}

/// The known bundles, comma-separated for an error message, or "none yet".
std::string existing_bundles() {
  const std::vector<std::string> names = ccat_lang::bundle_names(g_ctx.script_dir);
  if (names.empty()) {
    return "none yet";
  }
  std::string out;
  for (const std::string &name : names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
  }
  return out;
}

/// ", "-joined, or the caller's word for "none".
std::string joined(const std::vector<std::string> &_items, const char *_none) {
  std::string out;
  for (const std::string &item : _items) {
    if (!out.empty()) {
      out += ", ";
    }
    out += item;
  }
  return out.empty() ? _none : out;
}

/// `"ok"` -> `"ok.png"`; unchanged when it already ends in `.png`.
std::string with_png_ext(const std::string &_s) {
  return _s.size() >= 4 && _s.compare(_s.size() - 4, 4, ".png") == 0 ? _s
                                                                    : _s + ".png";
}

/// The frame a call names: `_shot` picks one by number, and an omitted number
/// means the session's current picture, which is what the sidecar used to send
/// when the arguments did not ask for anything older. `*_error` is already a
/// whole sentence for the model, and names the numbers that do exist.
const screen_state *frame_for(const std::optional<int> &_shot, std::string *_error) {
  if (!_shot.has_value()) {
    if (g_ctx.screen.png.empty()) {
      *_error = "no screenshot yet; call screenshot first";
      return nullptr;
    }
    return &g_ctx.screen;
  }
  const auto it = g_frames.find(*_shot);
  if (it == g_frames.end()) {
    *_error = "no screenshot #" + std::to_string(*_shot) + " in this session (" +
              (g_next_shot > 0 ? "frames so far: 1.." + std::to_string(g_next_shot)
                               : std::string("none taken yet")) +
              ")";
    return nullptr;
  }
  return &it->second;
}

/// Decodes the frame a call names. `*_error` is left empty on success.
bool frame_mat(const std::optional<int> &_shot, cv::Mat *_out, std::string *_error) {
  const screen_state *frame = frame_for(_shot, _error);
  if (frame == nullptr) {
    return false;
  }
  const cv::Mat buf(1, static_cast<int>(frame->png.size()), CV_8UC1,
                    const_cast<char *>(frame->png.data()));
  *_out = cv::imdecode(buf, cv::IMREAD_COLOR);
  if (_out->empty()) {
    // Only reachable if a stored PNG got corrupted, which no tool can do.
    *_error = "screenshot #" + std::to_string(_shot.value_or(0)) +
              " is not a decodable picture";
    return false;
  }
  return true;
}

/// Validates a new bundle name and refuses one that is taken.
std::unique_ptr<ccat_lang::Program> check_new_bundle(const std::string &_name,
                                                     const std::string &_source) {
  if (!safe_leaf_name(_name)) {
    throw js::bind_error("name \"" + _name + "\" is not a plain file name");
  }
  std::unique_ptr<ccat_lang::Program> prog = parse_or_throw(_source);
  std::error_code ec;
  if (std::filesystem::exists(ccat_lang::bundle_main(g_ctx.script_dir, _name), ec)) {
    throw js::bind_error("script bundle \"" + _name +
                         "\" already exists; pick another name (existing: " +
                         existing_bundles() + ")");
  }
  return prog;
}

/// Validates a template target and hands back the picture to cut. The box is
/// checked against that picture's own size, not the screen's, because the
/// picture may be an older frame of a different size.
///
/// `*_frame_png` receives that same frame's raw PNG -- the approval bubble draws
/// on the frame being cut, not on the current screen, so seeing it is how a human
/// notices a box aimed at the wrong one. Handed back here so the caller does not
/// have to look the frame up a second time and hope it is still there.
cv::Mat check_template_target(const std::string &_bundle, const std::string &_name,
                              const rect &_box, const std::optional<int> &_shot,
                              std::string *_file, std::string *_frame_png) {
  if (!safe_leaf_name(_bundle)) {
    throw js::bind_error("bundle \"" + _bundle + "\" is not a plain file name");
  }
  if (!safe_leaf_name(_name)) {
    throw js::bind_error("name \"" + _name + "\" is not a plain file name");
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(ccat_lang::bundle_main(g_ctx.script_dir, _bundle), ec)) {
    throw js::bind_error("unknown script bundle \"" + _bundle +
                         "\"; save_script first (existing: " + existing_bundles() +
                         ")");
  }
  *_file = with_png_ext(_name);
  std::string error;
  const screen_state *frame = frame_for(_shot, &error);
  if (frame == nullptr) {
    throw js::bind_error(error);
  }
  const cv::Mat buf(1, static_cast<int>(frame->png.size()), CV_8UC1,
                    const_cast<char *>(frame->png.data()));
  cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
  if (img.empty()) {
    // Only reachable if a stored PNG got corrupted, which no tool can do.
    throw js::bind_error("the stored screenshot is not a decodable picture");
  }
  if (!box_on_screen(img.cols, img.rows, _box.x, _box.y, _box.w, _box.h, &error)) {
    throw js::bind_error(error);
  }
  *_frame_png = frame->png;
  return img;
}

/// How a template request names the frame it cuts from, for the approval bubble.
std::string shot_phrase(const std::optional<int> &_shot) {
  return _shot.has_value() ? "screenshot #" + std::to_string(*_shot)
                           : "the latest screenshot";
}

/// One template an update will cut, with the crop already taken: everything the
/// human is about to be asked about has been checked and cut by the time an
/// approval request exists.
struct pending_cut {
  std::string file;
  int shot;
  rect box;
  cv::Mat crop;
  /// The frame being cut, raw, for the approval bubble.
  std::string frame_png;
};

/// Collects the template operands whose missing file fails the script: `tap`,
/// `tap_offset`, `swipe` (both ends) and `wait_until`. Deliberately not the `if`
/// and do-while conditions -- a missing template is just a false condition there
/// -- and not `run()`'s target, which is another .ccat resolved at run time.
///
/// ponytail: a statement type carrying a template must be added here; the
/// language only grows in the parser, so nothing can appear unseen.
void collect_required_templates(const ccat_lang::Stmt &_s,
                               std::vector<std::string> *_out) {
  using namespace ccat_lang;
  if (const auto *b = dynamic_cast<const BlockStmt *>(&_s)) {
    for (const auto &child : b->body) {
      collect_required_templates(*child, _out);
    }
  } else if (const auto *i = dynamic_cast<const IfStmt *>(&_s)) {
    if (i->then_branch) {
      collect_required_templates(*i->then_branch, _out);
    }
    if (i->else_branch) {
      collect_required_templates(*i->else_branch, _out);
    }
  } else if (const auto *r = dynamic_cast<const RetryStmt *>(&_s)) {
    if (r->body) {
      collect_required_templates(*r->body, _out);
    }
  } else if (const auto *d = dynamic_cast<const DoWhileStmt *>(&_s)) {
    if (d->body) {
      collect_required_templates(*d->body, _out);
    }
  } else if (const auto *l = dynamic_cast<const LoopStmt *>(&_s)) {
    if (l->body) {
      collect_required_templates(*l->body, _out);
    }
  } else if (const auto *t = dynamic_cast<const TapStmt *>(&_s)) {
    _out->push_back(t->image_path);
  } else if (const auto *t = dynamic_cast<const TapOffsetStmt *>(&_s)) {
    _out->push_back(t->image_path);
  } else if (const auto *s = dynamic_cast<const SwipeTemplatesStmt *>(&_s)) {
    _out->push_back(s->from_image_path);
    _out->push_back(s->to_image_path);
  } else if (const auto *w = dynamic_cast<const WaitUntilStmt *>(&_s)) {
    _out->push_back(w->image_path);
  }
}

adb_client &require_adb() {
  if (g_ctx.adb == nullptr) {
    throw js::bind_error("no adb session");
  }
  return *g_ctx.adb;
}

/// The CJK face the UI itself renders Chinese with, reused so a label looks the
/// same in the screenshot as it does in the chat.
constexpr char k_cjk_font_path[] = "/System/Library/Fonts/Hiragino Sans GB.ttc";
constexpr int k_label_font_h = 28;

/// Draws `_text` with its top-left at (`_x`, `_y`). `cv::putText` knows ASCII
/// only, so a Chinese label would come out as boxes; the freetype module draws
/// UTF-8 from a real font, and ASCII too, so every label looks the same. A
/// missing font or a build without the module falls back to `cv::putText`.
void draw_label(cv::Mat &_img, const std::string &_text, int _x, int _y,
                const cv::Scalar &_color) {
  try {
    cv::Ptr<cv::freetype::FreeType2> ft = cv::freetype::createFreeType2();
    ft->loadFontData(k_cjk_font_path, 0);
    ft->putText(_img, _text, cv::Point(_x, _y), k_label_font_h, _color, 2,
                cv::LINE_AA, false);
    return;
  } catch (const cv::Exception &) {
    // No font or no module: the box still marks the spot, so fall through.
  }
  cv::putText(_img, _text, cv::Point(_x, _y), cv::FONT_HERSHEY_SIMPLEX, 0.8,
              _color, 2);
}

} // namespace

namespace tools {

/// Swipe duration used when the model omits it. Declared as an optional
/// parameter rather than a C++ default argument: reflection can detect a
/// default argument but cannot read its value, so a missing key could not be
/// filled in at dispatch time.
constexpr int k_default_swipe_ms = 300;

[[= js::image_result]]
[[= js::doc{.text = js::str("Takes a fresh screenshot of the device screen.")}]]
std::string screenshot() {
  adb_client &adb = require_adb();
  cv::Mat bgr;
  std::string diag;
  if (!adb.screencap_png(&bgr, 45000, &diag)) {
    throw js::bind_error("screencap failed: " + diag);
  }
  std::vector<unsigned char> png;
  if (!cv::imencode(".png", bgr, png)) {
    throw js::bind_error("png encode failed");
  }
  // The approval bubble shows this same image, so keep the raw PNG: base64 is
  // the wire's problem, and the host encodes once on the way out. The size goes
  // with it, session-scoped rather than turn-scoped: every frame stays addressable
  // by `shot`, so a later turn may be tapping at coordinates taken from one.
  g_ctx.screen = screen_state{bgr.cols, bgr.rows, std::string(png.begin(), png.end())};
  g_frames[++g_next_shot] = g_ctx.screen;
  g_last_shot = g_next_shot;
  return g_ctx.screen.png;
}
CPP_REFLECT_TOOL(screenshot)

[[= js::image_result]]
[[= js::doc{.text = js::str("Draws a box on the last screenshot and returns the marked image, "
                            "so the user can see where you think something is. Nothing is "
                            "tapped and nothing needs approval; the box lives in the picture.")}]]
[[= js::param_docs(js::str("Short label for what the box marks, e.g. \"the login button\"."),
                   js::str("Left edge X of the box, in screenshot pixels."),
                   js::str("Top edge Y of the box, in screenshot pixels."),
                   js::str("Width of the box in pixels."),
                   js::str("Height of the box in pixels."))]]
std::string mark(std::string label, int x, int y, int w, int h) {
  std::string error;
  if (!box_on_screen(g_ctx.screen.w, g_ctx.screen.h, x, y, w, h, &error)) {
    throw js::bind_error(error);
  }
  const cv::Mat buf(1, static_cast<int>(g_ctx.screen.png.size()), CV_8UC1,
                    const_cast<char *>(g_ctx.screen.png.data()));
  cv::Mat bgr = cv::imdecode(buf, cv::IMREAD_COLOR);
  const cv::Scalar color(60, 200, 255); // BGR, near-orange against most UIs
  cv::rectangle(bgr, cv::Rect(x, y, w, h), color, 4);
  if (!label.empty()) {
    // Above the box, or below it when the box hugs the top edge.
    const int ty = y >= k_label_font_h + 4 ? y - k_label_font_h - 2 : y + h + 4;
    draw_label(bgr, label, x, ty, color);
  }
  std::vector<unsigned char> png;
  if (!cv::imencode(".png", bgr, png)) {
    throw js::bind_error("png encode failed");
  }
  return std::string(png.begin(), png.end());
}
CPP_REFLECT_TOOL(mark)

[[= js::doc{.text = js::str("Taps the element at this box, in screenshot pixels. While "
                            "human approval is on, the box is shown to the user and "
                            "nothing is tapped unless they approve it. The tap lands at "
                            "the box centre. Nothing else needs a prior call.")}]]
[[= js::param_docs(js::str("Left edge X of the element box, in screenshot pixels."),
                   js::str("Top edge Y of the element box, in screenshot pixels."),
                   js::str("Width of the box in pixels."),
                   js::str("Height of the box in pixels."))]]
std::string tap(int x, int y, int w, int h) {
  std::string error;
  if (!box_on_screen(g_ctx.screen.w, g_ctx.screen.h, x, y, w, h, &error)) {
    throw js::bind_error(error);
  }
  if (g_ctx.require_approval) {
    if (!g_ctx.request_approval) {
      throw js::bind_error("no human available to approve the tap");
    }
    llm::approval_request req;
    req.tool = "tap";
    req.summary = "Tap the highlighted element (" + std::to_string(w) + "x" +
                  std::to_string(h) + " at " + std::to_string(x) + "," +
                  std::to_string(y) + ")";
    req.highlight.push_back(rect{x, y, w, h});
    req.screen_png = g_ctx.screen.png;
    const llm::approval_decision decision = g_ctx.request_approval(req);
    if (!decision.approved) {
      // Empty guidance keeps this string byte-for-byte the same as the bare refusal.
      throw js::bind_error(decision.guidance.empty()
                               ? "user rejected the tap"
                               : "user rejected the tap: " + decision.guidance);
    }
  }
  // The tap lands at the box centre: the model marks the element, this picks the
  // point. It used to send the centre as a separate tap() argument every time.
  const int cx = x + w / 2;
  const int cy = y + h / 2;
  if (!require_adb().tap(cx, cy)) {
    throw js::bind_error("adb tap failed");
  }
  return "tapped " + std::to_string(cx) + "," + std::to_string(cy);
}
CPP_REFLECT_TOOL(tap)

[[= js::doc{.text = js::str("Runs a .ccat script on the device and returns what happened. "
                            "While human approval is on, the script is shown to the user "
                            "and nothing runs unless they approve it. Call ccat_help "
                            "for the language. The sandbox starts empty, so a statement that "
                            "needs a template PNG fails until save_template puts one in the "
                            "bundle (an `if` on a missing PNG is just false); tap_at and "
                            "swipe_at need no template, and run(\"<name>/main.ccat\") runs a "
                            "saved bundle.")}]]
[[= js::param_docs(js::str("The full .ccat script source to run."))]]
std::string run_script(std::string source) {
  // Parse first: a syntax error answers the model directly instead of asking a
  // human to read a script that cannot run.
  const std::unique_ptr<ccat_lang::Program> prog = parse_or_throw(source);
  if (g_ctx.require_approval) {
    if (!g_ctx.request_approval) {
      throw js::bind_error("no human available to approve running the script");
    }
    llm::approval_request req;
    req.tool = "run_script";
    req.summary = source; // the thing being approved is the script itself
    const llm::approval_decision decision = g_ctx.request_approval(req);
    if (!decision.approved) {
      throw js::bind_error(decision.guidance.empty()
                               ? "user rejected running the script"
                               : "user rejected running the script: " + decision.guidance);
    }
  }
  // Checked before adb so a refusal above is the reason reported, and so that
  // path is testable without a device.
  adb_client &adb = require_adb();
  if (g_ctx.cfg == nullptr) {
    throw js::bind_error("no shell config for the script tools");
  }
  script_run_guard guard;
  const auto res = run_ccat_program(&adb, g_ctx.cfg, g_ctx.script_dir, {}, *prog,
                                    [] { return g_script_stop.load(); });
  if (!res.ok) {
    throw js::bind_error(res.message.empty() ? std::string("script failed")
                                             : res.message);
  }
  return "script ran ok (" + std::to_string(prog->stmts.size()) + " statements)";
}
CPP_REFLECT_TOOL(run_script)

[[= js::doc{.text = js::str("Saves a .ccat script as a reusable bundle under the LLM script root: writes "
                            "main.ccat next to the template PNGs that script uses. While human approval is "
                            "on, the name and source are shown to the user and nothing is written unless "
                            "they approve it. It does not run anything; run it from a script with "
                            "run(\"<name>/main.ccat\").")}]]
[[= js::param_docs(js::str("Bundle name: a leaf name describing what the script does, e.g. \"claim_reward\"."),
                   js::str("The full .ccat script source to save."))]]
std::string save_script(std::string name, std::string source) {
  const std::unique_ptr<ccat_lang::Program> prog =
      check_new_bundle(name, source);
  if (g_ctx.require_approval) {
    if (!g_ctx.request_approval) {
      throw js::bind_error("no human available to approve saving the script");
    }
    llm::approval_request req;
    req.tool = "save_script";
    // The name goes in the summary: it is what the human has to judge, and the
    // sandbox root is already known to whoever is looking at the bubble.
    req.summary = "Save script bundle \"" + name + "\" as main.ccat:\n\n" + source;
    const llm::approval_decision decision = g_ctx.request_approval(req);
    if (!decision.approved) {
      throw js::bind_error(decision.guidance.empty()
                               ? "user rejected saving the script"
                               : "user rejected saving the script: " + decision.guidance);
    }
  }
  const std::filesystem::path path = ccat_lang::bundle_main(g_ctx.script_dir, name);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(source.data(), static_cast<std::streamsize>(source.size()));
  out.close();
  if (!out) {
    throw js::bind_error("could not write " + name + "/main.ccat (does " +
                         g_ctx.script_dir.string() + " exist and allow writes?)");
  }
  return "saved \"" + name + "/main.ccat\" (" +
         std::to_string(prog->stmts.size()) + " statements, " +
         std::to_string(source.size()) +
         " chars); put its templates in the same bundle with save_template, and "
         "run it from a script with run(\"" + name + "/main.ccat\")";
}
CPP_REFLECT_TOOL(save_script)

[[= js::doc{.text = js::str("Cuts a template PNG, out of one of the screenshots you have taken, into "
                            "an existing script bundle, so that bundle's script can use it with "
                            "tap(\"name.png\"). While human approval is on, the box is shown to the user and "
                            "nothing is cut unless they approve it. It cuts from the picture you "
                            "looked at, so it can only match as well as that picture does: screenshot right "
                            "before you cut, and crop tight around the part that stays put.")}]]
[[= js::param_docs(js::str("Bundle to put the template in; it must already exist (see save_script)."),
                   js::str("Template name, a leaf name; \".png\" is appended when missing."),
                   js::str("Which screenshot to cut from: the number its caption showed. Omit for the most "
                           "recent one."),
                   js::str("Left edge X of the crop box, in that screenshot's pixels."),
                   js::str("Top edge Y of the crop box, in that screenshot's pixels."),
                   js::str("Width of the crop in pixels."),
                   js::str("Height of the crop in pixels."))]]
std::string save_template(std::string bundle, std::string name,
                          std::optional<int> shot, int x, int y, int w, int h) {
  const rect box{x, y, w, h};
  std::string file;
  std::string frame_png;
  const cv::Mat img = check_template_target(bundle, name, box, shot, &file, &frame_png);
  if (g_ctx.require_approval) {
    if (!g_ctx.request_approval) {
      throw js::bind_error("no human available to approve saving the template");
    }
    llm::approval_request req;
    req.tool = "save_template";
    req.summary = "Save template \"" + bundle + "/" + file + "\" from " +
                  shot_phrase(shot) + " at (" + std::to_string(x) + "," +
                  std::to_string(y) + ") " + std::to_string(w) + "x" +
                  std::to_string(h);
    req.highlight.push_back(box);
    // The frame the model named, not the current screen: that is what is being cut,
    // and seeing it is how the human notices a box aimed at the wrong one.
    req.screen_png = frame_png;
    const llm::approval_decision decision = g_ctx.request_approval(req);
    if (!decision.approved) {
      throw js::bind_error(decision.guidance.empty()
                               ? "user rejected saving the template"
                               : "user rejected saving the template: " + decision.guidance);
    }
  }
  const std::filesystem::path path = g_ctx.script_dir / bundle / file;
  std::error_code ec;
  const bool existed = std::filesystem::exists(path, ec);
  try {
    // Overwriting is allowed on purpose: a template is normally cut a few times
    // before it is tight, and refusing would push the model to invent new names
    // and rewrite the script, costing another approval of the whole script.
    if (!cv::imwrite(path.string(), img(cv::Rect(x, y, w, h)))) {
      throw js::bind_error("could not write " + bundle + "/" + file);
    }
  } catch (const cv::Exception &e) {
    throw js::bind_error("could not write " + bundle + "/" + file + ": " + e.what());
  }
  return std::string(existed ? "overwrote" : "saved") + " template \"" + bundle +
         "/" + file + "\" (" + std::to_string(w) + "x" + std::to_string(h) +
         " at " + std::to_string(x) + "," + std::to_string(y) + "); use it as tap(\"" +
         file + "\") inside that bundle's script";
}
CPP_REFLECT_TOOL(save_template)

[[= js::quiet{.text = js::str("Read CCAT.md")}]]
[[= js::doc{.text = js::str("Returns the full reference for the .ccat scripting language. "
                            "Call it before writing a script with run_script.")}]]
std::string ccat_help() {
  if (g_ctx.cfg == nullptr) {
    throw js::bind_error("no shell config; cannot locate CCAT.md");
  }
  const std::filesystem::path path =
      g_ctx.cfg->config_home.parent_path() / "CCAT.md";
  // Read per call, no cache: a function-local static would freeze whichever
  // g_ctx the first call happened to see, and this tool is called rarely enough
  // that the read costs nothing.
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // Loud, with the path: an empty string would read as "the language is this
    // small", which is worse than a failure.
    throw js::bind_error("CCAT.md not found at " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}
CPP_REFLECT_TOOL(ccat_help)

[[= js::doc{.text = js::str("Lists the saved .ccat skill bundles, one \"name -- description\" "
                            "line each. Call this before writing a new skill: if a bundle "
                            "already does the job, use it with run(\"<name>/main.ccat\") "
                            "instead of writing it again, and call read_script to see how it "
                            "works or update_script to change it. The description is the "
                            "first // comment line of that bundle's main.ccat.")}]]
std::string list_scripts() {
  const std::vector<std::string> names = ccat_lang::bundle_names(g_ctx.script_dir);
  if (names.empty()) {
    return "none yet";
  }
  std::string out;
  for (const std::string &name : names) {
    const std::string description = ccat_lang::bundle_description(g_ctx.script_dir, name);
    if (!out.empty()) {
      out += "\n";
    }
    out += name + " -- " +
           (description.empty() ? std::string("(no description)") : description);
  }
  return out;
}
CPP_REFLECT_TOOL(list_scripts)

[[= js::quiet{.text = js::str("Read script {name}")}]]
[[= js::doc{.text = js::str("Returns a saved bundle's main.ccat exactly as written, prefixed by a "
                            "header naming the bundle and listing its template PNGs. The "
                            "source is what you pass back as update_script's source, so edit "
                            "it from what this returns rather than from memory.")}]]
[[= js::param_docs(js::str("Bundle name, as list_scripts showed it."))]]
std::string read_script(std::string name) {
  if (!safe_leaf_name(name)) {
    throw js::bind_error("name \"" + name + "\" is not a plain file name");
  }
  const std::filesystem::path path = ccat_lang::bundle_main(g_ctx.script_dir, name);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    throw js::bind_error("unknown script bundle \"" + name +
                         "\"; save_script first (existing: " + existing_bundles() + ")");
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw js::bind_error("could not read " + path.string());
  }
  const std::string source{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
  const std::string description = ccat_lang::bundle_description(g_ctx.script_dir, name);
  return "bundle \"" + name + "\": " +
         (description.empty() ? std::string("(no description)") : description) +
         "\ntemplates: " + joined(ccat_lang::bundle_templates(g_ctx.script_dir, name), "none") + "\n\n" + source;
}
CPP_REFLECT_TOOL(read_script)

/// A box to cut a template from, in the pixels of frame `shot`. Members are
/// reflected, so these names are the JSON keys the model writes.
struct box {
  [[= js::doc{.text = js::str("Which screenshot to cut from: the number its caption showed.")}]]
  int shot = 0;
  [[= js::doc{.text = js::str("Left edge X, in that screenshot's pixels.")}]]
  int x = 0;
  [[= js::doc{.text = js::str("Top edge Y, in that screenshot's pixels.")}]]
  int y = 0;
  [[= js::doc{.text = js::str("Width of the crop in pixels.")}]]
  int w = 0;
  [[= js::doc{.text = js::str("Height of the crop in pixels.")}]]
  int h = 0;
};

/// One entry of the template set an update declares: a name, and either a box to
/// cut that name from or nothing to keep the PNG already in the bundle. The
/// optional is what makes "keep" expressible -- a single update cannot re-cut
/// every template, since their screens are not all on one screenshot.
struct template_cut {
  [[= js::doc{.text = js::str("Template name, a leaf name; \".png\" is appended when missing.")}]]
  std::string name;
  [[= js::doc{.text = js::str("Omit to keep the template already in the bundle; give it to re-cut "
                              "that template from the named screenshot.")}]]
  std::optional<box> cut;
};

[[= js::doc{.text = js::str("Replaces an existing bundle's main.ccat and its whole template set in "
                            "one step, under one approval. Declare every template the updated "
                            "script should have afterwards: an entry with a cut is re-cut from that "
                            "screenshot, an entry with just a name keeps the PNG already there, and "
                            "PNGs you do not declare are deleted. Prefer this over saving a new "
                            "name when changing a skill you already have; save_script is for a new "
                            "one, and save_template for adjusting a single template while you work "
                            "out its box.")}]]
[[= js::param_docs(js::str("Bundle to update; it must already exist (see list_scripts)."),
                   js::str("The new main.ccat source, replacing the old one."),
                   js::str("Every template the bundle should have afterwards."))]]
std::string update_script(std::string name, std::string source,
                          std::vector<template_cut> templates) {
  if (!safe_leaf_name(name)) {
    throw js::bind_error("name \"" + name + "\" is not a plain file name");
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(ccat_lang::bundle_main(g_ctx.script_dir, name), ec)) {
    throw js::bind_error("unknown script bundle \"" + name +
                         "\"; save_script creates a new one (existing: " +
                         existing_bundles() + ")");
  }
  // Parsed first: a syntax error answers the model directly, rather than asking
  // a human to read a script that cannot run.
  const std::unique_ptr<ccat_lang::Program> prog = parse_or_throw(source);

  // The declared names, checked before anything is resolved against the disk, so
  // a duplicate is reported as a duplicate and not as whatever the first entry
  // happened to be missing.
  std::vector<std::string> declared;
  for (const template_cut &entry : templates) {
    const std::string file = with_png_ext(entry.name);
    if (!safe_leaf_name(file)) {
      throw js::bind_error("template name \"" + entry.name +
                           "\" is not a plain file name");
    }
    if (std::find(declared.begin(), declared.end(), file) != declared.end()) {
      throw js::bind_error("template \"" + file +
                           "\" is declared twice; one entry per template");
    }
    declared.push_back(file);
  }

  // A template operand that fails the script has to be declared, or the update
  // would write a script the bundle cannot run. Conditions are not operands (a
  // missing template is just false there), and neither is an absolute path,
  // which is not a file in the bundle.
  std::vector<std::string> operands;
  for (const auto &stmt : prog->stmts) {
    collect_required_templates(*stmt, &operands);
  }
  std::vector<std::string> missing;
  for (const std::string &operand : operands) {
    const std::string file = with_png_ext(operand);
    if (operand.empty() || std::filesystem::path(operand).is_absolute() ||
        std::find(declared.begin(), declared.end(), file) != declared.end() ||
        std::find(missing.begin(), missing.end(), file) != missing.end()) {
      continue;
    }
    missing.push_back(file);
  }
  if (!missing.empty()) {
    throw js::bind_error("the new source needs template(s) this update does not "
                         "declare: " + joined(missing, "") +
                         "; add them to templates (with a cut to cut them, or to "
                         "keep the PNG already in the bundle)");
  }

  // Now the disk work: cut what is to be cut, and confirm what is to be kept is
  // there. Every box is checked here, so a bad one costs nobody a look at the
  // bubble.
  std::vector<pending_cut> cuts;
  std::vector<std::string> kept;
  for (const template_cut &entry : templates) {
    const std::string file = with_png_ext(entry.name);
    if (!entry.cut.has_value()) {
      if (!std::filesystem::is_regular_file(g_ctx.script_dir / name / file, ec)) {
        throw js::bind_error("template \"" + file +
                             "\" has no cut and is not in the bundle, so there is "
                             "nothing to keep");
      }
      kept.push_back(file);
      continue;
    }
    const box &b = *entry.cut;
    pending_cut cut;
    std::string checked;
    const rect box_in{b.x, b.y, b.w, b.h};
    const cv::Mat frame =
        check_template_target(name, entry.name, box_in, b.shot, &checked, &cut.frame_png);
    cut.file = checked;
    cut.shot = b.shot;
    cut.box = box_in;
    cut.crop = frame(cv::Rect(b.x, b.y, b.w, b.h));
    cuts.push_back(std::move(cut));
  }

  // Everything left in the bundle that is not declared goes, which is what makes
  // the declared set the bundle's template set.
  std::vector<std::string> to_delete;
  for (const std::string &file : ccat_lang::bundle_templates(g_ctx.script_dir, name)) {
    if (std::find(declared.begin(), declared.end(), file) == declared.end()) {
      to_delete.push_back(file);
    }
  }

  if (g_ctx.require_approval) {
    if (!g_ctx.request_approval) {
      throw js::bind_error("no human available to approve updating the script");
    }
    llm::approval_request req;
    req.tool = "update_script";
    req.summary = "Update script \"" + name + "\" (replace main.ccat and its "
                  "templates)\n\n";
    for (const pending_cut &cut : cuts) {
      req.summary += "cut \"" + cut.file + "\" from screenshot #" +
                     std::to_string(cut.shot) + " at (" +
                     std::to_string(cut.box.x) + "," + std::to_string(cut.box.y) +
                     ") " + std::to_string(cut.box.w) + "x" +
                     std::to_string(cut.box.h) + "\n";
    }
    req.summary += "keep: " + joined(kept, "none") + "\n";
    req.summary += "delete: " + joined(to_delete, "none") + "\n\n";
    req.summary += source;
    // One picture fits in the bubble, so it is the frame the first cut comes
    // from, with only the boxes taken from that same frame drawn on it. Any
    // cuts from other frames are named in the summary above.
    if (!cuts.empty()) {
      req.screen_png = cuts.front().frame_png;
      for (const pending_cut &cut : cuts) {
        if (cut.shot == cuts.front().shot) {
          req.highlight.push_back(cut.box);
        }
      }
    }
    const llm::approval_decision decision = g_ctx.request_approval(req);
    if (!decision.approved) {
      throw js::bind_error(decision.guidance.empty()
                               ? "user rejected updating the script"
                               : "user rejected updating the script: " + decision.guidance);
    }
  }

  // Approved: the script first, then the templates it names, then the leftovers.
  // Nothing above this point touched the disk, so a refusal leaves it byte-for-byte
  // what it was.
  const std::filesystem::path main_path = ccat_lang::bundle_main(g_ctx.script_dir, name);
  {
    std::ofstream out(main_path, std::ios::binary | std::ios::trunc);
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    out.close();
    if (!out) {
      throw js::bind_error("could not write " + name + "/main.ccat");
    }
  }
  for (const pending_cut &cut : cuts) {
    const std::filesystem::path path = g_ctx.script_dir / name / cut.file;
    try {
      if (!cv::imwrite(path.string(), cut.crop)) {
        throw js::bind_error("could not write " + name + "/" + cut.file);
      }
    } catch (const cv::Exception &e) {
      throw js::bind_error("could not write " + name + "/" + cut.file + ": " + e.what());
    }
  }
  for (const std::string &file : to_delete) {
    std::filesystem::remove(g_ctx.script_dir / name / file, ec);
    if (ec) {
      throw js::bind_error("could not delete " + name + "/" + file + ": " + ec.message());
    }
  }
  return "updated \"" + name + "/main.ccat\" (" +
         std::to_string(prog->stmts.size()) + " statements, " +
         std::to_string(source.size()) + " chars); templates: " +
         std::to_string(cuts.size()) + " cut, " + std::to_string(kept.size()) +
         " kept, " + std::to_string(to_delete.size()) + " deleted; run it with run(\"" +
         name + "/main.ccat\")";
}
CPP_REFLECT_TOOL(update_script)

// ponytail: deliberately not registered -- swipe is unverified on a real device.
// Add `CPP_REFLECT_TOOL(swipe)` back once it has been tried there; until then the
// model never sees it and `dispatch` answers "unknown tool".
[[= js::doc{.text = js::str("Swipes between two points, in screenshot pixels.")}]]
[[= js::param_docs(js::str("Start X coordinate."), js::str("Start Y coordinate."),
                   js::str("End X coordinate."), js::str("End Y coordinate."),
                   js::str("Duration in milliseconds; omit for the default."))]]
std::string swipe(int x1, int y1, int x2, int y2, std::optional<int> duration_ms) {
  const int ms = duration_ms.value_or(k_default_swipe_ms);
  if (ms < 1 || ms > 10000) {
    throw js::bind_error("duration_ms out of range (1..10000)");
  }
  std::string error;
  if (!in_screen(x1, y1, &error) || !in_screen(x2, y2, &error)) {
    throw js::bind_error(error);
  }
  if (!require_adb().swipe(x1, y1, x2, y2, ms)) {
    throw js::bind_error("adb swipe failed");
  }
  return "swiped " + std::to_string(x1) + "," + std::to_string(y1) + " -> " +
         std::to_string(x2) + "," + std::to_string(y2) + " over " + std::to_string(ms) + "ms";
}

} // namespace tools

void set_context(const tool_context &_ctx) {
  g_ctx = _ctx;
  // Only the picture a call arrived with is the turn's; the screenshot in it and
  // the approvals are session-scoped (see spec §3), so they are not cleared here.
}

void current_screen(screen_state *_out) { *_out = g_ctx.screen; }

std::string tools_json() { return js::tools_json(); }

bool script_running() { return g_script_running.load(); }

void request_script_stop() { g_script_stop.store(true); }

void forget_frames() {
  g_frames.clear();
  g_next_shot = 0;
}

/// Fills a quiet note's `{param}` names from the call's arguments. Which names
/// are legal was settled when the tool registered (see `js::quiet`), and the
/// binder has since either bound the argument or failed the call, so a name that
/// never arrives here is one the schema rejected. A value that is not text or a
/// number is left as written: the log is one line, and a whole array in it reads
/// worse than the placeholder does.
std::string fill_quiet_note(std::string_view _note, const nlohmann::json &_args) {
  std::string out;
  std::size_t at = 0;
  while (at < _note.size()) {
    const std::size_t open = _note.find('{', at);
    if (open == std::string_view::npos) {
      break;
    }
    const std::size_t close = _note.find('}', open);
    if (close == std::string_view::npos) {
      break; // unclosed: not a placeholder, so print the rest as written
    }
    out.append(_note.substr(at, open - at));
    const auto it = _args.find(std::string(_note.substr(open + 1, close - open - 1)));
    if (it != _args.end() && (it->is_string() || it->is_number() || it->is_boolean())) {
      out += it->is_string() ? it->get<std::string>() : it->dump();
    } else {
      out.append(_note.substr(open, close - open + 1));
    }
    at = close + 1;
  }
  out.append(_note.substr(at));
  return out;
}

tool_reply dispatch(std::string_view _name, std::string_view _args_json) {
  tool_reply out;
  // Cleared before the busy check, so no early return can leave a frame number
  // behind for the next call to report as its own.
  g_last_shot = 0;
  if (g_ctx.adb_busy && g_ctx.adb_busy()) {
    out.error = "adb busy: an automation cycle is running";
    return out;
  }
  const nlohmann::json args = nlohmann::json::parse(_args_json, nullptr, false);
  if (args.is_discarded() || !args.is_object()) {
    out.error = "arguments must be a JSON object";
    return out;
  }
  for (const auto &entry : js::tool_registry()) {
    if (entry.name != _name) {
      continue;
    }
    try {
      const std::string result = entry.run(args);
      if (entry.returns_image) {
        out.image_png = result;
      } else {
        out.text = result;
      }
      out.ok = true;
      out.quiet_note = fill_quiet_note(entry.quiet_note, args);
      out.shot = g_last_shot;
    } catch (const std::exception &e) {
      out.error = e.what();
    }
    return out;
  }
  out.error = "unknown tool: " + std::string(_name);
  return out;
}

} // namespace campcat::llm
