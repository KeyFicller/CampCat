#include "core/box_snap.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <opencv2/imgproc.hpp>

// ponytail: the checks for this file live out of tree (/tmp/box_snap_check.cpp
// against /tmp/box_snap_ref.py), because the repo has no test framework and one
// throwaway program beat adding one. Nothing guards a regression once that shell
// is gone: bring them in tree the second time this algorithm needs changing.
namespace campcat {
namespace {

/// Band a candidate edge is searched in, as a fraction of that side of the box.
constexpr double k_band = 0.30;
/// Thickness of the strip whose mean colour is taken either side of an edge.
constexpr int k_strip = 5;
/// Narrowest box worth returning; below this a "snap" is a collapse.
constexpr int k_min_side = 4;

/// The component under the box centre, as a box in picture pixels. Nullopt when
/// neither polarity gives one.
///
/// `_gray` is single channel. The box is grown by a quarter of its own size
/// first, so an element the model framed too tightly still has whole.
std::optional<cv::Rect> snap_otsu(const cv::Mat &_gray, const cv::Rect &_box) {
    // Truncated after the arithmetic, not before: the margin is a fraction, and
    // rounding it first moves the search window a pixel off the prototype's.
    const int x0 = std::max(0, static_cast<int>(_box.x - _box.width * 0.25));
    const int y0 = std::max(0, static_cast<int>(_box.y - _box.height * 0.25));
    const int x1 = std::min(_gray.cols,
                            static_cast<int>(_box.x + _box.width * 1.25));
    const int y1 = std::min(_gray.rows,
                            static_cast<int>(_box.y + _box.height * 1.25));
    if (x1 - x0 < 2 || y1 - y0 < 2) {
        return std::nullopt;
    }
    const cv::Mat region = _gray(cv::Rect(x0, y0, x1 - x0, y1 - y0));
    const int cx = static_cast<int>(std::min(
        std::max(_box.x + _box.width / 2.0 - x0, 0.0),
        static_cast<double>(region.cols - 1)));
    const int cy = static_cast<int>(std::min(
        std::max(_box.y + _box.height / 2.0 - y0, 0.0),
        static_cast<double>(region.rows - 1)));

    std::optional<cv::Rect> best;
    // Both polarities: whether the element is dark on light is not known here,
    // and Otsu picks the split either way. The smaller of the two wins, which is
    // the one that hugged the element rather than its surroundings.
    for (const int flag : {cv::THRESH_BINARY, cv::THRESH_BINARY_INV}) {
        cv::Mat mask;
        cv::threshold(region, mask, 0, 255, flag | cv::THRESH_OTSU);
        cv::Mat labels;
        cv::Mat stats;
        cv::Mat centroids;
        const int count =
            cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8);
        if (count <= 1) {
            continue;
        }
        const int label = labels.at<int>(cy, cx);
        if (label == 0) {
            continue; // the centre sits on the background, so nothing to take
        }
        const cv::Rect local(stats.at<int>(label, cv::CC_STAT_LEFT),
                             stats.at<int>(label, cv::CC_STAT_TOP),
                             stats.at<int>(label, cv::CC_STAT_WIDTH),
                             stats.at<int>(label, cv::CC_STAT_HEIGHT));
        // A "component" that is the whole search window is the window's own
        // shape, not the element's: Otsu found nothing to split, which is what a
        // flat area does. Taking it would hand back the margin the caller grew
        // the box by.
        if (local.width == region.cols && local.height == region.rows) {
            continue;
        }
        const cv::Rect candidate(local.x + x0, local.y + y0, local.width,
                                 local.height);
        if (!best || candidate.area() < best->area()) {
            best = candidate;
        }
    }
    return best;
}

/// Sum of per-channel colour differences between two same-sized patches.
double colour_gap(const cv::Mat &_float_bgr, const cv::Rect &_a,
                  const cv::Rect &_b) {
    const cv::Scalar ma = cv::mean(_float_bgr(_a));
    const cv::Scalar mb = cv::mean(_float_bgr(_b));
    double gap = 0.0;
    for (int c = 0; c < 3; ++c) {
        gap += std::abs(ma[c] - mb[c]);
    }
    return gap;
}

/// One edge, moved to where the colour step across it is strongest.
///
/// `_from`/`_to` bound the search band, `_span` is the box side being searched
/// along (so the strip runs the length of the element, not across it), and
/// `_limit` is the picture size on that axis.
int snap_edge(const cv::Mat &_float_bgr, int _edge, int _span, int _limit,
              int _x, int _y, int _w, int _h, bool _vertical) {
    const int lo = static_cast<int>(
        std::max(static_cast<double>(k_strip + 1), _edge - _span * k_band));
    const int hi = static_cast<int>(std::min(
        static_cast<double>(_limit - k_strip - 1), _edge + _span * k_band));
    int best = _edge;
    double best_gap = -1.0;
    for (int p = lo; p <= hi; ++p) {
        // Strictly greater, so a tie keeps the first candidate: the same edge the
        // prototype's max() picks, which scans the same way.
        const double gap =
            _vertical
                ? colour_gap(_float_bgr, cv::Rect(_x, p - k_strip, _w, k_strip),
                             cv::Rect(_x, p, _w, k_strip))
                : colour_gap(_float_bgr, cv::Rect(p - k_strip, _y, k_strip, _h),
                             cv::Rect(p, _y, k_strip, _h));
        if (gap > best_gap) {
            best_gap = gap;
            best = p;
        }
    }
    // Nothing scored: the band was empty or every candidate was flat. Leave the
    // edge where the model put it rather than moving it to an arbitrary p.
    return best_gap < 0.0 ? _edge : best;
}

/// Each edge moved to its strongest colour step. `_float_bgr` is CV_32F.
cv::Rect snap_step(const cv::Mat &_float_bgr, const cv::Rect &_box) {
    const int x0 = std::max(0, _box.x);
    const int y0 = std::max(0, _box.y);
    const int x1 = std::min(_float_bgr.cols, _box.x + _box.width);
    const int y1 = std::min(_float_bgr.rows, _box.y + _box.height);
    if (x1 - x0 < 1 || y1 - y0 < 1) {
        return _box;
    }
    const int left = snap_edge(_float_bgr, _box.x, _box.width,
                               _float_bgr.cols, x0, y0, x1 - x0, y1 - y0, false);
    const int right = snap_edge(_float_bgr, _box.x + _box.width, _box.width,
                                _float_bgr.cols, x0, y0, x1 - x0, y1 - y0, false);
    const int top = snap_edge(_float_bgr, _box.y, _box.height, _float_bgr.rows,
                              x0, y0, x1 - x0, y1 - y0, true);
    const int bottom = snap_edge(_float_bgr, _box.y + _box.height, _box.height,
                                 _float_bgr.rows, x0, y0, x1 - x0, y1 - y0, true);
    return cv::Rect(left, top, std::max(1, right - left),
                    std::max(1, bottom - top));
}

/// Whether a candidate may replace the original. `_why` names the first failure.
bool acceptable(const cv::Rect &_orig, const cv::Rect &_cand,
                const cv::Size &_frame, const box_snap_options &_opts,
                std::string *_why) {
    if (_cand.width < k_min_side || _cand.height < k_min_side) {
        *_why = "the snapped box is degenerate";
        return false;
    }
    if (std::abs(_cand.x - _orig.x) > _orig.width * _opts.max_shift) {
        *_why = "the left edge moved too far";
        return false;
    }
    if (std::abs(_cand.y - _orig.y) > _orig.height * _opts.max_shift) {
        *_why = "the top edge moved too far";
        return false;
    }
    if (std::abs((_cand.x + _cand.width) - (_orig.x + _orig.width)) >
        _orig.width * _opts.max_shift) {
        *_why = "the right edge moved too far";
        return false;
    }
    if (std::abs((_cand.y + _cand.height) - (_orig.y + _orig.height)) >
        _orig.height * _opts.max_shift) {
        *_why = "the bottom edge moved too far";
        return false;
    }
    // The centre has to survive: a box that slid half off the element and landed
    // on a neighbour is worse than the loose box that came in.
    const double cx = _orig.x + _orig.width / 2.0;
    const double cy = _orig.y + _orig.height / 2.0;
    if (cx < _cand.x || cx > _cand.x + _cand.width || cy < _cand.y ||
        cy > _cand.y + _cand.height) {
        *_why = "the box centre escaped";
        return false;
    }
    const double ratio = static_cast<double>(_cand.area()) /
                         static_cast<double>(std::max(1, _orig.area()));
    if (ratio < _opts.area_lo || ratio > _opts.area_hi) {
        *_why = "the snapped area is out of range";
        return false;
    }
    // Last, and deliberately not left to the pixel work above: the caller hands
    // this box to a tap or a crop, so anything off the picture has to stop here.
    if (_cand.x < 0 || _cand.y < 0 || _cand.x + _cand.width > _frame.width ||
        _cand.y + _cand.height > _frame.height) {
        *_why = "the snapped box is off the picture";
        return false;
    }
    return true;
}

} // namespace

box_snap_result snap_box(const cv::Mat &_bgr, const cv::Rect &_box,
                         const box_snap_options &_opts) {
    box_snap_result out;
    out.box = _box;
    if (_bgr.empty()) {
        out.why = "no picture to snap against";
        return out;
    }
    if (_box.x < 0 || _box.y < 0 || _box.width <= 0 || _box.height <= 0 ||
        _box.x + _box.width > _bgr.cols || _box.y + _box.height > _bgr.rows) {
        out.why = "the box given is already off the picture";
        return out;
    }

    std::optional<cv::Rect> candidate;
    if (_opts.strategy == box_snap_strategy::otsu) {
        cv::Mat gray;
        cv::cvtColor(_bgr, gray, cv::COLOR_BGR2GRAY);
        candidate = snap_otsu(gray, _box);
    } else {
        cv::Mat floats;
        _bgr.convertTo(floats, CV_32F);
        candidate = snap_step(floats, _box);
    }
    if (!candidate) {
        out.why = "no element found under the box";
        return out;
    }
    if (!acceptable(_box, *candidate, _bgr.size(), _opts, &out.why)) {
        return out;
    }
    if (*candidate == _box) {
        out.why = "already tight";
        return out;
    }
    out.box = *candidate;
    out.moved = true;
    return out;
}

} // namespace campcat
