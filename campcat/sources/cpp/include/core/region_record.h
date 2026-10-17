#pragma once

#include <filesystem>
#include <string>

#include <opencv2/core.hpp>

#include "core/llm/llm_tools.h"

namespace campcat {

/// One region a human redrew, the only ground truth the region scorer will
/// learn from: the screen, what the model proposed on it, and what the human
/// drew instead. The regions are `llm::region` -- the type the tools and the
/// approval layer already speak -- so a box, a circle and a diamond are all
/// sampled, each written with its own fields.
struct region_sample {
    /// The screen the human was looking at, decoded. Encoded here, and one file
    /// per sample: no deduplication, so a screen redrawn twice costs two PNGs.
    cv::Mat frame;
    /// What the model proposed, and what the human drew over it.
    llm::region given;
    llm::region human;
};

/**
 * @brief Writes one sample as its own folder: `<_root>/<time>-<seq>/` holding
 * `frame.png` and `meta.json`.
 *
 * Never throws: a failed write is logged and reports false, because a sample is
 * bookkeeping and must not undo a template the human just approved.
 *
 * @param[in] _root Directory to write into; created when missing.
 * @param[in] _sample The redrawn region and the frame it was measured on.
 * @return true when both the frame and the meta file were written.
 */
bool record_region_sample(const std::filesystem::path &_root,
                          const region_sample &_sample);

} // namespace campcat
