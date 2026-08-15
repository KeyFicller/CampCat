#pragma once

#include <opencv2/core.hpp>

#include <optional>
#include <string>

namespace campcat {

/**
 * @brief Confidence payload returned after template-vs-screen correlation.
 */
struct match_result {
  bool found = false;
  cv::Point center{};
  double confidence = 0.0;
  cv::Rect bbox{};
};

/**
 * @brief OpenCV-based template matcher with optional brute-force scale sweep.
 */
class template_matcher {
public:
  /**
   * @brief Construct matcher knobs for consumer reuse.
   * @param[in] _default_threshold Baseline similarity threshold reused by callers.
   * @param[in] _multiscale Enables scaled template search heuristic.
   */
  explicit template_matcher(double _default_threshold, bool _multiscale);

  /**
   * @brief Correlate templ against screen optionally restricted by ROI rectangle.
   * @param[in] _screen_bgr Full frame BGR mat.
   * @param[in] _templ_bgr Template cropped mat.
   * @param[in] _threshold Confidence threshold overriding defaults per call-site.
   * @param[in] _roi Restrict search window (empty=all).
   * @return match_result honoring best correlated patch.
   */
  match_result match(const cv::Mat &_screen_bgr, const cv::Mat &_templ_bgr,
                     double _threshold, cv::Rect _roi = {}) const;

  /**
   * @brief Load png from `_png_path`, forward to `match` when decoding succeeds.
   * @param[in] _screen_bgr Incoming camera frame snapshot.
   * @param[in] _png_path Template asset path on filesystem.
   * @param[in] _threshold Similarity cutoff.
   * @param[in] _roi Optional ROI cropping search space.
   * @return nullopt when unreadable PNG; populated match payload otherwise.
   */
  std::optional<match_result> match_file(const cv::Mat &_screen_bgr,
                                         const std::string &_png_path,
                                         double _threshold,
                                         cv::Rect _roi = {}) const;

  /**
   * @brief Read png bytes into templ mat honoring OpenCV codecs.
   * @param[in] _png_path Template PNG path candidate.
   * @param[out] _templ_out Destination mat buffer.
   * @return False when codecs fail or filesystem missing payload.
   */
  bool load_template(const std::string &_png_path, cv::Mat *_templ_out) const;

  /**
   * @brief Draw rectangle/center overlays for QA screenshots.
   * @param[in] _screen_bgr Mutable copy base for overlays.
   * @param[in] _r Confidence payload annotated.
   * @param[in] _color BGR stroke color forwarded to rectangle primitive.
   * @return Cloned annotated mat safe for persistence.
   */
  static cv::Mat annotate(const cv::Mat &_screen_bgr, const match_result &_r,
                          const cv::Scalar &_color);

  /**
   * @brief Annotate even when below threshold (uses best peak center/bbox).
   */
  static cv::Mat annotate_debug(const cv::Mat &_screen_bgr,
                                const match_result &_r);

private:
  match_result match_once(const cv::Mat &_screen_bgr,
                          const cv::Mat &_templ_bgr, double _threshold,
                          cv::Rect _roi) const;

  match_result match_multiscale_inner(const cv::Mat &_screen_bgr,
                                      const cv::Mat &_templ_bgr,
                                      double _threshold,
                                      cv::Rect _roi) const;

  double m_default_threshold;
  bool m_multiscale;
};

} // namespace campcat
