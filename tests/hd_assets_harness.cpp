#include "hd_font.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static uint16_t pack_abgr1555(unsigned r, unsigned g, unsigned b) {
    return uint16_t(((r >> 3) & 31u) | (((g >> 3) & 31u) << 5) | (((b >> 3) & 31u) << 10));
}

static void write_4bpp_pixel(uint16_t *vram, unsigned base_x, unsigned base_y, unsigned lx, unsigned ly, uint8_t nibble) {
    unsigned idx = (base_y + ly) * 1024u + base_x + (lx >> 2);
    unsigned shift = (lx & 3u) * 4u;
    vram[idx] = uint16_t((vram[idx] & ~(0xfu << shift)) | ((nibble & 0xfu) << shift));
}

int main() {
    std::vector<uint16_t> vram(1024u * 512u, 0);

    /* 1. Build a synthetic 256x128 4bpp FF8-like sysfnt sheet at (640, 256). */
    const unsigned bx = 640, by = 256;
    for (unsigned idx = 1; idx < 63; ++idx) {
        unsigned col = idx % 21;
        unsigned row = idx / 21;
        unsigned cx = col * 12;
        unsigned cy = row * 12;
        /* Draw a small 4x8 glyph inside each 12x12 cell, leaving cell 0 (space) and gutter 252..255 blank. */
        for (unsigned dy = 2; dy < 10; ++dy) {
            for (unsigned dx = 3; dx < 7; ++dx) {
                write_4bpp_pixel(vram.data(), bx, by, cx + dx, cy + dy, 7);
            }
        }
    }
    assert(HostHD::HdFont::is_font_upload(bx, by, 64, 128, vram.data()));
    /* Reject framebuffer location (x < 512) and wrong widths. */
    assert(!HostHD::HdFont::is_font_upload(0, 0, 64, 128, vram.data()));
    assert(!HostHD::HdFont::is_font_upload(bx, by, 32, 128, vram.data()));
    /* Corrupt gutter (columns 252..255): must reject non-font textures. */
    for (unsigned ly = 0; ly < 12; ++ly) write_4bpp_pixel(vram.data(), bx, by, 253, ly, 5);
    assert(!HostHD::HdFont::is_font_upload(bx, by, 64, 128, vram.data()));

    /* 2. Extract foreground + shadow colors from live ABGR1555 CLUTs. */
    uint16_t white_clut[16] = {0};
    white_clut[1] = pack_abgr1555(16, 16, 16);
    white_clut[7] = pack_abgr1555(248, 248, 248);
    uint8_t fg[3] = {0}, sh[3] = {0};
    HostHD::HdFont::extract_clut_colors(white_clut, fg, sh);
    assert(fg[0] >= 240 && fg[1] >= 240 && fg[2] >= 240);
    assert(sh[0] <= 24 && sh[1] <= 24 && sh[2] <= 24);

    uint16_t yellow_clut[16] = {0};
    yellow_clut[1] = pack_abgr1555(8, 8, 0);
    yellow_clut[7] = pack_abgr1555(248, 232, 32);

    /* 2b. FF8 sysfnt CLUTs fill unused indices 2..11 with bright yellow (255,255,0)
     *     while glyph pixels use index 1 (shadow) and 12..15 (peak at 15). Ensure
     *     blue, green, red, grey, and purple text keep their real colours. */
    {
        uint16_t blue_clut[16] = {0};
        blue_clut[1] = pack_abgr1555(40, 40, 40);
        for (unsigned i = 2; i <= 11; ++i) blue_clut[i] = pack_abgr1555(255, 255, 0);
        blue_clut[12] = pack_abgr1555(48, 64, 64);
        blue_clut[13] = pack_abgr1555(64, 120, 180);
        blue_clut[14] = pack_abgr1555(80, 140, 200);
        blue_clut[15] = pack_abgr1555(104, 180, 240);
        uint8_t bfg[3] = {0}, bsh[3] = {0};
        HostHD::HdFont::extract_clut_colors(blue_clut, bfg, bsh);
        assert(bfg[2] >= 220 && bfg[0] <= 120);
        assert(bsh[0] <= 48 && bsh[1] <= 48 && bsh[2] <= 48);

        uint16_t green_clut[16] = {0};
        green_clut[1] = pack_abgr1555(40, 40, 40);
        for (unsigned i = 2; i <= 11; ++i) green_clut[i] = pack_abgr1555(255, 255, 0);
        green_clut[12] = pack_abgr1555(32, 56, 32);
        green_clut[13] = pack_abgr1555(16, 140, 16);
        green_clut[14] = pack_abgr1555(16, 180, 16);
        green_clut[15] = pack_abgr1555(0, 240, 0);
        uint8_t gfg[3] = {0}, gsh[3] = {0};
        HostHD::HdFont::extract_clut_colors(green_clut, gfg, gsh);
        assert(gfg[1] >= 220 && gfg[0] <= 24 && gfg[2] <= 24);
    }

    /* 3. Rasterize the built-in OFL Barlow font at 4x (1024x512). */
    HostHD::HdFont font;
    font.configure(HostHD::FontMode::Builtin, "");
    assert(font.active());

    HostHD::FontRgba white_atlas, yellow_atlas;
    assert(font.render_atlas(256, 128, white_clut, 4, white_atlas));
    assert(white_atlas.width == 1024 && white_atlas.height == 512);
    assert(white_atlas.pixels.size() == 1024u * 512u * 4u);

    auto inspect_cell = [&](const HostHD::FontRgba &atlas, unsigned cell_idx,
                            size_t &fg_px, size_t &bg_px, size_t &sentinel_px) {
        fg_px = bg_px = sentinel_px = 0;
        const unsigned cell = 48;
        const unsigned cx = (cell_idx % 21) * cell;
        const unsigned cy = (cell_idx / 21) * cell;
        for (unsigned py = 0; py < cell; ++py) {
            for (unsigned px = 0; px < cell; ++px) {
                const size_t off = (size_t(cy + py) * atlas.width + (cx + px)) * 4u;
                uint8_t r = atlas.pixels[off], g = atlas.pixels[off + 1],
                        b = atlas.pixels[off + 2], a = atlas.pixels[off + 3];
                if (a == 255 && r == 0 && g == 0 && b == 0) {
                    ++sentinel_px;
                } else if (a == 255 && r <= 32 && g <= 32 && b <= 32) {
                    ++bg_px;
                } else if (a == 255 && r >= 200) {
                    ++fg_px;
                }
            }
        }
    };

    /* Mapped Latin glyphs + apostrophe (0x43=cell 35) + VIII (0xA8=136) + brackets
     * (0xA9=137, 0xAA=138, 0xAE=142, 0xAF=143) + geometric corner brackets & UI symbols
     * (0x36=22, 0x37=23, 0xAB=139, 0xAD=141, 0xB0=144, 0xB2=146, 0xB3=147, 0xB4=148,
     *  0xB6=150, 0xBB=155) + multiply/dash (0xB8=152, 0xBF=159)
     * must render crisp foreground + dark backdrop with zero SD-fallthrough sentinels. */
    const unsigned checked_cells[] = {
        1, 22, 23, 35, 37, 63, 136, 137, 138, 139, 141, 142, 143, 144, 146, 147, 148, 150, 152, 155, 159
    };
    for (unsigned idx : checked_cells) {
        size_t fg_px = 0, bg_px = 0, sent_px = 0;
        inspect_cell(white_atlas, idx, fg_px, bg_px, sent_px);
        assert(sent_px == 0);
        assert(fg_px > 20);
        assert(bg_px > 20);
    }

    /* Unmapped icon/music/star symbol cells (0xAC=140 ◎, 0xB1=145 ★, 0xC3=163 ♪) must emit SD-fallthrough sentinel. */
    for (unsigned idx : {140u, 145u, 163u}) {
        size_t fg_px = 0, bg_px = 0, sent_px = 0;
        inspect_cell(white_atlas, idx, fg_px, bg_px, sent_px);
        assert(sent_px == 48u * 48u);
    }

    /* Live VRAM-guided alignment produces valid atlas too. */
    HostHD::FontRgba vram_guided;
    assert(font.render_atlas(256, 128, white_clut, 4, vram_guided, vram.data(), bx, by));
    assert(vram_guided.width == 1024 && vram_guided.height == 512);

    /* Different CLUTs must produce distinct tinted atlases (yellow vs white) of the
     * same glyphs: only the colours change. */
    assert(font.render_atlas(256, 128, yellow_clut, 4, yellow_atlas));
    assert(white_atlas.pixels != yellow_atlas.pixels);
    assert(white_atlas.pixels.size() == yellow_atlas.pixels.size());
    for (size_t i = 3; i < white_atlas.pixels.size(); i += 4) assert(white_atlas.pixels[i] == yellow_atlas.pixels[i]);

    /* 3b. Slot fitting. The game moves the pen by an advance of its own after each
     *     glyph: the glyph's stroke width in the sheet plus two texels. A sheet of
     *     made-up boxes with FF8-like proportions (a nearly fixed-width lowercase,
     *     capitals nine rows tall) must give glyphs that fill their slots, never
     *     leave them, and keep one stroke weight however much they were widened
     *     or narrowed. */
    {
        std::vector<uint16_t> fit(1024u * 512u, 0);
        const unsigned fx = 832, fy = 128, scale = 8, cell = 12 * scale;
        auto cell_of = [](char ch) -> unsigned {
            if (ch >= 'A' && ch <= 'Z') return 37u + unsigned(ch - 'A');
            if (ch >= 'a' && ch <= 'z') return 63u + unsigned(ch - 'a');
            return 1u + unsigned(ch - '0');
        };
        auto stroke_width = [&](unsigned idx) -> unsigned {
            if (idx >= 1 && idx <= 10) return idx == cell_of('1') ? 3u : 6u;
            if (idx == cell_of('I')) return 3;
            if (idx == cell_of('M') || idx == cell_of('T') || idx == cell_of('W')) return 7;
            if (idx >= 37 && idx <= 62) return 6;
            if (idx == cell_of('i') || idx == cell_of('l')) return 1;
            if (idx == cell_of('f') || idx == cell_of('j')) return 3;
            if (idx == cell_of('r') || idx == cell_of('t')) return 4;
            if (idx == cell_of('m')) return 7;
            return 5;
        };
        for (unsigned idx = 1; idx < 172; ++idx) {
            const unsigned cx = (idx % 21) * 12, cy = (idx / 21) * 12;
            const bool lower = idx >= 63 && idx <= 88;
            const unsigned x0 = idx == cell_of('1') ? 1u : 0u, w = stroke_width(idx);
            const unsigned top = idx <= 10 ? 1u : (lower ? 3u : 0u);
            for (unsigned dy = top; dy <= 8; ++dy)
                for (unsigned dx = x0; dx < x0 + w; ++dx) {
                    write_4bpp_pixel(fit.data(), fx, fy, cx + dx, cy + dy, 15);
                    /* Drop shadow: one texel down and right, palette index 1. */
                    if (dy == 8 || dx + 1 == x0 + w) write_4bpp_pixel(fit.data(), fx, fy, cx + dx + 1, cy + dy + 1, 1);
                }
        }
        assert(HostHD::HdFont::is_font_sheet_at(fx, fy, fit.data()));

        HostHD::FontRgba fitted, natural;
        assert(font.render_atlas(256, 120, white_clut, scale, fitted, fit.data(), fx, fy));
        assert(font.render_atlas(256, 120, white_clut, scale, natural));
        assert(fitted.width == 256 * scale && fitted.height == 120 * scale);

        struct Ink {
            int x0 = 1 << 20, x1 = -1, y0 = 1 << 20, y1 = -1;
        };
        auto bright = [&](const HostHD::FontRgba &atlas, unsigned idx, unsigned px, unsigned py) {
            const uint8_t *p = &atlas.pixels[(size_t((idx / 21) * cell + py) * atlas.width + (idx % 21) * cell + px) * 4u];
            return p[3] == 255 && p[0] >= 200;
        };
        auto ink_of = [&](const HostHD::FontRgba &atlas, char ch) {
            Ink ink;
            for (unsigned py = 0; py < cell; ++py)
                for (unsigned px = 0; px < cell; ++px) {
                    if (!bright(atlas, cell_of(ch), px, py)) continue;
                    ink.x0 = std::min(ink.x0, int(px)); ink.x1 = std::max(ink.x1, int(px));
                    ink.y0 = std::min(ink.y0, int(py)); ink.y1 = std::max(ink.y1, int(py));
                }
            assert(ink.x1 >= 0);
            return ink;
        };
        /* Width of the leftmost run of glyph pixels on a row: a vertical stem. */
        auto stem_at = [&](const HostHD::FontRgba &atlas, char ch, unsigned py) {
            unsigned px = 0, run = 0;
            while (px < cell && !bright(atlas, cell_of(ch), px, py)) ++px;
            while (px < cell && bright(atlas, cell_of(ch), px, py)) { ++px; ++run; }
            return int(run);
        };

        /* Every glyph stays inside its slot, off the cell's first column (its rim
         * needs it), and its shadow inside the cell. */
        for (const char *p = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"; *p; ++p) {
            const Ink ink = ink_of(fitted, *p);
            const unsigned idx = cell_of(*p);
            const int slot = int((idx <= 10 ? 6u : stroke_width(idx)) + 2u) * int(scale);
            if (ink.x0 < 1 || ink.x1 >= slot || ink.y0 < 1 || ink.y1 >= int(cell) - 1) {
                std::printf("glyph '%c': ink x %d..%d y %d..%d, slot %d\n", *p, ink.x0, ink.x1, ink.y0, ink.y1, slot);
                assert(false);
            }
        }
        /* Capitals are nearly as tall as the game's nine rows (0..8 here): they
         * start in the first one and stand on its baseline. */
        {
            const Ink h = ink_of(fitted, 'H');
            assert(h.y0 < int(scale) && h.y1 >= int(9 * scale - scale / 2) && h.y1 < int(9 * scale));
            assert((h.y1 - h.y0 + 1) * 4 >= int(33 * scale));
        }
        /* Narrow glyphs fill their wide slots, wide glyphs fit their narrow ones... */
        {
            const Ink t_fit = ink_of(fitted, 't'), t_nat = ink_of(natural, 't');
            const Ink w_fit = ink_of(fitted, 'w'), w_nat = ink_of(natural, 'w');
            const Ink n_fit = ink_of(fitted, 'n');
            assert((t_fit.x1 - t_fit.x0) * 10 > (t_nat.x1 - t_nat.x0) * 11);
            assert(w_nat.x1 - w_nat.x0 >= 7 * int(scale));
            assert((w_fit.x1 - w_fit.x0) * 10 < (w_nat.x1 - w_nat.x0) * 9);
            /* ...so a glyph's ink sits in the middle of its slot, not at one end. */
            for (const Ink &ink : {t_fit, w_fit, n_fit}) {
                const int slot = (&ink == &t_fit ? 6 : 7) * int(scale);
                assert(std::abs((ink.x0 + ink.x1 + 1) - slot) <= int(scale));
            }
        }
        /* ...and all of them keep the stem of a glyph that was not resized. */
        {
            const unsigned row = 6 * scale; /* mid x-height: stems only */
            const int stem = stem_at(fitted, 'l', row);
            assert(stem >= int(scale) && stem <= 2 * int(scale));
            for (const char *p = "hilmnrtu"; *p; ++p) {
                const int other = stem_at(fitted, *p, row);
                if (std::abs(other - stem) > 1) {
                    std::printf("stem of '%c' is %d px, 'l' is %d px\n", *p, other, stem);
                    assert(false);
                }
            }
            /* The capital I carries serifs like the game's: wider at its ends than its stem. */
            const Ink cap_i = ink_of(fitted, 'I');
            assert(stem_at(fitted, 'I', row) <= stem + 1);
            assert(cap_i.x1 - cap_i.x0 + 1 >= 2 * stem);
            assert(stem_at(fitted, 'I', unsigned(cap_i.y0) + 1) >= 2 * stem);
            assert(stem_at(fitted, 'I', unsigned(cap_i.y1) - 1) >= 2 * stem);
        }
        /* Digits are tabular: '1' is centred in the slot the other digits fill. */
        {
            const Ink one = ink_of(fitted, '1'), zero = ink_of(fitted, '0');
            assert(std::abs((one.x0 + one.x1) - (zero.x0 + zero.x1)) <= int(scale));
        }
    }

    /* 4. icon.tex text table: every cell is one of the game's sprite quads. */
    auto find_cell = [](unsigned x, unsigned y) -> HostHD::IconTextInfo {
        for (size_t i = 0; i < HostHD::HdFont::icon_text_count(); ++i) {
            HostHD::IconTextInfo c = HostHD::HdFont::icon_text(i);
            if (c.x == x && c.y == y) return c;
        }
        return {};
    };
    auto expect_cell = [&](unsigned x, unsigned y, unsigned w, unsigned h, const char *text) {
        HostHD::IconTextInfo c = find_cell(x, y);
        if (!c.text || c.w != w || c.h != h || std::strcmp(c.text, text) != 0) {
            std::printf("icon cell (%u,%u) expected %ux%u \"%s\", got %ux%u \"%s\"\n",
                        x, y, w, h, text, c.w, c.h, c.text ? c.text : "(none)");
            assert(false);
        }
    };
    /* Menu digits and the symbols the game draws with them: '/' sits at x=240 and
     * ':' at x=248 (HP cur/max and play time), '%' at x=232. */
    for (unsigned d = 0; d < 10; ++d) {
        const char digit[2] = {char('0' + d), 0};
        expect_cell(168 + d * 8, 16, 8, 8, digit);
        expect_cell(168 + d * 8, 24, 8, 8, digit);
        expect_cell(168 + d * 8, 32, 8, 16, digit);
        expect_cell(176 + d * 8, 64, 8, 16, digit);
        expect_cell(d * 16, 80, 16, 16, digit);
    }
    expect_cell(232, 152, 8, 8, "%");
    expect_cell(240, 152, 8, 8, "/");
    expect_cell(248, 152, 8, 8, ":");
    expect_cell(168, 64, 8, 16, "/");
    expect_cell(160, 80, 8, 16, ":");
    expect_cell(216, 152, 8, 8, "/");
    expect_cell(192, 144, 8, 8, "5");
    expect_cell(208, 152, 8, 8, "9");
    /* Status bar labels use the game's quads, so PLAY cannot reach into SeeD. */
    expect_cell(48, 192, 32, 8, "Fast");
    expect_cell(80, 192, 32, 8, "Slow");
    expect_cell(112, 192, 32, 8, "PLAY");
    expect_cell(144, 192, 24, 8, "SeeD");
    expect_cell(176, 192, 16, 8, "HP");
    expect_cell(192, 192, 16, 8, "Lv");
    expect_cell(208, 192, 32, 8, "TIME");
    expect_cell(48, 200, 16, 8, "DISC");
    expect_cell(48, 208, 16, 8, "DISC");
    expect_cell(0, 176, 48, 16, "DOUBLE!");
    expect_cell(0, 192, 48, 8, "TRIPLE!!");
    /* Header badges span the whole 8-row quad, including the last ink column. */
    expect_cell(144, 8, 32, 8, "STATUS");
    expect_cell(176, 8, 32, 8, "HELP");
    expect_cell(136, 0, 48, 8, "COMMAND");
    expect_cell(96, 120, 40, 8, "SPECIAL");
    expect_cell(128, 32, 40, 16, "Miss");
    expect_cell(24, 24, 8, 8, "P");
    expect_cell(32, 24, 8, 8, "G");
    expect_cell(128, 240, 16, 16, "Luck");

    /* Cells sit on VRAM word and mip boundaries, inside the sheet, and never overlap. */
    std::vector<uint8_t> cell_map(256u * 256u, 0);
    for (size_t i = 0; i < HostHD::HdFont::icon_text_count(); ++i) {
        HostHD::IconTextInfo c = HostHD::HdFont::icon_text(i);
        assert(c.text && c.text[0]);
        assert(c.w && c.h && c.x + c.w <= 256 && c.y + c.h <= 256);
        assert(c.x % 8 == 0 && c.y % 8 == 0 && c.w % 8 == 0 && c.h % 8 == 0);
        for (unsigned y = c.y; y < c.y + c.h; ++y)
            for (unsigned x = c.x; x < c.x + c.w; ++x) {
                assert(cell_map[y * 256u + x] == 0);
                cell_map[y * 256u + x] = 1;
            }
    }
    /* Bands tile exactly the text cells. */
    std::vector<uint8_t> band_map(256u * 256u, 0);
    for (size_t i = 0; i < HostHD::HdFont::icon_band_count(); ++i) {
        HostHD::IconTextInfo b = HostHD::HdFont::icon_band(i);
        assert(b.w && b.h && b.x + b.w <= 256 && b.y + b.h <= 256);
        for (unsigned y = b.y; y < b.y + b.h; ++y)
            for (unsigned x = b.x; x < b.x + b.w; ++x) {
                assert(band_map[y * 256u + x] == 0);
                band_map[y * 256u + x] = 1;
            }
    }
    assert(band_map == cell_map);
    /* A primitive finds its band even when the renderer reports one extra row, and
     * graphics (window gradient, cursor) belong to no band. */
    const int digit_band = HostHD::HdFont::icon_band_at(168, 24, 8, 9);
    assert(digit_band >= 0);
    {
        HostHD::IconTextInfo b = HostHD::HdFont::icon_band(size_t(digit_band));
        assert(b.y == 24 && b.h == 8 && b.x <= 168 && b.x + b.w >= 248);
    }
    assert(HostHD::HdFont::icon_band_at(112, 192, 32, 8) == HostHD::HdFont::icon_band_at(144, 192, 24, 8));
    assert(HostHD::HdFont::icon_band_at(0, 48, 256, 16) < 0);
    assert(HostHD::HdFont::icon_band_at(208, 0, 48, 16) < 0);

    /* 5. Build a synthetic 256x256 4bpp FF8 icon.tex sheet at (896, 256) and verify
     *    detection + HD numeric/label rendering + SD fallthrough on graphics regions. */
    const unsigned ix = 896, iy = 256;
    for (unsigned ly = 48; ly < 64; ++ly)
        for (unsigned lx = 0; lx < 256; ++lx)
            write_4bpp_pixel(vram.data(), ix, iy, lx, ly, 4);
    for (unsigned d = 0; d < 10; ++d) {
        for (unsigned dy = 1; dy < 7; ++dy)
            for (unsigned dx = 2; dx < 6; ++dx)
                write_4bpp_pixel(vram.data(), ix, iy, 168u + d * 8u + dx, 24u + dy, 7);
        for (unsigned dy = 2; dy < 12; ++dy)
            for (unsigned dx = 3; dx < 12; ++dx)
                write_4bpp_pixel(vram.data(), ix, iy, d * 16u + dx, 80u + dy, 7);
    }
    /* Labels whose ink reaches the last column of their quad (the stray pixels a
     * too-narrow replacement rect used to leave behind), plus '/' and ':'. */
    auto ink_block = [&](unsigned x, unsigned y, unsigned w, unsigned h, uint8_t nibble) {
        for (unsigned dy = 0; dy < h; ++dy)
            for (unsigned dx = 0; dx < w; ++dx)
                write_4bpp_pixel(vram.data(), ix, iy, x + dx, y + dy, nibble);
    };
    ink_block(145, 8, 30, 6, 7);  /* STATUS, ink up to x=174 */
    ink_block(176, 8, 24, 6, 7);  /* HELP, ink up to x=199 */
    ink_block(112, 192, 27, 8, 7); /* PLAY */
    ink_block(145, 192, 23, 8, 7); /* SeeD */
    ink_block(48, 200, 16, 8, 7);  /* DISC first half */
    ink_block(48, 208, 11, 8, 7);  /* DISC second half */
    ink_block(243, 153, 4, 6, 7);  /* '/' */
    ink_block(250, 153, 3, 5, 7);  /* ':' */
    /* A layer the active palette hides (CLUT entry 0) must not count as ink. */
    ink_block(80, 8, 30, 6, 9);   /* ITEM cell, invisible under white_clut */
    assert(white_clut[9] == 0);

    assert(HostHD::HdFont::is_icon_font_upload(ix, iy, 64, 256, vram.data()));
    HostHD::FontRgba icon_atlas;
    assert(font.render_icon_sheet(256, 256, white_clut, 4, icon_atlas, vram.data(), ix, iy));
    assert(icon_atlas.width == 1024 && icon_atlas.height == 1024);

    struct CellStats {
        size_t sentinel = 0, fg = 0, bg = 0, transparent = 0, border_opaque = 0;
    };
    auto cell_stats = [&](const HostHD::FontRgba &atlas, unsigned scale, unsigned x, unsigned y,
                          unsigned w, unsigned h, unsigned ox, unsigned oy) {
        CellStats st;
        for (unsigned py = 0; py < h * scale; ++py) {
            for (unsigned px = 0; px < w * scale; ++px) {
                const size_t off = (size_t((y - oy) * scale + py) * atlas.width + ((x - ox) * scale + px)) * 4u;
                const uint8_t r = atlas.pixels[off], g = atlas.pixels[off + 1],
                              b = atlas.pixels[off + 2], a = atlas.pixels[off + 3];
                const bool border = px == 0 || py == 0 || px == w * scale - 1 || py == h * scale - 1;
                if (a == 255 && r == 0 && g == 0 && b == 0) ++st.sentinel;
                else if (a == 0) { assert(r == 0 && g == 0 && b == 0); ++st.transparent; }
                else {
                    assert(a == 255);
                    if (border) ++st.border_opaque;
                    if (r >= 200) ++st.fg; else if (r <= 32) ++st.bg;
                }
            }
        }
        return st;
    };
    /* Window gradient strip (y=48..63) must remain SD-fallthrough sentinel (0,0,0,255). */
    {
        CellStats st = cell_stats(icon_atlas, 4, 0, 48, 256, 16, 0, 0);
        assert(st.sentinel == 256u * 16u * 16u);
    }
    /* Visible cells are replaced whole: no native pixel survives anywhere in the quad,
     * glyphs have foreground and backdrop, and a transparent one-texel frame remains. */
    const unsigned visible_cells[][4] = {
        {168, 24, 8, 8}, {240, 24, 8, 8}, {0, 80, 16, 16}, {144, 80, 16, 16},
        {144, 8, 32, 8}, {176, 8, 32, 8}, {112, 192, 32, 8}, {144, 192, 24, 8},
        {240, 152, 8, 8}, {248, 152, 8, 8},
    };
    for (const auto &c : visible_cells) {
        CellStats st = cell_stats(icon_atlas, 4, c[0], c[1], c[2], c[3], 0, 0);
        if (st.sentinel != 0 || st.fg <= 8 || st.bg <= 8 || st.transparent == 0 || st.border_opaque != 0) {
            std::printf("icon cell (%u,%u): sentinel=%zu fg=%zu bg=%zu transparent=%zu border=%zu\n",
                        c[0], c[1], st.sentinel, st.fg, st.bg, st.transparent, st.border_opaque);
            assert(false);
        }
    }
    /* Sliced DISC cells (48,200) + (48,208) replace both halves seamlessly across the seam. */
    {
        CellStats s0 = cell_stats(icon_atlas, 4, 48, 200, 16, 8, 0, 0);
        CellStats s1 = cell_stats(icon_atlas, 4, 48, 208, 16, 8, 0, 0);
        assert(s0.sentinel == 0 && s0.fg > 8 && s0.bg > 8 && s0.transparent > 0);
        assert(s1.sentinel == 0 && s1.fg > 8 && s1.bg > 8 && s1.transparent > 0);
    }
    /* Cells with nothing visible under this palette stay native. */
    {
        CellStats hidden = cell_stats(icon_atlas, 4, 80, 8, 32, 8, 0, 0);     /* ITEM: hidden layer only */
        CellStats empty = cell_stats(icon_atlas, 4, 40, 0, 24, 8, 0, 0);      /* NAME: no ink at all */
        assert(hidden.sentinel == 32u * 8u * 16u);
        assert(empty.sentinel == 24u * 8u * 16u);
    }
    /* Every pixel outside the text cells is the SD-fallthrough sentinel. */
    for (unsigned y = 0; y < 256; ++y) {
        for (unsigned x = 0; x < 256; ++x) {
            if (cell_map[y * 256u + x]) continue;
            const size_t off = (size_t(y * 4u) * 1024u + x * 4u) * 4u;
            assert(icon_atlas.pixels[off] == 0 && icon_atlas.pixels[off + 1] == 0 &&
                   icon_atlas.pixels[off + 2] == 0 && icon_atlas.pixels[off + 3] == 255);
        }
    }
    /* All digits of a strip share one size and baseline: same top and bottom glyph rows. */
    {
        int first_top = -1, first_bottom = -1;
        for (unsigned d = 0; d < 10; ++d) {
            int top = -1, bottom = -1;
            for (unsigned py = 0; py < 32; ++py) {
                for (unsigned px = 0; px < 32; ++px) {
                    const size_t off = (size_t(24 * 4 + py) * 1024u + ((168 + d * 8) * 4 + px)) * 4u;
                    if (icon_atlas.pixels[off + 3] == 255 && icon_atlas.pixels[off] >= 200) {
                        if (top < 0) top = int(py);
                        bottom = int(py);
                    }
                }
            }
            assert(top >= 0);
            if (d == 0) { first_top = top; first_bottom = bottom; }
            assert(std::abs(top - first_top) <= 1 && std::abs(bottom - first_bottom) <= 1);
        }
    }
    /* A band texture is the same picture as its region of the full sheet, at 2x, 4x and 8x. */
    for (unsigned scale : {2u, 4u, 8u}) {
        HostHD::FontRgba sheet;
        assert(font.render_icon_sheet(256, 256, white_clut, scale, sheet, vram.data(), ix, iy));
        assert(sheet.width == 256 * scale && sheet.height == 256 * scale);
        HostHD::IconTextInfo b = HostHD::HdFont::icon_band(size_t(digit_band));
        HostHD::FontRgba band;
        assert(font.render_icon_band(size_t(digit_band), white_clut, scale, band, vram.data(), ix, iy));
        assert(band.width == b.w * scale && band.height == b.h * scale);
        for (unsigned py = 0; py < band.height; ++py) {
            const uint8_t *row = &sheet.pixels[(size_t(b.y * scale + py) * sheet.width + b.x * scale) * 4u];
            assert(std::memcmp(row, &band.pixels[size_t(py) * band.width * 4u], size_t(band.width) * 4u) == 0);
        }
        CellStats st = cell_stats(band, scale, 168, 24, 8, 8, b.x, b.y);
        assert(st.sentinel == 0 && st.fg > 4 && st.border_opaque == 0);
    }
    /* A band with nothing visible under the palette reports that instead of a texture. */
    {
        HostHD::FontRgba none;
        const int big_band = HostHD::HdFont::icon_band_at(0, 240, 16, 16);
        assert(big_band >= 0);
        assert(!font.render_icon_band(size_t(big_band), white_clut, 4, none, vram.data(), ix, iy));
    }

    /* 6. Finding the sheets in VRAM itself. The host reports dirty VRAM, so a font
     *    upload can arrive merged with its neighbours, split, or as all of VRAM
     *    (battle loads, state loads); the sheets must be found by structure. */
    std::vector<uint16_t> scan(1024u * 512u, 0);
    const unsigned sx = 704, sy = 301;
    auto strict_font = [&](unsigned ox, unsigned oy) {
        for (unsigned idx = 1; idx < 172; ++idx) {
            unsigned cx = (idx % 21) * 12, cy = (idx / 21) * 12;
            /* Eight-pixel glyph plus its drop-shadow row: cell rows 1..9. */
            for (unsigned dy = 1; dy < 10; ++dy)
                for (unsigned dx = 0; dx < 6; ++dx)
                    write_4bpp_pixel(scan.data(), ox, oy, cx + dx, cy + dy, dy == 9 ? 1 : 15);
        }
    };
    strict_font(sx, sy);
    for (unsigned y = 0; y < 256; ++y)
        std::memcpy(&scan[(256u + y) * 1024u + 896u], &vram[(256u + y) * 1024u + 896u], 64u * sizeof(uint16_t));
    /* An unrelated texture right beside the font (same rows) must not confuse the search. */
    for (unsigned y = sy; y < sy + 120; ++y)
        for (unsigned x = sx + 64; x < sx + 128; ++x) scan[y * 1024u + x] = 0x5a5a;

    assert(HostHD::HdFont::is_font_sheet_at(sx, sy, scan.data()));
    assert(!HostHD::HdFont::is_font_sheet_at(sx, sy - 1, scan.data()));
    assert(!HostHD::HdFont::is_font_sheet_at(sx, sy + 1, scan.data()));
    assert(!HostHD::HdFont::is_font_sheet_at(sx, sy + 12, scan.data()));
    assert(!HostHD::HdFont::is_font_sheet_at(sx + 64, sy, scan.data()));

    auto expect_sheets = [&](unsigned x, unsigned y, unsigned w, unsigned h, bool want_font, bool want_icon) {
        std::vector<HostHD::SheetHit> hits;
        HostHD::HdFont::find_sheets(scan.data(), x, y, w, h, hits);
        bool got_font = false, got_icon = false;
        for (const auto &hit : hits) {
            if (hit.icon) {
                assert(!got_icon && hit.x == 896 && hit.y == 256 && hit.w == 64 && hit.h == 256);
                got_icon = true;
            } else {
                assert(!got_font && hit.x == sx && hit.y == sy && hit.w == 64 &&
                       hit.h == HostHD::HdFont::kFontSheetHeight);
                got_font = true;
            }
        }
        assert(got_font == want_font && got_icon == want_icon);
    };
    expect_sheets(0, 0, 1024, 512, true, true);        /* all of VRAM (state load) */
    expect_sheets(sx, sy, 128, 120, true, false);      /* merged with the neighbour texture */
    expect_sheets(sx - 64, sy - 40, 256, 60, true, false); /* only the top rows were dirty */
    expect_sheets(sx + 20, sy + 110, 2, 2, true, false);   /* a small write inside the sheet */
    expect_sheets(sx, sy + 120, 64, 16, false, false); /* rows just below the sheet */
    expect_sheets(sx + 64, sy, 64, 120, false, false); /* the neighbour alone */
    expect_sheets(960, 300, 64, 8, false, false);
    expect_sheets(900, 300, 4, 4, false, true);        /* a write inside the icon sheet */
    /* The found sheet renders with the same geometry as an exact upload. */
    HostHD::FontRgba found_atlas;
    assert(font.render_atlas(256, HostHD::HdFont::kFontSheetHeight, white_clut, 8, found_atlas, scan.data(), sx, sy));
    assert(found_atlas.width == 2048 && found_atlas.height == 960);
    /* Overwriting the digit row removes the sheet. */
    for (unsigned y = sy; y < sy + 12; ++y)
        for (unsigned x = sx; x < sx + 64; ++x) scan[y * 1024u + x] = 0x1111;
    expect_sheets(0, 0, 1024, 512, false, true);

    /* 7. Battle party names. The game renders each name on the CPU into a strip of
     *    the icon sheet, in a bold face that is in no texture: a strip is replaced
     *    only when exactly one candidate name accounts for its ink, glyph by glyph,
     *    against the sysfnt sheet's glyphs. Everything here is synthetic. */
    for (unsigned i = 0; i < HostHD::HdFont::kNameStripCount; ++i) {
        HostHD::IconTextInfo strip = HostHD::HdFont::name_strip(i);
        assert(strip.x == 0 && strip.y == 96 + 16 * i && strip.w == 96 && strip.h == 16);
        /* A strip is a sprite of its own: it shares no texel with a text cell. */
        for (unsigned y = strip.y; y < strip.y + strip.h; ++y)
            for (unsigned x = strip.x; x < strip.x + strip.w; ++x) assert(cell_map[y * 256u + x] == 0);
        assert(HostHD::HdFont::name_strip_at(strip.x, strip.y, strip.w, strip.h + 1) == int(i));
    }
    assert(HostHD::HdFont::name_strip_at(96, 112, 40, 16) < 0);
    assert(HostHD::HdFont::name_strip_at(0, 80, 96, 16) < 0);
    assert(HostHD::HdFont::name_strip_at(0, 144, 96, 16) < 0);
    assert(HostHD::HdFont::icon_band_at(0, 112, 96, 16) < 0);

    std::vector<uint16_t> nv(1024u * 512u, 0);
    const unsigned nfx = 704, nfy = 64, nix = 896, niy = 256;
    /* A font of distinct made-up glyphs: a stem, a top bar and a per-cell pattern,
     * capital or x-height, some with a descender, each with its drop shadow. */
    auto glyph_px = [](unsigned idx, unsigned r, unsigned c) -> bool {
        const unsigned w = 3 + (idx * 7u) % 4u;
        const unsigned top = idx % 3u == 0 ? 0u : 3u, bottom = idx % 5u == 0 ? 10u : 8u;
        if (c >= w || r < top || r > bottom) return false;
        if (c == 0 || r == top) return true;
        return (((idx * 2654435761u) ^ (r * 40503u) ^ (c * 9973u)) >> 7 & 3u) == 0;
    };
    auto glyph_width = [](unsigned idx) { return 3 + (idx * 7u) % 4u; };
    for (unsigned idx = 1; idx < 172; ++idx) {
        const unsigned cx = (idx % 21) * 12, cy = (idx / 21) * 12;
        for (unsigned r = 0; r < 11; ++r)
            for (unsigned c = 0; c < 11; ++c) {
                if (glyph_px(idx, r, c)) write_4bpp_pixel(nv.data(), nfx, nfy, cx + c, cy + r, 15);
                else if (r && c && glyph_px(idx, r - 1, c - 1)) write_4bpp_pixel(nv.data(), nfx, nfy, cx + c, cy + r, 1);
            }
    }
    /* Game text encoding: glyph cell + 0x20. */
    auto encode = [](const char *text) {
        std::string out;
        for (; *text; ++text) {
            if (*text >= 'A' && *text <= 'Z') out.push_back(char(0x45 + (*text - 'A')));
            else if (*text >= 'a' && *text <= 'z') out.push_back(char(0x5f + (*text - 'a')));
            else out.push_back(char(0x20));
        }
        return out;
    };
    /* The name face: every glyph one row lower and one column wider than in sysfnt,
     * anti-aliased over a dark outline, one column apart, ending at the right edge. */
    auto draw_name = [&](unsigned strip, const std::string &name) {
        const unsigned oy = 96 + 16 * strip;
        for (unsigned y = 0; y < 16; ++y)
            for (unsigned x = 0; x < 96; ++x) write_4bpp_pixel(nv.data(), nix, niy, x, oy + y, 0);
        unsigned width = 0;
        for (char ch : name) width += ch == 0x20 ? 4 : glyph_width(uint8_t(ch) - 0x20u) + 2;
        std::vector<uint8_t> level(16u * 96u, 0);
        unsigned x = 95 - width;
        for (char ch : name) {
            if (ch == 0x20) { x += 4; continue; }
            const unsigned idx = uint8_t(ch) - 0x20u;
            for (unsigned r = 0; r < 11; ++r)
                for (unsigned c = 0; c <= glyph_width(idx); ++c) {
                    const bool a = glyph_px(idx, r, c), b = c > 0 && glyph_px(idx, r, c - 1);
                    if (a || b) level[(r + 1) * 96u + x + c] = uint8_t(a && b ? 2 : (b ? 4 : 7));
                }
            x += glyph_width(idx) + 2;
        }
        for (unsigned y = 0; y < 16; ++y)
            for (unsigned px = 0; px < 96; ++px) {
                uint8_t nibble = level[y * 96u + px];
                if (!nibble) {
                    for (int dy = -1; dy <= 1 && !nibble; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int ny = int(y) + dy, nx = int(px) + dx;
                            if (ny >= 0 && ny < 16 && nx >= 0 && nx < 96 && level[unsigned(ny) * 96u + unsigned(nx)]) nibble = 13;
                        }
                }
                write_4bpp_pixel(nv.data(), nix, niy, px, oy + y, nibble);
            }
    };
    /* Grey ramp palette: index 2 is white, 13 the outline. */
    uint16_t name_clut[16] = {0};
    for (unsigned i = 2; i < 16; ++i) name_clut[i] = pack_abgr1555(248 - (i - 2) * 16, 248 - (i - 2) * 16, 248 - (i - 2) * 16);
    name_clut[1] = pack_abgr1555(120, 120, 120);

    const std::vector<std::string> party = {encode("Squall"), encode("Rinoa"), encode("Zell"), encode("Irvine"),
                                            encode("Quistis"), encode("Selphie"), encode("Kiros"), encode("Ward")};
    draw_name(0, encode("Zell"));
    draw_name(1, encode("Squall"));
    draw_name(2, encode("Quistis"));
    auto match = [&](unsigned strip, const std::vector<std::string> &names) {
        return HostHD::HdFont::match_name_strip(strip, name_clut, nv.data(), nix, niy, nfx, nfy, names);
    };
    assert(match(0, party) == 2);
    assert(match(1, party) == 0);
    assert(match(2, party) == 4);
    /* Names of the same length are told apart by their glyphs. */
    draw_name(0, encode("Kiros"));
    assert(match(0, party) == 6);
    draw_name(0, encode("Rinoa"));
    assert(match(0, party) == 1);
    /* A renamed character: any name the host lists, spaces included. */
    {
        std::vector<std::string> renamed = party;
        renamed[0] = encode("Leon Hart");
        draw_name(0, renamed[0]);
        assert(match(0, renamed) == 0);
        assert(match(0, party) < 0);
        /* The same name listed twice is still one name. */
        renamed.push_back(renamed[0]);
        assert(match(0, renamed) == 0);
    }
    /* A name that is not listed, an empty strip and an empty list match nothing;
     * neither does a strip whose ink only partly is a listed name. */
    draw_name(0, encode("Laguna"));
    assert(match(0, party) < 0);
    draw_name(0, encode("Zella"));
    assert(match(0, party) < 0);
    draw_name(0, encode("Zel"));
    assert(match(0, party) < 0);
    draw_name(0, "");
    assert(match(0, party) < 0);
    assert(match(1, {}) < 0);
    assert(HostHD::HdFont::match_name_strip(3, name_clut, nv.data(), nix, niy, nfx, nfy, party) < 0);
    /* Bytes that are no glyph of the sheet never match. */
    assert(match(1, {std::string("\x05\x06\x07", 3), std::string(1, char(0xff))}) < 0);
    /* Under a palette that shows no ink on the outline the strip stays native. */
    {
        uint16_t flat_clut[16] = {0};
        for (unsigned i = 1; i < 16; ++i) flat_clut[i] = pack_abgr1555(96, 96, 96);
        assert(HostHD::HdFont::match_name_strip(1, flat_clut, nv.data(), nix, niy, nfx, nfy, party) < 0);
    }
    /* Two different names that fit equally well are not guessed between: cells
     * 'O' and 'Q' get the same glyph, so "Oa" and "Qa" look alike. */
    {
        std::vector<uint16_t> twin = nv;
        const unsigned o_cell = 0x45 + ('O' - 'A') - 0x20, q_cell = 0x45 + ('Q' - 'A') - 0x20;
        for (unsigned r = 0; r < 12; ++r)
            for (unsigned c = 0; c < 12; ++c) {
                const unsigned from_x = (o_cell % 21) * 12 + c, from_y = (o_cell / 21) * 12 + r;
                const uint16_t word = twin[(nfy + from_y) * 1024u + nfx + (from_x >> 2)];
                write_4bpp_pixel(twin.data(), nfx, nfy, (q_cell % 21) * 12 + c, (q_cell / 21) * 12 + r,
                                 uint8_t((word >> ((from_x & 3u) * 4u)) & 0xfu));
            }
        std::swap(twin, nv);
        draw_name(0, encode("Oa"));
        std::swap(twin, nv);
        const std::vector<std::string> both = {encode("Oa"), encode("Qa")};
        assert(HostHD::HdFont::match_name_strip(0, name_clut, twin.data(), nix, niy, nfx, nfy, both) < 0);
        assert(HostHD::HdFont::match_name_strip(0, name_clut, twin.data(), nix, niy, nfx, nfy, {encode("Oa")}) == 0);
    }

    /* The matched name is drawn in the HD font: right-aligned on the original ink,
     * inside the 13 rows the game shows, transparent elsewhere, never a sentinel. */
    uint16_t yellow_name_clut[16] = {0};
    for (unsigned i = 2; i < 16; ++i) yellow_name_clut[i] = pack_abgr1555(248 - (i - 2) * 16, 232 - (i - 2) * 15, 32);
    for (unsigned scale : {2u, 4u, 8u}) {
        for (unsigned strip : {1u, 2u}) {
            HostHD::FontRgba name;
            assert(font.render_name_strip(strip, name_clut, scale, name, nv.data(), nix, niy, nfx, nfy, party));
            assert(name.width == 96 * scale && name.height == 16 * scale);
            unsigned left = name.width, right = 0, bottom = 0;
            size_t fg_px = 0, bg_px = 0;
            for (unsigned y = 0; y < name.height; ++y)
                for (unsigned x = 0; x < name.width; ++x) {
                    const uint8_t *px = &name.pixels[(size_t(y) * name.width + x) * 4u];
                    if (px[3] == 0) { assert(px[0] == 0 && px[1] == 0 && px[2] == 0); continue; }
                    assert(px[3] == 255 && !(px[0] == 0 && px[1] == 0 && px[2] == 0));
                    assert(x > 0 && y > 0 && x + 1 < name.width && y + 1 < name.height);
                    if (px[0] >= 200) ++fg_px; else if (px[0] <= 32) ++bg_px;
                    left = std::min(left, x); right = std::max(right, x); bottom = std::max(bottom, y);
                }
            assert(fg_px > 40 && bg_px > 40);
            assert(right >= 94 * scale && bottom < 13 * scale);
            /* The longer name starts further left; both leave the strip's left free. */
            assert(left > 16 * scale);
            if (strip == 2) {
                HostHD::FontRgba shorter;
                assert(font.render_name_strip(1, name_clut, scale, shorter, nv.data(), nix, niy, nfx, nfy, party));
                unsigned shorter_left = shorter.width;
                for (unsigned y = 0; y < shorter.height; ++y)
                    for (unsigned x = 0; x < shorter_left; ++x)
                        if (shorter.pixels[(size_t(y) * shorter.width + x) * 4u + 3]) shorter_left = x;
                assert(left < shorter_left);
            }
        }
    }
    {
        HostHD::FontRgba white_name, yellow_name, none;
        assert(font.render_name_strip(1, name_clut, 4, white_name, nv.data(), nix, niy, nfx, nfy, party));
        assert(font.render_name_strip(1, yellow_name_clut, 4, yellow_name, nv.data(), nix, niy, nfx, nfy, party));
        bool yellow_seen = false;
        for (size_t i = 0; i < yellow_name.pixels.size(); i += 4)
            if (yellow_name.pixels[i + 3] && yellow_name.pixels[i] >= 200 && yellow_name.pixels[i + 2] <= 80) yellow_seen = true;
        assert(yellow_seen && white_name.pixels != yellow_name.pixels);
        /* No match, no texture: the strip stays as the game drew it. */
        assert(!font.render_name_strip(0, name_clut, 4, none, nv.data(), nix, niy, nfx, nfy, party));
        assert(!font.render_name_strip(1, name_clut, 4, none, nv.data(), nix, niy, nfx, nfy, {}));
    }

    /* Off mode disables rendering. */
    font.configure(HostHD::FontMode::Off, "");
    assert(!font.active());
    HostHD::FontRgba off_atlas;
    assert(!font.render_atlas(256, 128, white_clut, 4, off_atlas));
    assert(!font.render_name_strip(1, name_clut, 4, off_atlas, nv.data(), nix, niy, nfx, nfy, party));

    std::puts("ok");
    return 0;
}
