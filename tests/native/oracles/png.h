// A PNG reader of its own, for judging what the display code writes: it shares no
// code with backends/host or zlib, which the display code is linked against, so a
// bug there cannot cancel itself out here. It accepts only the one format the
// runtime writes (RGB8, no interlace, filter None) and fails on anything else.
#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace lh::oracle {

class Inflater {
public:
    explicit Inflater(const std::string &input) : in_(input) {}

    // RFC 1950 and 1951.
    std::string zlib() {
        if (in_.size() < 6) bad("zlib stream too short");
        const unsigned cmf = byte_at(0), flg = byte_at(1);
        if ((cmf & 15) != 8 || cmf >> 4 > 7 || (cmf * 256 + flg) % 31 != 0 || (flg & 32)) bad("bad zlib header");
        at_ = 2;
        bool last = false;
        while (!last) {
            last = bits(1) != 0;
            const unsigned type = bits(2);
            if (type == 0) stored();
            else if (type == 1) fixed();
            else if (type == 2) dynamic();
            else bad("reserved deflate block type");
        }
        bit_buffer_ = bit_count_ = 0;
        uint32_t expected = 0;
        for (int i = 0; i < 4; ++i) expected = expected << 8 | next_byte();
        if (expected != adler32()) bad("adler32 mismatch");
        return out_;
    }

private:
    [[noreturn]] static void bad(const std::string &why) { throw std::runtime_error("PNG oracle: " + why); }

    unsigned byte_at(size_t index) const { return static_cast<uint8_t>(in_[index]); }

    unsigned next_byte() {
        if (at_ >= in_.size()) bad("deflate stream truncated");
        return byte_at(at_++);
    }

    unsigned bits(unsigned count) {
        unsigned value = 0;
        for (unsigned got = 0; got < count; ++got) {
            if (!bit_count_) {
                bit_buffer_ = next_byte();
                bit_count_ = 8;
            }
            value |= (bit_buffer_ & 1u) << got;
            bit_buffer_ >>= 1;
            --bit_count_;
        }
        return value;
    }

    uint32_t adler32() const {
        uint32_t a = 1, b = 0;
        for (char c : out_) {
            a = (a + static_cast<uint8_t>(c)) % 65521;
            b = (b + a) % 65521;
        }
        return b << 16 | a;
    }

    struct Huffman {
        std::array<uint16_t, 16> count{};
        std::vector<uint16_t> symbol;
    };

    static Huffman build(const std::vector<uint8_t> &lengths) {
        Huffman table;
        table.symbol.resize(lengths.size());
        for (uint8_t length : lengths) ++table.count[length];
        table.count[0] = 0;
        std::array<uint16_t, 16> offsets{};
        for (size_t i = 1; i < 15; ++i) offsets[i + 1] = static_cast<uint16_t>(offsets[i] + table.count[i]);
        for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            if (lengths[symbol]) table.symbol[offsets[lengths[symbol]]++] = static_cast<uint16_t>(symbol);
        }
        return table;
    }

    int decode(const Huffman &table) {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= 15; ++length) {
            code |= static_cast<int>(bits(1));
            const int count = table.count[static_cast<size_t>(length)];
            if (code - count < first) return table.symbol[static_cast<size_t>(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        bad("invalid huffman code");
    }

    void stored() {
        bit_buffer_ = bit_count_ = 0;
        const unsigned len = next_byte() | next_byte() << 8;
        const unsigned nlen = next_byte() | next_byte() << 8;
        if (len != (~nlen & 0xffffu)) bad("stored block length check failed");
        for (unsigned i = 0; i < len; ++i) out_.push_back(static_cast<char>(next_byte()));
    }

    void codes(const Huffman &literal, const Huffman &distance) {
        static const uint16_t kLengthBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
        static const uint8_t kLengthExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
        static const uint16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
        static const uint8_t kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
        for (;;) {
            int symbol = decode(literal);
            if (symbol < 256) {
                out_.push_back(static_cast<char>(symbol));
                continue;
            }
            if (symbol == 256) return;
            symbol -= 257;
            if (symbol >= 29) bad("invalid length symbol");
            const size_t length = kLengthBase[symbol] + bits(kLengthExtra[symbol]);
            const int dist_symbol = decode(distance);
            if (dist_symbol >= 30) bad("invalid distance symbol");
            const size_t dist = kDistBase[dist_symbol] + bits(kDistExtra[dist_symbol]);
            if (dist > out_.size()) bad("distance before start of output");
            for (size_t i = 0; i < length; ++i) out_.push_back(out_[out_.size() - dist]);
        }
    }

    void fixed() {
        std::vector<uint8_t> lengths(288);
        for (size_t i = 0; i < 288; ++i) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        const Huffman literal = build(lengths);
        const Huffman distance = build(std::vector<uint8_t>(30, 5));
        codes(literal, distance);
    }

    void dynamic() {
        static const uint8_t kOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        const size_t nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        std::vector<uint8_t> lengths(19);
        for (size_t i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<uint8_t>(bits(3));
        const Huffman code_lengths = build(lengths);
        std::vector<uint8_t> all(nlen + ndist);
        for (size_t index = 0; index < nlen + ndist;) {
            int symbol = decode(code_lengths);
            if (symbol < 16) {
                all[index++] = static_cast<uint8_t>(symbol);
                continue;
            }
            uint8_t previous = 0;
            size_t repeat = 0;
            if (symbol == 16) {
                if (!index) bad("repeat with no previous code length");
                previous = all[index - 1];
                repeat = 3 + bits(2);
            } else {
                repeat = symbol == 17 ? 3 + bits(3) : 11 + bits(7);
            }
            if (index + repeat > nlen + ndist) bad("too many code lengths");
            while (repeat--) all[index++] = previous;
        }
        codes(build({all.begin(), all.begin() + static_cast<std::ptrdiff_t>(nlen)}),
              build({all.begin() + static_cast<std::ptrdiff_t>(nlen), all.end()}));
    }

    const std::string &in_;
    std::string out_;
    size_t at_ = 0;
    unsigned bit_buffer_ = 0, bit_count_ = 0;
};

inline uint32_t png_crc32(const std::string &bytes) {
    uint32_t crc = 0xffffffffu;
    for (char c : bytes) {
        crc ^= static_cast<uint8_t>(c);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

struct PngImage {
    uint32_t width = 0, height = 0;
    std::string rgb;
};

inline PngImage read_png(const std::string &data) {
    const auto be32 = [&](size_t at) {
        if (at + 4 > data.size()) throw std::runtime_error("PNG oracle: truncated chunk");
        return uint32_t(static_cast<uint8_t>(data[at])) << 24 | uint32_t(static_cast<uint8_t>(data[at + 1])) << 16 |
               uint32_t(static_cast<uint8_t>(data[at + 2])) << 8 | uint32_t(static_cast<uint8_t>(data[at + 3]));
    };
    if (data.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) throw std::runtime_error("PNG oracle: bad signature");
    size_t at = 8;
    std::string compressed;
    PngImage image;
    bool have_header = false, ended = false;
    while (at < data.size()) {
        const uint32_t n = be32(at);
        if (at + 8 + size_t(n) + 4 > data.size()) throw std::runtime_error("PNG oracle: chunk past end of file");
        const std::string kind = data.substr(at + 4, 4), body = data.substr(at + 8, n);
        if (png_crc32(kind + body) != be32(at + 8 + n)) throw std::runtime_error("PNG oracle: bad CRC in " + kind);
        if (kind == "IHDR") {
            if (n != 13 || body[8] != 8 || body[9] != 2 || body[10] != 0 || body[11] != 0 || body[12] != 0) {
                throw std::runtime_error("PNG oracle: unexpected image header");
            }
            image.width = be32(at + 8);
            image.height = be32(at + 12);
            have_header = true;
        }
        if (kind == "IDAT") compressed += body;
        at += size_t(n) + 12;
        if (kind == "IEND") {
            if (at != data.size()) throw std::runtime_error("PNG oracle: data after IEND");
            ended = true;
            break;
        }
    }
    if (!have_header || !ended) throw std::runtime_error("PNG oracle: missing IHDR or IEND");
    const std::string raw = Inflater(compressed).zlib();
    const size_t stride = size_t(image.width) * 3 + 1;
    if (raw.size() != stride * image.height) throw std::runtime_error("PNG oracle: wrong decompressed size");
    image.rgb.reserve(raw.size());
    for (size_t row = 0; row < image.height; ++row) {
        if (raw[row * stride] != 0) throw std::runtime_error("PNG oracle: unexpected filter byte");
        image.rgb.append(raw, row * stride + 1, stride - 1);
    }
    return image;
}

} // namespace lh::oracle
