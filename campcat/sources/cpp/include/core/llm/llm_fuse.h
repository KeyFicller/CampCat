#pragma once

#include <string>

namespace campcat::llm {

/// A cap at or above this is the "never trips" stand-in, drawn as infinity.
constexpr int k_fuse_unlimited = 100000;
constexpr int k_default_max_dispatches = 10000;
constexpr int k_default_max_model_rounds = 1000;

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

} // namespace campcat::llm
