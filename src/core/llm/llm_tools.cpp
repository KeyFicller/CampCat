#include "core/llm/llm_tools.h"

#include "core/adb_client.h"

// Private: <meta> and the consteval cost stay out of include/.
#include "llm_tool_schema.h"

#include <opencv2/freetype.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace campcat::llm {

namespace {

/// Set by set_context() on the worker thread; read by every tool below.
tool_context g_ctx;

/// The last screenshot's raw PNG bytes, for the approval bubble to display. Written
/// by screenshot(), read by request_tap(): same worker thread, so no lock.
std::string g_last_screen_png;

/// The element box the human approved. Consumed by the first tap that lands in it;
/// cleared every turn by set_context().
std::optional<rect> g_grant;

/// Rejects a coordinate outside the screenshot the model last saw. Rejection,
/// not clamping: a clamped tap would look successful to the model.
bool in_screen(int _x, int _y, std::string *_error) {
  if (g_ctx.screen_w <= 0 || g_ctx.screen_h <= 0) {
    *_error = "no screenshot yet this turn; call screenshot first";
    return false;
  }
  if (_x < 0 || _y < 0 || _x >= g_ctx.screen_w || _y >= g_ctx.screen_h) {
    *_error = "(" + std::to_string(_x) + "," + std::to_string(_y) +
              ") out of range (screen " + std::to_string(g_ctx.screen_w) + "x" +
              std::to_string(g_ctx.screen_h) + ")";
    return false;
  }
  return true;
}

/// Rejects a box that is not wholly inside the screenshot the model last saw.
/// Same rejection-not-clamping rule as in_screen().
bool box_on_screen(int _x, int _y, int _w, int _h, std::string *_error) {
  if (g_ctx.screen_w <= 0 || g_ctx.screen_h <= 0) {
    *_error = "no screenshot yet this turn; call screenshot first";
    return false;
  }
  if (_w <= 0 || _h <= 0 || _x < 0 || _y < 0 || _x + _w > g_ctx.screen_w ||
      _y + _h > g_ctx.screen_h) {
    *_error = "box (" + std::to_string(_x) + "," + std::to_string(_y) + " " +
              std::to_string(_w) + "x" + std::to_string(_h) +
              ") out of range (screen " + std::to_string(g_ctx.screen_w) + "x" +
              std::to_string(g_ctx.screen_h) + ")";
    return false;
  }
  return true;
}

/// Left-top closed, right-bottom open, matching in_screen()'s half-open bounds.
bool inside(const rect &_r, int _x, int _y) {
  return _x >= _r.x && _x < _r.x + _r.w && _y >= _r.y && _y < _r.y + _r.h;
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
  // The size the model will be looking at, and therefore the coordinate space
  // the next tap or swipe has to use.
  g_ctx.screen_w = bgr.cols;
  g_ctx.screen_h = bgr.rows;
  std::vector<unsigned char> png;
  if (!cv::imencode(".png", bgr, png)) {
    throw js::bind_error("png encode failed");
  }
  // The approval bubble shows this same image, so keep the raw PNG: base64 is
  // the wire's problem, and the host encodes once on the way out.
  g_last_screen_png.assign(png.begin(), png.end());
  return std::string(png.begin(), png.end());
}
CPP_REFLECT_TOOL(screenshot)

[[= js::approval_gate]]
[[= js::doc{.text = js::str("Proposes tapping an element and waits for the user to approve "
                            "the highlighted box. A rejected proposal comes back as an "
                            "error, and `tap` then refuses until this succeeds.")}]]
[[= js::param_docs(js::str("Left edge X of the element box, in screenshot pixels."),
                   js::str("Top edge Y of the element box, in screenshot pixels."),
                   js::str("Width of the box in pixels."),
                   js::str("Height of the box in pixels."))]]
std::string request_tap(int x, int y, int w, int h) {
  std::string error;
  if (!box_on_screen(x, y, w, h, &error)) {
    throw js::bind_error(error);
  }
  g_grant.reset(); // a new proposal invalidates the old permission
  if (!g_ctx.request_approval) {
    throw js::bind_error("no human available to approve the tap");
  }
  llm::approval_request req;
  req.tool = "tap";
  req.summary = "Tap the highlighted element (" + std::to_string(w) + "x" +
                std::to_string(h) + " at " + std::to_string(x) + "," +
                std::to_string(y) + ")";
  req.highlight.push_back(rect{x, y, w, h});
  req.screen_png = g_last_screen_png;
  // ponytail: the box is checked against the last screenshot's size, but a turn
  // that never called screenshot() still reaches the human as a bubble with no
  // image behind it. Refuse that if a blind approval ever costs a bad tap.
  const llm::approval_decision decision = g_ctx.request_approval(req);
  if (!decision.approved) {
    // Empty guidance keeps this string byte-for-byte the same as the bare refusal.
    throw js::bind_error(decision.guidance.empty()
                             ? "user rejected the tap"
                             : "user rejected the tap: " + decision.guidance);
  }
  g_grant = rect{x, y, w, h};
  return "approved: tap box " + std::to_string(x) + "," + std::to_string(y) + " " +
         std::to_string(w) + "x" + std::to_string(h);
}
CPP_REFLECT_TOOL(request_tap)

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
  if (!box_on_screen(x, y, w, h, &error)) {
    throw js::bind_error(error);
  }
  const cv::Mat buf(1, static_cast<int>(g_last_screen_png.size()), CV_8UC1,
                    const_cast<char *>(g_last_screen_png.data()));
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

[[= js::doc{.text = js::str("Taps a point on the device screen, in screenshot pixels.")}]]
[[= js::param_docs(js::str("X coordinate, in screenshot pixels."),
                   js::str("Y coordinate, in screenshot pixels."))]]
std::string tap(int x, int y) {
  std::string error;
  if (!in_screen(x, y, &error)) {
    throw js::bind_error(error);
  }
  if (g_ctx.require_approval) {
    if (!g_grant.has_value() || !inside(*g_grant, x, y)) {
      throw js::bind_error("tap not approved; call request_tap first");
    }
  }
  if (!require_adb().tap(x, y)) {
    throw js::bind_error("adb tap failed");
  }
  g_grant.reset(); // one approval authorizes one tap; only on success
  return "tapped " + std::to_string(x) + "," + std::to_string(y);
}
CPP_REFLECT_TOOL(tap)

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
  // Every turn starts with no screenshot and no permission.
  g_grant.reset();
  g_last_screen_png.clear();
}

std::string tools_json() { return js::tools_json(g_ctx.require_approval); }

tool_reply dispatch(std::string_view _name, std::string_view _args_json) {
  tool_reply out;
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
    if (!js::offered(entry, g_ctx.require_approval)) {
      continue; // a gated tool the gate does not offer is not callable either
    }
    try {
      const std::string result = entry.run(args);
      if (entry.returns_image) {
        out.image_png = result;
      } else {
        out.text = result;
      }
      out.ok = true;
    } catch (const std::exception &e) {
      out.error = e.what();
    }
    return out;
  }
  out.error = "unknown tool: " + std::string(_name);
  return out;
}

} // namespace campcat::llm
