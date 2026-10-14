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
    double threshold = 0.0;
};

/**
 * @brief The matcher's verdict as one clause for an error message: how close
 * the best candidate came, where it was, and the bar it had to clear.
 *
 * Worded here rather than at each call site, so every template failure reports
 * the same numbers and a probe can assert the wording without a device. The bar
 * is read off `_r.threshold`, so a disc reports the score it was judged on; the
 * best candidate is reported even below it, to tell a near miss from a wrong
 * screen.
 *
 * @param[in] _r The correlation outcome, hit or miss.
 * @return One clause, leading space included, ready to append to a sentence.
 */
std::string match_verdict_text(const match_result &_r);

/**
 * @brief OpenCV-based template matcher with optional brute-force scale sweep.
 */
class template_matcher {
public:
    /**
     * @brief Construct matcher knobs for consumer reuse.
     * @param[in] _box_threshold Baseline similarity threshold reused by
     * callers, for square (3-channel) templates.
     * @param[in] _circle_threshold Baseline for disc templates, on the same
     * scale as `_box_threshold` but higher: a disc carries less texture
     * than the square around it, so the two are not interchangeable.
     * @param[in] _multiscale Enables scaled template search heuristic.
     */
    explicit template_matcher(double _box_threshold,
                              double _circle_threshold, bool _multiscale);

    /**
     * @brief Correlate templ against screen optionally restricted by ROI
     * rectangle.
     * @param[in] _screen_bgr Full frame BGR mat.
     * @param[in] _templ_bgr Template cropped mat.
     * @param[in] _threshold Confidence threshold; <= 0 falls back to the stored
     * default for the template's kind.
     * @param[in] _roi Restrict search window (empty=all); must lie inside the
     * frame.
     * @param[in] _mask Non-empty for a disc template: pixels the mask excludes
     * take no part in the score.
     * @return match_result honoring best correlated patch.
     */
    match_result match(const cv::Mat &_screen_bgr, const cv::Mat &_templ_bgr,
                       double _threshold, cv::Rect _roi = {},
                       const cv::Mat &_mask = {}) const;

    /**
     * @brief Load png from `_png_path`, forward to `match` when decoding
     * succeeds.
     * @param[in] _screen_bgr Incoming camera frame snapshot.
     * @param[in] _png_path Template asset path on filesystem.
     * @param[in] _threshold Similarity cutoff; <= 0 falls back to the stored
     * default. Ignored for a disc template, whose bar is the matcher's own:
     * callers carry one number, and they cannot know the template's kind before
     * it is loaded.
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
     * @param[out] _templ_out Destination mat buffer, always 3-channel BGR.
     * @param[out] _mask_out Filled from the alpha channel of a 4-channel PNG
     * (255 inside the shape, 0 outside); released for a 3-channel one.
     * @return False when codecs fail or filesystem missing payload.
     */
    bool load_template(const std::string &_png_path, cv::Mat *_templ_out,
                       cv::Mat *_mask_out) const;

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
                            cv::Rect _roi, const cv::Mat &_mask) const;

    match_result match_multiscale_inner(const cv::Mat &_screen_bgr,
                                        const cv::Mat &_templ_bgr,
                                        double _threshold, cv::Rect _roi,
                                        const cv::Mat &_mask) const;

    double m_box_threshold;
    double m_circle_threshold;
    bool m_multiscale;
};

} // namespace campcat
