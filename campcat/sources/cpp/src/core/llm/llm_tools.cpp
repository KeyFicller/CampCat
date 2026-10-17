#include "core/llm/llm_tools.h"

#include "ccat_driver/ccat_program_runner.h" // run_ccat_program
#include "core/adb_client.h"
#include "core/app_config.h"
#include "core/automation_log.h"
#include "core/box_snap.h"
#include "core/region_record.h"
#include "core/script/ccat_bundle.h" // bundle_main / bundle_names / ...
#include "core/script/ccat_graph.h"  // screen graph: scan / summary / tags
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

/// The numbered screenshots this session can still cut a template out of:
/// number
/// -> that frame's raw PNG. The sidecar used to keep them and attach one to a
/// call, which capped a call at one picture; here any tool can reach any frame
/// by number, so one call can cut from several.
///
/// `g_ctx.screen` stays as well, keeping the latest frame as its own copy: tap
/// and mark check coordinates against it, and the approval bubble draws on it.
/// Two copies of one picture buy leaving set_context()/m_screen() alone.
///
/// Same thread rule as g_ctx: only the worker running the sidecar exchange
/// reads this, and forget_frames() runs at session reset, where no turn is in
/// flight.
std::map<int, screen_state> g_frames;
/// Only ever goes up, so a number the model remembers from an earlier request
/// keeps pointing at the same picture. Reset with the table.
int g_next_shot = 0;
/// The number this call produced, if it produced one. Set by `screenshot`, read
/// by dispatch() on the way out, and cleared at the top of every dispatch so
/// one call's number can never be reported for the next.
int g_last_shot = 0;

/// The region the human drew, the last time a rejection handed one out. The
/// human's region comes back as a fresh `tap` / `save_template` the model relays
/// (see the box-correction spec, §9-1), so this is the only way the tools can
/// tell it from one the model guessed -- and it must not be snapped. Cleared at
/// session reset, with the frames it was measured on.
std::optional<llm::region> g_human_region;

/// A script is executing (the UI polls this) and it has been asked to stop (the
/// UI writes it, the interpreter reads it). Both cross threads, hence atomics.
std::atomic<bool> g_script_running{false};
std::atomic<bool> g_script_stop{false};

/// The same pair for the emulator wait: `start_emulator` blocks on the boot, so
/// a Stop has to be a flag the wait polls rather than a thread to interrupt.
std::atomic<bool> g_emulator_running{false};
std::atomic<bool> g_emulator_stop{false};

/// The one thing the model can do about a device that is not up, appended to
/// every failure this file reports as a device failure. `run_script`'s errors
/// come from the interpreter instead, so those are matched on the two phrases
/// it words a device failure with.
///
/// ponytail: a device failure the interpreter words differently gets no hint;
/// those phrases live in ccat_interpreter, and covering them properly is not
/// worth a wire field.
constexpr std::string_view k_device_hint =
    "; if the device is down, start_emulator boots it";

/// The two things the model can do about a template that did not match,
/// appended to the failures that are exactly that. Same shape, and the same
/// brittleness, as k_device_hint: the interpreter words these messages, so this
/// matches on their wording.
///
/// Triggered on the failure naming a `.png`, which is what every template
/// failure in the interpreter does and what a device failure does not -- one
/// condition covers all of them, including ones this file does not know about.
constexpr std::string_view k_template_hint =
    "; view_template shows the template it looked for, view_crop what is there "
    "now";

/// Clears a stale stop on entry and marks the script as running until it
/// returns. The clear is not decoration: a turn can run a second script, and
/// the stop the user pressed for the first one would otherwise kill it on
/// entry.
struct script_run_guard {
    script_run_guard() {
        g_script_stop.store(false);
        g_script_running.store(true);
    }
    ~script_run_guard() { g_script_running.store(false); }
};

/// Same shape, same reason: clear whatever Stop the last wait left set, and
/// publish that a wait is in flight so the UI can offer one.
struct emulator_run_guard {
    emulator_run_guard() {
        g_emulator_stop.store(false);
        g_emulator_running.store(true);
    }
    ~emulator_run_guard() { g_emulator_running.store(false); }
};

/// Whether this tool asks the human first. The context decides; a context with
/// no callback never asks.
bool needs_approval(std::string_view _tool) {
    return g_ctx.needs_approval && g_ctx.needs_approval(_tool);
}

/// Parses `_source` or turns the syntax error into the bind_error the model
/// reads.
std::unique_ptr<ccat_lang::Program> parse_or_throw(const std::string &_source) {
    try {
        return ccat_lang::parse_program(_source);
    } catch (const ccat_lang::parse_error &ex) {
        throw js::bind_error("parse error: " + std::string(ex.what()) +
                             " (line " + std::to_string(ex.line) + ", col " +
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
                  ") out of range (screen " + std::to_string(g_ctx.screen.w) +
                  "x" + std::to_string(g_ctx.screen.h) + ")";
        return false;
    }
    return true;
}

/// True when `_region` is wholly inside a picture of `_img_w` x `_img_h`,
/// filling `*_error` with the reason when it is not. The size is a parameter
/// because the region may come from an older, differently sized screenshot.
/// Refuses rather than clamps, as `in_screen` does.
bool region_on_screen(int _img_w, int _img_h, const llm::region &_region,
                      std::string *_error) {
    if (_img_w <= 0 || _img_h <= 0) {
        *_error = "no screenshot yet; call screenshot first";
        return false;
    }
    if (const llm::box *b = std::get_if<llm::box>(&_region)) {
        if (b->w <= 0 || b->h <= 0 || b->x < 0 || b->y < 0 ||
            b->x + b->w > _img_w || b->y + b->h > _img_h) {
            *_error = "box (" + std::to_string(b->x) + "," +
                      std::to_string(b->y) + " " + std::to_string(b->w) + "x" +
                      std::to_string(b->h) + ") out of range (screen " +
                      std::to_string(_img_w) + "x" + std::to_string(_img_h) +
                      ")";
            return false;
        }
        return true;
    }
    // The disc, not just its centre: every pixel it covers has to be in frame,
    // and the rightmost is cx+r (the bounding box is 2r+1 wide), so the test is
    // strict on that side.
    if (const llm::circle *c = std::get_if<llm::circle>(&_region)) {
        if (c->r < 1 || c->cx - c->r < 0 || c->cy - c->r < 0 ||
            c->cx + c->r >= _img_w || c->cy + c->r >= _img_h) {
            *_error = "circle (" + std::to_string(c->cx) + "," +
                      std::to_string(c->cy) + " r" + std::to_string(c->r) +
                      ") out of range (screen " + std::to_string(_img_w) + "x" +
                      std::to_string(_img_h) + ")";
            return false;
        }
        return true;
    }
    // Same edge rule as the disc: the rightmost vertex is cx+rx.
    const llm::diamond &d = std::get<llm::diamond>(_region);
    if (d.rx < 1 || d.ry < 1 || d.cx - d.rx < 0 || d.cy - d.ry < 0 ||
        d.cx + d.rx >= _img_w || d.cy + d.ry >= _img_h) {
        *_error = "diamond (" + std::to_string(d.cx) + "," +
                  std::to_string(d.cy) + " rx" + std::to_string(d.rx) + " ry" +
                  std::to_string(d.ry) + ") out of range (screen " +
                  std::to_string(_img_w) + "x" + std::to_string(_img_h) + ")";
        return false;
    }
    return true;
}

/// One region as the `region` argument, so a sentence read back to the model
/// can be passed straight in again: a box `{x,y,w,h}`, a circle `{cx,cy,r}`,
/// or a diamond `{cx,cy,rx,ry}`.
std::string region_arg_json(const llm::region &_region) {
    if (const llm::box *b = std::get_if<llm::box>(&_region)) {
        return "\"region\":{\"x\":" + std::to_string(b->x) + ",\"y\":" +
               std::to_string(b->y) + ",\"w\":" + std::to_string(b->w) +
               ",\"h\":" + std::to_string(b->h) + "}";
    }
    if (const llm::circle *c = std::get_if<llm::circle>(&_region)) {
        return "\"region\":{\"cx\":" + std::to_string(c->cx) + ",\"cy\":" +
               std::to_string(c->cy) + ",\"r\":" + std::to_string(c->r) + "}";
    }
    const llm::diamond &d = std::get<llm::diamond>(_region);
    return "\"region\":{\"cx\":" + std::to_string(d.cx) + ",\"cy\":" +
           std::to_string(d.cy) + ",\"rx\":" + std::to_string(d.rx) +
           ",\"ry\":" + std::to_string(d.ry) + "}";
}

/// What to append to a refusal when the human redrew the region; empty when they
/// did not, so the caller keeps its own wording byte for byte. `_shot` names the
/// frame they drew on, which `save_template` needs when it is not the current one.
std::string corrected_region_note(const llm::approval_decision &_d,
                                  const char *_tool, int _shot) {
    if (!_d.redrawn) {
        return {};
    }
    return "; call " + std::string(_tool) +
           " again with this corrected region {\"shot\":" +
           std::to_string(_shot) + "," + region_arg_json(*_d.redrawn) + "}";
}

/// The names, comma-separated, for a sentence read back to the model.
std::string join_names(const std::vector<std::string> &_names) {
    std::string out;
    for (const std::string &name : _names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

/// The known bundles, comma-separated for an error message, or "none yet".
std::string existing_bundles() {
    const std::vector<std::string> names =
        ccat_lang::bundle_names(g_ctx.script_dir);
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

/// The graphs in the library, for an error message that offers the model
/// somewhere else to look: "none yet" when the library has no graph at all.
std::string existing_graphs() {
    return joined(ccat_lang::graph_names(g_ctx.script_dir), "none yet");
}

/// `"ok"` -> `"ok.png"`; unchanged when it already ends in `.png`.
std::string with_png_ext(const std::string &_s) {
    return _s.size() >= 4 && _s.compare(_s.size() - 4, 4, ".png") == 0
               ? _s
               : _s + ".png";
}

/// The frame a call names: `_shot` picks one by number, and an omitted number
/// means the session's current picture, which is what the sidecar used to send
/// when the arguments did not ask for anything older. `*_error` is already a
/// whole sentence for the model, and names the numbers that do exist.
const screen_state *frame_for(const std::optional<int> &_shot,
                              std::string *_error) {
    if (!_shot.has_value()) {
        if (g_ctx.screen.png.empty()) {
            *_error = "no screenshot yet; call screenshot first";
            return nullptr;
        }
        return &g_ctx.screen;
    }
    const auto it = g_frames.find(*_shot);
    if (it == g_frames.end()) {
        *_error = "no screenshot #" + std::to_string(*_shot) +
                  " in this session (" +
                  (g_next_shot > 0
                       ? "frames so far: 1.." + std::to_string(g_next_shot)
                       : std::string("none taken yet")) +
                  ")";
        return nullptr;
    }
    return &it->second;
}

/// Decodes the frame a call names. `*_error` is left empty on success.
bool frame_mat(const std::optional<int> &_shot, cv::Mat *_out,
               std::string *_error) {
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
std::unique_ptr<ccat_lang::Program>
check_new_bundle(const std::string &_name, const std::string &_source) {
    if (!ccat_lang::safe_leaf_name(_name)) {
        throw js::bind_error("name \"" + _name + "\" is not a plain file name");
    }
    std::unique_ptr<ccat_lang::Program> prog = parse_or_throw(_source);
    std::error_code ec;
    if (std::filesystem::exists(ccat_lang::bundle_main(g_ctx.script_dir, _name),
                                ec)) {
        throw js::bind_error(
            "script bundle \"" + _name +
            "\" already exists; pick another name (existing: " +
            existing_bundles() + ")");
    }
    return prog;
}

/// Validates a template target and hands back the picture to cut. The region is
/// checked against that picture's own size, not the screen's, because the
/// picture may be an older frame of a different size.
///
/// `*_frame_png` receives that same frame's raw PNG -- the approval bubble
/// draws on the frame being cut, not on the current screen, so seeing it is how
/// a human notices a region aimed at the wrong one. Handed back here so the
/// caller does not have to look the frame up a second time and hope it is still
/// there.
cv::Mat check_template_target(const std::string &_bundle,
                              const std::string &_name,
                              const llm::region &_region,
                              const std::optional<int> &_shot,
                              std::string *_file, std::string *_frame_png,
                              bool _require_bundle = true) {
    if (!ccat_lang::safe_leaf_name(_bundle)) {
        throw js::bind_error("bundle \"" + _bundle +
                             "\" is not a plain file name");
    }
    if (!ccat_lang::safe_leaf_name(_name)) {
        throw js::bind_error("name \"" + _name + "\" is not a plain file name");
    }
    std::error_code ec;
    if (_require_bundle &&
        !std::filesystem::is_regular_file(
            ccat_lang::bundle_main(g_ctx.script_dir, _bundle), ec)) {
        throw js::bind_error(
            "unknown script bundle \"" + _bundle +
            "\"; save_script first (existing: " + existing_bundles() + ")");
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
        throw js::bind_error(
            "the stored screenshot is not a decodable picture");
    }
    if (!region_on_screen(img.cols, img.rows, _region, &error)) {
        throw js::bind_error(error);
    }
    *_frame_png = frame->png;
    return img;
}

/// The current screen as pixels, or an empty Mat when there is nothing to
/// decode. Only `tap` needs this: it takes no `shot`, and every other tool that
/// takes a box already holds the picture it is about to act on.
cv::Mat current_screen_mat() {
    cv::Mat frame;
    std::string error;
    if (!frame_mat(std::nullopt, &frame, &error)) {
        // An empty frame reads as "do not snap" downstream, which is the right
        // answer: tapping must not fail because an optional refinement could not.
        return {};
    }
    return frame;
}

/// The box to act on, after the model's box has been through the optional CV
/// snap. The switch is checked by the caller, before it spends anything on the
/// frame: with the snap off, a tap decodes nothing.
///
/// An empty `_frame` is not an error here: it means the snap cannot run, and the
/// model's own box is returned untouched.
llm::box snap_model_box(const cv::Mat &_frame, const llm::box &_box,
                        const box_snap_config &_cfg) {
    box_snap_options opts;
    opts.strategy = _cfg.strategy;
    const box_snap_result snapped =
        snap_box(_frame, cv::Rect(_box.x, _box.y, _box.w, _box.h), opts);
    if (!snapped.moved) {
        return _box;
    }
    return llm::box{snapped.box.x, snapped.box.y, snapped.box.width,
                    snapped.box.height};
}

/// Whether two regions are the same shape in the same place. It earns a name
/// because it guards the "never snap a human's region" rule.
bool same_region(const llm::region &_a, const llm::region &_b) {
    return _a == _b;
}

/// The region's bounding box, in screenshot pixels: what the snap works in, and
/// what a masked crop, label and draw all need.
llm::box region_bounds(const llm::region &_region) {
    if (const llm::box *b = std::get_if<llm::box>(&_region)) {
        return *b;
    }
    if (const llm::circle *c = std::get_if<llm::circle>(&_region)) {
        return llm::box{c->cx - c->r, c->cy - c->r, 2 * c->r + 1, 2 * c->r + 1};
    }
    const llm::diamond &d = std::get<llm::diamond>(_region);
    return llm::box{d.cx - d.rx, d.cy - d.ry, 2 * d.rx + 1, 2 * d.ry + 1};
}

/// The point a region acts at: a box acts at its centre, a disc or diamond at
/// its own.
std::pair<int, int> region_center(const llm::region &_region) {
    if (const llm::box *b = std::get_if<llm::box>(&_region)) {
        return {b->x + b->w / 2, b->y + b->h / 2};
    }
    if (const llm::circle *c = std::get_if<llm::circle>(&_region)) {
        return {c->cx, c->cy};
    }
    const llm::diamond &d = std::get<llm::diamond>(_region);
    return {d.cx, d.cy};
}

/// A region's size and place, for a sentence read by a human: "40x30 at 10,20",
/// "r=20 at 100,200", or "rx=20 ry=10 at 100,200".
std::string region_phrase(const llm::region &_region) {
    if (const llm::box *b = std::get_if<llm::box>(&_region)) {
        return std::to_string(b->w) + "x" + std::to_string(b->h) + " at " +
               std::to_string(b->x) + "," + std::to_string(b->y);
    }
    if (const llm::circle *c = std::get_if<llm::circle>(&_region)) {
        return "r=" + std::to_string(c->r) + " at " + std::to_string(c->cx) +
               "," + std::to_string(c->cy);
    }
    const llm::diamond &d = std::get<llm::diamond>(_region);
    return "rx=" + std::to_string(d.rx) + " ry=" + std::to_string(d.ry) +
           " at " + std::to_string(d.cx) + "," + std::to_string(d.cy);
}

/// The region's pixels, as a template: BGR for a box, BGRA for a disc or a
/// diamond with the alpha carrying the shape. That alpha makes the template
/// self-describing -- the matcher reads the same channel back for its mask, so
/// nothing on disk has to remember which shape was cut.
cv::Mat crop_region(const cv::Mat &_img, const llm::region &_region) {
    const llm::box b = region_bounds(_region);
    if (std::holds_alternative<llm::box>(_region)) {
        return _img(cv::Rect(b.x, b.y, b.w, b.h)).clone();
    }
    cv::Mat out;
    cv::cvtColor(_img(cv::Rect(b.x, b.y, b.w, b.h)), out, cv::COLOR_BGR2BGRA);
    if (const llm::circle *c = std::get_if<llm::circle>(&_region)) {
        for (int y = 0; y < out.rows; ++y) {
            auto *row = out.ptr<cv::Vec4b>(y);
            for (int x = 0; x < out.cols; ++x) {
                const int dx = b.x + x - c->cx;
                const int dy = b.y + y - c->cy;
                row[x][3] = dx * dx + dy * dy <= c->r * c->r ? 255 : 0;
            }
        }
        return out;
    }
    const llm::diamond &d = std::get<llm::diamond>(_region);
    for (int y = 0; y < out.rows; ++y) {
        auto *row = out.ptr<cv::Vec4b>(y);
        for (int x = 0; x < out.cols; ++x) {
            const long long dx = b.x + x - d.cx;
            const long long dy = b.y + y - d.cy;
            const long long adx = dx < 0 ? -dx : dx;
            const long long ady = dy < 0 ? -dy : dy;
            row[x][3] = adx * d.ry + ady * d.rx <=
                                static_cast<long long>(d.rx) * d.ry
                            ? 255
                            : 0;
        }
    }
    return out;
}

/// How far past the cut `view_crop` shows. Clipped at the frame, so a region
/// on the edge simply has less context on that side.
constexpr int k_view_margin = 16;

/// The cut plus `k_view_margin` of screen, with the cut stroked. Opaque: a
/// circle's corners stay visible here, unlike the masked template.
cv::Mat view_with_margin(const cv::Mat &_img, const llm::region &_region) {
    const llm::box b = region_bounds(_region);
    const int x0 = std::max(0, b.x - k_view_margin);
    const int y0 = std::max(0, b.y - k_view_margin);
    const int x1 = std::min(_img.cols, b.x + b.w + k_view_margin);
    const int y1 = std::min(_img.rows, b.y + b.h + k_view_margin);
    cv::Mat out = _img(cv::Rect(x0, y0, x1 - x0, y1 - y0)).clone();
    const cv::Scalar color(60, 200, 255); // same stroke as `mark`
    if (const llm::circle *c = std::get_if<llm::circle>(&_region)) {
        cv::circle(out, cv::Point(c->cx - x0, c->cy - y0), c->r, color, 4);
    } else if (const llm::diamond *d = std::get_if<llm::diamond>(&_region)) {
        const std::vector<std::vector<cv::Point>> pts = {{
            {d->cx - x0, d->cy - d->ry - y0},
            {d->cx + d->rx - x0, d->cy - y0},
            {d->cx - x0, d->cy + d->ry - y0},
            {d->cx - d->rx - x0, d->cy - y0},
        }};
        cv::polylines(out, pts, true, color, 4);
    } else {
        cv::rectangle(out, cv::Rect(b.x - x0, b.y - y0, b.w, b.h), color, 4);
    }
    return out;
}

/// A template of one flat colour has no variance, and normalized correlation is
/// undefined without it: OpenCV scores such a template 1.0 everywhere, so it
/// would match every screen and its anchor would be useless. Refused at the
/// cut, where the model can still aim somewhere else.
///
/// `_what` names the file for the message, since both cut paths call this.
void require_textured(const cv::Mat &_crop, const std::string &_what) {
    cv::Scalar mean;
    cv::Scalar sd;
    cv::meanStdDev(_crop, mean, sd);
    // One channel varying is enough -- a solid red control leaves green and
    // blue flat -- and a disc's alpha is its shape, not its content.
    double most = 0;
    for (int c = 0; c < std::min(3, _crop.channels()); ++c) {
        most = std::max(most, sd[c]);
    }
    if (most >= 1.0) {
        return;
    }
    throw js::bind_error(
        _what + " is a patch of one flat colour, so it would match every "
                "screen; cut a control with an edge in it -- a button, a label, "
                "a border");
}

/// Appended to a tool's success sentence when the snap moved the box, so the
/// model learns its own numbers were adjusted instead of believing the tool used
/// them. Empty when the box came through unchanged, which keeps that wording
/// byte for byte what it was.
///
/// Shorter than `corrected_region_note` on purpose: that one asks the model to
/// call again with the human's region, while this call has already happened.
std::string snapped_box_note(const llm::box &_from, const llm::box &_to) {
    if (_from.x == _to.x && _from.y == _to.y && _from.w == _to.w &&
        _from.h == _to.h) {
        return {};
    }
    return " (box " + std::to_string(_from.x) + "," + std::to_string(_from.y) +
           " " + std::to_string(_from.w) + "x" + std::to_string(_from.h) +
           " snapped to " + std::to_string(_to.x) + "," + std::to_string(_to.y) +
           " " + std::to_string(_to.w) + "x" + std::to_string(_to.h) + ")";
}

/// Records a region the human redrew, when sampling is on. Never throws -- the
/// sample must not undo the refusal it rode in on.
void record_human_region(const cv::Mat &_frame, const llm::region &_given,
                         const std::optional<llm::region> &_human) {
    if (g_ctx.cfg == nullptr || !g_ctx.cfg->region_score.record ||
        !_human.has_value() || _frame.empty()) {
        return;
    }
    const std::string &dir = g_ctx.cfg->region_score.data_dir;
    std::filesystem::path root = dir.empty() ? "training_data" : dir;
    if (root.is_relative()) {
        root = std::filesystem::path(g_ctx.cfg->config_home).parent_path() / root;
    }
    region_sample sample;
    sample.frame = _frame;
    sample.given = _given;
    sample.human = *_human;
    record_region_sample(root, sample);
}

/// How a template request names the frame it cuts from, for the approval
/// bubble.
std::string shot_phrase(const std::optional<int> &_shot) {
    return _shot.has_value() ? "screenshot #" + std::to_string(*_shot)
                             : "the latest screenshot";
}

/// One template an update will cut, with the crop already taken: everything the
/// human is about to be asked about has been checked and cut by the time an
/// approval request exists.
struct pending_cut {
    std::string file;
    /// Resolved to a frame number: an entry that omitted `shot` cuts from the
    /// latest frame, and the bubble compares cuts by number to decide which
    /// regions belong on the one picture it shows.
    int shot = 0;
    llm::region region;
    cv::Mat crop;
    /// The frame being cut, raw, for the approval bubble.
    std::string frame_png;
};

/// Collects the template operands whose missing file fails the script: `tap`,
/// `tap_offset`, `swipe` (both ends) and `wait_until`, plus those same actions
/// when they stand as a condition -- `if (tap("x.png"))` really runs, so its
/// template has to be in the bundle.
///
/// Not `has(...)` unless `_with_has`: "that template is not here" is the
/// `false` a `has` exists to report, not a failure, so a typo in it is the
/// author's to find in the else branch. An anchor is the exception -- a `has`
/// it can never match makes the anchor never recognise its screen. Not
/// `run()`'s target either, which is another .ccat resolved at run time.
///
/// ponytail: a statement type carrying a template must be added here; the
/// language only grows in the parser, so nothing can appear unseen.
void collect_required_templates(const ccat_lang::Stmt &_s,
                                std::vector<std::string> *_out,
                                bool _with_has = false) {
    using namespace ccat_lang;
    if (const auto *b = dynamic_cast<const BlockStmt *>(&_s)) {
        for (const auto &child : b->body) {
            collect_required_templates(*child, _out, _with_has);
        }
    } else if (const auto *i = dynamic_cast<const IfStmt *>(&_s)) {
        if (i->action) {
            collect_required_templates(*i->action, _out, _with_has);
        }
        if (i->then_branch) {
            collect_required_templates(*i->then_branch, _out, _with_has);
        }
        if (i->else_branch) {
            collect_required_templates(*i->else_branch, _out, _with_has);
        }
    } else if (const auto *d = dynamic_cast<const DoWhileStmt *>(&_s)) {
        if (d->condition_action) {
            collect_required_templates(*d->condition_action, _out, _with_has);
        }
        if (d->body) {
            collect_required_templates(*d->body, _out, _with_has);
        }
    } else if (const auto *l = dynamic_cast<const LoopStmt *>(&_s)) {
        if (l->body) {
            collect_required_templates(*l->body, _out, _with_has);
        }
    } else if (const auto *h = dynamic_cast<const HasStmt *>(&_s)) {
        if (_with_has) {
            _out->push_back(h->image_path);
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

/// A template path that is not a file name in this bundle. `run("../other/...")`
/// is a different statement and is not collected here.
bool template_leaves_bundle(const std::string &_path) {
    return _path.find('/') != std::string::npos ||
           _path.find("..") != std::string::npos;
}

/// Refuses a script whose PNG paths reach outside its own bundle. `has` counts
/// too: borrowing another bundle's template is the same coupling as tapping it.
void require_local_templates(const ccat_lang::Program &_prog) {
    std::vector<std::string> paths;
    for (const auto &stmt : _prog.stmts) {
        collect_required_templates(*stmt, &paths, true);
    }
    std::vector<std::string> bad;
    for (const std::string &path : paths) {
        if (!template_leaves_bundle(path) ||
            std::find(bad.begin(), bad.end(), path) != bad.end()) {
            continue;
        }
        bad.push_back(path);
    }
    if (!bad.empty()) {
        std::string listed;
        for (const std::string &path : bad) {
            if (!listed.empty()) {
                listed += ", ";
            }
            listed += "\"" + path + "\"";
        }
        throw js::bind_error(
            "template " + listed +
            " is outside this bundle; cut it into this bundle and use the "
            "file name");
    }
}

adb_api &require_adb() {
    if (g_ctx.adb == nullptr) {
        throw js::bind_error("no adb session");
    }
    return *g_ctx.adb;
}

/// The same CJK face the UI renders Chinese with, so a label looks the same in
/// the screenshot as it does in the chat.
constexpr char k_cjk_font_path[] = "campcat/sources/cpp/assets/fonts/NotoSansSC-Common.ttf";
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

/// A rectangle on a screenshot, in screenshot pixels. Reflected, so its member
/// names are the JSON keys the model writes. No default member initializers: an
/// absent key must be an error rather than a silent zero, and defaulted shapes
/// would decode into each other, making `shape` below ambiguous.
struct box {
    [[= js::desc{.text =
                     js::str("Left edge X, in screenshot pixels.")}]] int x;
    [[= js::desc{.text =
                     js::str("Top edge Y, in screenshot pixels.")}]] int y;
    [[= js::desc{.text = js::str("Width in pixels.")}]] int w;
    [[= js::desc{.text = js::str("Height in pixels.")}]] int h;
};

/// A filled disc: centre (cx,cy) and radius r, in screenshot pixels. Covers the
/// pixels where `dx*dx + dy*dy <= r*r`.
struct circle {
    [[= js::desc{.text =
                     js::str("Centre X, in screenshot pixels.")}]] int cx;
    [[= js::desc{.text =
                     js::str("Centre Y, in screenshot pixels.")}]] int cy;
    [[= js::desc{.text =
                     js::str("Radius in pixels, at least 1.")}]] int r;
};

/// A filled diamond: centre (cx,cy) and half-diagonals rx, ry. Vertices sit on
/// the axes. Covers the pixels where `|dx|/rx + |dy|/ry <= 1`.
struct diamond {
    [[= js::desc{.text =
                     js::str("Centre X, in screenshot pixels.")}]] int cx;
    [[= js::desc{.text =
                     js::str("Centre Y, in screenshot pixels.")}]] int cy;
    [[= js::desc{.text = js::str(
                     "Horizontal half-diagonal in pixels, at least 1.")}]] int rx;
    [[= js::desc{.text = js::str(
                     "Vertical half-diagonal in pixels, at least 1.")}]] int ry;
};

/// A region, told apart by its fields (`{x,y,w,h}`, `{cx,cy,r}`, or
/// `{cx,cy,rx,ry}`), so a tool that takes a region is one tool. No tag for the
/// model to name: each alternative carries its own fields and none has a default.
using shape = std::variant<box, circle, diamond>;

/// The model's spelling of a shape as the shared one. The only place the two
/// sets of types meet: everything below `region_on_screen` speaks `llm::region`,
/// and the model-facing structs must not travel past this file.
llm::region to_region(const shape &_shape) {
    if (const box *b = std::get_if<box>(&_shape)) {
        return llm::box{b->x, b->y, b->w, b->h};
    }
    if (const circle *c = std::get_if<circle>(&_shape)) {
        return llm::circle{c->cx, c->cy, c->r};
    }
    const diamond &d = std::get<diamond>(_shape);
    return llm::diamond{d.cx, d.cy, d.rx, d.ry};
}

/// The current screen decoded, or a `bind_error` when it cannot be. Every tool
/// that cuts something out of it works on this one picture.
cv::Mat decode_current_screen() {
    const cv::Mat buf(1, static_cast<int>(g_ctx.screen.png.size()), CV_8UC1,
                      const_cast<char *>(g_ctx.screen.png.data()));
    cv::Mat bgr = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (bgr.empty()) {
        // Only reachable if a stored PNG got corrupted, which no tool can do.
        throw js::bind_error("the stored screenshot is not a decodable picture");
    }
    return bgr;
}

/// Swipe duration used when the model omits it. Declared as an optional
/// parameter rather than a C++ default argument: reflection can detect a
/// default argument but cannot read its value, so a missing key could not be
/// filled in at dispatch time.
constexpr int k_default_swipe_ms = 300;

[[= js::image_result]][[= js::doc{
    .text =
        js::str("Takes a fresh screenshot of the device screen.")}]] std::string
    screenshot() {
    adb_api &adb = require_adb();
    cv::Mat bgr;
    std::string diag;
    if (!adb.screencap_bgr(&bgr, 45000, &diag)) {
        throw js::bind_error("screencap failed: " + diag +
                             std::string(k_device_hint));
    }
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", bgr, png)) {
        throw js::bind_error("png encode failed");
    }
    // The approval bubble shows this same image, so keep the raw PNG: base64 is
    // the wire's problem, and the host encodes once on the way out. The size
    // goes with it, session-scoped rather than turn-scoped: every frame stays
    // addressable by `shot`, so a later turn may be tapping at coordinates
    // taken from one.
    g_ctx.screen =
        screen_state{bgr.cols, bgr.rows, std::string(png.begin(), png.end())};
    g_frames[++g_next_shot] = g_ctx.screen;
    g_last_shot = g_next_shot;
    return g_ctx.screen.png;
}
CPP_REFLECT_TOOL(screenshot)

[[= js::image_result]][[= js::doc{
    .text = js::str(
        "Shows a screenshot you took earlier in this session again, by its "
        "number. Frames leave the message list as the turn moves on, so use "
        "this when you need to look at an older one before cutting a template "
        "from it. It does not change the current screen: a tap still lands on "
        "the newest one.")}]][[= js::param_docs(
    js::str("Frame number from a screenshot caption."))]] std::string
    view_shot(int shot) {
    std::string error;
    const screen_state *frame = frame_for(shot, &error);
    if (frame == nullptr) {
        throw js::bind_error(error);
    }
    return frame->png;
}
CPP_REFLECT_TOOL(view_shot)

[[= js::image_result]][[= js::doc{
    .text = js::str(
        "Shows a template PNG from a script bundle: what a failed tap, "
        "tap_offset, swipe or wait_until was matching against. Look at it "
        "together with a fresh screenshot or view_crop -- the template is the "
        "side that did not match, so the question is whether the control it "
        "holds is on the screen at all (then the step before it did not land, or "
        "the screen is not the one you expected) or is there but different "
        "(then the cut is loose or stale, or holds text that changes between "
        "runs).")}]]
    [[= js::param_docs(
        js::str("Bundle the template is in, as list_scripts showed it."),
        js::str("Template name, as read_script lists it; \".png\" is appended "
                "when missing."))]] std::string
    view_template(std::string bundle, std::string name) {
    if (!ccat_lang::safe_leaf_name(bundle)) {
        throw js::bind_error("bundle \"" + bundle +
                             "\" is not a plain file name");
    }
    const std::string file = with_png_ext(name);
    if (!ccat_lang::safe_leaf_name(file)) {
        throw js::bind_error("name \"" + name + "\" is not a plain file name");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(
            ccat_lang::bundle_main(g_ctx.script_dir, bundle), ec)) {
        throw js::bind_error("unknown script bundle \"" + bundle +
                             "\"; save_script first (existing: " +
                             existing_bundles() + ")");
    }
    const std::filesystem::path path = g_ctx.script_dir / bundle / file;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw js::bind_error(
            "\"" + file + "\" is not in bundle \"" + bundle + "\" (templates: " +
            joined(ccat_lang::bundle_templates(g_ctx.script_dir, bundle),
                   "none") +
            ")");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw js::bind_error("could not read " + path.string());
    }
    const std::string png{std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>()};
    // Decoded once on purpose: a file that is not a picture is exactly what
    // `tap: unreadable template` reports, so the model gets that answer here
    // instead of from another failed run.
    const cv::Mat buf(1, static_cast<int>(png.size()), CV_8UC1,
                      const_cast<char *>(png.data()));
    if (png.empty() || cv::imdecode(buf, cv::IMREAD_COLOR).empty()) {
        throw js::bind_error("\"" + bundle + "/" + file +
                             "\" is not a decodable picture; re-cut it with "
                             "save_template");
    }
    return png;
}
CPP_REFLECT_TOOL(view_template)

[[= js::image_result]][[= js::doc{
    .text = js::str(
        "Shows one region of a screenshot plus 16 pixels of screen around it "
        "where the frame has room, with the cut drawn on it. Pick the shape "
        "that matches the control: a box {x,y,w,h} for a rectangle, a circle "
        "{cx,cy,r} for a round control, a diamond {cx,cy,rx,ry} for a diamond "
        "(vertices on the axes). The drawn shape is the cut. Pixels outside "
        "the line are context, not part of the template. save_template still "
        "writes the tight cut, and a circle or a diamond is transparent "
        "outside the shape there. It does not change the current screen. One "
        "look per element on a screenshot, then save_template or a different "
        "element. Do not call it again only to tighten the same box.")}]]
    [[= js::param_docs(
        js::str("The region to cut out: a box {x,y,w,h}, a circle {cx,cy,r}, or "
                "a diamond {cx,cy,rx,ry}."),
        js::str("Which screenshot to look at: the number its caption showed. "
                "Omit for the current screen; looking at another frame does "
                "not make it current."))]] std::string
    view_crop(shape region, std::optional<int> shot) {
    cv::Mat bgr;
    std::string error;
    if (!frame_mat(shot, &bgr, &error)) {
        throw js::bind_error(error);
    }
    const llm::region r = to_region(region);
    if (!region_on_screen(bgr.cols, bgr.rows, r, &error)) {
        throw js::bind_error(error);
    }
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", view_with_margin(bgr, r), png)) {
        throw js::bind_error("png encode failed");
    }
    return std::string(png.begin(), png.end());
}
CPP_REFLECT_TOOL(view_crop)

[[= js::image_result]][[= js::doc{
    .text = js::str(
        "The tight cut of a region, with nothing drawn on it. A circle or a "
        "diamond is transparent outside the shape. Review uses this.")}]]
    [[= js::param_docs(
        js::str("The region to cut out: a box {x,y,w,h}, a circle {cx,cy,r}, or "
                "a diamond {cx,cy,rx,ry}."),
        js::str("Which screenshot to look at: the number its caption showed. "
                "Omit for the current screen; looking at another frame does "
                "not make it current."))]] std::string
    tight_crop(shape region, std::optional<int> shot) {
    cv::Mat bgr;
    std::string error;
    if (!frame_mat(shot, &bgr, &error)) {
        throw js::bind_error(error);
    }
    const llm::region r = to_region(region);
    if (!region_on_screen(bgr.cols, bgr.rows, r, &error)) {
        throw js::bind_error(error);
    }
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", crop_region(bgr, r), png)) {
        throw js::bind_error("png encode failed");
    }
    return std::string(png.begin(), png.end());
}
CPP_REFLECT_TOOL(tight_crop)

[[= js::image_result]][[= js::doc{
    .text = js::str(
        "Draws a region on a screenshot and returns the marked image. Nothing "
        "is tapped and nothing needs approval; the region lives in the "
        "picture. A box is {x,y,w,h}, a circle is {cx,cy,r} and a diamond is "
        "{cx,cy,rx,ry}.")}]]
    [[= js::param_docs(
        js::str("Short label for what the region marks, e.g. \"the login button\"."),
        js::str("The region to mark: a box {x,y,w,h}, a circle {cx,cy,r}, or a "
                "diamond {cx,cy,rx,ry}."),
        js::str("Which screenshot to look at: the number its caption showed. "
                "Omit for the current screen; looking at another frame does "
                "not make it current."))]] std::string
    mark(std::string label, shape region, std::optional<int> shot) {
    cv::Mat bgr;
    std::string error;
    if (!frame_mat(shot, &bgr, &error)) {
        throw js::bind_error(error);
    }
    const llm::region r = to_region(region);
    if (!region_on_screen(bgr.cols, bgr.rows, r, &error)) {
        throw js::bind_error(error);
    }
    const cv::Scalar color(60, 200, 255); // BGR, near-orange against most UIs
    const llm::box b = region_bounds(r);
    if (const llm::circle *c = std::get_if<llm::circle>(&r)) {
        cv::circle(bgr, cv::Point(c->cx, c->cy), c->r, color, 4);
    } else if (const llm::diamond *d = std::get_if<llm::diamond>(&r)) {
        const std::vector<std::vector<cv::Point>> pts = {{
            {d->cx, d->cy - d->ry},
            {d->cx + d->rx, d->cy},
            {d->cx, d->cy + d->ry},
            {d->cx - d->rx, d->cy},
        }};
        cv::polylines(bgr, pts, true, color, 4);
    } else {
        cv::rectangle(bgr, cv::Rect(b.x, b.y, b.w, b.h), color, 4);
    }
    if (!label.empty()) {
        const int ty =
            b.y >= k_label_font_h + 4 ? b.y - k_label_font_h - 2 : b.y + b.h + 4;
        draw_label(bgr, label, b.x, ty, color);
    }
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", bgr, png)) {
        throw js::bind_error("png encode failed");
    }
    return std::string(png.begin(), png.end());
}
CPP_REFLECT_TOOL(mark)

/// The tap `tap` runs, for either shape.
std::string tap_region(const llm::region &_given) {
    std::string error;
    if (!region_on_screen(g_ctx.screen.w, g_ctx.screen.h, _given, &error)) {
        throw js::bind_error(error);
    }
    // Snap first, then use that region everywhere below: the preview, the tap
    // point and the sentence back all describe one region, so approving what is
    // seen approves what happens. Only a box is snapped -- the snap reads an
    // element's edges, which a boundary the human drew has no part in.
    llm::region use = _given;
    std::string snap_note;
    if (const llm::box *given = std::get_if<llm::box>(&_given)) {
        const box_snap_config snap =
            g_ctx.box_snap ? g_ctx.box_snap() : box_snap_config{};
        // A region the human drew is the answer, not a guess: the CV must not
        // move it.
        const bool human = g_human_region && same_region(*g_human_region, _given);
        if (snap.strategy != box_snap_strategy::none && !human) {
            use = snap_model_box(current_screen_mat(), *given, snap);
            snap_note =
                snapped_box_note(*given, std::get<llm::box>(use));
        }
    }
    if (needs_approval("tap")) {
        if (!g_ctx.request_approval) {
            throw js::bind_error("no human available to approve the tap");
        }
        llm::approval_request req;
        req.tool = "tap";
        req.summary =
            "Tap the highlighted element (" + region_phrase(use) + ")";
        req.highlight.push_back(use);
        // Only when they differ: an identical second frame would just draw over
        // the first.
        if (use != _given) {
            req.given_region = _given;
        }
        req.screen_png = g_ctx.screen.png;
        req.editable = true;
        const llm::approval_decision decision = g_ctx.request_approval(req);
        if (!decision.approved) {
            record_human_region(current_screen_mat(), _given, decision.redrawn);
            // Empty guidance and no redrawn region keep this string
            // byte-for-byte the same as the bare refusal.
            std::string msg =
                decision.guidance.empty()
                    ? std::string("user rejected the tap")
                    : "user rejected the tap: " + decision.guidance;
            msg += corrected_region_note(decision, "tap", g_next_shot);
            // The call relaying this region is the human's, not the model's.
            g_human_region = decision.redrawn;
            throw js::bind_error(msg);
        }
    }
    // The tap lands at the region's centre: the model marks the element, this
    // picks the point. It used to send the centre as a separate tap() argument
    // every time.
    const auto [cx, cy] = region_center(use);
    if (!require_adb().tap(cx, cy)) {
        throw js::bind_error(std::string("adb tap failed") +
                             std::string(k_device_hint));
    }
    return "tapped " + std::to_string(cx) + "," + std::to_string(cy) +
           snap_note;
}

[[= js::doc{.text = js::str(
                "Taps the element at this region, in screenshot pixels. A box "
                "is {x,y,w,h}, a circle is {cx,cy,r} and a diamond is "
                "{cx,cy,rx,ry}; the tap lands at its centre. While human "
                "approval is on, the region is shown to the user and nothing "
                "is tapped unless they approve it. Nothing else needs a prior "
                "call.")}]]
    [[= js::param_docs(js::str(
        "The region to tap: a box {x,y,w,h}, a circle {cx,cy,r}, or a diamond "
        "{cx,cy,rx,ry}."))]] std::string
    tap(shape region) {
    return tap_region(to_region(region));
}
CPP_REFLECT_TOOL(tap)

[[= js::doc{
    .text = js::str(
        "Boots the Android emulator (MuMu) this shell is configured for and "
        "waits until the instance is up. Call it when the device is not "
        "reachable and the user asks for something on the device. It blocks "
        "for as long as the boot takes, up to about four minutes, and returns "
        "either that the emulator is up or why it did not come up. A device "
        "call batched with it runs before the boot finishes and fails, so give "
        "it a round of its own. An emulator "
        "that is already up is reported as such and nothing is started "
        "again.")}]] std::string start_emulator() {
    if (!g_ctx.start_emulator) {
        throw js::bind_error("no emulator control in this session");
    }
    const emulator_run_guard guard;
    const mumu_status st =
        g_ctx.start_emulator([] { return g_emulator_stop.load(); });
    if (st.state != mumu_state::running) {
        throw js::bind_error(st.message.empty()
                                 ? std::string("the emulator did not start")
                                 : st.message);
    }
    std::string text = "the emulator is up";
    if (st.message == "already up") {
        text += "; nothing was started";
    } else if (!st.message.empty() && st.message != "running") {
        text += ": " + st.message;
    }
    // The client was built when this turn started, with the emulator still
    // down, so this is what makes the device reachable to the tools that come
    // next.
    if (g_ctx.adb != nullptr && g_ctx.cfg != nullptr) {
        // This client was built when the turn started, with the emulator still
        // down, so it carries no address: ask mumutool for the port the launch
        // just produced and point the client at it.
        const std::string addr = campcat::mumu_adb_address(
            g_ctx.cfg->mumu.tool_path, g_ctx.cfg->mumu.device_index);
        g_ctx.adb->set_serial(addr.empty() ? g_ctx.cfg->adb.serial : addr);
        std::string diag;
        if (!g_ctx.adb->connect_remote(addr, 15000, nullptr, &diag)) {
            text += " (adb connect failed: " + diag + ")";
        }
    }
    return text;
}
CPP_REFLECT_TOOL(start_emulator)

[[= js::doc{
    .text = js::str(
        "Returns to the Android launcher and force-stops every recent app, "
        "so the device is on the home screen with nothing else running. Call "
        "it before a flow that has to start from that screen: each run then "
        "begins in the same place. It blocks until the apps are stopped. "
        "While human approval is on, this is shown to the user and nothing "
        "runs unless they approve it.")}]]
    std::string go_home() {
    if (needs_approval("go_home")) {
        if (!g_ctx.request_approval) {
            throw js::bind_error("no human available to approve going home");
        }
        llm::approval_request req;
        req.tool = "go_home";
        req.summary = "return to the launcher and force-stop every recent app";
        const llm::approval_decision decision = g_ctx.request_approval(req);
        if (!decision.approved) {
            throw js::bind_error(decision.guidance.empty()
                                     ? "user rejected going home"
                                     : "user rejected going home: " +
                                           decision.guidance);
        }
    }
    adb_api &adb = require_adb();
    int gap_ms = 0;
    if (g_ctx.cfg != nullptr && g_ctx.cfg->input.tap_delay_ms > 0) {
        gap_ms = g_ctx.cfg->input.tap_delay_ms;
    }
    if (!adb.home_and_kill_all(gap_ms)) {
        throw js::bind_error(std::string("returning home failed") +
                             std::string(k_device_hint));
    }
    return "on the launcher; recent apps were closed";
}
CPP_REFLECT_TOOL(go_home)

[[= js::quiet{.text = js::str("Ran script")}]][[= js::doc{
    .text = js::str(
        "Runs a .ccat script on the device and returns what happened. "
        "While human approval is on, the script is shown to the user "
        "and nothing runs unless they approve it. Call ccat_help "
        "for the language. The sandbox starts empty, so a statement that "
        "needs a template PNG fails until save_template puts one in the "
        "bundle (an `if` on a missing PNG is just false); tap_at and "
        "swipe_at need no template, and run(\"<name>/main.ccat\") runs a "
        "saved bundle.")}]]
    [[= js::param_docs(
        js::str("The full .ccat script source to run."))]] std::string
        run_script(std::string source) {
    // Parse first: a syntax error answers the model directly instead of asking
    // a human to read a script that cannot run.
    const std::unique_ptr<ccat_lang::Program> prog = parse_or_throw(source);
    if (needs_approval("run_script")) {
        if (!g_ctx.request_approval) {
            throw js::bind_error(
                "no human available to approve running the script");
        }
        llm::approval_request req;
        req.tool = "run_script";
        req.summary = source; // the thing being approved is the script itself
        const llm::approval_decision decision = g_ctx.request_approval(req);
        if (!decision.approved) {
            throw js::bind_error(decision.guidance.empty()
                                     ? "user rejected running the script"
                                     : "user rejected running the script: " +
                                           decision.guidance);
        }
    }
    // Checked before adb so a refusal above is the reason reported, and so that
    // path is testable without a device.
    adb_api &adb = require_adb();
    if (g_ctx.cfg == nullptr) {
        throw js::bind_error("no shell config for the script tools");
    }
    script_run_guard guard;
    // Declared before `capture` on purpose: locals die in reverse order, and
    // this is what keeps the vector alive for the sink's last call.
    std::vector<std::string> script_logs;
    [[maybe_unused]] const automation_log::handle capture =
        automation_log::add_sink([&script_logs](const std::string &_line) {
            if (std::optional<std::string> text = script_log_text(_line)) {
                script_logs.push_back(std::move(*text));
            }
        });
    const auto res =
        run_ccat_program(&adb, g_ctx.cfg, g_ctx.script_dir, {}, *prog,
                         [] { return g_script_stop.load(); });
    if (!res.ok) {
        std::string msg =
            res.message.empty() ? std::string("script failed") : res.message;
        // A statement that could not see the device is the one failure here the
        // model can act on (see k_device_hint).
        if (msg.find("screencap failed") != std::string::npos ||
            msg.find("adb tap failed") != std::string::npos) {
            msg += k_device_hint;
        }
        if (msg.find(".png") != std::string::npos) {
            msg += k_template_hint;
        }
        throw js::bind_error(msg + script_log_suffix(script_logs));
    }
    return "script ran ok (" + std::to_string(prog->stmts.size()) +
           " statements)" + script_log_suffix(script_logs);
}
CPP_REFLECT_TOOL(run_script)

/// What the graph `_graph` looks like after a write into it, for the reply:
/// a broken graph is refused by `goto`, so the author hears about it now.
std::string graph_check(const std::string &_graph) {
    const ccat_lang::scanned_graph scanned =
        ccat_lang::scan_graph(g_ctx.script_dir, _graph);
    if (scanned.problems.empty()) {
        return "graph " + _graph + ": no problems";
    }
    std::string text = "graph " + _graph + " problems (goto refuses it until "
                                           "they are fixed):";
    for (const std::string &p : scanned.problems) {
        text += "\n  - " + p;
    }
    return text;
}

[[= js::quiet{.text = js::str("Saved script {name}")}]][[= js::doc{
    .text = js::str(
        "Saves a .ccat script as a reusable bundle under the LLM script root: "
        "writes "
        "main.ccat next to the template PNGs that script uses. While human "
        "approval is "
        "on, the name and source are shown to the user and nothing is written "
        "unless "
        "they approve it. It does not run anything; run it from a script with "
        "run(\"<name>/main.ccat\").")}]]
    [[= js::param_docs(
        js::str("Bundle name: a leaf name describing what the script does, "
                "e.g. \"claim_reward\"."),
        js::str("The full .ccat script source to save."))]] std::string
        save_script(std::string name, std::string source) {
    const std::unique_ptr<ccat_lang::Program> prog =
        check_new_bundle(name, source);
    if (const std::string block =
            ccat_lang::script_graph_block(g_ctx.script_dir, name, *prog);
        !block.empty()) {
        throw js::bind_error(block);
    }
    require_local_templates(*prog);
    if (needs_approval("save_script")) {
        if (!g_ctx.request_approval) {
            throw js::bind_error(
                "no human available to approve saving the script");
        }
        llm::approval_request req;
        req.tool = "save_script";
        // The name goes in the summary: it is what the human has to judge, and
        // the sandbox root is already known to whoever is looking at the
        // bubble.
        req.summary =
            "Save script bundle \"" + name + "\" as main.ccat:\n\n" + source;
        const llm::approval_decision decision = g_ctx.request_approval(req);
        if (!decision.approved) {
            throw js::bind_error(decision.guidance.empty()
                                     ? "user rejected saving the script"
                                     : "user rejected saving the script: " +
                                           decision.guidance);
        }
    }
    const std::filesystem::path path =
        ccat_lang::bundle_main(g_ctx.script_dir, name);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    out.close();
    if (!out) {
        throw js::bind_error("could not write " + name + "/main.ccat (does " +
                             g_ctx.script_dir.string() +
                             " exist and allow writes?)");
    }
    const ccat_lang::bundle_tag tag =
        ccat_lang::bundle_tags(g_ctx.script_dir, name);
    return "saved \"" + name + "/main.ccat\" (" +
           std::to_string(prog->stmts.size()) + " statements, " +
           std::to_string(source.size()) +
           " chars); put its templates in the same bundle with save_template, "
           "and "
           "run it from a script with run(\"" +
           name + "/main.ccat\")" +
           (tag.edge_of.empty() ? "" : "\n" + graph_check(tag.edge_of));
}
CPP_REFLECT_TOOL(save_script)

/// The cut `save_template` writes, for either shape.
std::string save_template_region(std::string bundle, std::string name,
                                 const std::optional<int> &shot,
                                 const llm::region &_given) {
    std::string file;
    std::string frame_png;
    const cv::Mat img = check_template_target(bundle, name, _given, shot, &file,
                                              &frame_png);
    // Given region first, so a bad one is refused on its own terms; then the
    // snap, against the very frame this cut comes from. Everything below cuts,
    // previews and reports `use`. Only a box is snapped -- the snap reads an
    // element's edges, which a human's boundary has no part in.
    llm::region use = _given;
    std::string snap_note;
    if (const llm::box *given = std::get_if<llm::box>(&_given)) {
        const box_snap_config snap =
            g_ctx.box_snap ? g_ctx.box_snap() : box_snap_config{};
        // A region the human drew is the answer, not a guess: the CV must not
        // move it.
        const bool human = g_human_region && same_region(*g_human_region, _given);
        if (snap.strategy != box_snap_strategy::none && !human) {
            use = snap_model_box(img, *given, snap);
            snap_note = snapped_box_note(*given, std::get<llm::box>(use));
        }
    }
    // Before the approval gate: a flat patch is refused on the model's own
    // numbers, and bothering the human to reject it would be a wasted round.
    const cv::Mat crop = crop_region(img, use);
    require_textured(crop, "template \"" + bundle + "/" + file + "\"");
    if (needs_approval("save_template")) {
        if (!g_ctx.request_approval) {
            throw js::bind_error(
                "no human available to approve saving the template");
        }
        llm::approval_request req;
        req.tool = "save_template";
        req.summary = "Save template \"" + bundle + "/" + file + "\" from " +
                      shot_phrase(shot) + " at " + region_phrase(use);
        req.highlight.push_back(use);
        if (use != _given) {
            req.given_region = _given;
        }
        // The frame the model named, not the current screen: that is what is
        // being cut, and seeing it is how the human notices a region aimed at
        // the wrong one.
        req.screen_png = frame_png;
        req.editable = true;
        const llm::approval_decision decision = g_ctx.request_approval(req);
        if (!decision.approved) {
            record_human_region(img, _given, decision.redrawn);
            std::string msg =
                decision.guidance.empty()
                    ? std::string("user rejected saving the template")
                    : "user rejected saving the template: " + decision.guidance;
            msg += corrected_region_note(decision, "save_template",
                                         shot.value_or(g_next_shot));
            // The call relaying this region is the human's, not the model's.
            g_human_region = decision.redrawn;
            throw js::bind_error(msg);
        }
    }
    const std::filesystem::path path = g_ctx.script_dir / bundle / file;
    std::error_code ec;
    const bool existed = std::filesystem::exists(path, ec);
    try {
        // Overwriting is allowed on purpose: a template is normally cut a few
        // times before it is tight, and refusing would push the model to invent
        // new names and rewrite the script, costing another approval of the
        // whole script.
        if (!cv::imwrite(path.string(), crop)) {
            throw js::bind_error("could not write " + bundle + "/" + file);
        }
    } catch (const cv::Exception &e) {
        throw js::bind_error("could not write " + bundle + "/" + file + ": " +
                             e.what());
    }
    return std::string(existed ? "overwrote" : "saved") + " template \"" +
           bundle + "/" + file + "\" (" + region_phrase(use) +
           "); use it as tap(\"" + file +
           "\") inside that bundle's script" + snap_note;
}

[[= js::doc{
    .text = js::str(
        "Cuts one template PNG, out of one of the screenshots you have taken, "
        "into an existing script bundle, so that bundle's script can use it "
        "with tap(\"name.png\"). Pick the shape that matches the control: a box "
        "{x,y,w,h} for a rectangle, a circle {cx,cy,r} for a round control, a "
        "diamond {cx,cy,rx,ry} for a diamond (vertices on the axes). A box "
        "around a round or diamond control keeps background the match must "
        "ignore. A circle or a diamond is written with that shape's alpha and "
        "matching then ignores every pixel "
        "outside it. While human approval "
        "is on, the region is shown to the user and nothing is cut unless they "
        "approve it. It cuts from the picture you looked at, so screenshot "
        "right before you cut, and crop tight around the part that stays put.")}]]
    [[= js::param_docs(
        js::str("Bundle to put the template in; it must already exist (see "
                "save_script)."),
        js::str(
            "Template name, a leaf name; \".png\" is appended when missing."),
        js::str("Which screenshot to cut from: the number its caption showed. "
                "Omit for the most recent one."),
        js::str("The region to cut out: a box {x,y,w,h}, a circle {cx,cy,r}, or "
                "a diamond {cx,cy,rx,ry}."))]] std::string
        save_template(std::string bundle, std::string name,
                      std::optional<int> shot, shape region) {
    return save_template_region(std::move(bundle), std::move(name), shot,
                                to_region(region));
}
CPP_REFLECT_TOOL(save_template)

[[= js::quiet{.text = js::str("Read CCAT.md")}]][[= js::doc{
    .text = js::str(
        "Returns the full reference for the .ccat scripting language. "
        "Call it before writing a script with run_script.")}]] std::string
    ccat_help() {
    if (g_ctx.cfg == nullptr) {
        throw js::bind_error("no shell config; cannot locate CCAT.md");
    }
    // CCAT.md sits in the library beside the scripts it documents.
    const std::filesystem::path path =
        std::filesystem::path(g_ctx.cfg->config_home) / "CCAT.md";
    // Read per call, no cache: a function-local static would freeze whichever
    // g_ctx the first call happened to see, and this tool is called rarely
    // enough that the read costs nothing.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // Loud, with the path: an empty string would read as "the language is
        // this small", which is worse than a failure.
        throw js::bind_error("CCAT.md not found at " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}
CPP_REFLECT_TOOL(ccat_help)

[[= js::quiet{.text = js::str("List scripts")}]][[= js::doc{
    .text = js::str(
        "Lists the saved .ccat skill bundles, one \"name -- description\" "
        "line each. Call this before writing a new skill: if a bundle "
        "already does the job, use it with run(\"<name>/main.ccat\") "
        "instead of writing it again, and call read_script to see how it "
        "works or update_script to change it. The description is the "
        "first // comment line of that bundle's main.ccat.")}]] std::string
    list_scripts() {
    const std::vector<std::string> names =
        ccat_lang::bundle_names(g_ctx.script_dir);
    if (names.empty()) {
        return "none yet";
    }
    std::string out;
    for (const std::string &name : names) {
        const std::string description =
            ccat_lang::bundle_description(g_ctx.script_dir, name);
        if (!out.empty()) {
            out += "\n";
        }
        // Anchors and edges are not skills to run on their own: the tag says
        // so, and it is where the model learns a graph exists at all.
        const ccat_lang::bundle_tag tag =
            ccat_lang::bundle_tags(g_ctx.script_dir, name);
        std::string suffix;
        if (tag.anchors > 0) {
            suffix = " [graph " + tag.anchor_of + ", " +
                     std::to_string(tag.anchors) + " anchors, " +
                     std::to_string(tag.edges) + " edges, " +
                     std::to_string(tag.actions) + " actions]";
        } else if (!tag.anchor_of.empty()) {
            const char *kind = tag.kind == ccat_lang::anchor_kind::popup ? "popup"
                               : tag.kind == ccat_lang::anchor_kind::transient
                                   ? "transient"
                                   : "page";
            suffix = " [anchor of " + tag.anchor_of + ", " + kind + "]";
        } else if (!tag.edge_of.empty()) {
            suffix = " [edge of " + tag.edge_of + "]";
        } else if (!tag.action_of.empty()) {
            suffix = " [action of " + tag.action_of + " on " + tag.action_on +
                     "]";
        }
        out += name + " -- " +
               (description.empty() ? std::string("(no description)")
                                    : description) +
               suffix;
    }
    return out;
}
CPP_REFLECT_TOOL(list_scripts)

[[= js::quiet{.text = js::str("Read script {name}")}]][[= js::doc{
    .text = js::str(
        "Returns a saved bundle's main.ccat exactly as written, prefixed by a "
        "header naming the bundle and listing its template PNGs. The "
        "source is what you pass back as update_script's source, so edit "
        "it from what this returns rather than from memory.")}]][
    [= js::param_docs(
        js::str("Bundle name, as list_scripts showed it."))]] std::string
    read_script(std::string name) {
    if (!ccat_lang::safe_leaf_name(name)) {
        throw js::bind_error("name \"" + name + "\" is not a plain file name");
    }
    const std::filesystem::path path =
        ccat_lang::bundle_main(g_ctx.script_dir, name);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw js::bind_error(
            "unknown script bundle \"" + name +
            "\"; save_script first (existing: " + existing_bundles() + ")");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw js::bind_error("could not read " + path.string());
    }
    const std::string source{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};
    const std::string description =
        ccat_lang::bundle_description(g_ctx.script_dir, name);
    return "bundle \"" + name + "\": " +
           (description.empty() ? std::string("(no description)")
                                : description) +
           "\ntemplates: " +
           joined(ccat_lang::bundle_templates(g_ctx.script_dir, name), "none") +
           "\n\n" + source;
}
CPP_REFLECT_TOOL(read_script)

/// The bundle `_name` must be, in the words a wrong name always gets.
void require_bundle(const std::string &_name) {
    if (!ccat_lang::safe_leaf_name(_name)) {
        throw js::bind_error("name \"" + _name +
                             "\" is not a plain file name");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(
            ccat_lang::bundle_main(g_ctx.script_dir, _name), ec)) {
        throw js::bind_error("unknown script bundle \"" + _name +
                             "\"; save_script creates one (existing: " +
                             existing_bundles() + ")");
    }
}

/// One entry of the template set an update declares: a name, either a shape to
/// cut that name from or nothing to keep the PNG already in the bundle, and
/// which screenshot to cut from. "Keep" needs the optional because one update
/// cannot re-cut every template -- their screens are not all on one frame.
struct template_cut {
    [[= js::desc{.text = js::str("Template name, a leaf name; \".png\" is "
                                "appended when missing.")}]] std::string name;
    [[= js::desc{
        .text = js::str(
            "Omit to keep the template already in the bundle; give it to "
            "re-cut "
            "that template.")}]] std::optional<shape> cut;
    [[= js::desc{
        .text = js::str("Which screenshot to cut from: the number its caption "
                        "showed. Omit for the most recent one; ignored when "
                        "there is no cut.")}]] std::optional<int> shot;
};

/// What a declared template set does to a bundle. Everything in it has been
/// checked and cut, so a human is only ever asked about a set that can be
/// written.
struct template_plan {
    std::vector<pending_cut> cuts;
    std::vector<std::string> kept;
    std::vector<std::string> to_delete;
};

/// Checks `_templates` as the whole template set `_bundle` will have once it
/// runs `_prog`, and takes the crops. `_anchor` counts `has(...)` operands as
/// required too; `_new_bundle` lets cuts go to a bundle not on disk yet.
template_plan plan_templates(const std::string &_bundle,
                             const ccat_lang::Program &_prog,
                             const std::vector<template_cut> &_templates,
                             bool _anchor, bool _new_bundle) {
    require_local_templates(_prog);
    // The declared names, checked before anything is resolved against the disk,
    // so a duplicate is reported as a duplicate and not as whatever the first
    // entry happened to be missing.
    std::vector<std::string> declared;
    for (const template_cut &entry : _templates) {
        const std::string file = with_png_ext(entry.name);
        if (!ccat_lang::safe_leaf_name(file)) {
            throw js::bind_error("template name \"" + entry.name +
                                 "\" is not a plain file name");
        }
        if (std::find(declared.begin(), declared.end(), file) !=
            declared.end()) {
            throw js::bind_error(
                "template \"" + file +
                "\" is declared twice; one entry per template");
        }
        declared.push_back(file);
    }

    // A template operand that fails the script has to be declared, or the
    // update would write a script the bundle cannot run. A condition's action
    // is an operand for the same reason (`if (tap("x.png"))` really runs).
    // Neither is an absolute path, which is not a file in the bundle.
    std::vector<std::string> operands;
    for (const auto &stmt : _prog.stmts) {
        collect_required_templates(*stmt, &operands, _anchor);
    }
    std::vector<std::string> missing;
    for (const std::string &operand : operands) {
        const std::string file = with_png_ext(operand);
        if (operand.empty() || std::filesystem::path(operand).is_absolute() ||
            std::find(declared.begin(), declared.end(), file) !=
                declared.end() ||
            std::find(missing.begin(), missing.end(), file) != missing.end()) {
            continue;
        }
        missing.push_back(file);
    }
    if (!missing.empty()) {
        throw js::bind_error(
            "the new source needs template(s) this update does not "
            "declare: " +
            joined(missing, "") +
            "; add them to templates (with a cut to cut them, or to "
            "keep the PNG already in the bundle)");
    }

    // Now the disk work: cut what is to be cut, and confirm what is to be kept
    // is there. Every region is checked here, so a bad one costs nobody a look
    // at the bubble.
    template_plan plan;
    std::error_code ec;
    for (const template_cut &entry : _templates) {
        const std::string file = with_png_ext(entry.name);
        if (!entry.cut.has_value()) {
            if (!std::filesystem::is_regular_file(
                    g_ctx.script_dir / _bundle / file, ec)) {
                throw js::bind_error(
                    "template \"" + file +
                    "\" has no cut and is not in the bundle, so there is "
                    "nothing to keep");
            }
            plan.kept.push_back(file);
            continue;
        }
        const llm::region region = to_region(*entry.cut);
        pending_cut cut;
        std::string checked;
        const cv::Mat frame =
            check_template_target(_bundle, entry.name, region, entry.shot,
                                  &checked, &cut.frame_png, !_new_bundle);
        cut.file = checked;
        cut.shot = entry.shot.value_or(g_next_shot);
        cut.region = region;
        cut.crop = crop_region(frame, region);
        require_textured(cut.crop,
                         "template \"" + _bundle + "/" + cut.file + "\"");
        plan.cuts.push_back(std::move(cut));
    }

    // Everything left in the bundle that is not declared goes, which is what
    // makes the declared set the bundle's template set.
    for (const std::string &file :
         ccat_lang::bundle_templates(g_ctx.script_dir, _bundle)) {
        if (std::find(declared.begin(), declared.end(), file) ==
            declared.end()) {
            plan.to_delete.push_back(file);
        }
    }
    return plan;
}

/// The template lines of an approval summary, and its picture.
void describe_templates(const template_plan &_plan,
                        llm::approval_request *_req) {
    for (const pending_cut &cut : _plan.cuts) {
        _req->summary += "cut \"" + cut.file + "\" from " +
                         shot_phrase(cut.shot) + " at " +
                         region_phrase(cut.region) + "\n";
    }
    _req->summary += "keep: " + joined(_plan.kept, "none") + "\n";
    _req->summary += "delete: " + joined(_plan.to_delete, "none") + "\n\n";
    // One picture fits in the bubble, so it is the frame the first cut comes
    // from, with only the regions taken from that same frame drawn on it. Any
    // cuts from other frames are named in the summary.
    if (!_plan.cuts.empty()) {
        _req->screen_png = _plan.cuts.front().frame_png;
        for (const pending_cut &cut : _plan.cuts) {
            if (cut.shot == _plan.cuts.front().shot) {
                _req->highlight.push_back(cut.region);
            }
        }
    }
}

void write_text(const std::string &_bundle, const std::string &_file,
                const std::string &_text) {
    std::ofstream out(g_ctx.script_dir / _bundle / _file,
                      std::ios::binary | std::ios::trunc);
    out.write(_text.data(), static_cast<std::streamsize>(_text.size()));
    out.close();
    if (!out) {
        throw js::bind_error("could not write " + _bundle + "/" + _file);
    }
}

/// Writes `_source` as `_bundle`'s main.ccat, then the templates it names,
/// then deletes the leftovers.
void write_bundle(const std::string &_bundle, const std::string &_source,
                  const template_plan &_plan) {
    std::error_code ec;
    std::filesystem::create_directories(g_ctx.script_dir / _bundle, ec);
    write_text(_bundle, "main.ccat", _source);
    for (const pending_cut &cut : _plan.cuts) {
        const std::filesystem::path path = g_ctx.script_dir / _bundle / cut.file;
        try {
            if (!cv::imwrite(path.string(), cut.crop)) {
                throw js::bind_error("could not write " + _bundle + "/" +
                                     cut.file);
            }
        } catch (const cv::Exception &e) {
            throw js::bind_error("could not write " + _bundle + "/" +
                                 cut.file + ": " + e.what());
        }
    }
    for (const std::string &file : _plan.to_delete) {
        std::filesystem::remove(g_ctx.script_dir / _bundle / file, ec);
        if (ec) {
            throw js::bind_error("could not delete " + _bundle + "/" + file +
                                 ": " + ec.message());
        }
    }
}

void require_anchor_script(const ccat_lang::Program &_prog) {
    const std::vector<std::string> problems =
        ccat_lang::anchor_script_problems(_prog);
    if (!problems.empty()) {
        throw js::bind_error("not an anchor script: " + joined(problems, "") +
                             "; an anchor only judges whether its screen is "
                             "showing and returns true or false");
    }
}


[[= js::quiet{.text = js::str("Updated script {name}")}]][[= js::doc{
    .text = js::str(
        "Replaces an existing bundle's main.ccat and its whole template set in "
        "one step, under one approval. Declare every template the updated "
        "script should have afterwards: an entry with a cut is re-cut from "
        "that "
        "screenshot, an entry with just a name keeps the PNG already there, "
        "and "
        "PNGs you do not declare are deleted. Prefer this "
        "over saving a new "
        "name when changing a skill you already have; save_script is for a new "
        "one, and save_template for adjusting a single template while you work "
        "out its box.")}]]
    [[= js::param_docs(
        js::str("Bundle to update; it must already exist (see list_scripts)."),
        js::str("The new main.ccat source, replacing the old one."),
        js::str(
            "Every template the bundle should have afterwards."))]] std::string
        update_script(std::string name, std::string source,
                      std::vector<template_cut> templates) {
    if (!ccat_lang::safe_leaf_name(name)) {
        throw js::bind_error("name \"" + name + "\" is not a plain file name");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(
            ccat_lang::bundle_main(g_ctx.script_dir, name), ec)) {
        throw js::bind_error("unknown script bundle \"" + name +
                             "\"; save_script creates a new one (existing: " +
                             existing_bundles() + ")");
    }
    // Parsed first: a syntax error answers the model directly, rather than
    // asking a human to read a script that cannot run.
    const std::unique_ptr<ccat_lang::Program> prog = parse_or_throw(source);
    if (const std::string block =
            ccat_lang::script_graph_block(g_ctx.script_dir, name, *prog);
        !block.empty()) {
        throw js::bind_error(block);
    }
    const ccat_lang::bundle_tag tag =
        ccat_lang::bundle_tags(g_ctx.script_dir, name);
    const bool anchor = !tag.anchor_of.empty();
    if (anchor) {
        require_anchor_script(*prog);
    }
    const template_plan plan =
        plan_templates(name, *prog, templates, anchor, false);

    if (needs_approval("update_script")) {
        if (!g_ctx.request_approval) {
            throw js::bind_error(
                "no human available to approve updating the script");
        }
        llm::approval_request req;
        req.tool = "update_script";
        req.summary = "Update script \"" + name +
                      "\" (replace main.ccat and its "
                      "templates)\n\n";
        describe_templates(plan, &req);
        req.summary += source;
        const llm::approval_decision decision = g_ctx.request_approval(req);
        if (!decision.approved) {
            throw js::bind_error(decision.guidance.empty()
                                     ? "user rejected updating the script"
                                     : "user rejected updating the script: " +
                                           decision.guidance);
        }
    }

    // Approved. Nothing above this point touched the disk, so a refusal leaves
    // it byte-for-byte what it was.
    write_bundle(name, source, plan);
    std::string reply =
        "updated \"" + name + "/main.ccat\" (" +
        std::to_string(prog->stmts.size()) + " statements, " +
        std::to_string(source.size()) +
        " chars); templates: " + std::to_string(plan.cuts.size()) + " cut, " +
        std::to_string(plan.kept.size()) + " kept, " +
        std::to_string(plan.to_delete.size()) + " deleted; run it with run(\"" +
        name + "/main.ccat\")";
    if (!tag.edge_of.empty()) {
        reply += "\n" + graph_check(tag.edge_of);
    }
    return reply;
}
CPP_REFLECT_TOOL(update_script)

[[= js::quiet{.text = js::str("Save anchor {graph}.{anchor}")}]][[= js::doc{
    .text = js::str(
        "Saves one screen of a game's graph as anchor bundle <graph>.<anchor>: "
        "golden.md (what the screen is), main.ccat (whether it is showing) and "
        "the templates that script names, under one approval. The script may "
        "only judge -- has, if/elif/else, log, return true/false -- because "
        "every anchor runs on every locate. A new anchor is first checked "
        "against the latest screenshot: when an existing anchor already "
        "recognises it, nothing is saved and you are told which one, so reuse "
        "it. Saving an existing anchor replaces all three, like "
        "update_script. The first anchor of a graph must be \"entry\", the "
        "screen every navigation can start from. Join two anchors with an "
        "edge by save_script(\"<graph>.<from>-<to>\", ...).")}]]
    [[= js::param_docs(
        js::str("Graph name, [a-z0-9_]+, e.g. the game: \"shuaitu\"."),
        js::str("Anchor name, [a-z0-9_]+: the screen, e.g. \"main_city\"."),
        js::str("golden.md: front matter between --- lines (kind: page | "
                "transient | popup, default page; a popup needs close: <bundle "
                "that closes it>), then one natural-language description of "
                "the screen."),
        js::str("main.ccat: judges the screen and returns true or false."),
        js::str("Every template the anchor should have afterwards, as in "
                "update_script."))]] std::string
        save_anchor(std::string graph, std::string anchor, std::string golden,
                    std::string source, std::vector<template_cut> templates) {
    if (!ccat_lang::graph_part_name(graph) ||
        !ccat_lang::graph_part_name(anchor)) {
        throw js::bind_error("graph \"" + graph + "\" and anchor \"" + anchor +
                             "\" must both be [a-z0-9_]+");
    }
    const std::string bundle = graph + "." + anchor;
    std::string error;
    if (!ccat_lang::parse_golden_md(golden, &error)) {
        throw js::bind_error("golden.md: " + error);
    }
    const std::unique_ptr<ccat_lang::Program> prog = parse_or_throw(source);
    require_anchor_script(*prog);
    std::error_code ec;
    const bool is_new = !std::filesystem::is_regular_file(
        g_ctx.script_dir / bundle / "golden.md", ec);
    const template_plan plan =
        plan_templates(bundle, *prog, templates, true, is_new);

    // One screen, one anchor: a second anchor for it would make locate
    // ambiguous there. Only a new one is checked -- an existing anchor
    // recognising its own screen is the point.
    if (is_new && ccat_lang::scan_graph(g_ctx.script_dir, graph).exists) {
        if (g_ctx.cfg == nullptr) {
            throw js::bind_error(
                "no app config to run the existing anchors with");
        }
        cv::Mat frame;
        if (!frame_mat(std::nullopt, &frame, &error)) {
            throw js::bind_error("cannot check for a duplicate anchor: " +
                                 error);
        }
        const ccat_lang::locate_hit hit =
            locate_on_frame(&require_adb(), g_ctx.cfg, graph, frame, &error);
        using ccat_lang::locate_kind;
        if (hit.kind == locate_kind::page ||
            hit.kind == locate_kind::transient ||
            hit.kind == locate_kind::interrupt) {
            throw js::bind_error("this screen is already anchor " + hit.id +
                                 " of graph " + graph +
                                 "; reuse it instead of saving " + anchor);
        }
        if (hit.kind != locate_kind::unknown) {
            throw js::bind_error(
                "cannot check for a duplicate anchor: " +
                (hit.detail.empty() ? error : hit.detail));
        }
    }

    if (needs_approval("save_anchor")) {
        if (!g_ctx.request_approval) {
            throw js::bind_error(
                "no human available to approve saving the anchor");
        }
        llm::approval_request req;
        req.tool = "save_anchor";
        req.summary = std::string(is_new ? "Save new" : "Replace") +
                      " anchor \"" + bundle + "\"\n\n";
        describe_templates(plan, &req);
        req.summary += "golden.md:\n" + golden + "\n\nmain.ccat:\n" + source;
        const llm::approval_decision decision = g_ctx.request_approval(req);
        if (!decision.approved) {
            throw js::bind_error(decision.guidance.empty()
                                     ? "user rejected saving the anchor"
                                     : "user rejected saving the anchor: " +
                                           decision.guidance);
        }
    }

    write_bundle(bundle, source, plan);
    write_text(bundle, "golden.md", golden);
    return "saved anchor \"" + bundle + "\" (golden.md, main.ccat, " +
           std::to_string(plan.cuts.size() + plan.kept.size()) +
           " templates)\n" + graph_check(graph);
}
CPP_REFLECT_TOOL(save_anchor)

[[= js::quiet{.text = js::str("Read graph {name}")}]][[= js::doc{
    .text = js::str(
        "Returns a screen graph as its bundles describe it: every anchor "
        "(bundle <graph>.<anchor> with a golden.md) with its kind and the first "
        "line of its description, every edge (bundle <graph>.<a>-<b>), and "
        "every problem that would stop goto from using it. Read an anchor's or "
        "an edge's script with read_script.")}]]
    [[= js::param_docs(
        js::str("Graph name, the part before the dot (list_scripts marks its "
                "<graph>.entry bundle [graph ...])."))]] std::string
        read_graph(std::string name) {
    const ccat_lang::scanned_graph scanned =
        ccat_lang::scan_graph(g_ctx.script_dir, name);
    if (!scanned.exists) {
        throw js::bind_error("no graph \"" + name + "\": it has no " + name +
                             ".entry anchor; save_anchor creates one "
                             "(existing graphs: " +
                             existing_graphs() + ")");
    }
    return ccat_lang::graph_summary(scanned);
}
CPP_REFLECT_TOOL(read_graph)

[[= js::doc{.text = js::str("Deletes a saved script bundle: its main.ccat and "
                            "every template in the "
                            "folder. It cannot be undone. It is refused while "
                            "another script reaches it "
                            "with run(...) -- update or delete that script "
                            "first (list_scripts shows what "
                            "exists). While human approval is on, the deletion "
                            "is shown to the user and "
                            "nothing is removed unless they approve it.")}]]
    [[= js::param_docs(
        js::str("Bundle to delete, as list_scripts showed it."))]] std::string
        delete_script(std::string name) {
    if (!ccat_lang::safe_leaf_name(name)) {
        throw js::bind_error("name \"" + name + "\" is not a plain file name");
    }
    // Guard first: a deletion that cannot go through is answered here, rather
    // than by showing a human an approval for something that was never going to
    // happen.
    std::string err;
    const ccat_lang::delete_status checked =
        ccat_lang::check_bundle_deletable(g_ctx.script_dir, name, &err);
    if (checked != ccat_lang::delete_status::ok) {
        throw js::bind_error(err);
    }
    if (needs_approval("delete_script")) {
        if (!g_ctx.request_approval) {
            throw js::bind_error(
                "no human available to approve deleting the script");
        }
        llm::approval_request req;
        req.tool = "delete_script";
        req.summary = "delete script bundle \"" + name + "\"";
        const llm::approval_decision decision = g_ctx.request_approval(req);
        if (!decision.approved) {
            throw js::bind_error(decision.guidance.empty()
                                     ? "user rejected deleting the script"
                                     : "user rejected deleting the script: " +
                                           decision.guidance);
        }
    }
    if (ccat_lang::delete_bundle(g_ctx.script_dir, name, &err) !=
        ccat_lang::delete_status::ok) {
        // The guard passed a moment ago, so this is the editor having deleted
        // it in between, or the filesystem refusing.
        throw js::bind_error(err);
    }
    return "deleted \"" + name + "\"";
}
CPP_REFLECT_TOOL(delete_script)

[[= js::doc{
    .text = js::str(
        "Renames a saved script bundle and rewrites every script that runs it, "
        "so "
        "nothing is left pointing at the old name -- do not edit those scripts "
        "yourself afterwards, it is already done here. Refused when a script "
        "reaches "
        "it through a `defs` name instead of a literal path (there is no text "
        "to "
        "rewrite; update that script first), and when the new name is taken or "
        "is "
        "not a plain file name. While human approval is on, the rename and the "
        "scripts it will update are shown to the user and nothing happens "
        "unless "
        "they approve it.")}]]
    [[= js::param_docs(
        js::str("Bundle to rename, as list_scripts showed it."),
        js::str("The new name: a plain file name, not a path."))]] std::string
        rename_script(std::string name, std::string new_name) {
    if (!ccat_lang::safe_leaf_name(name) ||
        !ccat_lang::safe_leaf_name(new_name)) {
        throw js::bind_error("the name must be a plain file name, not a path");
    }
    // Guard first: a rename that cannot go through is answered here, rather
    // than by showing a human an approval for something that was never going to
    // happen.
    std::vector<std::string> referrers;
    std::string err;
    if (ccat_lang::check_bundle_renamable(g_ctx.script_dir, name, new_name,
                                          &referrers, &err) !=
        ccat_lang::rename_status::ok) {
        throw js::bind_error(err);
    }
    if (needs_approval("rename_script")) {
        if (!g_ctx.request_approval) {
            throw js::bind_error(
                "no human available to approve renaming the script");
        }
        llm::approval_request req;
        req.tool = "rename_script";
        // The blast radius is part of what the human is approving.
        req.summary = "rename script bundle \"" + name + "\" to \"" + new_name +
                      "\"" +
                      (referrers.empty()
                           ? " (nothing runs it)"
                           : " (also updates " + join_names(referrers) + ")");
        const llm::approval_decision decision = g_ctx.request_approval(req);
        if (!decision.approved) {
            throw js::bind_error(decision.guidance.empty()
                                     ? "user rejected renaming the script"
                                     : "user rejected renaming the script: " +
                                           decision.guidance);
        }
    }
    std::vector<std::string> rewritten;
    if (ccat_lang::rename_bundle(g_ctx.script_dir, name, new_name, &rewritten,
                                 &err) != ccat_lang::rename_status::ok) {
        throw js::bind_error(err);
    }
    return "renamed \"" + name + "\" to \"" + new_name + "\"" +
           (rewritten.empty() ? ""
                              : " (updated " + join_names(rewritten) + ")");
}
CPP_REFLECT_TOOL(rename_script)

// ponytail: deliberately not registered -- swipe is unverified on a real
// device. Add `CPP_REFLECT_TOOL(swipe)` back once it has been tried there;
// until then the model never sees it and `dispatch` answers "unknown tool".
[[= js::doc{
    .text = js::str("Swipes between two points, in screenshot pixels.")}]]
    [[= js::param_docs(
        js::str("Start X coordinate."), js::str("Start Y coordinate."),
        js::str("End X coordinate."), js::str("End Y coordinate."),
        js::str(
            "Duration in milliseconds; omit for the default."))]] std::string
        swipe(int x1, int y1, int x2, int y2, std::optional<int> duration_ms) {
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
           std::to_string(x2) + "," + std::to_string(y2) + " over " +
           std::to_string(ms) + "ms";
}

} // namespace tools

bool approval_gate_required(const approval_config &_gates,
                            std::string_view _tool) {
    if (!_gates.require) {
        return false;
    }
    if (_tool == "tap") {
        return _gates.gates.tap;
    }
    if (_tool == "run_script") {
        return _gates.gates.run_script;
    }
    if (_tool == "save_script" || _tool == "save_anchor") {
        return _gates.gates.save_script;
    }
    if (_tool == "save_template") {
        return _gates.gates.save_template;
    }
    if (_tool == "update_script") {
        return _gates.gates.update_script;
    }
    if (_tool == "delete_script") {
        return _gates.gates.delete_script;
    }
    if (_tool == "rename_script") {
        return _gates.gates.rename_script;
    }
    if (_tool == "go_home") {
        return _gates.gates.go_home;
    }
    // A gated tool this table has not heard of asks rather than skips: an
    // unguarded surprise is the one failure worth avoiding here.
    return true;
}

void set_context(const tool_context &_ctx) {
    g_ctx = _ctx;
    // Only the picture a call arrived with is the turn's; the screenshot in it
    // and the approvals are session-scoped (see spec §3), so they are not
    // cleared here.
}

void current_screen(screen_state *_out) { *_out = g_ctx.screen; }

std::string tools_json() { return js::tools_json(); }

bool script_running() { return g_script_running.load(); }

void request_script_stop() { g_script_stop.store(true); }

bool emulator_starting() { return g_emulator_running.load(); }

void request_emulator_stop() { g_emulator_stop.store(true); }

void forget_frames() {
    g_frames.clear();
    g_next_shot = 0;
    g_human_region.reset();
}

/// Fills a quiet note's `{param}` names from the call's arguments. Which names
/// are legal was settled when the tool registered (see `js::quiet`), and the
/// binder has since either bound the argument or failed the call, so a name
/// that never arrives here is one the schema rejected. A value that is not text
/// or a number is left as written: the log is one line, and a whole array in it
/// reads worse than the placeholder does.
std::string fill_quiet_note(std::string_view _note,
                            const nlohmann::json &_args) {
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
        const auto it =
            _args.find(std::string(_note.substr(open + 1, close - open - 1)));
        if (it != _args.end() &&
            (it->is_string() || it->is_number() || it->is_boolean())) {
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
    // Cleared before the busy check, so no early return can leave a frame
    // number behind for the next call to report as its own.
    g_last_shot = 0;
    if (g_ctx.adb_busy && g_ctx.adb_busy()) {
        out.error = "adb busy: an automation cycle is running";
        return out;
    }
    const nlohmann::json args =
        nlohmann::json::parse(_args_json, nullptr, false);
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
