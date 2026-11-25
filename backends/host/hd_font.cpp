#ifdef _WIN32
#undef fopen
#undef open
#undef mkdir
#undef unlink
#undef remove
#undef rename
#undef getenv
#endif

#include "hd_font.hpp"
#include "hd_font_assets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace HostHD {
namespace {

/* FF8 US/European sysfnt glyph table starting at cell 0 (code 0x20 = space).
 * 21 glyphs per row, each cell 12x12 pixels inside a 256px-wide 4bpp texture page.
 * Entries set to 0 (special symbols/icons/corner quotes) intentionally fall through
 * to native SD VRAM via parallel-psx's (0, 0, 0, 255) sentinel so no glyph vanishes. */
constexpr uint32_t kFf8Glyphs[] = {
    /* 0x20..0x2F */
    ' ', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '%', '/', ':', '!', '?',
    /* 0x30..0x3F */
    0x2026 /* … */, '+', '-', '=', '*', '&', 0 /* 「 */, 0 /* 」 */,
    '(', ')', 0x00B7 /* · */, '.', ',', '~', 0x201C /* “ */, 0x201D /* ” */,
    /* 0x40..0x4F */
    0x2018 /* ‘ */, '#', '$', 0x2019 /* ’ */, '_',
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
    /* 0x50..0x5F */
    'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a',
    /* 0x60..0x6F */
    'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q',
    /* 0x70..0x7F */
    'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    0x00C0 /* À */, 0x00C1 /* Á */, 0x00C2 /* Â */, 0x00C4 /* Ä */,
    0x00C7 /* Ç */, 0x00C8 /* È */, 0x00C9 /* É */,
    /* 0x80..0x8F */
    0x00CA /* Ê */, 0x00CB /* Ë */, 0x00CC /* Ì */, 0x00CD /* Í */,
    0x00CE /* Î */, 0x00CF /* Ï */, 0x00D1 /* Ñ */, 0x00D2 /* Ò */,
    0x00D3 /* Ó */, 0x00D4 /* Ô */, 0x00D6 /* Ö */, 0x00D9 /* Ù */,
    0x00DA /* Ú */, 0x00DB /* Û */, 0x00DC /* Ü */, 0x0152 /* Œ */,
    /* 0x90..0x9F */
    0x00DF /* ß */, 0x00E0 /* à */, 0x00E1 /* á */, 0x00E2 /* â */,
    0x00E4 /* ä */, 0x00E7 /* ç */, 0x00E8 /* è */, 0x00E9 /* é */,
    0x00EA /* ê */, 0x00EB /* ë */, 0x00EC /* ì */, 0x00ED /* í */,
    0x00EE /* î */, 0x00EF /* ï */, 0x00F1 /* ñ */, 0x00F2 /* ò */,
    /* 0xA0..0xAF */
    0x00F3 /* ó */, 0x00F4 /* ô */, 0x00F6 /* ö */, 0x00F9 /* ù */,
    0x00FA /* ú */, 0x00FB /* û */, 0x00FC /* ü */, 0x0153 /* œ */,
    0 /* Ⅷ */, '[', ']', 0 /* ■ */, 0 /* ◎ */, 0 /* ♦ */, '[' /* 〖 */, ']' /* 〗 */,
    /* 0xB0..0xBF */
    0 /* □ */, 0 /* ★ */, 0 /* 『 */, 0 /* 』 */, 0 /* ▽ */, ';', 0 /* ▼ */, 0x00AF /* ¯ */,
    0x00D7 /* × */, 0 /* ☆ */, 0x2032 /* ′ */, 0 /* ↓ */,
    0x00B0 /* ° */, 0x00A1 /* ¡ */, 0x00BF /* ¿ */, 0x2014 /* — */,
    /* 0xC0..0xCB */
    0x00AB /* « */, 0x00BB /* » */, 0x00B1 /* ± */, 0 /* ♪ */, 0, 0x2020 /* † */, 0 /* VI */, 0 /* II */, 0, 0x2122 /* ™ */, '<', '>',
};

/* Geometric vector symbols for FF8 sysfnt cells absent from Latin TTF cmaps
 * (corner brackets 「」『』 and clean UI shapes ■□♦▼▽↓). */
bool is_geometric_sysfnt(size_t cell_idx) {
    switch (cell_idx + 0x20u) {
    case 0x36u: /* 「 */
    case 0x37u: /* 」 */
    case 0xABu: /* ■ */
    case 0xADu: /* ♦ */
    case 0xB0u: /* □ */
    case 0xB2u: /* 『 */
    case 0xB3u: /* 』 */
    case 0xB4u: /* ▽ */
    case 0xB6u: /* ▼ */
    case 0xBBu: /* ↓ */
        return true;
    default:
        return false;
    }
}

/* Multi-character Roman numeral ligatures packed into single 12x12 sysfnt cells. */
const char *sysfnt_ligature(size_t cell_idx) {
    switch (cell_idx) {
    case 0xA8u - 0x20u: return "VIII";
    case 0xC6u - 0x20u: return "VI";
    case 0xC7u - 0x20u: return "II";
    default: return nullptr;
    }
}

constexpr unsigned kGlyphCols = 21;
constexpr unsigned kCellNative = 12;
/* Texture sheets never live inside FF8's 320px-wide double framebuffer columns. */
constexpr unsigned kMinSheetX = 320;

inline uint8_t expand5(unsigned c) { return uint8_t((c << 3) | (c >> 2)); }

inline void unpack_abgr1555(uint16_t raw, uint8_t rgb[3]) {
    rgb[0] = expand5(raw & 31u);
    rgb[1] = expand5((raw >> 5) & 31u);
    rgb[2] = expand5((raw >> 10) & 31u);
}

inline unsigned luma(const uint8_t rgb[3]) {
    return unsigned(rgb[0]) * 30u + unsigned(rgb[1]) * 59u + unsigned(rgb[2]) * 11u;
}

inline void clamp_non_sentinel(uint8_t rgba[4]) {
    /* parallel-psx hdtextures.h treats exact (0, 0, 0, 255) as a fallthrough
     * sentinel to SD VRAM. Bump pure opaque black to (1, 1, 1, 255). */
    if (rgba[3] == 255 && rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0) {
        rgba[0] = rgba[1] = rgba[2] = 1;
    }
}

/* FF8 icon.tex (256x256, 4bpp) text cells. Every rect is one of the game's own
 * sprite quads (factual coordinates), so an HD cell always replaces a whole quad:
 * no original pixel of a label can survive beside its replacement, and no quad
 * reaches into a neighbouring label. Graphics (cursor, window gradient, status and
 * element icons, controller buttons, the stylised Trigger!/Perfect!! art) are not
 * listed and stay native. */
enum IconStyle : uint8_t {
    kBadge = 0,    /* 4px-cap window header badges, outlined */
    kShadow = 1,   /* plain glyphs with a 1px drop shadow (menu digits, labels) */
    kOutlined = 2, /* glyphs with a full 1px outline (tall and large digits) */
};

struct IconTextRect {
    uint8_t x, y, w, h;
    const char *text;
    uint8_t style;
    bool digits;        /* scale to figure height instead of cap/ascender height */
    uint8_t ref_x, ref_y, ref_w, ref_h; /* cell whose ink rows set the vertical metrics (0 size = self) */
    uint8_t slice_x, full_w; /* non-zero full_w: render across full_w and copy [slice_x, slice_x + w) */
};

#define ICON_BADGE(X, Y, W, T) {X, Y, W, 8, T, kBadge, false, 0, 0, 0, 0, 0, 0}
#define ICON_DIGIT(X, Y, W, H, T, STYLE, RX, RY) {X, Y, W, H, T, STYLE, true, RX, RY, W, H, 0, 0}
#define ICON_LABEL(X, Y, W, H, T, STYLE) {X, Y, W, H, T, STYLE, false, 0, 0, 0, 0, 0, 0}
#define ICON_LABEL_REF(X, Y, W, H, T, STYLE, RX, RY) {X, Y, W, H, T, STYLE, false, RX, RY, W, H, 0, 0}
#define ICON_SLICE(X, Y, W, H, T, STYLE, RX, RY, SX, FW) {X, Y, W, H, T, STYLE, false, RX, RY, W, H, SX, FW}

constexpr IconTextRect kIconTexts[] = {
    /* Window header badges, y=0..47 */
    ICON_BADGE(40, 0, 24, "NAME"),    ICON_BADGE(64, 0, 16, "HP"),       ICON_BADGE(80, 0, 24, "TIME"),
    ICON_BADGE(104, 0, 32, "CHOICE2"), ICON_BADGE(136, 0, 48, "COMMAND"), ICON_BADGE(184, 0, 24, "NUM."),
    ICON_BADGE(40, 8, 40, "PAUSE"),   ICON_BADGE(80, 8, 32, "ITEM"),     ICON_BADGE(112, 8, 32, "MAGIC"),
    ICON_BADGE(144, 8, 32, "STATUS"), ICON_BADGE(176, 8, 32, "HELP"),
    ICON_BADGE(40, 16, 32, "CHOICE"), ICON_BADGE(72, 16, 16, "GF"),      ICON_BADGE(88, 16, 40, "TARGET"),
    ICON_BADGE(128, 16, 40, "ATTACK"),
    /* Small 5px digits, y=16 */
    ICON_DIGIT(168, 16, 8, 8, "0", kShadow, 168, 16), ICON_DIGIT(176, 16, 8, 8, "1", kShadow, 168, 16),
    ICON_DIGIT(184, 16, 8, 8, "2", kShadow, 168, 16), ICON_DIGIT(192, 16, 8, 8, "3", kShadow, 168, 16),
    ICON_DIGIT(200, 16, 8, 8, "4", kShadow, 168, 16), ICON_DIGIT(208, 16, 8, 8, "5", kShadow, 168, 16),
    ICON_DIGIT(216, 16, 8, 8, "6", kShadow, 168, 16), ICON_DIGIT(224, 16, 8, 8, "7", kShadow, 168, 16),
    ICON_DIGIT(232, 16, 8, 8, "8", kShadow, 168, 16), ICON_DIGIT(240, 16, 8, 8, "9", kShadow, 168, 16),
    /* Unit letters (EXP points, Gil) and badges, y=24 */
    ICON_LABEL(24, 24, 8, 8, "P", kShadow),           ICON_LABEL(32, 24, 8, 8, "G", kShadow),
    ICON_BADGE(40, 24, 32, "NOTICE"), ICON_BADGE(80, 24, 16, "Lv."),     ICON_BADGE(96, 24, 32, "TRIPLE"),
    ICON_BADGE(128, 24, 40, "DOUBLE"),
    /* Menu digits (HP, EXP, Gil, play time), y=24 */
    ICON_DIGIT(168, 24, 8, 8, "0", kShadow, 168, 24), ICON_DIGIT(176, 24, 8, 8, "1", kShadow, 168, 24),
    ICON_DIGIT(184, 24, 8, 8, "2", kShadow, 168, 24), ICON_DIGIT(192, 24, 8, 8, "3", kShadow, 168, 24),
    ICON_DIGIT(200, 24, 8, 8, "4", kShadow, 168, 24), ICON_DIGIT(208, 24, 8, 8, "5", kShadow, 168, 24),
    ICON_DIGIT(216, 24, 8, 8, "6", kShadow, 168, 24), ICON_DIGIT(224, 24, 8, 8, "7", kShadow, 168, 24),
    ICON_DIGIT(232, 24, 8, 8, "8", kShadow, 168, 24), ICON_DIGIT(240, 24, 8, 8, "9", kShadow, 168, 24),
    ICON_BADGE(40, 32, 32, "ABILITY"), ICON_BADGE(72, 32, 32, "INFO."),  ICON_BADGE(104, 32, 24, "WAIT"),
    ICON_LABEL(128, 32, 40, 16, "Miss", kOutlined),
    /* Tall outlined digits, y=32 */
    ICON_DIGIT(168, 32, 8, 16, "0", kOutlined, 168, 32), ICON_DIGIT(176, 32, 8, 16, "1", kOutlined, 168, 32),
    ICON_DIGIT(184, 32, 8, 16, "2", kOutlined, 168, 32), ICON_DIGIT(192, 32, 8, 16, "3", kOutlined, 168, 32),
    ICON_DIGIT(200, 32, 8, 16, "4", kOutlined, 168, 32), ICON_DIGIT(208, 32, 8, 16, "5", kOutlined, 168, 32),
    ICON_DIGIT(216, 32, 8, 16, "6", kOutlined, 168, 32), ICON_DIGIT(224, 32, 8, 16, "7", kOutlined, 168, 32),
    ICON_DIGIT(232, 32, 8, 16, "8", kOutlined, 168, 32), ICON_DIGIT(240, 32, 8, 16, "9", kOutlined, 168, 32),
    ICON_BADGE(40, 40, 40, "DEFENSE"), ICON_BADGE(80, 40, 48, "ELEMENTAL"),
    /* Outlined '/' and digits, y=64 */
    ICON_DIGIT(168, 64, 8, 16, "/", kOutlined, 176, 64),
    ICON_DIGIT(176, 64, 8, 16, "0", kOutlined, 176, 64), ICON_DIGIT(184, 64, 8, 16, "1", kOutlined, 176, 64),
    ICON_DIGIT(192, 64, 8, 16, "2", kOutlined, 176, 64), ICON_DIGIT(200, 64, 8, 16, "3", kOutlined, 176, 64),
    ICON_DIGIT(208, 64, 8, 16, "4", kOutlined, 176, 64), ICON_DIGIT(216, 64, 8, 16, "5", kOutlined, 176, 64),
    ICON_DIGIT(224, 64, 8, 16, "6", kOutlined, 176, 64), ICON_DIGIT(232, 64, 8, 16, "7", kOutlined, 176, 64),
    ICON_DIGIT(240, 64, 8, 16, "8", kOutlined, 176, 64), ICON_DIGIT(248, 64, 8, 16, "9", kOutlined, 176, 64),
    /* Large 16x16 digits and ':', y=80 */
    ICON_DIGIT(0, 80, 16, 16, "0", kOutlined, 0, 80),   ICON_DIGIT(16, 80, 16, 16, "1", kOutlined, 0, 80),
    ICON_DIGIT(32, 80, 16, 16, "2", kOutlined, 0, 80),  ICON_DIGIT(48, 80, 16, 16, "3", kOutlined, 0, 80),
    ICON_DIGIT(64, 80, 16, 16, "4", kOutlined, 0, 80),  ICON_DIGIT(80, 80, 16, 16, "5", kOutlined, 0, 80),
    ICON_DIGIT(96, 80, 16, 16, "6", kOutlined, 0, 80),  ICON_DIGIT(112, 80, 16, 16, "7", kOutlined, 0, 80),
    ICON_DIGIT(128, 80, 16, 16, "8", kOutlined, 0, 80), ICON_DIGIT(144, 80, 16, 16, "9", kOutlined, 0, 80),
    {160, 80, 8, 16, ":", kOutlined, true, 0, 80, 16, 16, 0, 0},
    ICON_LABEL(208, 80, 32, 16, "S-Lv.", kOutlined),
    /* y=96..127 */
    ICON_LABEL(160, 96, 16, 16, "Lv.", kShadow),
    ICON_LABEL(224, 96, 16, 16, "A", kOutlined),      ICON_LABEL(240, 96, 16, 16, "G", kOutlined),
    ICON_BADGE(96, 120, 40, "SPECIAL"),
    /* y=144..159: Triple Triad / shop badges and their small digits */
    ICON_LABEL(0, 144, 88, 16, "Vibration", kShadow),
    ICON_BADGE(112, 144, 32, "BULLET"),
    ICON_DIGIT(144, 144, 8, 8, "1", kShadow, 112, 152), ICON_DIGIT(152, 144, 8, 8, "2", kShadow, 112, 152),
    ICON_BADGE(160, 144, 32, "CARDS"),
    ICON_DIGIT(192, 144, 8, 8, "5", kShadow, 112, 152), ICON_DIGIT(200, 144, 8, 8, "6", kShadow, 112, 152),
    ICON_BADGE(208, 144, 24, "PRICE"),
    ICON_LABEL(240, 144, 16, 8, "Lv.", kShadow),
    ICON_DIGIT(112, 152, 8, 8, "0", kShadow, 112, 152),
    ICON_BADGE(128, 152, 16, "DE"),
    ICON_DIGIT(144, 152, 8, 8, "3", kShadow, 112, 152), ICON_DIGIT(152, 152, 8, 8, "4", kShadow, 112, 152),
    ICON_BADGE(160, 152, 32, "ESTADO"),
    ICON_DIGIT(192, 152, 8, 8, "7", kShadow, 112, 152), ICON_DIGIT(200, 152, 8, 8, "8", kShadow, 112, 152),
    ICON_DIGIT(208, 152, 8, 8, "9", kShadow, 112, 152), ICON_DIGIT(216, 152, 8, 8, "/", kShadow, 112, 152),
    /* '%', '/' and ':' drawn alongside the y=24 menu digits (HP cur/max, play time) */
    ICON_DIGIT(232, 152, 8, 8, "%", kShadow, 168, 24), ICON_DIGIT(240, 152, 8, 8, "/", kShadow, 168, 24),
    ICON_DIGIT(248, 152, 8, 8, ":", kShadow, 168, 24),
    /* y=160 */
    ICON_LABEL(0, 160, 32, 16, "ON", kOutlined),      ICON_LABEL(32, 160, 32, 16, "OFF", kOutlined),
    ICON_LABEL_REF(64, 160, 64, 16, "Complete!", kOutlined, 32, 160),
    /* y=176: battle Double cast banner */
    ICON_LABEL(0, 176, 48, 16, "DOUBLE!", kOutlined),
    /* Menu status bar labels, y=192 */
    ICON_LABEL(0, 192, 48, 8, "TRIPLE!!", kOutlined),
    ICON_LABEL(48, 192, 32, 8, "Fast", kShadow),      ICON_LABEL(80, 192, 32, 8, "Slow", kShadow),
    ICON_LABEL(112, 192, 32, 8, "PLAY", kShadow),     ICON_LABEL(144, 192, 24, 8, "SeeD", kShadow),
    ICON_LABEL(176, 192, 16, 8, "HP", kShadow),       ICON_LABEL(192, 192, 16, 8, "Lv", kShadow),
    ICON_LABEL(208, 192, 32, 8, "TIME", kShadow),
    /* Save/Load "DISC" label split across two vertically stacked 16x8 halves (y=200 & y=208) */
    ICON_SLICE(48, 200, 16, 8, "DISC", kShadow, 48, 200, 0, 27),
    ICON_SLICE(48, 208, 16, 8, "DISC", kShadow, 48, 200, 16, 27),
    ICON_LABEL(40, 216, 24, 8, "A", kShadow),
    /* Character stat labels, y=240 */
    ICON_LABEL_REF(0, 240, 16, 16, "HP", kShadow, 0, 240),    ICON_LABEL_REF(16, 240, 16, 16, "Str", kShadow, 0, 240),
    ICON_LABEL_REF(32, 240, 16, 16, "Vit", kShadow, 0, 240),  ICON_LABEL_REF(48, 240, 16, 16, "Mag", kShadow, 0, 240),
    ICON_LABEL_REF(64, 240, 16, 16, "Spr", kShadow, 0, 240),  ICON_LABEL_REF(80, 240, 16, 16, "Spd", kShadow, 0, 240),
    ICON_LABEL_REF(96, 240, 16, 16, "Eva", kShadow, 0, 240),  ICON_LABEL_REF(112, 240, 16, 16, "Hit", kShadow, 0, 240),
    ICON_LABEL_REF(128, 240, 16, 16, "Luck", kShadow, 0, 240),
};
constexpr size_t kIconTextCount = sizeof(kIconTexts) / sizeof(kIconTexts[0]);

#undef ICON_BADGE
#undef ICON_DIGIT
#undef ICON_LABEL
#undef ICON_LABEL_REF
#undef ICON_SLICE

/* Bands: maximal runs of consecutive table cells that sit side by side. */
struct IconBand {
    unsigned x, y, w, h;
    size_t first, last; /* table range [first, last) */
};

const std::vector<IconBand> &icon_bands() {
    static const std::vector<IconBand> bands = [] {
        std::vector<IconBand> out;
        for (size_t i = 0; i < kIconTextCount; ++i) {
            const IconTextRect &c = kIconTexts[i];
            if (!out.empty()) {
                IconBand &b = out.back();
                if (b.y == c.y && b.h == c.h && b.x + b.w == c.x) {
                    b.w += c.w;
                    b.last = i + 1;
                    continue;
                }
            }
            out.push_back({c.x, c.y, c.w, c.h, i, i + 1});
        }
        return out;
    }();
    return bands;
}

bool ends_with_ci(const std::string &s, const char *suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = char(a + 32);
        if (b >= 'A' && b <= 'Z') b = char(b + 32);
        if (a != b) return false;
    }
    return true;
}

} // namespace

void HdFont::configure(FontMode mode, const std::string &custom_path) {
    if (mode_ == mode && custom_path_ == custom_path && generation_ > 0) return;
    mode_ = mode;
    custom_path_ = custom_path;
    ttf_storage_.clear();
    ttf_data_ = nullptr;
    ttf_size_ = 0;
    custom_png_ = {};
    has_custom_png_ = false;
    ++generation_;

    if (mode_ == FontMode::Off) return;

    if (mode_ == FontMode::Custom && !custom_path_.empty()) {
        if (ends_with_ci(custom_path_, ".png")) {
            if (load_custom_png(custom_path_)) return;
        } else {
            std::ifstream in(custom_path_, std::ios::binary | std::ios::ate);
            if (in) {
                std::streamsize sz = in.tellg();
                if (sz > 64 && sz < 32 * 1024 * 1024) {
                    in.seekg(0, std::ios::beg);
                    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
                    if (in.read(reinterpret_cast<char *>(bytes.data()), sz) &&
                        load_ttf_bytes(bytes.data(), bytes.size())) {
                        ttf_storage_ = std::move(bytes);
                        ttf_data_ = ttf_storage_.data();
                        ttf_size_ = ttf_storage_.size();
                        return;
                    }
                }
            }
        }
    }

    /* Built-in OFL Barlow Medium (also the fallback if the custom path is missing). */
    if (load_ttf_bytes(hd_font_builtin_ttf, hd_font_builtin_ttf_size)) {
        ttf_data_ = hd_font_builtin_ttf;
        ttf_size_ = hd_font_builtin_ttf_size;
    }
}

bool HdFont::load_ttf_bytes(const uint8_t *data, size_t size) {
    if (!data || size < 64) return false;
    stbtt_fontinfo info{};
    return stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)) != 0;
}

bool HdFont::load_custom_png(const std::string &path) {
    int w = 0, h = 0, comp = 0;
    unsigned char *decoded = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!decoded || w < 256 || h < 128) {
        if (decoded) stbi_image_free(decoded);
        return false;
    }
    custom_png_.width = unsigned(w);
    custom_png_.height = unsigned(h);
    custom_png_.pixels.assign(decoded, decoded + size_t(w) * size_t(h) * 4);
    stbi_image_free(decoded);
    for (size_t i = 0; i < custom_png_.pixels.size(); i += 4) {
        clamp_non_sentinel(&custom_png_.pixels[i]);
    }
    has_custom_png_ = true;
    return true;
}

bool HdFont::is_font_upload(unsigned vram_x, unsigned vram_y, unsigned vram_w, unsigned vram_h,
                            const uint16_t *vram) {
    /* FF8 sysfnt is a 4bpp 256px-wide (64 VRAM 16-bit words) sheet uploaded into
     * upper UI/system VRAM (x >= 512), 128..256 pixels tall, with 21 columns of
     * 12x12 cells (leaving columns 252..255 unused) and sparse glyph bitmaps. */
    if (!vram || vram_x < kMinSheetX || vram_w != 64 || vram_h < 120 || vram_h > 256) return false;
    if (vram_x + vram_w > 1024 || vram_y + vram_h > 512) return false;

    auto nibble_at = [&](unsigned lx, unsigned ly) -> uint8_t {
        uint16_t word = vram[(vram_y + ly) * 1024u + (vram_x + (lx >> 2))];
        return uint8_t((word >> ((lx & 3u) * 4u)) & 0xfu);
    };

    /* Unused right 4-pixel gutter (252..255) on the first 4 glyph rows (0..47) is empty. */
    unsigned gutter_nonzero = 0;
    for (unsigned ly = 0; ly < 48; ++ly) {
        for (unsigned lx = 252; lx < 256; ++lx) {
            if (nibble_at(lx, ly) != 0) ++gutter_nonzero;
        }
    }
    if (gutter_nonzero > 4) return false;

    /* Cell 0 (space) should be blank, while cells 1..10 (digits 0..9) and uppercase
     * letters in rows 1..3 should have clean glyph density (5%..75% non-zero pixels). */
    unsigned space_pixels = 0;
    for (unsigned dy = 0; dy < kCellNative; ++dy)
        for (unsigned dx = 0; dx < kCellNative; ++dx)
            if (nibble_at(dx, dy) != 0) ++space_pixels;
    if (space_pixels > 4) return false;

    unsigned valid_cells = 0;
    const unsigned check_cells = std::min<unsigned>(63u, (vram_h / kCellNative) * kGlyphCols);
    for (unsigned idx = 1; idx < check_cells; ++idx) {
        unsigned col = idx % kGlyphCols;
        unsigned row = idx / kGlyphCols;
        unsigned cx = col * kCellNative;
        unsigned cy = row * kCellNative;
        unsigned count = 0;
        for (unsigned dy = 0; dy < kCellNative; ++dy)
            for (unsigned dx = 0; dx < kCellNative; ++dx)
                if (nibble_at(cx + dx, cy + dy) != 0) ++count;
        if (count >= 6 && count <= 115) ++valid_cells;
    }
    return valid_cells >= 36;
}

bool HdFont::is_icon_font_upload(unsigned vram_x, unsigned vram_y, unsigned vram_w, unsigned vram_h,
                                 const uint16_t *vram) {
    /* FF8 icon.tex is a 4bpp 256x256 (64x256 VRAM words) menu UI sheet uploaded
     * into upper VRAM (x >= 512) containing the solid metallic window gradient at
     * y=48..63, 8x8 menu digits at x=168..247 y=24..31, and 16x16 digits at y=80..95. */
    if (!vram || vram_x < kMinSheetX || vram_w != 64 || vram_h != 256) return false;
    if (vram_x + vram_w > 1024 || vram_y + vram_h > 512) return false;

    const uint16_t *base = vram + size_t(vram_y) * 1024u + vram_x;
    /* O(1) early reject: window gradient y=48..63 must be non-zero across the page. */
    if (!base[48u * 1024u] || !base[48u * 1024u + 32u] ||
        !base[63u * 1024u] || !base[63u * 1024u + 32u])
        return false;

    auto nibble_at = [&](unsigned lx, unsigned ly) -> uint8_t {
        uint16_t word = base[size_t(ly) * 1024u + (lx >> 2)];
        return uint8_t((word >> ((lx & 3u) * 4u)) & 0xfu);
    };

    /* 1. Window background gradient strip y=48..63 is dense across all 256px. */
    unsigned bg_nonzero = 0;
    for (unsigned ly = 48; ly < 64; ly += 2) {
        for (unsigned lx = 0; lx < 256; lx += 4) {
            if (nibble_at(lx, ly) != 0) ++bg_nonzero;
        }
    }
    if (bg_nonzero < 480) return false;

    /* 2. Menu 8x8 digit strip (x=168..247, y=24..31) has 10 non-empty digit cells. */
    unsigned digits_ok = 0;
    for (unsigned d = 0; d < 10; ++d) {
        unsigned nz = 0;
        for (unsigned dy = 0; dy < 8; ++dy)
            for (unsigned dx = 0; dx < 8; ++dx)
                if (nibble_at(168u + d * 8u + dx, 24u + dy) != 0) ++nz;
        if (nz >= 12 && nz <= 60) ++digits_ok;
    }
    if (digits_ok < 9) return false;

    /* 3. Big 16x16 digit strip (x=0..159, y=80..95) has 10 digits with blank bottom row y=95. */
    unsigned big_ok = 0, bottom_nz = 0;
    for (unsigned d = 0; d < 10; ++d) {
        unsigned nz = 0;
        for (unsigned dy = 0; dy < 14; ++dy)
            for (unsigned dx = 0; dx < 16; ++dx)
                if (nibble_at(d * 16u + dx, 80u + dy) != 0) ++nz;
        if (nz >= 40 && nz <= 200) ++big_ok;
        for (unsigned dx = 0; dx < 16; ++dx)
            if (nibble_at(d * 16u + dx, 95u) != 0) ++bottom_nz;
    }
    return big_ok >= 9 && bottom_nz <= 4;
}

bool HdFont::is_font_sheet_at(unsigned vram_x, unsigned vram_y, const uint16_t *vram) {
    if (!vram || vram_x < kMinSheetX || vram_x + kSheetWords > 1024u || vram_y + kFontSheetHeight > 512u)
        return false;
    const uint16_t *base = vram + size_t(vram_y) * 1024u + vram_x;

    /* O(1) early reject: this runs for every candidate row of a dirty column.
     * Digit '0' (cell 1, texels 12..23 = words 3..5) has ink on rows 1..9 and
     * blank rows 0 and 10; rejecting blank or misaligned VRAM in 4 word reads
     * avoids scanning empty pages during boot, menus, and FMVs. */
    if ((base[1024u + 3u] | base[1024u + 4u] | base[1024u + 5u]) == 0) return false;
    if ((base[9u * 1024u + 3u] | base[9u * 1024u + 4u] | base[9u * 1024u + 5u]) == 0) return false;
    if ((base[3u] | base[4u] | base[5u]) != 0) return false;
    if ((base[10u * 1024u + 3u] | base[10u * 1024u + 4u] | base[10u * 1024u + 5u]) != 0) return false;

    /* The gutter (texels 252..255 = the last VRAM word) of the first four glyph
     * rows is empty, and so is cell 0 (space = texels 0..11 = words 0..2). */
    for (unsigned ly = 0; ly < 48; ++ly)
        if (base[size_t(ly) * 1024u + 63u] != 0) return false;
    for (unsigned ly = 0; ly < kCellNative; ++ly) {
        const uint16_t *row = base + size_t(ly) * 1024u;
        if (row[0] | row[1] | row[2]) return false;
    }

    auto nibble_at = [&](unsigned lx, unsigned ly) -> uint8_t {
        uint16_t word = base[size_t(ly) * 1024u + (lx >> 2)];
        return uint8_t((word >> ((lx & 3u) * 4u)) & 0xfu);
    };
    auto cell_ink = [&](unsigned idx, int &top, int &bottom) -> unsigned {
        const unsigned cx = (idx % kGlyphCols) * kCellNative;
        const unsigned cy = (idx / kGlyphCols) * kCellNative;
        unsigned count = 0;
        top = int(kCellNative);
        bottom = -1;
        for (unsigned dy = 0; dy < kCellNative; ++dy) {
            for (unsigned dx = 0; dx < kCellNative; ++dx) {
                if (nibble_at(cx + dx, cy + dy) == 0) continue;
                ++count;
                if (int(dy) < top) top = int(dy);
                bottom = int(dy);
            }
        }
        return count;
    };

    /* Cells 1..10 hold the digits 0..9: eight pixels tall plus a one-pixel drop
     * shadow, i.e. ink on cell rows 1..9 exactly. This is what pins the origin
     * row; a sheet seen one row too high or too low fails here. */
    unsigned digits_ok = 0;
    for (unsigned idx = 1; idx <= 10; ++idx) {
        int top = 0, bottom = 0;
        unsigned count = cell_ink(idx, top, bottom);
        if (count >= 6 && count <= 115 && top == 1 && bottom == 9) ++digits_ok;
    }
    if (digits_ok < 9) return false;

    unsigned valid_cells = digits_ok;
    for (unsigned idx = 11; idx < 63; ++idx) {
        int top = 0, bottom = 0;
        unsigned count = cell_ink(idx, top, bottom);
        if (count >= 6 && count <= 115) ++valid_cells;
    }
    return valid_cells >= 36;
}

void HdFont::find_sheets(const uint16_t *vram, unsigned x, unsigned y, unsigned w, unsigned h,
                         std::vector<SheetHit> &out) {
    if (!vram || !w || !h || x >= 1024u || y >= 512u) return;
    const unsigned x_end = std::min(1024u, x + w);
    const unsigned y_end = std::min(512u, y + h);
    for (unsigned cx = (x / kSheetWords) * kSheetWords; cx < x_end; cx += kSheetWords) {
        if (cx < kMinSheetX) continue;
        /* The icon sheet fills a whole 256-row texture page. */
        for (unsigned py = 0; py < 512u; py += kIconSheetHeight) {
            if (py >= y_end || py + kIconSheetHeight <= y) continue;
            if (is_icon_font_upload(cx, py, kSheetWords, kIconSheetHeight, vram))
                out.push_back({cx, py, kSheetWords, kIconSheetHeight, true});
        }
        /* sysfnt can sit at any row of its page column. */
        const unsigned first = y >= kFontSheetHeight - 1u ? y - (kFontSheetHeight - 1u) : 0u;
        const unsigned last = std::min(y_end - 1u, 512u - kFontSheetHeight);
        for (unsigned fy = first; fy <= last; ++fy) {
            if (!is_font_sheet_at(cx, fy, vram)) continue;
            out.push_back({cx, fy, kSheetWords, kFontSheetHeight, false});
            fy += kFontSheetHeight - 1u;
        }
    }
}

size_t HdFont::icon_text_count() { return kIconTextCount; }

IconTextInfo HdFont::icon_text(size_t index) {
    IconTextInfo info;
    if (index >= kIconTextCount) return info;
    const IconTextRect &c = kIconTexts[index];
    info.x = c.x; info.y = c.y; info.w = c.w; info.h = c.h;
    info.text = c.text;
    return info;
}

size_t HdFont::icon_band_count() { return icon_bands().size(); }

IconTextInfo HdFont::icon_band(size_t index) {
    IconTextInfo info;
    const auto &bands = icon_bands();
    if (index >= bands.size()) return info;
    info.x = bands[index].x; info.y = bands[index].y;
    info.w = bands[index].w; info.h = bands[index].h;
    return info;
}

int HdFont::icon_band_at(unsigned lx, unsigned ly, unsigned lw, unsigned lh) {
    const auto &bands = icon_bands();
    int best = -1;
    unsigned best_area = 0;
    for (size_t i = 0; i < bands.size(); ++i) {
        const IconBand &b = bands[i];
        const unsigned x0 = std::max(lx, b.x), x1 = std::min(lx + lw, b.x + b.w);
        const unsigned y0 = std::max(ly, b.y), y1 = std::min(ly + lh, b.y + b.h);
        if (x1 <= x0 || y1 <= y0) continue;
        const unsigned area = (x1 - x0) * (y1 - y0);
        if (area > best_area) {
            best_area = area;
            best = int(i);
        }
    }
    return best;
}

void HdFont::extract_clut_colors(const uint16_t clut[16], uint8_t fg[3], uint8_t shadow[3]) {
    fg[0] = fg[1] = fg[2] = 238;
    shadow[0] = shadow[1] = shadow[2] = 16;
    if (!clut) return;

    /* FF8's sysfnt CLUTs fill unused indices 2..11 with a single sentinel colour
     * (usually bright yellow 0x03FF or 0x0000), while actual glyph pixels only use
     * index 1 (drop shadow) and 12..15 (anti-aliased ramp peaking at index 15).
     * Never let unused filler entries override blue/green/red/grey/purple text. */
    if (clut[1] != 0 && clut[15] != 0) {
        bool uniform_filler = true;
        for (unsigned i = 3; i <= 11; ++i) {
            if (clut[i] != clut[2]) { uniform_filler = false; break; }
        }
        if (uniform_filler && clut[15] != clut[2]) {
            unpack_abgr1555(clut[15], fg);
            unpack_abgr1555(clut[1], shadow);
            return;
        }
    }

    unsigned best_fg_luma = 0;
    unsigned best_sh_luma = 0xffffffffu;
    bool found_fg = false;
    bool found_sh = false;

    for (unsigned i = 1; i < 16; ++i) {
        uint16_t entry = clut[i];
        if (entry == 0) continue;
        uint8_t rgb[3];
        unpack_abgr1555(entry, rgb);
        unsigned y = luma(rgb);
        if (!found_fg || y > best_fg_luma) {
            best_fg_luma = y;
            std::memcpy(fg, rgb, 3);
            found_fg = true;
        }
        if (!found_sh || y < best_sh_luma) {
            best_sh_luma = y;
            std::memcpy(shadow, rgb, 3);
            found_sh = true;
        }
    }
    if (!found_sh || best_sh_luma * 2 >= best_fg_luma) {
        shadow[0] = uint8_t(std::max(1, int(fg[0]) / 7));
        shadow[1] = uint8_t(std::max(1, int(fg[1]) / 7));
        shadow[2] = uint8_t(std::max(1, int(fg[2]) / 7));
    }
}

namespace {

struct PlacedGlyph {
    int glyph;
    float x; /* pen position in font units */
};

/* --- sysfnt cells ---------------------------------------------------------
 * The game draws every character as a 12x12 sprite of its cell and then moves
 * the pen by an advance the texture does not carry. The sheet shows it all the
 * same: a glyph's strokes start at the left edge of its cell, and its advance
 * is their width plus two texels, one of them taken by the drop shadow
 * (palette index 1). The digits all advance like the widest digit. So the game
 * fixes, per glyph, a slot the HD glyph has to fill: a face drawn with its own
 * advances there leaves holes after its narrow letters and collides its wide
 * ones, whatever its size. */
constexpr unsigned kSlotGap = 2;
constexpr unsigned kShadowIndex = 1;
constexpr unsigned kDigitFirst = 1, kDigitLast = 10; /* '0'..'9' */
constexpr unsigned kCapFirst = 37, kCapLast = 62;    /* 'A'..'Z' */

struct SheetLayout {
    int cap_row = 0;  /* first row of a capital's strokes */
    int base_row = 9; /* first row below them */
    std::vector<uint8_t> slot; /* advance of each cell in texels, 0 = unknown */
    uint64_t hash = 0;
};

void measure_sheet(const uint16_t *vram, unsigned vram_x, unsigned vram_y, size_t cells, SheetLayout &layout) {
    layout.slot.assign(cells, 0);
    unsigned top_votes[kCellNative] = {0}, bottom_votes[kCellNative] = {0}, caps = 0;
    for (size_t idx = 0; idx < cells; ++idx) {
        const unsigned cx = (unsigned(idx) % kGlyphCols) * kCellNative;
        const unsigned cy = (unsigned(idx) / kGlyphCols) * kCellNative;
        int any_x1 = -1, x1 = -1, top = -1, bottom = -1;
        for (unsigned dy = 0; dy < kCellNative; ++dy) {
            const uint16_t *row = vram + size_t(vram_y + cy + dy) * 1024u + vram_x;
            for (unsigned dx = 0; dx < kCellNative; ++dx) {
                const unsigned lx = cx + dx;
                const unsigned n = (row[lx >> 2] >> ((lx & 3u) * 4u)) & 0xfu;
                if (!n) continue;
                any_x1 = std::max(any_x1, int(dx));
                if (n == kShadowIndex) continue;
                x1 = std::max(x1, int(dx));
                if (top < 0) top = int(dy);
                bottom = int(dy);
            }
        }
        if (any_x1 < 0) continue;
        if (x1 < 0) x1 = any_x1 - 1;
        layout.slot[idx] = uint8_t(std::clamp(x1 + 1 + int(kSlotGap), 1, 15));
        if (idx >= kCapFirst && idx <= kCapLast && bottom >= 0) {
            ++top_votes[top];
            ++bottom_votes[bottom];
            ++caps;
        }
    }
    uint8_t digit_slot = 0;
    for (size_t idx = kDigitFirst; idx <= kDigitLast && idx < cells; ++idx) digit_slot = std::max(digit_slot, layout.slot[idx]);
    for (size_t idx = kDigitFirst; idx <= kDigitLast && idx < cells; ++idx)
        if (layout.slot[idx]) layout.slot[idx] = digit_slot;

    if (caps >= 10) {
        const int top = int(std::max_element(top_votes, top_votes + kCellNative) - top_votes);
        const int below = int(std::max_element(bottom_votes, bottom_votes + kCellNative) - bottom_votes) + 1;
        if (below - top >= 6 && below <= int(kCellNative) - 1) {
            layout.cap_row = top;
            layout.base_row = below;
        }
    }
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    auto mix = [&](unsigned v) { h = (h ^ v) * UINT64_C(0x100000001b3); };
    mix(unsigned(layout.cap_row));
    mix(unsigned(layout.base_row));
    for (uint8_t v : layout.slot) mix(v);
    layout.hash = h;
}

/* Grows (radius > 0) or shrinks (radius < 0) coverage along x by a fractional
 * number of columns. A glyph rasterized k times wider than the face draws it
 * has vertical strokes k times heavier; shrinking it by half the difference on
 * each side gives every stroke its weight back, so a glyph can take the width
 * of its slot and still match its neighbours. */
void morph_columns(std::vector<uint8_t> &buf, std::vector<uint8_t> &line, int w, int h, float radius) {
    const bool grow = radius > 0.0f;
    const float r = std::fabs(radius);
    if (r < 0.02f) return;
    const int r0 = int(r);
    const int frac = int(std::lround((r - float(r0)) * 256.0f));
    line.resize(size_t(w));
    for (int y = 0; y < h; ++y) {
        uint8_t *row = &buf[size_t(y) * size_t(w)];
        int first = 0;
        while (first < w && !row[first]) ++first;
        if (first == w) continue;
        int last = w - 1;
        while (!row[last]) --last;
        const int x_begin = grow ? std::max(0, first - r0 - 1) : first;
        const int x_end = grow ? std::min(w - 1, last + r0 + 1) : last;
        for (int x = x_begin; x <= x_end; ++x) {
            int v0 = row[x];
            for (int d = 1; d <= r0; ++d) {
                const int a = x - d >= 0 ? row[x - d] : 0;
                const int b = x + d < w ? row[x + d] : 0;
                v0 = grow ? std::max({v0, a, b}) : std::min({v0, a, b});
            }
            const int a = x - r0 - 1 >= 0 ? row[x - r0 - 1] : 0;
            const int b = x + r0 + 1 < w ? row[x + r0 + 1] : 0;
            const int v1 = grow ? std::max({v0, a, b}) : std::min({v0, a, b});
            line[size_t(x)] = uint8_t(v0 + ((v1 - v0) * frac) / 256);
        }
        std::memcpy(row + x_begin, &line[size_t(x_begin)], size_t(x_end - x_begin + 1));
    }
}

/* Share of a slot's surplus width a narrow glyph takes by widening; the rest
 * is split between its two sides. */
constexpr float kWidenShare = 0.8f;
constexpr float kMaxWiden = 1.4f;
/* Least coverage that counts as glyph: the renderer keeps or drops a texel whole. */
constexpr uint8_t kCutout = 110;

} // namespace

bool HdFont::render_atlas(unsigned local_w, unsigned local_h, const uint16_t clut[16],
                          unsigned scale, FontRgba &out,
                          const uint16_t *vram, unsigned vram_x, unsigned vram_y) const {
    if (mode_ == FontMode::Off || !local_w || !local_h) return false;

    uint8_t fg[3], shadow[3];
    extract_clut_colors(clut, fg, shadow);
    if (clut && vram && vram_x + (local_w >> 2) <= 1024u && vram_y + local_h <= 512u &&
        local_w >= kGlyphCols * kCellNative) {
        /* Measure which 4-bit palette indices the sheet's actual glyph pixels use so
         * unused CLUT entries (e.g. FF8's yellow filler at indices 2..11) are ignored. */
        unsigned counts[16] = {0};
        const unsigned sample_rows = std::min(local_h, 4u * kCellNative);
        for (unsigned dy = 0; dy < sample_rows; ++dy) {
            const uint16_t *row = vram + size_t(vram_y + dy) * 1024u + vram_x;
            for (unsigned dx = 0; dx < kGlyphCols * kCellNative; ++dx) {
                const unsigned n = (row[dx >> 2] >> ((dx & 3u) * 4u)) & 0xfu;
                ++counts[n];
            }
        }
        unsigned best_idx = 0, best_count = 0;
        for (unsigned i = 2; i < 16; ++i) {
            if (counts[i] > best_count && clut[i] != 0) {
                best_count = counts[i];
                best_idx = i;
            }
        }
        if (best_idx != 0) {
            unpack_abgr1555(clut[best_idx], fg);
            if (counts[kShadowIndex] > 0 && clut[kShadowIndex] != 0) {
                unpack_abgr1555(clut[kShadowIndex], shadow);
            }
        }
    }

    /* The backdrop is always a dark charcoal, so bright or yellow glyphs stay
     * legible against FF8's metallic grey windows. */
    uint8_t backdrop[3];
    for (int ch = 0; ch < 3; ++ch) {
        int s = std::min<int>(shadow[ch], 28);
        if (s > int(fg[ch]) / 4) s = int(fg[ch]) / 6;
        backdrop[ch] = uint8_t(std::clamp(s, 14, 30));
    }

    if (has_custom_png_) {
        out.width = custom_png_.width;
        out.height = custom_png_.height;
        out.pixels = custom_png_.pixels;
        for (size_t i = 0; i < out.pixels.size(); i += 4) {
            if (out.pixels[i + 3] == 0) continue;
            out.pixels[i + 0] = uint8_t((unsigned(out.pixels[i + 0]) * fg[0] + 127u) / 255u);
            out.pixels[i + 1] = uint8_t((unsigned(out.pixels[i + 1]) * fg[1] + 127u) / 255u);
            out.pixels[i + 2] = uint8_t((unsigned(out.pixels[i + 2]) * fg[2] + 127u) / 255u);
            clamp_non_sentinel(&out.pixels[i]);
        }
        return true;
    }

    if (!ttf_data_ || !ttf_size_ || local_w < kGlyphCols * kCellNative) return false;
    scale = scale >= 8 ? 8u : (scale >= 4 ? 4u : 2u);
    if (!build_coverage(local_w, local_h, scale, vram, vram_x, vram_y)) return false;

    /* The glyphs are the same picture under every palette; only the colours differ. */
    const FontCoverage &cov = coverage_;
    const unsigned cell = kCellNative * scale;
    out.width = cov.width;
    out.height = cov.height;
    out.pixels.assign(size_t(out.width) * size_t(out.height) * 4u, 0);
    uint8_t ramp[256][4];
    for (unsigned a = 0; a < 256; ++a) {
        const float t = float(a) / 255.0f;
        for (int ch = 0; ch < 3; ++ch) {
            const float c = float(backdrop[ch]) * (1.0f - t) + float(fg[ch]) * t;
            ramp[a][ch] = uint8_t(std::clamp(int(std::lround(c)), 0, 255));
        }
        ramp[a][3] = 255;
        clamp_non_sentinel(ramp[a]);
    }
    const unsigned rows = out.height / cell;
    for (unsigned row = 0; row < rows; ++row) {
        for (unsigned col = 0; col < kGlyphCols; ++col) {
            const size_t idx = size_t(row) * kGlyphCols + col;
            const bool native = idx >= cov.native_cell.size() || cov.native_cell[idx];
            for (unsigned py = 0; py < cell; ++py) {
                const size_t line = size_t(row * cell + py) * out.width + col * cell;
                uint8_t *dst = &out.pixels[line * 4u];
                if (native) {
                    /* parallel-psx reads (0, 0, 0, 255) as "sample the original texel". */
                    for (unsigned px = 0; px < cell; ++px) dst[px * 4u + 3] = 255;
                    continue;
                }
                const uint8_t *glyph = &cov.glyph[line];
                const uint8_t *back = &cov.backdrop[line];
                for (unsigned px = 0; px < cell; ++px)
                    if (back[px]) std::memcpy(dst + px * 4u, ramp[glyph[px]], 4);
            }
        }
    }
    /* Columns past the last cell (the sheet's 4-texel gutter) hold nothing. */
    return true;
}

bool HdFont::build_coverage(unsigned local_w, unsigned local_h, unsigned scale,
                            const uint16_t *vram, unsigned vram_x, unsigned vram_y) const {
    const unsigned max_rows = local_h / kCellNative;
    const size_t total_cells = size_t(max_rows) * kGlyphCols;
    const bool has_vram = vram != nullptr && vram_x + (local_w >> 2) <= 1024u && vram_y + local_h <= 512u &&
                          local_w >= kGlyphCols * kCellNative;
    SheetLayout layout;
    if (has_vram) measure_sheet(vram, vram_x, vram_y, total_cells, layout);

    uint64_t key = UINT64_C(0x9e3779b97f4a7c15) ^ generation_;
    for (uint64_t v : {uint64_t(local_w), uint64_t(local_h), uint64_t(scale), uint64_t(has_vram), layout.hash})
        key = (key ^ v) * UINT64_C(0x100000001b3);
    FontCoverage &cov = coverage_;
    if (cov.width && cov.key == key) return true;

    stbtt_fontinfo font{};
    if (!stbtt_InitFont(&font, ttf_data_, stbtt_GetFontOffsetForIndex(ttf_data_, 0))) return false;

    const int s = int(scale);
    const int cell = int(kCellNative) * s;
    cov.key = key;
    cov.width = local_w * scale;
    cov.height = local_h * scale;
    cov.glyph.assign(size_t(cov.width) * size_t(cov.height), 0);
    cov.backdrop.assign(size_t(cov.width) * size_t(cov.height), 0);
    cov.native_cell.assign(total_cells, 0);

    auto glyph_box = [&](int cp, int box[4]) -> bool {
        const int g = stbtt_FindGlyphIndex(&font, cp);
        return g && stbtt_GetGlyphBox(&font, g, &box[0], &box[1], &box[2], &box[3]) && box[2] > box[0];
    };
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
    int box[4] = {0, 0, 0, 0};
    const float cap_units = glyph_box('H', box) && box[3] > 0 ? float(box[3]) : float(std::max(1, (ascent * 7) / 10));
    const float side_units = glyph_box('H', box) ? float(std::max(0, box[0])) : 0.1f * cap_units;
    float desc_units = 0.0f;
    for (int cp : {'g', 'j', 'p', 'q', 'y'})
        if (glyph_box(cp, box)) desc_units = std::max(desc_units, float(-box[1]));
    float digit_adv = 0.0f;
    for (int cp = '0'; cp <= '9'; ++cp) {
        int adv = 0, lsb = 0;
        if (const int g = stbtt_FindGlyphIndex(&font, cp)) {
            stbtt_GetGlyphHMetrics(&font, g, &adv, &lsb);
            digit_adv = std::max(digit_adv, float(adv));
        }
    }

    /* A thin dark rim keeps the glyph edges anti-aliased under the renderer's
     * alpha cutout; the depth is a drop shadow down and right, like the game's
     * own, a little over half a texel long. */
    const int outline_r = std::max(1, s / 8);
    const int shadow_px = std::max(1, int(std::lround(0.625f * float(s))));

    /* Capitals span the rows the game's capitals do, a little inside the first
     * one so that the rim fits above them. The size gives way only to keep the
     * descenders and their shadow inside the cell. */
    const float top_lim = float(outline_r);
    const float bot_lim = float(cell - std::max(shadow_px, outline_r));
    const int base_y = layout.base_row * s - std::max(1, s / 8);
    const float cap_y = float(layout.cap_row * s) + std::max(float(outline_r) + 0.5f, 0.3f * float(s));
    float sf = (float(base_y) - cap_y) / cap_units;
    if (desc_units * sf > bot_lim - float(base_y))
        sf = std::max((bot_lim - float(base_y)) / desc_units, 0.75f * sf);

    /* Stroke weights of the face at this size, measured on its own glyphs: the
     * stem of 'l' halfway up, and the bar of 'T' near its left end. */
    std::vector<uint8_t> bmp;
    auto stroke_px = [&](int cp, bool across_row) -> float {
        const int g = stbtt_FindGlyphIndex(&font, cp);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!g) return 0.0f;
        stbtt_GetGlyphBitmapBox(&font, g, sf, sf, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw < 1 || gh < 4) return 0.0f;
        bmp.assign(size_t(gw) * size_t(gh), 0);
        stbtt_MakeGlyphBitmap(&font, bmp.data(), gw, gh, gw, sf, sf, g);
        unsigned sum = 0;
        if (across_row)
            for (int x = 0; x < gw; ++x) sum += bmp[size_t(gh / 2) * size_t(gw) + size_t(x)];
        else
            for (int y = 0; y < gh; ++y) sum += bmp[size_t(y) * size_t(gw) + size_t(gw / 8)];
        return float(sum) / 255.0f;
    };
    const float cap_px = cap_units * sf;
    float stem_px = stroke_px('l', true);
    if (stem_px < 0.05f * cap_px || stem_px > 0.25f * cap_px) stem_px = 0.13f * cap_px;
    float bar_px = stroke_px('T', false);
    if (bar_px < 0.4f * stem_px || bar_px > 1.5f * stem_px) bar_px = 0.88f * stem_px;

    /* Glyphs are rasterized on columns 32 times finer than a texel, so that the
     * stroke compensation works in small fractions of a pixel at every scale. */
    const int fine = 32 / s;
    const int wide_w = cell * fine;
    std::vector<uint8_t> wide(size_t(wide_w) * size_t(cell)), line;
    std::vector<PlacedGlyph> run;

    const size_t table_count = sizeof(kFf8Glyphs) / sizeof(kFf8Glyphs[0]);
    for (size_t idx = 0; idx < total_cells; ++idx) {
        const char *lig = idx < table_count ? sysfnt_ligature(idx) : nullptr;
        const uint32_t cp = idx < table_count ? kFf8Glyphs[idx] : 0u;
        const bool geom = !lig && cp == 0 && idx < table_count && is_geometric_sysfnt(idx);
        if (!lig && cp == ' ') continue;
        /* Symbols and icons the table does not name stay native. */
        if (!lig && cp == 0 && !geom) {
            cov.native_cell[idx] = 1;
            continue;
        }

        auto add_bar = [&](float x0, float x1, float y0, float y1) {
            x0 *= float(fine);
            x1 *= float(fine);
            for (int y = std::max(0, int(std::floor(y0))); y < cell && float(y) < y1; ++y) {
                const float cy = std::min(y1, float(y + 1)) - std::max(y0, float(y));
                for (int x = std::max(0, int(std::floor(x0))); x < wide_w && float(x) < x1; ++x) {
                    const float cx = std::min(x1, float(x + 1)) - std::max(x0, float(x));
                    uint8_t &dst = wide[size_t(y) * size_t(wide_w) + size_t(x)];
                    dst = std::max(dst, uint8_t(std::lround(255.0f * std::clamp(cx * cy, 0.0f, 1.0f))));
                }
            }
        };

        if (geom) {
            std::fill(wide.begin(), wide.end(), uint8_t(0));
            const uint32_t code = uint32_t(idx + 0x20u);
            const float room = float(cell - shadow_px - outline_r);
            const bool is_corner = (code == 0x36u || code == 0x37u || code == 0xB2u || code == 0xB3u);
            const float def_slot = (is_corner ? 0.72f : 0.90f) * cap_px + 2.0f * float(outline_r);
            const float slot = std::min(has_vram && layout.slot[idx] ? float(int(layout.slot[idx]) * s) : def_slot, room);
            const float gy0 = std::max(top_lim, float(base_y) - cap_px);
            const float gy1 = std::min(bot_lim, float(base_y));
            const float gh = std::max(2.0f * bar_px, gy1 - gy0);
            const float cy = 0.5f * (gy0 + gy1);
            const float avail_l = float(outline_r);
            const float avail_r = std::clamp(slot - float(outline_r), avail_l + 3.0f * stem_px, float(cell - shadow_px));
            const float gw = avail_r - avail_l;
            const float cx = 0.5f * (avail_l + avail_r);

            auto add_sdf = [&](auto sdf_fn) {
                for (int y = 0; y < cell; ++y) {
                    const float py = float(y) + 0.5f;
                    for (int x = 0; x < wide_w; ++x) {
                        const float px = (float(x) + 0.5f) / float(fine);
                        const float cov_v = std::clamp(sdf_fn(px, py), 0.0f, 1.0f);
                        if (cov_v > 0.0f) {
                            uint8_t &dst = wide[size_t(y) * size_t(wide_w) + size_t(x)];
                            dst = std::max(dst, uint8_t(std::lround(255.0f * cov_v)));
                        }
                    }
                }
            };

            auto tri_down_sd = [](float px, float py, float tcx, float ty0, float ty1, float tw) -> float {
                const float th = ty1 - ty0;
                const float hw = 0.5f * tw;
                const float len = std::hypot(th, hw);
                const float nx = th / len, ny = hw / len;
                const float d_top = ty0 - py;
                const float d_left = -(px - tcx) * nx + (py - ty1) * ny;
                const float d_right = (px - tcx) * nx + (py - ty1) * ny;
                return std::max(d_top, std::max(d_left, d_right));
            };

            switch (code) {
            case 0x36u: { /* 「 */
                const float w = std::min(gw, 0.55f * gh);
                const float xl = avail_r - w, xr = avail_r;
                const float yt = gy0, yb = gy0 + 0.65f * gh;
                add_bar(xl, xr, yt, yt + bar_px);
                add_bar(xl, xl + stem_px, yt, yb);
                break;
            }
            case 0x37u: { /* 」 */
                const float w = std::min(gw, 0.55f * gh);
                const float xl = avail_l, xr = avail_l + w;
                const float yb = gy1, yt = gy1 - 0.65f * gh;
                add_bar(xl, xr, yb - bar_px, yb);
                add_bar(xr - stem_px, xr, yt, yb);
                break;
            }
            case 0xB2u: { /* 『 */
                const float tv = 0.62f * stem_px, th = 0.62f * bar_px;
                const float w = std::min(gw, 0.68f * gh), h = 0.74f * gh;
                const float arm = std::clamp(0.30f * gh, 2.2f * tv, std::min(w, h) * 0.55f);
                const float xl = avail_r - w, xr = avail_r;
                const float yt = gy0, yb = gy0 + h;
                add_bar(xl, xr, yt, yt + th);
                add_bar(xl, xl + tv, yt, yb);
                add_bar(xr - tv, xr, yt, yt + arm);
                add_bar(xl + arm - tv, xr, yt + arm - th, yt + arm);
                add_bar(xl + arm - tv, xl + arm, yt + arm - th, yb);
                add_bar(xl, xl + arm, yb - th, yb);
                break;
            }
            case 0xB3u: { /* 』 */
                const float tv = 0.62f * stem_px, th = 0.62f * bar_px;
                const float w = std::min(gw, 0.68f * gh), h = 0.74f * gh;
                const float arm = std::clamp(0.30f * gh, 2.2f * tv, std::min(w, h) * 0.55f);
                const float xl = avail_l, xr = avail_l + w;
                const float yb = gy1, yt = gy1 - h;
                add_bar(xl, xr, yb - th, yb);
                add_bar(xr - tv, xr, yt, yb);
                add_bar(xl, xl + tv, yb - arm, yb);
                add_bar(xl, xr - arm + tv, yb - arm, yb - arm + th);
                add_bar(xr - arm, xr - arm + tv, yt, yb - arm + th);
                add_bar(xr - arm, xr, yt, yt + th);
                break;
            }
            case 0xABu: { /* ■ */
                const float side = std::min(gw, 0.74f * gh);
                add_bar(cx - 0.5f * side, cx + 0.5f * side, cy - 0.5f * side, cy + 0.5f * side);
                break;
            }
            case 0xB0u: { /* □ */
                const float side = std::min(gw, 0.78f * gh);
                const float xl = cx - 0.5f * side, xr = cx + 0.5f * side;
                const float yt = cy - 0.5f * side, yb = cy + 0.5f * side;
                const float tv = 0.85f * stem_px, th = 0.85f * bar_px;
                add_bar(xl, xr, yt, yt + th);
                add_bar(xl, xr, yb - th, yb);
                add_bar(xl, xl + tv, yt, yb);
                add_bar(xr - tv, xr, yt, yb);
                break;
            }
            case 0xADu: { /* ♦ */
                const float rx = 0.5f * std::min(gw, 0.82f * gh);
                const float ry = 0.5f * std::min(0.86f * gh, rx * 2.2f);
                const float k_norm = std::min(rx, ry) * 0.70710678f;
                add_sdf([&](float px, float py) {
                    const float d = (std::fabs(px - cx) / rx + std::fabs(py - cy) / ry - 1.0f) * k_norm;
                    return 0.5f - d;
                });
                break;
            }
            case 0xB6u: { /* ▼ */
                const float tw = std::min(gw, 0.84f * gh), th = 0.72f * gh;
                const float ty0 = cy - 0.45f * th, ty1 = cy + 0.55f * th;
                add_sdf([&](float px, float py) {
                    return 0.5f - tri_down_sd(px, py, cx, ty0, ty1, tw);
                });
                break;
            }
            case 0xB4u: { /* ▽ */
                const float tw = std::min(gw, 0.84f * gh), th = 0.74f * gh;
                const float ty0 = cy - 0.45f * th, ty1 = cy + 0.55f * th;
                const float stroke = 0.85f * bar_px;
                add_sdf([&](float px, float py) {
                    const float sd = tri_down_sd(px, py, cx, ty0, ty1, tw);
                    return std::clamp(0.5f - sd, 0.0f, 1.0f) - std::clamp(0.5f - (sd + stroke), 0.0f, 1.0f);
                });
                break;
            }
            case 0xBBu: { /* ↓ */
                const float tw = std::min(gw, 0.76f * gh);
                const float y_head = gy1 - 0.44f * gh;
                add_bar(cx - 0.45f * stem_px, cx + 0.45f * stem_px, gy0, gy1 - 0.35f * gh);
                add_sdf([&](float px, float py) {
                    return 0.5f - tri_down_sd(px, py, cx, y_head, gy1, tw);
                });
                break;
            }
            default:
                break;
            }
        } else {
            /* Lay the cell's glyph (or numeral) out in font units. */
            run.clear();
            float adv_units = 0.0f, min_ux = 1e9f, max_ux = -1e9f, min_uy = 1e9f, max_uy = -1e9f;
            bool complete = true;
            int prev = 0;
            for (const char *p = lig ? lig : "?"; *p; ++p) {
                const int g = stbtt_FindGlyphIndex(&font, lig ? int(uint8_t(*p)) : int(cp));
                if (!g) { complete = false; break; }
                if (prev) adv_units += float(stbtt_GetGlyphKernAdvance(&font, prev, g));
                int adv = 0, lsb = 0, b[4] = {0, 0, 0, 0};
                stbtt_GetGlyphHMetrics(&font, g, &adv, &lsb);
                if (stbtt_GetGlyphBox(&font, g, &b[0], &b[1], &b[2], &b[3])) {
                    min_ux = std::min(min_ux, adv_units + float(b[0]));
                    max_ux = std::max(max_ux, adv_units + float(b[2]));
                    min_uy = std::min(min_uy, float(b[1]));
                    max_uy = std::max(max_uy, float(b[3]));
                    run.push_back({g, adv_units});
                }
                adv_units += float(adv);
                prev = g;
            }
            if (!complete || run.empty() || max_ux <= min_ux || max_uy <= min_uy || adv_units <= 0.0f) {
                cov.native_cell[idx] = 1;
                continue;
            }

            /* The slot: the game's advance, or the face's own when the sheet is not
             * given; never more than the cell can show with rim and shadow. */
            const float overhang = lig ? 0.45f * stem_px : 0.0f; /* of a numeral's bars */
            const float room = float(cell - shadow_px - outline_r) - 2.0f * overhang;
            const float slot = std::min(has_vram && layout.slot[idx] ? float(int(layout.slot[idx]) * s) : adv_units * sf, room);

            /* A capital I that is a bare stem is far narrower than the slot the
             * game's serifed I leaves it; it gets the same serifs, as wide as the
             * slot allows between two ordinary side bearings. */
            float serif_w = 0.0f;
            if (!lig && cp == 'I' && (max_ux - min_ux) * sf < 1.3f * stem_px) {
                serif_w = std::min(slot - 2.0f * side_units * sf, 3.0f * stem_px);
                if (serif_w < 1.6f * stem_px) serif_w = 0.0f;
            }

            /* Fit the glyph's advance to its slot: wider slots widen it (most of the
             * way; the remainder is shared by its two sides), narrower ones narrow it.
             * Digits are fitted as one family, so each stays centred in the common
             * slot. Then keep the ink, its rim and its shadow inside the cell: the
             * neighbouring cells belong to other glyphs. */
            const bool digit = !lig && idx >= kDigitFirst && idx <= kDigitLast && digit_adv > 0.0f;
            const float k_raw = slot / ((digit ? digit_adv : adv_units) * sf);
            float k = k_raw >= 1.0f ? std::min(std::pow(k_raw, kWidenShare), kMaxWiden) : std::max(k_raw, 0.4f);
            if (serif_w > 0.0f) k = 1.0f;
            const float span_px = (max_ux - min_ux) * sf;
            if (span_px > stem_px && span_px * k - (k - 1.0f) * stem_px > room - float(outline_r))
                k = std::max(0.4f, (room - float(outline_r) - stem_px) / (span_px - stem_px));
            const float thin = (k - 1.0f) * stem_px * 0.5f; /* per side; negative thickens */
            float pen = (slot - adv_units * sf * k) * 0.5f;
            const float ink_w = serif_w > 0.0f ? serif_w : span_px * k - 2.0f * thin;
            float ink_l = serif_w > 0.0f ? pen + (min_ux + max_ux) * 0.5f * sf - serif_w * 0.5f : pen + min_ux * sf * k + thin;
            const float min_shift = float(outline_r) + overhang - ink_l;
            const float shift = std::clamp(0.0f, min_shift, std::max(min_shift, float(cell - shadow_px) - overhang - ink_w - ink_l));
            pen += shift;
            ink_l += shift;

            /* Vertical fit. Accented capitals rise above the cell and are shortened
             * onto the baseline, as in the game's own font; brackets and the like,
             * which reach both above the capitals and below the baseline, keep their
             * shape and move. */
            float sy = sf, base = float(base_y);
            if (base - max_uy * sy < top_lim || base - min_uy * sy > bot_lim) {
                if (max_uy > 1.02f * cap_units && min_uy < -0.08f * cap_units) {
                    sy = std::min(sy, (bot_lim - top_lim) / (max_uy - min_uy));
                    base = std::clamp(base, top_lim + max_uy * sy, bot_lim + min_uy * sy);
                } else if (base - max_uy * sy < top_lim) {
                    sy = (base - top_lim) / max_uy;
                } else {
                    sy = (bot_lim - base) / -min_uy;
                }
            }
            const int base_i = int(std::lround(base));

            std::fill(wide.begin(), wide.end(), uint8_t(0));
            const float sx_fine = sf * k * float(fine);
            for (const PlacedGlyph &pg : run) {
                const float gx = (pen + pg.x * sf * k) * float(fine);
                const float gx_floor = std::floor(gx);
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                stbtt_GetGlyphBitmapBoxSubpixel(&font, pg.glyph, sx_fine, sy, gx - gx_floor, 0.0f, &x0, &y0, &x1, &y1);
                const int gw = x1 - x0, gh = y1 - y0;
                if (gw <= 0 || gh <= 0) continue;
                bmp.assign(size_t(gw) * size_t(gh), 0);
                stbtt_MakeGlyphBitmapSubpixel(&font, bmp.data(), gw, gh, gw, sx_fine, sy, gx - gx_floor, 0.0f, pg.glyph);
                const int ox = int(gx_floor) + x0, oy = base_i + y0;
                for (int py = 0; py < gh; ++py) {
                    const int dy = oy + py;
                    if (dy < 0 || dy >= cell) continue;
                    for (int px = 0; px < gw; ++px) {
                        const int dx = ox + px;
                        if (dx < 0 || dx >= wide_w) continue;
                        uint8_t &dst = wide[size_t(dy) * size_t(wide_w) + size_t(dx)];
                        dst = std::max(dst, bmp[size_t(py) * size_t(gw) + size_t(px)]);
                    }
                }
            }
            morph_columns(wide, line, wide_w, cell, -thin * float(fine));
            /* Bars: the serifs of I, and the lines that close a Roman numeral above
             * all of it and below its I's, as in the game's own numerals. */
            const float cap_line = float(base_i) - cap_units * sy;
            if (serif_w > 0.0f) {
                add_bar(ink_l, ink_l + serif_w, cap_line, cap_line + bar_px);
                add_bar(ink_l, ink_l + serif_w, float(base_i) - bar_px, float(base_i));
            } else if (lig) {
                add_bar(ink_l - overhang, ink_l + ink_w + overhang, cap_line, cap_line + bar_px);
                const int stem_glyph = stbtt_FindGlyphIndex(&font, 'I');
                float i_l = 1e9f, i_r = -1e9f;
                for (const PlacedGlyph &pg : run) {
                    int b[4] = {0, 0, 0, 0};
                    if (pg.glyph != stem_glyph || !stbtt_GetGlyphBox(&font, pg.glyph, &b[0], &b[1], &b[2], &b[3])) continue;
                    i_l = std::min(i_l, pen + (pg.x + float(b[0])) * sf * k + thin);
                    i_r = std::max(i_r, pen + (pg.x + float(b[2])) * sf * k - thin);
                }
                if (i_r > i_l) add_bar(i_l - overhang, i_r + overhang, float(base_i) - bar_px, float(base_i));
            }
        }

        const size_t cell_x = (idx % kGlyphCols) * size_t(cell);
        const size_t cell_y = (idx / kGlyphCols) * size_t(cell);
        uint8_t *glyph0 = &cov.glyph[cell_y * cov.width + cell_x];
        uint8_t *back0 = &cov.backdrop[cell_y * cov.width + cell_x];
        for (int y = 0; y < cell; ++y) {
            const uint8_t *src = &wide[size_t(y) * size_t(wide_w)];
            uint8_t *dst = glyph0 + size_t(y) * cov.width;
            for (int x = 0; x < cell; ++x) {
                unsigned sum = 0;
                for (int f = 0; f < fine; ++f) sum += src[x * fine + f];
                dst[x] = uint8_t((sum + unsigned(fine) / 2u) / unsigned(fine));
            }
        }

        /* Backdrop: the rim around the strokes and their shadow swept down-right.
         * The renderer cuts alpha at one half, so it is either there or not. */
        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < cell; ++x) {
                if (glyph0[size_t(y) * cov.width + size_t(x)] < kCutout) continue;
                for (int dy = -outline_r; dy <= outline_r; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= cell) continue;
                    for (int dx = -outline_r; dx <= outline_r; ++dx) {
                        const int nx = x + dx;
                        if (nx < 0 || nx >= cell || dx * dx + dy * dy > outline_r * outline_r + 1) continue;
                        back0[size_t(ny) * cov.width + size_t(nx)] = 1;
                    }
                }
                for (int d = 1; d <= shadow_px; ++d) {
                    if (x + d >= cell || y + d >= cell) break;
                    back0[size_t(y + d) * cov.width + size_t(x + d)] = 1;
                    back0[size_t(y + d) * cov.width + size_t(x + d - 1)] = 1;
                }
            }
        }
    }
    return true;
}

namespace {

/* --- Battle party names --------------------------------------------------
 * Three 96x16 strips at local (0, 96 + 16 * n) of the icon sheet. The name face
 * shares its metrics with sysfnt: a glyph sits one row lower than in its sysfnt
 * cell and is one column wider, glyphs are one column apart, and the string
 * ends at the strip's right edge. Its drawings are its own, though, so shapes
 * are only compared loosely, against the sysfnt glyph smeared one column. */
constexpr unsigned kNameX = 0, kNameY = 96, kNameW = 96, kNameH = 16;
/* Capitals occupy strip rows 1..9. The party's names are drawn 15 rows apart, so
 * the game clips each strip to its first 13 rows. */
constexpr int kNameCapRow = 1, kNameBaseRow = 9, kNameShownRows = 13;
constexpr float kNameStretch = 1.12f;
constexpr unsigned kFontCells = (HdFont::kFontSheetHeight / kCellNative) * kGlyphCols;
constexpr size_t kMaxNameGlyphs = 12;
constexpr float kNameInk = 0.15f;       /* coverage that counts as glyph ink */
constexpr float kNameGlyphFit = 0.5f;   /* least shape correlation of one glyph */
constexpr float kNameFit = 0.65f;       /* least mean correlation of a name */
constexpr float kNameMargin = 0.02f;    /* lead over the next different name */
constexpr unsigned kGlyphBox = kCellNative + 2;

struct NameSegment {
    int x0, x1, top, bottom;
};

struct NameInk {
    float cov[kNameH][kNameW]; /* 0 = outline or nothing, 1 = brightest ink */
    std::vector<NameSegment> segments;
    uint8_t fg[3];
    uint8_t dark[3];
};

/* Ink of a strip as the player sees it under this palette, split into the runs
 * of inked columns that the one-column glyph spacing leaves. */
bool read_name_ink(size_t strip, const uint16_t clut[16], const uint16_t *vram,
                   unsigned icon_x, unsigned icon_y, NameInk &ink) {
    if (!vram || !clut || strip >= HdFont::kNameStripCount) return false;
    if (icon_x + HdFont::kSheetWords > 1024u || icon_y + HdFont::kIconSheetHeight > 512u) return false;

    unsigned lum[16] = {0};
    uint8_t rgb[16][3] = {{0}};
    for (unsigned i = 1; i < 16; ++i) {
        if (clut[i] == 0) continue;
        unpack_abgr1555(clut[i], rgb[i]);
        lum[i] = luma(rgb[i]);
    }
    uint8_t nib[kNameH][kNameW];
    unsigned lo = 0xffffffffu, hi = 0;
    int lo_i = -1, hi_i = -1;
    const unsigned y0 = icon_y + kNameY + unsigned(strip) * kNameH;
    for (unsigned y = 0; y < kNameH; ++y) {
        const uint16_t *row = vram + size_t(y0 + y) * 1024u + icon_x;
        for (unsigned x = 0; x < kNameW; ++x) {
            const unsigned lx = kNameX + x;
            const unsigned n = (row[lx >> 2] >> ((lx & 3u) * 4u)) & 0xfu;
            nib[y][x] = uint8_t(clut[n] ? n : 0u);
            if (!nib[y][x]) continue;
            if (lum[n] < lo) { lo = lum[n]; lo_i = int(n); }
            if (lum[n] > hi) { hi = lum[n]; hi_i = int(n); }
        }
    }
    /* A name needs its outline and something clearly brighter drawn on it. */
    if (hi_i < 0 || hi < lo + 2550u) return false;
    std::memcpy(ink.fg, rgb[hi_i], 3);
    std::memcpy(ink.dark, rgb[lo_i], 3);

    const float range = float(hi - lo);
    bool column[kNameW];
    for (unsigned x = 0; x < kNameW; ++x) column[x] = false;
    for (unsigned y = 0; y < kNameH; ++y) {
        for (unsigned x = 0; x < kNameW; ++x) {
            const unsigned n = nib[y][x];
            ink.cov[y][x] = n ? float(lum[n] - lo) / range : 0.0f;
            if (ink.cov[y][x] >= kNameInk) column[x] = true;
        }
    }
    ink.segments.clear();
    for (unsigned x = 0; x < kNameW;) {
        if (!column[x]) { ++x; continue; }
        NameSegment seg = {int(x), int(x), int(kNameH), -1};
        while (x < kNameW && column[x]) {
            for (unsigned y = 0; y < kNameH; ++y) {
                if (ink.cov[y][x] < kNameInk) continue;
                if (int(y) < seg.top) seg.top = int(y);
                if (int(y) > seg.bottom) seg.bottom = int(y);
            }
            seg.x1 = int(x++);
        }
        ink.segments.push_back(seg);
    }
    return !ink.segments.empty();
}

/* What a sysfnt glyph should roughly look like in the name face. */
struct NameGlyph {
    bool valid;
    int x0, w;       /* inked columns of the smeared glyph */
    int top, bottom; /* sysfnt cell rows of its ink */
    int runs;        /* separate runs of inked columns (1 for a letter) */
    float p[kGlyphBox][kGlyphBox]; /* [cell row + 1][cell column] */
};

void build_name_glyph(const uint16_t *vram, unsigned font_x, unsigned font_y, unsigned code, NameGlyph &g) {
    g.valid = false;
    if (code <= 0x20u || code - 0x20u >= kFontCells) return;
    const unsigned cell = code - 0x20u;
    const unsigned cx = (cell % kGlyphCols) * kCellNative;
    const unsigned cy = (cell / kGlyphCols) * kCellNative;
    /* Glyph strokes use the top four indices as coverage; the drop shadow is index 1. */
    float f[kGlyphBox][kGlyphBox] = {{0.0f}};
    g.top = int(kCellNative);
    g.bottom = -1;
    for (unsigned r = 0; r < kCellNative; ++r) {
        const uint16_t *row = vram + size_t(font_y + cy + r) * 1024u + font_x;
        for (unsigned c = 0; c < kCellNative; ++c) {
            const unsigned lx = cx + c;
            const unsigned n = (row[lx >> 2] >> ((lx & 3u) * 4u)) & 0xfu;
            if (n < 12u) continue;
            f[r + 1][c + 1] = float(n - 11u) * 0.25f;
            if (int(r) < g.top) g.top = int(r);
            g.bottom = int(r);
        }
    }
    if (g.bottom < 0) return;

    bool column[kGlyphBox - 1];
    for (unsigned c = 0; c + 1 < kGlyphBox; ++c) {
        column[c] = false;
        for (unsigned r = 1; r <= kCellNative; ++r)
            if (f[r][c + 1] > 0.0f || f[r][c] > 0.0f) column[c] = true;
    }
    g.x0 = -1;
    g.runs = 0;
    int last = -1;
    for (unsigned c = 0; c + 1 < kGlyphBox; ++c) {
        if (!column[c]) continue;
        if (g.x0 < 0) g.x0 = int(c);
        if (c == 0 || !column[c - 1]) ++g.runs;
        last = int(c);
    }
    g.w = last - g.x0 + 1;

    /* The stroke and its copy one column to the right, with a little bleed into
     * the rows above and below, as the name face's anti-aliasing leaves it. */
    for (unsigned r = 0; r < kGlyphBox; ++r) {
        for (unsigned c = 0; c + 1 < kGlyphBox; ++c) {
            float v = 0.45f * f[r][c + 1] + 0.5f * f[r][c];
            if (r + 1 < kGlyphBox) v += 0.10f * (f[r + 1][c + 1] + f[r + 1][c]);
            if (r > 0) v += 0.05f * (f[r - 1][c + 1] + f[r - 1][c]);
            g.p[r][c] = v;
        }
        g.p[r][kGlyphBox - 1] = 0.0f;
    }
    g.valid = true;
}

/* Normalized correlation of a strip segment with a glyph, over small shifts. */
float name_glyph_fit(const NameInk &ink, const NameSegment &seg, const NameGlyph &g) {
    const int cols = seg.x1 - seg.x0 + 3;
    float best = -1.0f;
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = 0; dy <= 1; ++dy) {
            double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
            for (int y = 0; y < int(kNameH); ++y) {
                const int pr = y - dy;
                for (int i = 0; i < cols; ++i) {
                    const int x = seg.x0 - 1 + i;
                    const int tc = g.x0 + (x - seg.x0) - dx;
                    const double a = (x >= seg.x0 && x <= seg.x1) ? double(ink.cov[y][x]) : 0.0;
                    const double b = (pr >= 0 && pr < int(kGlyphBox) && tc >= 0 && tc < int(kGlyphBox))
                                         ? double(g.p[pr][tc]) : 0.0;
                    sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
                }
            }
            const double n = double(cols) * double(kNameH);
            const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
            if (va <= 0.0 || vb <= 0.0) continue;
            const float fit = float((sab - sa * sb / n) / std::sqrt(va * vb));
            if (fit > best) best = fit;
        }
    }
    return best;
}

/* Mean glyph fit when the strip's ink is exactly this name, else a negative value. */
float name_fit(const NameInk &ink, const std::string &name, const uint16_t *vram,
               unsigned font_x, unsigned font_y, std::vector<NameGlyph> &glyphs,
               std::vector<uint8_t> &built) {
    size_t seg_index = 0, count = 0;
    bool space = false;
    float sum = 0.0f;
    for (const char ch : name) {
        const unsigned code = uint8_t(ch);
        if (code == 0x20u) {
            space = count > 0;
            continue;
        }
        if (!built[code]) {
            build_name_glyph(vram, font_x, font_y, code, glyphs[code]);
            built[code] = 1;
        }
        const NameGlyph &g = glyphs[code];
        if (!g.valid || ++count > kMaxNameGlyphs) return -1.0f;
        if (seg_index + size_t(g.runs) > ink.segments.size()) return -1.0f;
        NameSegment seg = ink.segments[seg_index];
        for (int r = 1; r < g.runs; ++r) {
            const NameSegment &more = ink.segments[seg_index + size_t(r)];
            seg.x1 = more.x1;
            seg.top = std::min(seg.top, more.top);
            seg.bottom = std::max(seg.bottom, more.bottom);
        }
        /* A word gap is wider than the column between two glyphs. */
        if (seg_index > 0) {
            const int gap = seg.x0 - ink.segments[seg_index - 1].x1 - 1;
            if (space != (gap >= 3)) return -1.0f;
        }
        space = false;
        seg_index += size_t(g.runs);
        if (std::abs((seg.x1 - seg.x0 + 1) - g.w) > 1) return -1.0f;
        if (std::abs(seg.top - (g.top + 1)) > 1 || std::abs(seg.bottom - (g.bottom + 1)) > 1) return -1.0f;
        const float fit = name_glyph_fit(ink, seg, g);
        if (fit < kNameGlyphFit) return -1.0f;
        sum += fit;
    }
    if (!count || seg_index != ink.segments.size()) return -1.0f;
    return sum / float(count);
}

int match_name(const NameInk &ink, const uint16_t *vram, unsigned font_x, unsigned font_y,
               const std::vector<std::string> &names) {
    if (font_x + HdFont::kSheetWords > 1024u || font_y + HdFont::kFontSheetHeight > 512u) return -1;
    std::vector<NameGlyph> glyphs(256);
    std::vector<uint8_t> built(256, 0);
    int best = -1;
    float best_fit = -1.0f, next_fit = -1.0f;
    for (size_t i = 0; i < names.size(); ++i) {
        const float fit = name_fit(ink, names[i], vram, font_x, font_y, glyphs, built);
        if (fit < 0.0f) continue;
        if (best >= 0 && names[i] == names[size_t(best)]) continue;
        if (fit > best_fit) {
            next_fit = best_fit;
            best_fit = fit;
            best = int(i);
        } else if (fit > next_fit) {
            next_fit = fit;
        }
    }
    if (best < 0 || best_fit < kNameFit || best_fit - next_fit < kNameMargin) return -1;
    return best;
}

} // namespace

namespace {

/* Rasterizes laid-out glyphs into a bw x bh box of out at (dst_x0, dst_y0):
 * anti-aliased glyphs over an opaque dark outline and drop shadow, transparent
 * elsewhere. left is the box x of the leftmost ink (font unit min_ux). */
void draw_text_box(const stbtt_fontinfo &font, const std::vector<PlacedGlyph> &glyphs, float min_ux,
                   float left, float sx, float sy, int base_y, int bw, int bh,
                   int outline_r, int shadow_shift, const uint8_t fg[3], const uint8_t backdrop[3],
                   FontRgba &out, unsigned dst_x0, unsigned dst_y0,
                   std::vector<uint8_t> &box_fg, std::vector<uint8_t> &box_bg, std::vector<uint8_t> &bmp) {
    box_fg.assign(size_t(bw) * size_t(bh), 0);
    box_bg.assign(size_t(bw) * size_t(bh), 0);

    /* A one-texel transparent frame keeps bilinear taps of a neighbouring quad clean. */
    const int clip0 = 1, clip_x1 = bw - 1, clip_y1 = bh - 1;

    for (const auto &pg : glyphs) {
        const float gx = left + (pg.x - min_ux) * sx;
        const float gx_floor = std::floor(gx);
        const float shift_x = gx - gx_floor;
        int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&font, pg.glyph, sx, sy, shift_x, 0.0f, &gx0, &gy0, &gx1, &gy1);
        const int gw = gx1 - gx0, gh = gy1 - gy0;
        if (gw <= 0 || gh <= 0) continue;
        bmp.assign(size_t(gw) * size_t(gh), 0);
        stbtt_MakeGlyphBitmapSubpixel(&font, bmp.data(), gw, gh, gw, sx, sy, shift_x, 0.0f, pg.glyph);
        /* stbtt glyph boxes are y-up; bitmap boxes are y-down from the baseline. */
        const int ox = int(gx_floor) + gx0;
        const int oy = base_y + gy0;
        for (int py = 0; py < gh; ++py) {
            const int dy = oy + py;
            if (dy < clip0 || dy >= clip_y1) continue;
            for (int px = 0; px < gw; ++px) {
                const int dx = ox + px;
                if (dx < clip0 || dx >= clip_x1) continue;
                uint8_t &dst = box_fg[size_t(dy) * size_t(bw) + size_t(dx)];
                const uint8_t v = bmp[size_t(py) * size_t(gw) + size_t(px)];
                if (v > dst) dst = v;
            }
        }
    }

    /* Dark outline plus a connected down-right drop shadow behind the glyphs. */
    for (int py = clip0; py < clip_y1; ++py) {
        for (int px = clip0; px < clip_x1; ++px) {
            const uint8_t a = box_fg[size_t(py) * size_t(bw) + size_t(px)];
            if (a < 32) continue;
            for (int dy = -outline_r; dy <= outline_r; ++dy) {
                const int ny = py + dy;
                if (ny < clip0 || ny >= clip_y1) continue;
                for (int dx = -outline_r; dx <= outline_r; ++dx) {
                    const int nx = px + dx;
                    if (nx < clip0 || nx >= clip_x1) continue;
                    if (dx * dx + dy * dy <= outline_r * outline_r + 1) {
                        uint8_t &dst = box_bg[size_t(ny) * size_t(bw) + size_t(nx)];
                        if (a > dst) dst = a;
                    }
                }
            }
            for (int k = 1; k <= shadow_shift; ++k) {
                const int pts[3][2] = {{px + k, py + k}, {px + k - 1, py + k}, {px + k, py + k - 1}};
                for (const auto &pt : pts) {
                    if (pt[0] >= clip0 && pt[0] < clip_x1 && pt[1] >= clip0 && pt[1] < clip_y1) {
                        uint8_t &dst = box_bg[size_t(pt[1]) * size_t(bw) + size_t(pt[0])];
                        if (a > dst) dst = a;
                    }
                }
            }
        }
    }

    /* Anti-aliased glyph over an opaque backdrop: parallel-psx cuts alpha at 0.5,
     * so every kept pixel is opaque and never the (0, 0, 0, 255) sentinel. */
    for (int py = 0; py < bh; ++py) {
        for (int px = 0; px < bw; ++px) {
            const uint8_t a_fg = box_fg[size_t(py) * size_t(bw) + size_t(px)];
            const uint8_t a_bg = box_bg[size_t(py) * size_t(bw) + size_t(px)];
            if (std::max(a_fg, a_bg) < 110) continue;
            const float t = float(a_fg) / 255.0f;
            uint8_t rgba[4];
            for (int ch = 0; ch < 3; ++ch) {
                const float c = float(backdrop[ch]) * (1.0f - t) + float(fg[ch]) * t;
                rgba[ch] = uint8_t(std::clamp(int(std::lround(c)), 0, 255));
            }
            rgba[3] = 255;
            clamp_non_sentinel(rgba);
            std::memcpy(&out.pixels[(size_t(dst_y0 + unsigned(py)) * out.width + (dst_x0 + unsigned(px))) * 4u], rgba, 4);
        }
    }
}

} // namespace

bool HdFont::render_icon_sheet(unsigned local_w, unsigned local_h, const uint16_t clut[16],
                               unsigned scale, FontRgba &out,
                               const uint16_t *vram, unsigned vram_x, unsigned vram_y) const {
    if (mode_ == FontMode::Off || local_w != 256 || local_h != 256) return false;
    if (!ttf_data_ || !ttf_size_) return false;
    scale = scale >= 8 ? 8u : (scale >= 4 ? 4u : 2u);

    out.width = local_w * scale;
    out.height = local_h * scale;
    out.pixels.resize(size_t(out.width) * size_t(out.height) * 4u);
    /* Fill the entire sheet with parallel-psx's SD-fallthrough sentinel (0, 0, 0, 255)
     * so all graphical UI icons (cursor, borders, window gradient, elemental/status
     * icons, buttons) stay untouched in native VRAM. */
    for (size_t i = 0; i < out.pixels.size(); i += 4) {
        out.pixels[i + 0] = 0;
        out.pixels[i + 1] = 0;
        out.pixels[i + 2] = 0;
        out.pixels[i + 3] = 255;
    }
    render_icon_cells(0, kIconTextCount, 0, 0, clut, scale, out, vram, vram_x, vram_y);
    return true;
}

bool HdFont::render_icon_band(size_t band, const uint16_t clut[16], unsigned scale, FontRgba &out,
                              const uint16_t *vram, unsigned vram_x, unsigned vram_y) const {
    const auto &bands = icon_bands();
    if (mode_ == FontMode::Off || band >= bands.size()) return false;
    if (!ttf_data_ || !ttf_size_) return false;
    scale = scale >= 8 ? 8u : (scale >= 4 ? 4u : 2u);

    const IconBand &b = bands[band];
    out.width = b.w * scale;
    out.height = b.h * scale;
    out.pixels.resize(size_t(out.width) * size_t(out.height) * 4u);
    for (size_t i = 0; i < out.pixels.size(); i += 4) {
        out.pixels[i + 0] = 0;
        out.pixels[i + 1] = 0;
        out.pixels[i + 2] = 0;
        out.pixels[i + 3] = 255;
    }
    return render_icon_cells(b.first, b.last, b.x, b.y, clut, scale, out, vram, vram_x, vram_y) > 0;
}

unsigned HdFont::render_icon_cells(size_t first, size_t last, unsigned origin_x, unsigned origin_y,
                                   const uint16_t clut[16], unsigned scale, FontRgba &out,
                                   const uint16_t *vram, unsigned vram_x, unsigned vram_y) const {
    stbtt_fontinfo font{};
    if (!stbtt_InitFont(&font, ttf_data_, stbtt_GetFontOffsetForIndex(ttf_data_, 0))) return 0;

    const int s = int(scale);
    const bool has_vram = vram != nullptr && vram_x + kSheetWords <= 1024u && vram_y + kIconSheetHeight <= 512u;

    /* Ink of a cell as the player sees it: the sheet layers several images in its
     * 4-bit indices, and the active CLUT decides which of them is visible. */
    struct Ink {
        int x0, y0, x1, y1;
        unsigned used;
    };
    auto measure = [&](unsigned x, unsigned y, unsigned w, unsigned h, Ink &ink) -> bool {
        ink = {int(w), int(h), -1, -1, 0u};
        if (!has_vram) return false;
        for (unsigned dy = 0; dy < h; ++dy) {
            const uint16_t *row = vram + size_t(vram_y + y + dy) * 1024u + vram_x;
            for (unsigned dx = 0; dx < w; ++dx) {
                const unsigned lx = x + dx;
                const unsigned nib = (row[lx >> 2] >> ((lx & 3u) * 4u)) & 0xfu;
                if (nib == 0 || (clut && clut[nib] == 0)) continue;
                ink.used |= 1u << nib;
                if (int(dx) < ink.x0) ink.x0 = int(dx);
                if (int(dx) > ink.x1) ink.x1 = int(dx);
                if (int(dy) < ink.y0) ink.y0 = int(dy);
                if (int(dy) > ink.y1) ink.y1 = int(dy);
            }
        }
        return ink.x1 >= ink.x0;
    };

    auto glyph_top = [&](int cp) -> int {
        int g = stbtt_FindGlyphIndex(&font, cp);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!g || !stbtt_GetGlyphBox(&font, g, &x0, &y0, &x1, &y1)) return 0;
        return y1;
    };
    const int cap_top = std::max(1, glyph_top('H'));
    const int fig_top = glyph_top('7') > 0 ? glyph_top('7') : cap_top;
    const int asc_top = std::max({cap_top, glyph_top('l'), glyph_top('d')});

    uint8_t default_fg[3], default_sh[3];
    extract_clut_colors(clut, default_fg, default_sh);

    std::vector<PlacedGlyph> glyphs;
    std::vector<uint8_t> box_fg, box_bg, bmp;
    unsigned drawn = 0;

    for (size_t idx = first; idx < last && idx < kIconTextCount; ++idx) {
        const IconTextRect &item = kIconTexts[idx];
        if (item.x < origin_x || item.y < origin_y) continue;
        const int bw = int(item.w) * s;
        const int bh = int(item.h) * s;
        const unsigned dst_x0 = (item.x - origin_x) * scale;
        const unsigned dst_y0 = (item.y - origin_y) * scale;
        if (dst_x0 + unsigned(bw) > out.width || dst_y0 + unsigned(bh) > out.height) continue;

        Ink own;
        const bool own_ink = measure(item.x, item.y, item.w, item.h, own);
        /* Nothing of this cell shows under this palette: leave it native. */
        if (has_vram && !own_ink) continue;
        if (!own_ink) own = {0, 0, int(item.w) - 1, int(item.h) - 1, 0u};

        /* Vertical metrics come from the family's reference cell so that every digit
         * of a strip (and every label of a row) shares one size and one baseline.
         * Sliced labels (full_w > 0, e.g. DISC split across two stacked 16x8 cells)
         * share ref metrics and palette indices so both slices match seamlessly. */
        int row0 = own.y0, row1 = own.y1;
        bool self_rows = true;
        int slice_ref_x0 = own.x0;
        unsigned color_used = own.used;
        if (item.ref_w && !(item.ref_x == item.x && item.ref_y == item.y)) {
            Ink ref;
            if (measure(item.ref_x, item.ref_y, item.ref_w, item.ref_h, ref)) {
                row0 = ref.y0;
                row1 = ref.y1;
                if (item.full_w > 0) {
                    slice_ref_x0 = ref.x0;
                    color_used = ref.used ? ref.used : own.used;
                } else {
                    self_rows = false;
                }
            }
        }

        /* Foreground = brightest visible colour of the cell, backdrop from its darkest. */
        uint8_t fg[3] = {default_fg[0], default_fg[1], default_fg[2]};
        uint8_t sh[3] = {default_sh[0], default_sh[1], default_sh[2]};
        if (clut && color_used) {
            unsigned best_fg = 0, best_sh = 0xffffffffu;
            bool got = false;
            for (unsigned ci = 1; ci < 16; ++ci) {
                if (!(color_used & (1u << ci)) || clut[ci] == 0) continue;
                uint8_t rgb[3];
                unpack_abgr1555(clut[ci], rgb);
                const unsigned y = luma(rgb);
                if (!got || y > best_fg) { best_fg = y; std::memcpy(fg, rgb, 3); }
                if (!got || y < best_sh) { best_sh = y; std::memcpy(sh, rgb, 3); }
                got = true;
            }
        }
        uint8_t backdrop[3];
        for (int ch = 0; ch < 3; ++ch) {
            int v = std::min<int>(sh[ch], 28);
            if (v > int(fg[ch]) / 4) v = int(fg[ch]) / 6;
            backdrop[ch] = uint8_t(std::clamp(v, 14, 28));
        }

        int outline_r = 1, shadow_shift = 1;
        float min_ratio = 0.70f, max_ratio = 1.15f, tracking = 0.0f;
        switch (item.style) {
        case kBadge:
            outline_r = std::max(1, s / 3);
            shadow_shift = std::max(1, s / 2);
            max_ratio = 1.22f;
            tracking = 0.06f * float(cap_top);
            break;
        case kOutlined:
            outline_r = std::max(1, s / 2);
            shadow_shift = std::max(1, s / 4);
            break;
        default:
            outline_r = std::max(1, s / 4);
            shadow_shift = std::max(2, (s * 3) / 4);
            break;
        }
        const int pad_l = outline_r, pad_t = outline_r;
        const int pad_r = std::max(outline_r, shadow_shift), pad_b = pad_r;

        /* Lay the string out in font units to get its tight bounds. */
        glyphs.clear();
        float pen_u = 0.0f;
        float min_ux = 1e9f, max_ux = -1e9f;
        int min_uy = 1000000, max_uy = -1000000;
        int prev = 0;
        bool complete = true;
        for (const char *ch = item.text; *ch; ++ch) {
            int g = stbtt_FindGlyphIndex(&font, int(uint8_t(*ch)));
            if (!g) { complete = false; break; }
            if (prev) pen_u += float(stbtt_GetGlyphKernAdvance(&font, prev, g));
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&font, g, &adv, &lsb);
            int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
            if (stbtt_GetGlyphBox(&font, g, &gx0, &gy0, &gx1, &gy1)) {
                min_ux = std::min(min_ux, pen_u + float(gx0));
                max_ux = std::max(max_ux, pen_u + float(gx1));
                min_uy = std::min(min_uy, gy0);
                max_uy = std::max(max_uy, gy1);
            }
            glyphs.push_back({g, pen_u});
            pen_u += float(adv) + tracking;
            prev = g;
        }
        /* A custom font without one of the glyphs keeps the cell native rather
         * than showing half a word. */
        if (!complete || glyphs.empty() || max_ux <= min_ux || max_uy <= min_uy) continue;

        /* The whole quad becomes HD: transparent wherever no glyph lands. */
        for (int py = 0; py < bh; ++py) {
            std::memset(&out.pixels[(size_t(dst_y0 + unsigned(py)) * out.width + dst_x0) * 4u], 0, size_t(bw) * 4u);
        }
        ++drawn;

        /* Vertical fit: glyph tops on the first ink row, baseline above the shadow. */
        const float top_units = item.digits ? float(fig_top)
                                            : (item.style == kBadge ? float(cap_top) : float(asc_top));
        float fill_top = float(row0 * s + pad_t);
        float base = float((row1 + 1) * s - pad_b);
        if (base - fill_top < float(s)) {
            fill_top = float(1 + pad_t);
            base = float(bh - 1 - pad_b);
        }
        /* Round glyphs overshoot the baseline and cap line a little; that is not
         * an ascender or descender and must not change the size of one cell. */
        const float text_top = float(max_uy) > top_units * 1.05f ? float(max_uy) : top_units;
        const float text_bot = float(min_uy) < -0.06f * float(cap_top) ? float(min_uy) : 0.0f;
        float sy = 0.0f;
        if (self_rows) {
            /* The cell's own ink rows span its ascenders and descenders. */
            sy = (base - fill_top) / (text_top - text_bot);
            base += text_bot * sy;
        } else {
            sy = (base - fill_top) / top_units;
        }
        const float top_lim = float(1 + outline_r);
        const float bot_lim = float(bh - 1 - pad_b);
        if ((text_top - text_bot) * sy > bot_lim - top_lim) {
            sy = (bot_lim - top_lim) / (text_top - text_bot);
            base = bot_lim + text_bot * sy;
        } else {
            if (base - text_top * sy < top_lim) base = top_lim + text_top * sy;
            if (base - text_bot * sy > bot_lim) base = bot_lim + text_bot * sy;
        }
        /* Horizontal fit: stay close to the font's own proportions, centred on the
         * original ink so labels keep their place next to game-drawn values. */
        const int layout_w = item.full_w > 0 ? int(item.full_w) * s : bw;
        const float span_ux = max_ux - min_ux;
        const float ink_l = item.full_w > 0 ? float(slice_ref_x0 * s) : float(own.x0 * s);
        const float ink_r = item.full_w > 0 ? float(layout_w) : float((own.x1 + 1) * s);
        const float avail_w = std::max(2.0f, ink_r - ink_l - float(pad_l + pad_r));
        float sx = sy * std::clamp(avail_w / (span_ux * sy), min_ratio, max_ratio);
        const float hard_w = float(std::max(2, layout_w - 2 - pad_l - pad_r));
        if (span_ux * sx > hard_w) sx = hard_w / span_ux;
        if (sx < sy * 0.62f) sy = sx / 0.62f; /* keep very long words legible */
        const float rendered_w = span_ux * sx;
        const float centre = (ink_l + ink_r) * 0.5f - float(pad_r - pad_l) * 0.5f;
        const float left_min = float(1 + pad_l);
        const float left_max = std::max(left_min, float(layout_w - 1 - pad_r) - rendered_w);
        const float left = std::clamp(centre - rendered_w * 0.5f, left_min, left_max);
        const int base_y = int(std::lround(base));

        if (item.full_w > 0) {
            FontRgba full_img;
            full_img.width = unsigned(layout_w);
            full_img.height = unsigned(bh);
            full_img.pixels.assign(size_t(layout_w) * size_t(bh) * 4u, 0);
            draw_text_box(font, glyphs, min_ux, left, sx, sy, base_y, layout_w, bh, outline_r, shadow_shift,
                          fg, backdrop, full_img, 0, 0, box_fg, box_bg, bmp);
            const int sx0 = int(item.slice_x) * s;
            const int copy_w = std::min(bw, std::max(0, layout_w - sx0));
            for (int py = 0; py < bh; ++py) {
                if (copy_w > 0) {
                    const uint8_t *src = &full_img.pixels[(size_t(py) * size_t(layout_w) + size_t(sx0)) * 4u];
                    uint8_t *dst = &out.pixels[(size_t(dst_y0 + unsigned(py)) * out.width + dst_x0) * 4u];
                    std::memcpy(dst, src, size_t(copy_w) * 4u);
                }
            }
        } else {
            draw_text_box(font, glyphs, min_ux, left, sx, sy, base_y, bw, bh, outline_r, shadow_shift,
                          fg, backdrop, out, dst_x0, dst_y0, box_fg, box_bg, bmp);
        }
    }
    return drawn;
}

IconTextInfo HdFont::name_strip(size_t index) {
    IconTextInfo info;
    if (index >= kNameStripCount) return info;
    info.x = kNameX;
    info.y = kNameY + unsigned(index) * kNameH;
    info.w = kNameW;
    info.h = kNameH;
    return info;
}

int HdFont::name_strip_at(unsigned lx, unsigned ly, unsigned lw, unsigned lh) {
    if (lx >= kNameX + kNameW || lx + lw <= kNameX) return -1;
    int best = -1;
    unsigned best_rows = 0;
    for (unsigned i = 0; i < kNameStripCount; ++i) {
        const unsigned top = kNameY + i * kNameH;
        const unsigned y0 = std::max(ly, top), y1 = std::min(ly + lh, top + kNameH);
        if (y1 > y0 && y1 - y0 > best_rows) {
            best_rows = y1 - y0;
            best = int(i);
        }
    }
    return best;
}

int HdFont::match_name_strip(size_t strip, const uint16_t clut[16], const uint16_t *vram,
                             unsigned icon_x, unsigned icon_y, unsigned font_x, unsigned font_y,
                             const std::vector<std::string> &names) {
    if (names.empty()) return -1;
    NameInk ink;
    if (!read_name_ink(strip, clut, vram, icon_x, icon_y, ink)) return -1;
    return match_name(ink, vram, font_x, font_y, names);
}

bool HdFont::render_name_strip(size_t strip, const uint16_t clut[16], unsigned scale, FontRgba &out,
                               const uint16_t *vram, unsigned icon_x, unsigned icon_y,
                               unsigned font_x, unsigned font_y,
                               const std::vector<std::string> &names) const {
    if (mode_ == FontMode::Off || names.empty() || !ttf_data_ || !ttf_size_) return false;
    NameInk ink;
    if (!read_name_ink(strip, clut, vram, icon_x, icon_y, ink)) return false;
    const int match = match_name(ink, vram, font_x, font_y, names);
    if (match < 0) return false;

    stbtt_fontinfo font{};
    if (!stbtt_InitFont(&font, ttf_data_, stbtt_GetFontOffsetForIndex(ttf_data_, 0))) return false;
    int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
    const int cap_glyph = stbtt_FindGlyphIndex(&font, 'H');
    if (!cap_glyph || !stbtt_GetGlyphBox(&font, cap_glyph, &hx0, &hy0, &hx1, &hy1) || hy1 <= 0) return false;
    const float cap_units = float(hy1);

    /* Lay the name out in font units; a font without one of its glyphs keeps
     * the strip native rather than showing part of a name. */
    const size_t table_count = sizeof(kFf8Glyphs) / sizeof(kFf8Glyphs[0]);
    const float tracking = 0.04f * cap_units;
    std::vector<PlacedGlyph> glyphs;
    float pen_u = 0.0f, min_ux = 1e9f, max_ux = -1e9f;
    int prev = 0;
    for (const char ch : names[size_t(match)]) {
        const unsigned cell = unsigned(uint8_t(ch)) - 0x20u;
        const uint32_t cp = cell < table_count ? kFf8Glyphs[cell] : 0u;
        const int g = cp ? stbtt_FindGlyphIndex(&font, int(cp)) : 0;
        if (!g) return false;
        if (prev) pen_u += float(stbtt_GetGlyphKernAdvance(&font, prev, g));
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&font, g, &adv, &lsb);
        int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
        if (stbtt_GetGlyphBox(&font, g, &gx0, &gy0, &gx1, &gy1)) {
            min_ux = std::min(min_ux, pen_u + float(gx0));
            max_ux = std::max(max_ux, pen_u + float(gx1));
            glyphs.push_back({g, pen_u});
        }
        pen_u += float(adv) + tracking;
        prev = g;
    }
    if (glyphs.empty() || max_ux <= min_ux) return false;

    scale = scale >= 8 ? 8u : (scale >= 4 ? 4u : 2u);
    const int s = int(scale);
    const int bw = int(kNameW) * s, bh = int(kNameH) * s;
    out.width = unsigned(bw);
    out.height = unsigned(bh);
    out.pixels.assign(size_t(bw) * size_t(bh) * 4u, 0);

    /* Same outline weight as the HP digits drawn beside the name. Capitals take
     * the name face's rows (strip rows 1..9) and half a texel of its outline at
     * each end, as the digits do in their cells, unless this face's descenders
     * would then leave the rows the game shows. One size for every name. */
    const int outline_r = std::max(1, s / 2);
    const int shadow_shift = std::max(1, s / 4);
    float desc_units = 0.0f;
    for (int dcp : {'g', 'j', 'p', 'q', 'y'}) {
        int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
        const int dg = stbtt_FindGlyphIndex(&font, dcp);
        if (dg && stbtt_GetGlyphBox(&font, dg, &dx0, &dy0, &dx1, &dy1)) desc_units = std::max(desc_units, float(-dy0));
    }
    const float cap_top = float(outline_r);
    const float bottom = float(int(kNameShownRows) * s - std::max(outline_r, shadow_shift));
    float sy = std::min(float((kNameBaseRow - kNameCapRow + 1) * s + s) / cap_units,
                        (bottom - cap_top) / (cap_units + desc_units));
    float sx = sy * kNameStretch;
    const float base = cap_top + cap_units * sy;
    /* Right-aligned on the original ink, like the game's own layout: the HP
     * digits follow two texels further on. */
    const float right = std::min(float(bw - 1 - outline_r), float((ink.segments.back().x1 + 1) * s));
    const float room = right - float(1 + outline_r);
    const float span_ux = max_ux - min_ux;
    if (span_ux * sx > room) sx = room / span_ux;
    if (sx < sy * 0.62f) sy = sx / 0.62f; /* keep very long names legible */
    const float left = right - span_ux * sx;

    uint8_t backdrop[3];
    for (int ch = 0; ch < 3; ++ch) {
        int v = std::min<int>(ink.dark[ch], 28);
        if (v > int(ink.fg[ch]) / 4) v = int(ink.fg[ch]) / 6;
        backdrop[ch] = uint8_t(std::clamp(v, 14, 28));
    }
    std::vector<uint8_t> box_fg, box_bg, bmp;
    draw_text_box(font, glyphs, min_ux, left, sx, sy, int(std::lround(base)), bw, bh, outline_r, shadow_shift,
                  ink.fg, backdrop, out, 0, 0, box_fg, box_bg, bmp);
    return true;
}

} // namespace HostHD
