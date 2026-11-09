// Small helpers for failure text when comparing integer vectors and tables.
#pragma once

#include "lh_test.h"

#include <cstddef>
#include <sstream>
#include <string>

namespace lh {

// Every argument, streamed, without separators: lh::str("step=", step).
template <class... Parts> std::string str(const Parts &... parts) {
    std::ostringstream out;
    (out << ... << parts);
    return out.str();
}

namespace detail {
template <class T> long long number(const T &value) { return static_cast<long long>(value); }
template <class A, class B> std::string range_diff(const A &actual, const B &expected, const std::string &context) {
    std::ostringstream out;
    out << "lengths " << actual.size() << " and " << expected.size();
    const size_t n = actual.size() < expected.size() ? actual.size() : expected.size();
    for (size_t i = 0; i < n; ++i) {
        if (number(actual[i]) != number(expected[i])) {
            out << "; first difference at index " << i << ": actual " << number(actual[i])
                << ", expected " << number(expected[i]);
            break;
        }
    }
    if (!context.empty()) out << "\n  " << context;
    return out.str();
}
} // namespace detail
} // namespace lh

// Two contiguous integer sequences, with their first difference and a context on failure.
#define LH_SEQ_EQ(actual, expected, context)                                                    \
    do {                                                                                        \
        const auto &lh_a = (actual);                                                            \
        const auto &lh_b = (expected);                                                          \
        bool lh_same = lh_a.size() == lh_b.size();                                              \
        for (size_t lh_i = 0; lh_same && lh_i < lh_a.size(); ++lh_i) lh_same = lh_a[lh_i] == lh_b[lh_i]; \
        if (!lh_same) lh::fail_at(__FILE__, __LINE__, #actual " differs from " #expected ": " + \
                                  lh::detail::range_diff(lh_a, lh_b, (context)));               \
    } while (0)
