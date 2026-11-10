#pragma once
// Writes tar archives in one xz stream, as the official Linux compiler
// archive is packed, without an encoder: LZMA2 can store chunks as they are,
// and the container around them is plain bytes and checksums. Each part is
// built here so a case can break exactly one of them.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace tar_xz {
inline void put_le(std::string &s, uint64_t v, int n) { for (int k = 0; k < n; ++k) s += char(v >> (8 * k)); }

inline uint32_t crc32(const std::string &bytes) {
    uint32_t crc = ~0u;
    for (unsigned char c : bytes) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

inline uint64_t crc64(const std::string &bytes) {
    uint64_t crc = ~uint64_t(0);
    for (unsigned char c : bytes) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (UINT64_C(0xC96C5795D7870F42) & (uint64_t(0) - (crc & 1)));
    }
    return ~crc;
}

inline std::string varint(uint64_t v) {
    std::string s;
    while (v >= 0x80) { s += char(0x80 | (v & 0x7f)); v >>= 7; }
    return s + char(v);
}

// One xz stream with one block of stored LZMA2 chunks and a CRC64 check.
inline std::string xz(const std::string &data) {
    // The magic ends in a zero byte, which the literal's terminator supplies.
    std::string out = std::string("\xFD" "7zXZ", 6);
    const std::string flags("\x00\x04", 2);
    out += flags;
    put_le(out, crc32(flags), 4);
    // Size field, flags, LZMA2 with one property byte (a 4 KiB dictionary), padding.
    std::string header("\x02\x00\x21\x01\x00\x00\x00\x00", 8);
    put_le(header, crc32(header), 4);
    std::string body;
    for (size_t at = 0; at < data.size(); at += 65536) {
        const size_t n = std::min<size_t>(65536, data.size() - at);
        body += char(at ? 2 : 1);
        body += char((n - 1) >> 8);
        body += char((n - 1) & 0xff);
        body += data.substr(at, n);
    }
    body += '\0';
    const uint64_t unpadded = header.size() + body.size() + 8;
    out += header + body;
    while ((header.size() + body.size()) % 4) { out += '\0'; body += '\0'; }
    put_le(out, crc64(data), 8);
    std::string index = std::string(1, '\0') + varint(1) + varint(unpadded) + varint(data.size());
    while (index.size() % 4) index += '\0';
    put_le(index, crc32(index), 4);
    out += index;
    std::string footer;
    put_le(footer, index.size() / 4 - 1, 4);
    footer += flags;
    std::string tail;
    put_le(tail, crc32(footer), 4);
    return out + tail + footer + "YZ";
}

struct Entry {
    std::string name;
    char type = '0';
    std::string body;
    // POSIX ustar with the name split at its last slash into prefix and name.
    bool ustar_prefix = false;
};

inline std::string octal(uint64_t v, size_t width) {
    std::string digits;
    do { digits.insert(digits.begin(), char('0' + (v & 7))); v >>= 3; } while (v);
    return std::string(width - 1 - digits.size(), '0') + digits + '\0';
}

inline std::string header(const std::string &name, char type, uint64_t size, const std::string &prefix = "", bool posix = false) {
    std::string h(512, '\0');
    h.replace(0, std::min<size_t>(name.size(), 100), name.substr(0, 100));
    h.replace(100, 8, octal(type == '5' ? 0755 : 0644, 8));
    h.replace(108, 8, octal(0, 8));
    h.replace(116, 8, octal(0, 8));
    h.replace(124, 12, octal(size, 12));
    h.replace(136, 12, octal(0, 12));
    h[156] = type;
    if (posix) { h.replace(257, 8, std::string("ustar\0" "00", 8)); h.replace(345, prefix.size(), prefix); }
    else h.replace(257, 8, std::string("ustar  \0", 8));
    h.replace(148, 8, "        ");
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    std::string field = octal(sum, 7) + ' ';
    h.replace(148, 8, field);
    return h;
}

inline std::string padded(const std::string &body) { return body + std::string((512 - body.size() % 512) % 512, '\0'); }

inline std::string member(const Entry &e) {
    if (e.ustar_prefix) {
        const auto slash = e.name.rfind('/');
        return header(e.name.substr(slash + 1), e.type, e.body.size(), e.name.substr(0, slash), true) + padded(e.body);
    }
    std::string out;
    // GNU tar puts a longer name in an entry of its own before the header.
    if (e.name.size() > 100) out += header("././@LongLink", 'L', e.name.size() + 1) + padded(e.name + '\0');
    return out + header(e.name, e.type, e.body.size()) + padded(e.body);
}

inline std::string pax_record(const std::string &key, const std::string &value) {
    const std::string tail = " " + key + "=" + value + "\n";
    size_t length = tail.size() + 1;
    while (std::to_string(length).size() + tail.size() != length) ++length;
    return std::to_string(length) + tail;
}

// Ends as tar does: two zero blocks, then zeros to a whole 10 KiB record.
inline std::string tar(const std::vector<std::string> &members) {
    std::string out;
    for (const auto &m : members) out += m;
    out += std::string(1024, '\0');
    return out + std::string((10240 - out.size() % 10240) % 10240, '\0');
}
}
