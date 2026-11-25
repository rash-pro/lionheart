#include "dds_decode.hpp"
#include "ff8_mod_import.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

void push_u16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(uint8_t(v & 0xffu));
    b.push_back(uint8_t((v >> 8) & 0xffu));
}

void push_u32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(uint8_t(v & 0xffu));
    b.push_back(uint8_t((v >> 8) & 0xffu));
    b.push_back(uint8_t((v >> 16) & 0xffu));
    b.push_back(uint8_t((v >> 24) & 0xffu));
}

void push_u64(std::vector<uint8_t> &b, uint64_t v) {
    push_u32(b, uint32_t(v & 0xffffffffu));
    push_u32(b, uint32_t((v >> 32) & 0xffffffffu));
}

std::vector<uint8_t> make_dds_header(uint32_t w, uint32_t h, uint32_t pf_flags,
                                     const char *fourcc, uint32_t rgb_bits,
                                     uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask,
                                     uint32_t dxgi_format = 0) {
    std::vector<uint8_t> d;
    d.insert(d.end(), {'D', 'D', 'S', ' '});
    push_u32(d, 124);         /* dwSize */
    push_u32(d, 0x1007u);     /* dwFlags */
    push_u32(d, h);
    push_u32(d, w);
    push_u32(d, 0);           /* pitch */
    push_u32(d, 0);           /* depth */
    push_u32(d, 1);           /* mipmaps */
    for (int i = 0; i < 11; ++i) push_u32(d, 0);
    /* DDS_PIXELFORMAT (32 bytes at offset 76) */
    push_u32(d, 32);
    push_u32(d, pf_flags);
    if (fourcc) d.insert(d.end(), fourcc, fourcc + 4);
    else push_u32(d, 0);
    push_u32(d, rgb_bits);
    push_u32(d, rmask);
    push_u32(d, gmask);
    push_u32(d, bmask);
    push_u32(d, amask);
    /* caps */
    push_u32(d, 0x1000u);
    for (int i = 0; i < 4; ++i) push_u32(d, 0);
    check(d.size() == 128, "DDS header size must be 128");
    if (fourcc && std::memcmp(fourcc, "DX10", 4) == 0) {
        push_u32(d, dxgi_format);
        push_u32(d, 3); /* D3D10_RESOURCE_DIMENSION_TEXTURE2D */
        push_u32(d, 0);
        push_u32(d, 1);
        push_u32(d, 0);
        check(d.size() == 148, "DDS DX10 header size must be 148");
    }
    return d;
}

struct BitWriter128 {
    uint64_t lo = 0, hi = 0;
    unsigned pos = 0;
    void write(uint32_t val, unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            uint64_t bit = uint64_t((val >> i) & 1u);
            if (pos < 64) lo |= (bit << pos);
            else hi |= (bit << (pos - 64));
            ++pos;
        }
    }
};

void test_dds_decoders() {
    /* 1. BC1 (DXT1): c0 = pure red (0xF800), c1 = pure blue (0x001F), c0 > c1 */
    {
        auto dds = make_dds_header(4, 4, 0x04, "DXT1", 0, 0, 0, 0, 0);
        push_u16(dds, 0xF800u);
        push_u16(dds, 0x001Fu);
        /* pixel 0 = idx 0 (red), pixel 1 = idx 1 (blue), pixel 2 = idx 2 (2/3 red + 1/3 blue) */
        uint32_t bits = 0u | (1u << 2) | (2u << 4) | (3u << 6);
        push_u32(dds, bits);
        HostHD::DdsImage img;
        std::string err;
        check(HostHD::decode_dds(dds.data(), dds.size(), img, &err), "BC1 decode");
        check(img.width == 4 && img.height == 4 && img.rgba.size() == 64, "BC1 dims");
        check(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0 && img.rgba[3] == 255, "BC1 px0");
        check(img.rgba[4] == 0 && img.rgba[5] == 0 && img.rgba[6] == 255 && img.rgba[7] == 255, "BC1 px1");
        check(img.rgba[8] == 170 && img.rgba[10] == 85 && img.rgba[11] == 255, "BC1 px2");
    }

    /* 2. BC3 (DXT5): alpha0 = 200, alpha1 = 100, px0 = idx 0 (200), px1 = idx 1 (100) */
    {
        auto dds = make_dds_header(4, 4, 0x04, "DXT5", 0, 0, 0, 0, 0);
        dds.push_back(200);
        dds.push_back(100);
        uint64_t abits = 0u | (1u << 3);
        for (int i = 0; i < 6; ++i) dds.push_back(uint8_t((abits >> (8 * i)) & 0xffu));
        push_u16(dds, 0x07E0u); /* green */
        push_u16(dds, 0x07E0u);
        push_u32(dds, 0u);
        HostHD::DdsImage img;
        check(HostHD::decode_dds(dds.data(), dds.size(), img), "BC3 decode");
        check(img.rgba[1] == 255 && img.rgba[3] == 200, "BC3 px0");
        check(img.rgba[5] == 255 && img.rgba[7] == 100, "BC3 px1");
    }

    /* 3. DX10 BC7 Mode 6 (1 subset, 7-bit RGB+A, unique P-bits, 4-bit indices):
     *    ep0 = (R=0x10, G=0x20, B=0x30, A=0x40) with p0=0 -> (0x20, 0x40, 0x60, 0x80)
     *    ep1 = (R=0x7F, G=0x7F, B=0x7F, A=0x7F) with p1=1 -> (0xFF, 0xFF, 0xFF, 0xFF) */
    {
        auto dds = make_dds_header(4, 4, 0x04, "DX10", 0, 0, 0, 0, 0, 98);
        BitWriter128 bw;
        bw.write(1u << 6, 7); /* Mode 6: six 0s then one 1 */
        bw.write(0x10, 7); bw.write(0x7F, 7); /* R0, R1 */
        bw.write(0x20, 7); bw.write(0x7F, 7); /* G0, G1 */
        bw.write(0x30, 7); bw.write(0x7F, 7); /* B0, B1 */
        bw.write(0x40, 7); bw.write(0x7F, 7); /* A0, A1 */
        bw.write(0, 1); bw.write(1, 1);       /* p0=0, p1=1 */
        bw.write(0, 3);                       /* px0 anchor index (3 bits) = 0 -> ep0 */
        bw.write(15, 4);                      /* px1 index (4 bits) = 15 -> ep1 */
        for (int i = 2; i < 16; ++i) bw.write(0, 4);
        check(bw.pos == 128, "BC7 Mode 6 bit count == 128");
        push_u64(dds, bw.lo);
        push_u64(dds, bw.hi);

        HostHD::DdsImage img;
        std::string err;
        check(HostHD::decode_dds(dds.data(), dds.size(), img, &err), "BC7 Mode 6 decode");
        check(img.rgba[0] == 0x20 && img.rgba[1] == 0x40 && img.rgba[2] == 0x60 && img.rgba[3] == 0x80, "BC7 Mode 6 px0");
        check(img.rgba[4] == 0xFF && img.rgba[5] == 0xFF && img.rgba[6] == 0xFF && img.rgba[7] == 0xFF, "BC7 Mode 6 px1");
    }

    /* 4. Uncompressed 32-bit BGRA */
    {
        auto dds = make_dds_header(2, 1, 0x41, nullptr, 32, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u);
        /* BGRA pixels: (B=10, G=20, R=30, A=40), (B=50, G=60, R=70, A=80) */
        dds.insert(dds.end(), {10, 20, 30, 40, 50, 60, 70, 80});
        HostHD::DdsImage img;
        check(HostHD::decode_dds(dds.data(), dds.size(), img), "BGRA32 decode");
        check(img.rgba[0] == 30 && img.rgba[1] == 20 && img.rgba[2] == 10 && img.rgba[3] == 40, "BGRA32 px0");
        check(img.rgba[4] == 70 && img.rgba[5] == 60 && img.rgba[6] == 50 && img.rgba[7] == 80, "BGRA32 px1");
    }
}

void test_mod_xml_evaluator() {
    const std::string xml = R"XML(
<ModInfo>
  <ModFolder Folder="No Change" ActiveWhen="POLY_ID = 0" />
  <ModFolder Folder="FIELD_MODELS\HIGH_POLY" ActiveWhen="POLY_ID = 1" />
  <ModFolder Folder="FIELD_MODELS\LOW_POLY" ActiveWhen="POLY_ID = 2" />
  <ModFolder Folder="Horizonpack" ActiveWhen="Horizonpack_ID = 1" />
  <ModFolder Folder="UI_Theme\Active" >
    <ActiveWhen>
      <And>
        <Option>UI_ID = 1</Option>
        <Option>THEME_ID != 0</Option>
      </And>
    </ActiveWhen>
  </ModFolder>
  <ModFolder Folder="UI_Theme\Inactive" >
    <ActiveWhen>
      <And>
        <Option>UI_ID = 1</Option>
        <Option>THEME_ID = 0</Option>
      </And>
    </ActiveWhen>
  </ModFolder>
  <ConfigOption><ID>POLY_ID</ID><Default>1</Default></ConfigOption>
  <ConfigOption><ID>horizonpack_ID</ID><Default>1</Default></ConfigOption>
  <ConfigOption><ID>UI_ID</ID><Default>1</Default></ConfigOption>
  <ConfigOption><ID>THEME_ID</ID><Default>5</Default></ConfigOption>
</ModInfo>
)XML";

    auto res = HostHD::parse_and_eval_mod_xml(xml);
    check(res.has_folder_rules, "has_folder_rules");
    auto has_active = [&](const std::string &f) {
        for (const auto &a : res.active_folders) if (a == f) return true;
        return false;
    };
    check(has_active("field_models/high_poly"), "HIGH_POLY must be active");
    check(!has_active("field_models/low_poly"), "LOW_POLY must be inactive");
    check(has_active("horizonpack"), "case-insensitive Horizonpack_ID must be active");
    check(has_active("ui_theme/active"), "XML child ActiveWhen active");
    check(!has_active("ui_theme/inactive"), "XML child ActiveWhen inactive");

    check(HostHD::evaluate_active_when_expr("(A = 1 AND B != 2) OR C = 3", {{"a", 1}, {"b", 3}, {"c", 0}}), "compound true");
    check(!HostHD::evaluate_active_when_expr("(A = 1 AND B != 2) OR C = 3", {{"a", 1}, {"b", 2}, {"c", 0}}), "compound false");
}

void test_iros_parser(const std::filesystem::path &temp_dir) {
    std::filesystem::create_directories(temp_dir);
    auto archive = temp_dir / "synthetic.iroj";

    struct Rec { std::string name; std::string payload; };
    std::vector<Rec> recs = {
        {"mod.xml", "<ModInfo/>"},
        {"HIGH_POLY\\mods\\textures\\d000-0_00.dds", "DDS_STUB_BYTES"},
        {"..\\escape.txt", "SHOULD_BE_SKIPPED"}
    };

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), {'I', 'R', 'O', 'S'});
    push_u32(buf, 0x10002u);
    push_u32(buf, 0u);
    push_u32(buf, 16u); /* dir at offset 16 */

    /* Calculate directory size first */
    size_t dir_bytes = 4;
    for (const auto &r : recs) dir_bytes += 20 + r.name.size() * 2;
    uint64_t data_cursor = 16 + dir_bytes;

    push_u32(buf, uint32_t(recs.size()));
    for (const auto &r : recs) {
        uint16_t name_bytes = uint16_t(r.name.size() * 2);
        uint16_t rec_len = uint16_t(20 + name_bytes);
        push_u16(buf, rec_len);
        push_u16(buf, name_bytes);
        for (char c : r.name) push_u16(buf, uint16_t(static_cast<unsigned char>(c)));
        push_u32(buf, 0u);
        push_u64(buf, data_cursor);
        push_u32(buf, uint32_t(r.payload.size()));
        data_cursor += r.payload.size();
    }
    for (const auto &r : recs) buf.insert(buf.end(), r.payload.begin(), r.payload.end());

    {
        std::ofstream out(archive, std::ios::binary);
        out.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
    }

    std::vector<HostHD::IrosEntry> parsed;
    std::string err;
    check(HostHD::parse_iros_directory(archive.string(), parsed, &err), "parse_iros_directory");
    check(parsed.size() == 2, "traversal entry must be skipped");
    check(parsed[0].name == "mod.xml" && parsed[0].size == 10, "entry 0");
    check(parsed[1].name == "HIGH_POLY/mods/textures/d000-0_00.dds" && parsed[1].size == 14, "entry 1");
}

} // namespace

int main(int argc, char **argv) {
    test_dds_decoders();
    test_mod_xml_evaluator();
    std::filesystem::path tmp = (argc > 1) ? std::filesystem::path(argv[1])
                                           : (std::filesystem::temp_directory_path() / "lh_mod_import_test");
    test_iros_parser(tmp);
    std::printf("ok\n");
    return 0;
}
