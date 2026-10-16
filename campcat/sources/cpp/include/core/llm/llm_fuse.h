#pragma once

#include <string>

namespace campcat::llm {

/// A cap at or above this is the "never trips" stand-in, drawn as infinity.
constexpr int k_fuse_unlimited = 100000;
constexpr int k_default_max_dispatches = 10000;
constexpr int k_default_max_model_rounds = 1000;
/// Consecutive `view_crop` calls that count as too many. The prompt already
/// says one look per element; the third in a row is the alarm.
constexpr int k_view_crop_streak_alarm = 3;

inline bool fuse_is_unlimited(int _limit) { return _limit >= k_fuse_unlimited; }

inline std::string format_fuse(int _limit) {
    if (fuse_is_unlimited(_limit)) {
        return "\xE2\x88\x9E"; // U+221E, the status font requests this one
                               // glyph
    }
    return std::to_string(_limit);
}

/// One model round can dispatch several tools, so a tighter round cap would
/// cut a turn the dispatch cap still considers legal.
inline bool fuse_pair_ok(int _max_dispatches, int _max_model_rounds) {
    return _max_dispatches >= 1 && _max_model_rounds >= 1 &&
           _max_model_rounds <= _max_dispatches;
}

/// A warning for the model, empty when this call is not an alarm.
/// `_view_crop_streak` is how many `view_crop` calls just landed in a row.
/// The half-limit line fires once, on the call that reaches half of a finite
/// dispatch cap. An unlimited cap has no half.
inline std::string dispatch_alarm(int _call_index, int _max_dispatches,
                                  int _view_crop_streak) {
    std::string out;
    if (_view_crop_streak >= k_view_crop_streak_alarm) {
        out = "Too many view_crop calls in a row (" +
              std::to_string(_view_crop_streak) +
              "). One look per element, then save_template or move on. "
              "Another crop of the same element does not make a tighter match.";
    }
    if (!fuse_is_unlimited(_max_dispatches)) {
        const int half = _max_dispatches / 2;
        if (half >= 1 && _call_index == half) {
            if (!out.empty()) {
                out += " ";
            }
            out += "Tool calls are at half the limit (" +
                   std::to_string(_call_index) + "/" +
                   std::to_string(_max_dispatches) +
                   "). Finish this turn; calls past the limit are refused.";
        }
    }
    return out;
}

} // namespace campcat::llm
