#pragma once

#include <string>

#include <opencv2/core.hpp>

namespace campcat {

/// How a box is pulled onto the element's boundary.
///
/// Serialised by reflection as a string (`"otsu"` / `"step"`), so the value in
/// `config.json` is the enumerator's own name and stays readable.
enum class box_snap_strategy {
    /// Leave the box where it is. This is the off switch: `snap_box` returns the
    /// caller's own box untouched, so a `none` that reaches the engine costs a
    /// little and means exactly what a caller that never called it meant.
    none,
    /// Otsu threshold, connected components, take the one under the box centre.
    /// Measured: smallest worst case (+2px on the three test elements).
    otsu,
    /// Per edge, the strongest colour step across it. Measured: largest single
    /// win (18px -> 10px on one element) but it has no way to say "I am not sure".
    step,
};

/// Limits a snap must respect. Failing any of them keeps the model's box.
///
/// The defaults come from measurement, not taste: a snap moved the box 10-32px
/// on real screenshots, the same order as the noise it removes, so `max_shift`
/// is what stops it running onto a neighbouring element.
struct box_snap_options {
    box_snap_strategy strategy = box_snap_strategy::otsu;
    /// Most an edge may move, as a fraction of that side of the box.
    double max_shift = 0.30;
    /// Smallest and largest area the snapped box may have, relative to the original.
    double area_lo = 0.3;
    double area_hi = 3.5;
};

/// What one snap attempt produced.
struct box_snap_result {
    /// The box to use: the snapped one, or the caller's own when nothing moved.
    cv::Rect box;
    /// True when `box` is not the box that came in.
    bool moved = false;
    /// Why the snap was declined; empty when it moved. For the log and for the
    /// note the tool appends, never for control flow.
    std::string why;
};

/**
 * @brief Pulls `_box` onto the boundary of the element under it.
 *
 * `_box` must already be inside the picture (the caller has been through
 * `box_on_screen`), and the returned `box` is guaranteed to be as well. A result
 * that fails any limit in `_opts` is the input box unchanged, with `moved` false
 * and `why` filled: this never throws and never returns a box the caller may not
 * use, because snapping is an optimisation the tool performs on the way to doing
 * its real job, not a decision of its own.
 */
box_snap_result snap_box(const cv::Mat &_bgr, const cv::Rect &_box,
                         const box_snap_options &_opts = {});

} // namespace campcat
