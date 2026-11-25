#ifndef FF8_HOST_HD_FONT_HPP
#define FF8_HOST_HD_FONT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HostHD {

enum class FontMode {
    Off = 0,
    Builtin = 1,
    Custom = 2,
};

struct FontRgba {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<uint8_t> pixels;
};

/* One HD-replaced text cell of FF8's menu/battle icon sheet (local texels). */
struct IconTextInfo {
    unsigned x = 0, y = 0, w = 0, h = 0;
    const char *text = nullptr;
};

/* A font sheet located in VRAM (VRAM words x rows), independent of how the
 * host happened to batch the uploads that produced it. */
struct SheetHit {
    unsigned x = 0, y = 0, w = 0, h = 0;
    bool icon = false;
};

/* A rendered sysfnt atlas before it is coloured: the glyphs are the same under
 * every palette, so they are rasterized once per sheet and scale. */
struct FontCoverage {
    uint64_t key = 0;
    unsigned width = 0, height = 0;
    std::vector<uint8_t> glyph;       /* anti-aliased glyph coverage */
    std::vector<uint8_t> backdrop;    /* 1 = glyph, rim or drop shadow */
    std::vector<uint8_t> native_cell; /* 1 = the cell keeps its original texels */
};

/* Clean-room HD font rasterizer for FF8's 4bpp 21-column 12x12 glyph atlas.
 * Uses the vendored OFL Barlow font (or a user-supplied TTF/OTF
 * or RGBA PNG atlas) and colors glyphs from the live PS1 ABGR1555 CLUT. */
class HdFont {
public:
    HdFont() = default;

    void configure(FontMode mode, const std::string &custom_path);
    FontMode mode() const { return mode_; }
    const std::string &custom_path() const { return custom_path_; }
    uint64_t generation() const { return generation_; }
    bool active() const { return mode_ != FontMode::Off; }

    /* FF8's sysfnt sheet: 256 texels (64 VRAM words) wide, 10 glyph rows of 12px. */
    static constexpr unsigned kSheetWords = 64;
    static constexpr unsigned kFontSheetHeight = 120;
    static constexpr unsigned kIconSheetHeight = 256;

    /* Clean-room structural check for a 4bpp 256px-wide FF8 sysfnt upload. */
    static bool is_font_upload(unsigned vram_x, unsigned vram_y, unsigned vram_w, unsigned vram_h,
                               const uint16_t *vram);

    /* Clean-room structural check for FF8's 4bpp 256x256 menu icon/numeric font sheet (icon.tex). */
    static bool is_icon_font_upload(unsigned vram_x, unsigned vram_y, unsigned vram_w, unsigned vram_h,
                                    const uint16_t *vram);

    /* Strict structural check for the sysfnt sheet at an exact VRAM origin. Unlike
     * is_font_upload() it pins the origin to the pixel (the digit cells 1..10 must
     * span cell rows 1..9), so it can be used to search VRAM for the sheet when the
     * host merges or splits the uploads that carried it. */
    static bool is_font_sheet_at(unsigned vram_x, unsigned vram_y, const uint16_t *vram);

    /* Appends every sysfnt / icon sheet whose VRAM rect intersects the given dirty
     * rect. Sheets are searched on 64-word texture-page columns at any row. */
    static void find_sheets(const uint16_t *vram, unsigned x, unsigned y, unsigned w, unsigned h,
                            std::vector<SheetHit> &out);

    /* The icon-sheet text cells replaced by render_icon_sheet() (for tests/lookups). */
    static size_t icon_text_count();
    static IconTextInfo icon_text(size_t index);
    /* Text cells are grouped into bands: maximal runs of horizontally adjacent cells
     * (a digit strip, a row of labels). Each band is replaced by its own small HD
     * texture, so palettes that only ever draw graphics cost nothing. */
    static size_t icon_band_count();
    static IconTextInfo icon_band(size_t index);
    /* Band with the largest overlap with a local texel rect of the icon sheet, or -1. */
    static int icon_band_at(unsigned lx, unsigned ly, unsigned lw, unsigned lh);

    /* Battle party names. The game composes each name on the CPU, in a bold
     * anti-aliased face that is in no texture, into its own 96x16 strip of the
     * icon sheet and draws the strip as one sprite. The strip alone does not say
     * which glyphs it holds, so it is identified against the names the game can
     * show (given in its own text encoding, one byte per glyph): a name is taken
     * only when the strip's ink splits into exactly its glyphs, each with the box
     * and roughly the shape of the same glyph in the live sysfnt sheet, and no
     * other name fits as well. Anything else stays native. */
    static constexpr unsigned kNameStripCount = 3;
    static IconTextInfo name_strip(size_t index);
    /* Strip with the largest overlap with a local texel rect of the icon sheet, or -1. */
    static int name_strip_at(unsigned lx, unsigned ly, unsigned lw, unsigned lh);
    /* Index into names of the name drawn in a strip, or -1. icon_* and font_* are
     * the VRAM origins of the icon sheet and of the sysfnt sheet. */
    static int match_name_strip(size_t strip, const uint16_t clut[16], const uint16_t *vram,
                                unsigned icon_x, unsigned icon_y, unsigned font_x, unsigned font_y,
                                const std::vector<std::string> &names);
    /* Renders a strip's name at scale 2, 4 or 8, right-aligned on the original
     * ink like the game's. Returns false when the strip matches no name. */
    bool render_name_strip(size_t strip, const uint16_t clut[16], unsigned scale, FontRgba &out,
                           const uint16_t *vram, unsigned icon_x, unsigned icon_y,
                           unsigned font_x, unsigned font_y,
                           const std::vector<std::string> &names) const;

    /* Extracts foreground and shadow RGB888 colors from a 16-entry ABGR1555 CLUT. */
    static void extract_clut_colors(const uint16_t clut[16], uint8_t fg[3], uint8_t shadow[3]);

    /* Renders a scale-multiplied RGBA8 atlas (scale 2, 4 or 8; e.g. 1024x480 for a
     * 256x120 sheet at 4) matching FF8's 21xN grid of 12x12 cells.
     * When vram is provided, the sheet itself gives the metrics the game lays
     * text out with: the rows its capitals span, and each glyph's advance (its
     * stroke width plus two texels). Every HD glyph is fitted to that slot, with
     * its stroke weight kept, so text is spaced as evenly as the game's own.
     * Without vram, glyphs keep the face's own advances. */
    bool render_atlas(unsigned local_w, unsigned local_h, const uint16_t clut[16],
                      unsigned scale, FontRgba &out,
                      const uint16_t *vram = nullptr,
                      unsigned vram_x = 0, unsigned vram_y = 0) const;

    /* Renders HD Barlow numeric fonts (0-9, /, :, %, P, G) and UI text labels
     * (Lv, HP, PLAY, HELP, STATUS, COMMAND, Str..Luck, Miss, etc.) into FF8's 256x256
     * icon.tex sheet while emitting (0, 0, 0, 255) SD-fallthrough for all
     * non-text graphics (cursor, borders, elemental icons, controller buttons).
     * Every replaced cell is one of the game's own sprite quads, so no original
     * pixel of a replaced label can survive next to its HD version. */
    bool render_icon_sheet(unsigned local_w, unsigned local_h, const uint16_t clut[16],
                           unsigned scale, FontRgba &out,
                           const uint16_t *vram = nullptr,
                           unsigned vram_x = 0, unsigned vram_y = 0) const;

    /* Renders one band of the icon sheet (see icon_band()) at scale 2, 4 or 8.
     * Returns false when nothing in the band is visible under this CLUT. */
    bool render_icon_band(size_t band, const uint16_t clut[16], unsigned scale, FontRgba &out,
                          const uint16_t *vram = nullptr,
                          unsigned vram_x = 0, unsigned vram_y = 0) const;

private:
    /* Draws text cells [first, last) into an image whose top-left is the local
     * texel (origin_x, origin_y). Returns the number of cells drawn. */
    unsigned render_icon_cells(size_t first, size_t last, unsigned origin_x, unsigned origin_y,
                               const uint16_t clut[16], unsigned scale, FontRgba &out,
                               const uint16_t *vram, unsigned vram_x, unsigned vram_y) const;
    /* Rasterizes the sysfnt atlas into coverage_, unless it already holds it. */
    bool build_coverage(unsigned local_w, unsigned local_h, unsigned scale,
                        const uint16_t *vram, unsigned vram_x, unsigned vram_y) const;
    bool load_ttf_bytes(const uint8_t *data, size_t size);
    bool load_custom_png(const std::string &path);

    FontMode mode_ = FontMode::Off;
    std::string custom_path_;
    uint64_t generation_ = 0;
    std::vector<uint8_t> ttf_storage_;
    const uint8_t *ttf_data_ = nullptr;
    size_t ttf_size_ = 0;
    FontRgba custom_png_;
    bool has_custom_png_ = false;
    /* Only the renderer thread draws, so the cache needs no lock. */
    mutable FontCoverage coverage_;
};

} // namespace HostHD

#endif
