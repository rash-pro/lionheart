// Host-side decoding and stream helpers; no renderer implementation is shared.
#pragma once
#include "lh_test.h"
#include <array>
#include <regex>
namespace lh {
inline int64_t json_integer(const std::string &text, const std::string &key) {
    return std::stoll(capture(text, "\\\"" + key + "\\\"\\s*:\\s*(-?[0-9]+)"));
}
inline bool json_boolean(const std::string &text, const std::string &key) {
    return capture(text, "\\\"" + key + "\\\"\\s*:\\s*(true|false)") == "true";
}
inline std::vector<std::map<std::string, std::string>> csv_rows(const fs::path &path) {
    std::istringstream input(read_file(path));
    std::string line, cell;
    std::getline(input, line);
    std::vector<std::string> header;
    std::istringstream names(line);
    while (std::getline(names, cell, ',')) header.push_back(cell);
    std::vector<std::map<std::string, std::string>> result;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::map<std::string, std::string> row;
        for (const auto &name : header) { std::getline(fields, cell, ','); row[name] = cell; }
        result.push_back(row);
    }
    return result;
}
inline void little(std::string &out, uint64_t value, int count = 4) {
    for (int i = 0; i < count; ++i) out += char(value >> (8 * i));
}
struct GpuStream {
    std::string data;
    explicit GpuStream(int version = 1) : data(version == 1 ? "FF8GPUR1" : "FF8GPUR2") {}
    void row(uint32_t kind, uint32_t value) { little(data, kind); little(data, value); }
    void gp0(std::initializer_list<uint32_t> words) { for (auto value : words) row(0, value); }
    void display() {
        for (auto value : {0x05000000u, 0x06000000u | 600u | (3160u << 12),
                           0x07000000u | 16u | (256u << 10), 0x08000001u, 0x03000000u}) row(1, value);
    }
    void save(const fs::path &path) const { write_file(path, data); }
};
inline uint32_t xy(unsigned x, unsigned y) { return (x & 65535u) | ((y & 65535u) << 16); }
}
