// Independent numeric image equations used to judge presentation output.
#pragma once
#include "png.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
namespace lh::oracle {
struct Rect { int x, y, w, h; };
inline uint8_t channel(const PngImage &image, int x, int y, int c) {
    return static_cast<uint8_t>(image.rgb.at((size_t(y) * image.width + x) * 3 + c));
}
inline PngImage crop(const PngImage &image, Rect rect) {
    if (rect.x < 0 || rect.y < 0 || rect.x + rect.w > int(image.width) || rect.y + rect.h > int(image.height))
        throw std::runtime_error("image crop outside source");
    PngImage out{uint32_t(rect.w), uint32_t(rect.h), {}};
    for (int y = rect.y; y < rect.y + rect.h; ++y)
        out.rgb.append(image.rgb, (size_t(y) * image.width + rect.x) * 3, size_t(rect.w) * 3);
    return out;
}
inline int max_difference(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) throw std::runtime_error("image dimensions differ");
    int maximum = 0;
    for (size_t i = 0; i < a.size(); ++i)
        maximum = std::max(maximum, std::abs(int(uint8_t(a[i])) - int(uint8_t(b[i]))));
    return maximum;
}
inline double linear(uint8_t value) {
    const double v = value / 255.0;
    return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
}
inline double mean_linear(const PngImage &image, Rect rect) {
    double sum = 0;
    for (int y = rect.y; y < rect.y + rect.h; ++y)
        for (int x = rect.x; x < rect.x + rect.w; ++x)
            for (int c = 0; c < 3; ++c) sum += linear(channel(image, x, y, c));
    return sum / (rect.w * rect.h * 3);
}
inline std::vector<double> row_profile(const PngImage &image, Rect rect) {
    std::vector<double> result;
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        double sum = 0;
        for (int x = rect.x; x < rect.x + rect.w; ++x)
            for (int c = 0; c < 3; ++c) sum += channel(image, x, y, c);
        result.push_back(sum / (rect.w * 3));
    }
    return result;
}
inline int dominant_cycles(const std::vector<double> &profile) {
    const double mean = std::accumulate(profile.begin(), profile.end(), 0.0) / profile.size();
    double maximum = 0;
    int best = 0;
    const double tau = 2 * std::acos(-1.0);
    // A direct DFT avoids sharing an FFT implementation with the renderer.
    for (size_t k = 1; k <= profile.size() / 2; ++k) {
        double real = 0, imag = 0;
        for (size_t n = 0; n < profile.size(); ++n) {
            const double phase = tau * k * n / profile.size(), value = profile[n] - mean;
            real += value * std::cos(phase); imag -= value * std::sin(phase);
        }
        const double power = real * real + imag * imag;
        if (power > maximum) { maximum = power; best = int(k); }
    }
    return best;
}
inline bool black_outside(const PngImage &image, Rect rect) {
    for (int y = 0; y < int(image.height); ++y)
        for (int x = 0; x < int(image.width); ++x)
            if (x < rect.x || x >= rect.x + rect.w || y < rect.y || y >= rect.y + rect.h)
                for (int c = 0; c < 3; ++c) if (channel(image, x, y, c)) return false;
    return true;
}
// Stored DEFLATE blocks make synthetic PNGs without linking the writer's zlib.
inline std::string write_png(const PngImage &image) {
    auto be32 = [](std::string &out, uint32_t value) { for (int i = 3; i >= 0; --i) out += char(value >> (8*i)); };
    std::string raw;
    for (size_t y = 0; y < image.height; ++y) { raw += '\0'; raw.append(image.rgb, y * image.width * 3, image.width * 3); }
    std::string compressed = "\x78\x01";
    for (size_t at = 0; at < raw.size();) {
        const unsigned count = unsigned(std::min(size_t(65535), raw.size() - at));
        compressed += char(at + count == raw.size());
        for (auto value : {count, count ^ 65535u}) { compressed += char(value); compressed += char(value >> 8); }
        compressed.append(raw, at, count); at += count;
    }
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a+c)%65521; b = (b+a)%65521; }
    be32(compressed, b << 16 | a);
    std::string out = "\x89PNG\r\n\x1a\n", header;
    be32(header, image.width); be32(header, image.height); header.append("\x08\x02\0\0\0", 5);
    auto chunk = [&](const std::string &kind, const std::string &body) {
        be32(out, uint32_t(body.size())); out += kind + body; be32(out, png_crc32(kind+body));
    };
    chunk("IHDR", header); chunk("IDAT", compressed); chunk("IEND", "");
    return out;
}
}
