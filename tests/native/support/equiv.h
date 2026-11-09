// While Python is still installed, tests/native/check_against_python.py re-runs
// the real pytest bodies on what the native cases recorded here, with the
// runtime replaced by a replay of these records. It stops mattering when Python
// is deleted; delete this header and the lh::equiv::record calls with it.
#pragma once

#include "lh_test.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

namespace lh::equiv {

// The stream for this case's records, or null when the check is not running.
inline FILE *stream(const Case &t) {
    static FILE *file = [&]() -> FILE * {
        const char *dir = std::getenv("LH_EQUIV_DIR");
        if (!dir || !*dir) return nullptr;
        std::string name = t.name();
        for (char &c : name) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return std::fopen((fs::path(dir) / (name + ".bin")).c_str(), "wb");
    }();
    return file;
}

template <class T> void put(FILE *file, const T &value) {
    if constexpr (std::is_arithmetic_v<T>) {
        std::fwrite(&value, sizeof(T), 1, file);
    } else if constexpr (std::is_same_v<T, std::string>) {
        std::fwrite(value.data(), 1, value.size(), file);
    } else {
        std::fwrite(value.data(), sizeof(*value.data()), value.size(), file);
    }
}

// Appends each value's bytes, in order, to the case's record file. The check
// knows the widths, so the records need no framing.
template <class... Values> void record(const Case &t, const Values &... values) {
    if (FILE *file = stream(t)) (put(file, values), ...);
}

} // namespace lh::equiv
