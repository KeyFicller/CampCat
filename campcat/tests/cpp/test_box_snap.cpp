// `none` is the off switch now, so it has to mean exactly what never calling
// the snap meant. These two cases are a pair on purpose: only together do they
// say "none does nothing" and "everyone else still does something", and either
// one alone passes for the wrong reason.

#include <core/box_snap.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <gtest/gtest.h>

namespace {

/// A white square on black, deliberately smaller than the box handed to the
/// snap, so a real strategy has something to pull onto and `none` has something
/// it would visibly fail to resist.
cv::Mat square_on_black() {
    cv::Mat img(32, 32, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::rectangle(img, cv::Rect(10, 10, 12, 12), cv::Scalar(255, 255, 255),
                  cv::FILLED);
    return img;
}

const cv::Rect kLooseBox(8, 8, 16, 16);
const cv::Rect kSquare(10, 10, 12, 12);

TEST(BoxSnap, NoneReturnsTheBoxItWasGiven) {
    campcat::box_snap_options opts;
    opts.strategy = campcat::box_snap_strategy::none;
    const campcat::box_snap_result r =
        campcat::snap_box(square_on_black(), kLooseBox, opts);
    EXPECT_FALSE(r.moved);
    EXPECT_EQ(r.box, kLooseBox);
    EXPECT_EQ(r.why, "snapping is off");
}

TEST(BoxSnap, OtsuStillPullsThatSameBoxOntoTheSquare) {
    campcat::box_snap_options opts;
    opts.strategy = campcat::box_snap_strategy::otsu;
    const campcat::box_snap_result r =
        campcat::snap_box(square_on_black(), kLooseBox, opts);
    EXPECT_TRUE(r.moved);
    EXPECT_EQ(r.box, kSquare);
}

} // namespace
