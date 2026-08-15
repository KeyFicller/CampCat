#include "core/template_matcher.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdio>

namespace campcat {

template_matcher::template_matcher(double _default_threshold, bool _multiscale)
    : m_default_threshold(_default_threshold), m_multiscale(_multiscale) {}

match_result template_matcher::match_once(const cv::Mat& _screen_bgr,
                                          const cv::Mat& _templ_bgr,
                                          double _threshold,
                                          cv::Rect _roi) const {
  if (_screen_bgr.empty() || _templ_bgr.empty()) {
    return {};
  }

  cv::Mat hay = _screen_bgr;
  if (_roi.area() > 0) {
    hay = _screen_bgr(_roi);
  }

  if (hay.cols < _templ_bgr.cols || hay.rows < _templ_bgr.rows) {
    return {};
  }

  cv::Mat result;
  cv::matchTemplate(hay, _templ_bgr, result, cv::TM_CCOEFF_NORMED);
  double min_val = 0;
  double max_val = 0;
  cv::Point min_loc;
  cv::Point max_loc;
  cv::minMaxLoc(result, &min_val, &max_val, &min_loc, &max_loc);

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
  mr.found = max_val >= _threshold;
  return mr;
}

match_result template_matcher::match(const cv::Mat& _screen_bgr,
                                     const cv::Mat& _templ_bgr,
                                     double _threshold,
                                     cv::Rect _roi) const {
  if (m_multiscale) {
    return match_multiscale_inner(_screen_bgr, _templ_bgr, _threshold, _roi);
  }
  return match_once(_screen_bgr, _templ_bgr, _threshold, _roi);
}

match_result template_matcher::match_multiscale_inner(
    const cv::Mat& _screen_bgr, const cv::Mat& _templ_bgr, double _threshold,
    cv::Rect _roi) const {
  match_result best;
  const double scales[] = {1.0, 0.95, 0.9, 0.85, 0.8};
  for (double s : scales) {
    cv::Mat tpl_r;
    cv::resize(_templ_bgr, tpl_r, cv::Size(), s, s, cv::INTER_AREA);
    if (tpl_r.cols < 8 || tpl_r.rows < 8) {
      continue;
    }
    match_result mr = match_once(_screen_bgr, tpl_r, _threshold, _roi);
    if (mr.confidence > best.confidence) {
      best = mr;
    }
  }
  best.found = best.confidence >= _threshold;
  return best;
}

std::optional<match_result> template_matcher::match_file(
    const cv::Mat& _screen_bgr, const std::string& _png_path,
    double _threshold, cv::Rect _roi) const {
  cv::Mat templ;
  if (!load_template(_png_path, &templ)) {
    return std::nullopt;
  }
  const double thr = _threshold > 0 ? _threshold : m_default_threshold;
  return match(_screen_bgr, templ, thr, _roi);
}

bool template_matcher::load_template(const std::string& _png_path,
                                       cv::Mat* _templ_out) const {
  cv::Mat t = cv::imread(_png_path, cv::IMREAD_COLOR);
  if (t.empty()) {
    return false;
  }
  *_templ_out = t;
  return true;
}

cv::Mat template_matcher::annotate(const cv::Mat& _screen_bgr,
                                   const match_result& _r,
                                   const cv::Scalar& _color) {
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

} // namespace campcat
