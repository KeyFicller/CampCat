#pragma once

#include <string>

namespace campcat::llm {

/// A cap at or above this is the "never trips" stand-in, drawn as infinity.
constexpr int k_fuse_unlimited = 100000;
constexpr int k_default_max_tool_calls = 1000000;
constexpr int k_default_max_tool_rounds = 100000;

inline bool fuse_is_unlimited(int _limit) {
    return _limit >= k_fuse_unlimited;
}

inline std::string format_fuse(int _limit) {
    if (fuse_is_unlimited(_limit)) {
        return "\xE2\x88\x9E"; // U+221E, the status font requests this one glyph
    }
    return std::to_string(_limit);
}

/// A round can issue several calls, so a tighter round cap would cut a turn
/// the call cap still considers legal.
inline bool fuse_pair_ok(int _max_calls, int _max_rounds) {
    return _max_calls >= 1 && _max_rounds >= 1 && _max_rounds <= _max_calls;
}

} // namespace campcat::llm
