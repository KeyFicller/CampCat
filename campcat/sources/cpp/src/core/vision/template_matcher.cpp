#include "core/template_matcher.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace campcat {

namespace {

/// Masked cross-correlation built from TM_CCORR, the one mode OpenCV documents
/// as mask-capable; an all-ones mask reduces it to plain TM_CCOEFF_NORMED.
/// TM_CCORR_NORMED itself never subtracts the mean, so a bright flat patch
/// scored 0.99-1.00 on real targets that were not there: no threshold could
/// separate a hit from a miss.
///
/// @param[in] _hay_bgr Screen region to search, 3-channel.
/// @param[in] _templ_bgr Template to look for, 3-channel, no larger than hay.
/// @param[in] _mask Membership, any depth: non-zero pixels take part in the
/// score, zero pixels are ignored.
/// @return CV_64F score map, higher is better, in [-1, 1].
cv::Mat masked_ncc(const cv::Mat &_hay_bgr, const cv::Mat &_templ_bgr,
                   const cv::Mat &_mask) {
    cv::Mat mask_bin;
    cv::compare(_mask, 0, mask_bin, cv::CMP_GT);
    cv::Mat mask_f;
    mask_bin.convertTo(mask_f, CV_32F, 1.0 / 255.0);
    const double n = cv::sum(mask_f)[0];

    cv::Mat hay_ch[3];
    cv::Mat templ_ch[3];
    cv::Mat hay_raw[3];
    cv::Mat templ_raw[3];
    cv::split(_hay_bgr, hay_raw);
    cv::split(_templ_bgr, templ_raw);
    // Float: `ones` below is a matchTemplate argument, so the types must agree.
    for (int c = 0; c < 3; ++c) {
        hay_raw[c].convertTo(hay_ch[c], CV_32F);
        templ_raw[c].convertTo(templ_ch[c], CV_32F);
    }
    const cv::Mat ones = cv::Mat::ones(_templ_bgr.size(), CV_32F);

    cv::Mat num;
    cv::Mat var_s;
    double var_t = 0.0;
    for (int c = 0; c < 3; ++c) {
        const cv::Mat &t = templ_ch[c];
        const double st = cv::sum(t.mul(mask_f))[0];
        const double st2 = cv::sum(t.mul(t).mul(mask_f))[0];
        cv::Mat a;
        cv::Mat b;
        cv::Mat sq;
        cv::matchTemplate(hay_ch[c], t, a, cv::TM_CCORR, mask_bin);
        cv::matchTemplate(hay_ch[c], ones, b, cv::TM_CCORR, mask_bin);
        cv::matchTemplate(hay_ch[c].mul(hay_ch[c]), ones, sq, cv::TM_CCORR,
                          mask_bin);
        // Double from here: the terms run to ~1e9, and the score is a
        // difference of same-sized ones that float32 would cancel away.
        a.convertTo(a, CV_64F);
        b.convertTo(b, CV_64F);
        sq.convertTo(sq, CV_64F);
        const cv::Mat nc = a - st * b / n;
        num = num.empty() ? nc : num + nc;
        const cv::Mat vs = sq - b.mul(b) / n;
        var_s = var_s.empty() ? vs : var_s + vs;
        var_t += st2 - st * st / n;
    }

    if (var_t <= 1e-9) {
        // Nothing inside the mask varies, so no candidate is better than another.
        return cv::Mat::zeros(num.size(), CV_64F);
    }
    cv::Mat den;
    cv::max(var_s, cv::Mat::zeros(var_s.size(), CV_64F), var_s);
    cv::sqrt(var_s, den);
    den *= std::sqrt(var_t);
    cv::Mat result;
    cv::divide(num, den, result);
    // A window whose masked content is flat carries no correlation. Report 0
    // there so a ratio over a near-zero denominator cannot win the max.
    cv::Mat flat;
    cv::compare(den, 1e-9, flat, cv::CMP_LT);
    result.setTo(0.0, flat);
    return result;
}

} // namespace

template_matcher::template_matcher(double _box_threshold,
                                   double _circle_threshold, bool _multiscale)
    : m_box_threshold(_box_threshold),
      m_circle_threshold(_circle_threshold), m_multiscale(_multiscale) {}

match_result template_matcher::match_once(const cv::Mat &_screen_bgr,
                                          const cv::Mat &_templ_bgr,
                                          double _threshold, cv::Rect _roi,
                                          const cv::Mat &_mask) const {
    if (_screen_bgr.empty() || _templ_bgr.empty()) {
        return {};
    }

    cv::Mat hay = _screen_bgr;
    if (_roi.area() > 0) {
        const cv::Rect frame{0, 0, _screen_bgr.cols, _screen_bgr.rows};
        if ((_roi & frame) != _roi) {
            return {}; // ROI must lie fully inside the captured frame
        }
        hay = _screen_bgr(_roi);
    }

    if (hay.cols < _templ_bgr.cols || hay.rows < _templ_bgr.rows) {
        return {};
    }

    cv::Mat result;
    if (_mask.empty()) {
        cv::matchTemplate(hay, _templ_bgr, result, cv::TM_CCOEFF_NORMED);
    } else {
        result = masked_ncc(hay, _templ_bgr, _mask);
    }
    double max_val = 0;
    cv::Point max_loc;
    cv::minMaxLoc(result, nullptr, &max_val, nullptr, &max_loc);

    match_result mr;
    mr.confidence = max_val;
    const int cx =
        max_loc.x + _templ_bgr.cols / 2 + (_roi.area() > 0 ? _roi.x : 0);
    const int cy =
        max_loc.y + _templ_bgr.rows / 2 + (_roi.area() > 0 ? _roi.y : 0);
    mr.center = {cx, cy};
    mr.bbox = cv::Rect(max_loc.x + (_roi.area() > 0 ? _roi.x : 0),
                       max_loc.y + (_roi.area() > 0 ? _roi.y : 0),
                       _templ_bgr.cols, _templ_bgr.rows);
    mr.threshold = _threshold;
    mr.found = max_val >= _threshold;
    return mr;
}

match_result template_matcher::match(const cv::Mat &_screen_bgr,
                                     const cv::Mat &_templ_bgr,
                                     double _threshold, cv::Rect _roi,
                                     const cv::Mat &_mask) const {
    // <= 0 means "use the stored default", per kind: a raw 0 would otherwise
    // make `confidence >= threshold` always true, and a disc is held to a
    // higher bar than a square.
    const double thr =
        _threshold > 0
            ? _threshold
            : (_mask.empty() ? m_box_threshold : m_circle_threshold);
    if (m_multiscale) {
        return match_multiscale_inner(_screen_bgr, _templ_bgr, thr, _roi,
                                      _mask);
    }
    return match_once(_screen_bgr, _templ_bgr, thr, _roi, _mask);
}

match_result template_matcher::match_multiscale_inner(
    const cv::Mat &_screen_bgr, const cv::Mat &_templ_bgr, double _threshold,
    cv::Rect _roi, const cv::Mat &_mask) const {
    match_result best;
    const double scales[] = {1.0, 0.95, 0.9, 0.85, 0.8};
    for (double s : scales) {
        cv::Mat tpl_r;
        cv::resize(_templ_bgr, tpl_r, cv::Size(), s, s, cv::INTER_AREA);
        if (tpl_r.cols < 8 || tpl_r.rows < 8) {
            continue;
        }
        cv::Mat mask_r;
        if (!_mask.empty()) {
            // NEAREST, not the INTER_AREA above: interpolating the alpha would
            // grow a soft grey rim, and a mask is membership, not a shade.
            cv::resize(_mask, mask_r, tpl_r.size(), 0, 0, cv::INTER_NEAREST);
            cv::compare(mask_r, 0, mask_r, cv::CMP_GT);
        }
        match_result mr = match_once(_screen_bgr, tpl_r, _threshold, _roi,
                                     mask_r);
        if (mr.confidence > best.confidence) {
            best = mr;
        }
    }
    best.threshold = _threshold; // even when no scale ran, the bar is known
    best.found = best.confidence >= _threshold;
    return best;
}

std::optional<match_result>
template_matcher::match_file(const cv::Mat &_screen_bgr,
                             const std::string &_png_path, double _threshold,
                             cv::Rect _roi) const {
    cv::Mat templ;
    cv::Mat mask;
    if (!load_template(_png_path, &templ, &mask)) {
        return std::nullopt;
    }
    // A disc template carries its own bar: the caller passes one threshold and
    // cannot know the template's kind before it is on disk. Six call sites
    // would otherwise each have to learn about shapes.
    const double thr = mask.empty() ? _threshold : m_circle_threshold;
    return match(_screen_bgr, templ, thr, _roi, mask);
}

bool template_matcher::load_template(const std::string &_png_path,
                                     cv::Mat *_templ_out,
                                     cv::Mat *_mask_out) const {
    // UNCHANGED, because the alpha channel *is* the shape: COLOR would drop it
    // and a disc would match as a square of whatever the transparent pixels
    // hold.
    cv::Mat t = cv::imread(_png_path, cv::IMREAD_UNCHANGED);
    if (t.empty()) {
        return false;
    }
    if (t.channels() == 4) {
        cv::cvtColor(t, *_templ_out, cv::COLOR_BGRA2BGR);
        cv::Mat alpha;
        cv::extractChannel(t, alpha, 3);
        cv::compare(alpha, 0, *_mask_out, cv::CMP_GT);
        return true;
    }
    if (t.channels() == 1) {
        cv::cvtColor(t, *_templ_out, cv::COLOR_GRAY2BGR);
    } else {
        *_templ_out = t;
    }
    _mask_out->release();
    return true;
}

cv::Mat template_matcher::annotate(const cv::Mat &_screen_bgr,
                                   const match_result &_r,
                                   const cv::Scalar &_color) {
    cv::Mat out = _screen_bgr.clone();
    if (!_r.found) {
        return out;
    }
    cv::rectangle(out, _r.bbox, _color, 2);
    cv::drawMarker(out, _r.center, _color, cv::MARKER_CROSS, 14, 2);
    return out;
}

cv::Mat template_matcher::annotate_debug(const cv::Mat &_screen_bgr,
                                         const match_result &_r) {
    cv::Mat out = _screen_bgr.clone();
    const cv::Scalar color =
        _r.found ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
    if (_r.bbox.area() > 0) {
        cv::rectangle(out, _r.bbox, color, 2);
    }
    cv::drawMarker(out, _r.center, color, cv::MARKER_CROSS, 20, 2);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s conf=%.3f @(%d,%d)",
                  _r.found ? "HIT" : "MISS", _r.confidence, _r.center.x,
                  _r.center.y);
    const int tx = std::max(8, _r.bbox.x);
    const int ty = std::max(24, _r.bbox.y - 8);
    cv::putText(out, buf, cv::Point(tx, ty), cv::FONT_HERSHEY_SIMPLEX, 0.8,
                color, 2);
    return out;
}

std::string match_verdict_text(const match_result &_r) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  " (best=%.3f box=%d,%d,%d,%d threshold=%.3f)", _r.confidence,
                  _r.bbox.x, _r.bbox.y, _r.bbox.width, _r.bbox.height,
                  _r.threshold);
    return buf;
}

} // namespace campcat
