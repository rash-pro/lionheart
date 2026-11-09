// Python's floor semantics for the integer oracles. C++ division and shifts
// truncate toward zero; the reference equations were written against floor.
#pragma once

#include <cstdint>

namespace lh::oracle {

inline int64_t floor_div(int64_t value, int64_t divisor) {
    int64_t q = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) --q;
    return q;
}

inline int64_t floor_shift(int64_t value, unsigned bits) { return floor_div(value, int64_t(1) << bits); }

inline int64_t clip(int64_t value, int64_t low = -32768, int64_t high = 32767) {
    return value < low ? low : value > high ? high : value;
}

} // namespace lh::oracle
