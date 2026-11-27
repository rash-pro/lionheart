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

    /* Custom option overrides must override <Default> values cleanly. */
    auto res_ov = HostHD::parse_and_eval_mod_xml(xml, {{"POLY_ID", 2}, {"THEME_ID", 0}});
    auto has_ov = [&](const std::string &f) {
        for (const auto &a : res_ov.active_folders) if (a == f) return true;
        return false;
    };
    check(!has_ov("field_models/high_poly"), "override POLY_ID=2 disables HIGH_POLY");
    check(has_ov("field_models/low_poly"), "override POLY_ID=2 enables LOW_POLY");
    check(!has_ov("ui_theme/active"), "override THEME_ID=0 disables active theme");
    check(has_ov("ui_theme/inactive"), "override THEME_ID=0 enables inactive theme");

    /* parse_mod_xml_config extracts metadata, bool options, and list choices. */
    const std::string rich_xml = R"XML(
<ModInfo>
  <Name>Synthetic Remaster Pack</Name>
  <Author>Test Suite</Author>
  <Version>1.2.3</Version>
  <ConfigOption>
    <Type>List</Type>
    <ID>POLY_ID</ID>
    <Name>Character Models</Name>
    <Description>Select mesh detail</Description>
    <Default>1</Default>
    <Option Value="0" Name="Original" />
    <Option Value="1" Name="High Poly" />
    <Option><Value>2</Value><Name>Low Poly</Name></Option>
  </ConfigOption>
  <ConfigOption>
    <Type>Bool</Type>
    <ID>UI_ID</ID>
    <Name>HD UI</Name>
    <Default>1</Default>
  </ConfigOption>
</ModInfo>
)XML";
    host_mod_xml_config_t cfg{};
    check(HostHD::parse_mod_xml_config(rich_xml, cfg), "parse_mod_xml_config");
    check(std::strcmp(cfg.mod_name, "Synthetic Remaster Pack") == 0, "mod_name");
    check(std::strcmp(cfg.author, "Test Suite") == 0, "author");
    check(std::strcmp(cfg.version, "1.2.3") == 0, "version");
    check(cfg.option_count == 2, "option_count == 2");
    check(std::strcmp(cfg.options[0].id, "POLY_ID") == 0 && cfg.options[0].type == 1, "list opt");
    check(cfg.options[0].choice_count == 3, "3 choices");
    check(std::strcmp(cfg.options[0].choices[1].name, "High Poly") == 0, "choice 1 name");
    check(std::strcmp(cfg.options[0].choices[2].name, "Low Poly") == 0 && cfg.options[0].choices[2].value == 2, "choice 2");
    check(std::strcmp(cfg.options[1].id, "UI_ID") == 0 && cfg.options[1].type == 0 && cfg.options[1].default_value == 1, "bool opt");
}

void test_managed_import_preserves_existing_movies(const std::filesystem::path &temp_dir) {
    namespace fs = std::filesystem;
    fs::path data = temp_dir / "managed-import";
    fs::path pack = data / "pack";
    fs::path movies = data / "movies-hd";
    fs::path other_movies = data / "other-movies";
    fs::remove_all(data);
    fs::create_directories(pack);
    fs::create_directories(movies);
    fs::create_directories(other_movies);

    /* A minimal ISO9660 directory and an empty FF8 disc index. */
    constexpr size_t sector = 2048;
    std::vector<uint8_t> iso(19 * sector);
    std::memcpy(iso.data() + 16 * sector + 1, "CD001", 5);
    iso[16 * sector + 158] = 17;
    iso[16 * sector + 167] = 8;
    iso[17 * sector] = 46;
    iso[17 * sector + 2] = 18;
    iso[17 * sector + 11] = 8;
    iso[17 * sector + 32] = 12;
    std::memcpy(iso.data() + 17 * sector + 33, "FF8DISC1.IMG", 12);
    fs::path disc = data / "synthetic.iso";
    std::ofstream(disc, std::ios::binary).write(reinterpret_cast<const char *>(iso.data()), iso.size());

    std::ofstream(pack / "existing.mp4") << "new";
    std::ofstream(pack / "installed.mp4") << "imported";
    std::ofstream(movies / "existing.mp4") << "old";
    std::ofstream(other_movies / "installed.mp4") << "unrelated";
    std::string disc_arg = disc.string(), pack_arg = pack.string();
    const char *discs[] = {disc_arg.c_str()};
    const char *packs[] = {pack_arg.c_str()};
    host_mod_import_status_t status{};
    check(host_mod_import_run_ex(discs, 1, packs, 1, nullptr, nullptr, movies.string().c_str(),
                                 nullptr, data.string().c_str(), nullptr, 0, &status) == 1,
          status.error);
    check(status.movies_written == 1, "an existing movie is not claimed by the new pack");
    check(host_mod_registry_remove(data.string().c_str(), "pack", nullptr, nullptr,
                                   other_movies.string().c_str()) == 0,
          "remove refuses to operate in a different folder");
    check(fs::exists(other_movies / "installed.mp4"), "separately configured asset preserved");
    check(host_mod_registry_remove(data.string().c_str(), "pack", nullptr, nullptr,
                                   movies.string().c_str()) == 1,
          "remove from original folder succeeds");
    std::string existing;
    std::ifstream(movies / "existing.mp4") >> existing;
    check(existing == "old", "preexisting movie is preserved after removing a pack");
    check(!fs::exists(movies / "installed.mp4"), "only the imported movie is removed");

    check(host_mod_import_run_ex(discs, 1, packs, 1, nullptr, nullptr, movies.string().c_str(),
                                 nullptr, data.string().c_str(), nullptr, 0, &status) == 1,
          status.error);
    check(host_mod_registry_set_enabled(data.string().c_str(), "pack", 0, nullptr, nullptr,
                                        movies.string().c_str()) == 1, "disable the imported movie");
    std::ofstream(movies / "installed.mp4") << "manually-added";
    check(host_mod_registry_set_enabled(data.string().c_str(), "pack", 1, nullptr, nullptr,
                                        movies.string().c_str()) == 0, "refuse to overwrite on enable");
    check(host_mod_registry_remove(data.string().c_str(), "pack", nullptr, nullptr,
                                   movies.string().c_str()) == 1, "remove the disabled pack");
    std::ifstream(movies / "installed.mp4") >> existing;
    check(existing == "manually-added", "removing a disabled pack preserves the active folder");
}

void test_mod_registry_lifecycle(const std::filesystem::path &temp_dir) {
    namespace fs = std::filesystem;
    fs::path data = temp_dir / "reg-data";
    fs::remove_all(data);
    fs::path tex = data / "textures-hd";
    fs::path mod = data / "models-hd";
    fs::path mov = data / "movies-hd";
    fs::path aud = data / "audio-hd";
    fs::create_directories(tex);
    fs::create_directories(mod / "field");
    fs::create_directories(mov);
    fs::create_directories(aud / "music");

    auto touch = [](const fs::path &p, const char *s) {
        std::ofstream o(p, std::ios::binary);
        o << s;
    };
    touch(tex / "aaaa.png", "tex-a");
    touch(tex / "shared.png", "tex-shared");
    touch(mod / "field/d000.glb", "glb-a");
    touch(mod / "field/d001.glb", "glb-b");
    touch(aud / "music/track01.ogg", "ogg-a");
    touch(mod / "manifest.json", R"JSON({
  "models": {
    "field/d000": { "file": "field/d000.glb", "scale": 1.0 },
    "field/d001": { "file": "field/d001.glb", "scale": 1.0 }
  }
})JSON");
    touch(data / "mods-registry.json", R"JSON({
  "version": 1,
  "mods": [
    {
      "id": "pack-a",
      "name": "Pack A",
      "source": "/mods/pack-a.iroj",
      "enabled": true,
      "textures_written": 2,
      "models_written": 1,
      "movies_written": 0,
      "audio_written": 1,
      "warnings": 1,
      "failed": 0,
      "log": ["Active folders (1/2)", "[WARN] 1 compressed entry skipped"],
      "files": {
        "textures": ["aaaa.png", "shared.png"],
        "models": ["field/d000.glb"],
        "movies": [],
        "audio": ["music/track01.ogg"]
      },
      "manifest_models": {
        "field/d000": { "file": "field/d000.glb", "scale": 1.0 }
      }
    },
    {
      "id": "pack-b",
      "name": "Pack B",
      "source": "/mods/pack-b.iroj",
      "enabled": true,
      "textures_written": 1,
      "models_written": 1,
      "movies_written": 0,
      "audio_written": 0,
      "warnings": 0,
      "failed": 0,
      "log": [],
      "files": {
        "textures": ["shared.png"],
        "models": ["field/d001.glb"],
        "movies": [],
        "audio": []
      },
      "manifest_models": {
        "field/d001": { "file": "field/d001.glb", "scale": 1.0 }
      }
    }
  ]
})JSON");

    host_mod_entry_t entries[8];
    unsigned n = host_mod_registry_list(data.string().c_str(), entries, 8);
    check(n == 2, "registry list == 2");
    check(std::strcmp(entries[0].id, "pack-a") == 0 && entries[0].enabled == 1 && entries[0].audio_written == 1 && entries[0].warnings == 1, "entry 0 fields");
    check(std::strstr(entries[0].summary_log, "[WARN]") != nullptr, "entry 0 log preserved");

    /* Disable pack-a: unique files stashed, shared.png kept active for pack-b, manifest drops d000. */
    check(host_mod_registry_set_enabled(data.string().c_str(), "pack-a", 0,
                                        tex.string().c_str(), mod.string().c_str(), mov.string().c_str()) == 1,
          "disable pack-a");
    check(!fs::exists(tex / "aaaa.png"), "aaaa.png stashed");
    check(fs::exists(tex / "shared.png"), "shared.png stays active for pack-b");
    check(!fs::exists(mod / "field/d000.glb"), "d000.glb stashed");
    check(fs::exists(mod / "field/d001.glb"), "d001.glb stays active");
    check(!fs::exists(aud / "music/track01.ogg"), "audio stashed");
    {
        std::ifstream min(mod / "manifest.json");
        std::string mtxt((std::istreambuf_iterator<char>(min)), std::istreambuf_iterator<char>());
        check(mtxt.find("field/d000") == std::string::npos, "manifest drops disabled d000");
        check(mtxt.find("field/d001") != std::string::npos, "manifest keeps enabled d001");
    }

    /* Re-enable pack-a: unique files restored and manifest restores d000. */
    check(host_mod_registry_set_enabled(data.string().c_str(), "pack-a", 1,
                                        tex.string().c_str(), mod.string().c_str(), mov.string().c_str()) == 1,
          "enable pack-a");
    check(fs::exists(tex / "aaaa.png"), "aaaa.png restored");
    check(fs::exists(mod / "field/d000.glb"), "d000.glb restored");
    check(fs::exists(aud / "music/track01.ogg"), "audio restored");
    {
        std::ifstream min(mod / "manifest.json");
        std::string mtxt((std::istreambuf_iterator<char>(min)), std::istreambuf_iterator<char>());
        check(mtxt.find("field/d000") != std::string::npos, "manifest restores d000");
    }

    /* Remove pack-a: unique files removed, shared.png stays for pack-b, list shrinks to 1. */
    check(host_mod_registry_remove(data.string().c_str(), "pack-a",
                                   tex.string().c_str(), mod.string().c_str(), mov.string().c_str()) == 1,
          "remove pack-a");
    check(!fs::exists(tex / "aaaa.png"), "aaaa.png deleted");
    check(fs::exists(tex / "shared.png"), "shared.png still kept for pack-b");
    check(!fs::exists(mod / "field/d000.glb"), "d000.glb deleted");
    check(host_mod_registry_list(data.string().c_str(), entries, 8) == 1, "registry shrunk to 1");
    check(std::strcmp(entries[0].id, "pack-b") == 0, "remaining entry is pack-b");
}

void test_iros_parser(const std::filesystem::path &temp_dir) {
    std::filesystem::create_directories(temp_dir);
    auto archive = temp_dir / "synthetic.iroj";

    struct Rec { std::string name; std::string payload; };
    std::vector<Rec> recs = {
        {"mod.xml", "<ModInfo><Name>Synthetic IROS</Name><ConfigOption><ID>OPT</ID><Name>Mode</Name><Default>1</Default><Option Value=\"0\" Name=\"A\"/><Option Value=\"1\" Name=\"B\"/></ConfigOption></ModInfo>"},
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
    check(parsed[0].name == "mod.xml" && parsed[0].size > 10, "entry 0");
    check(parsed[1].name == "HIGH_POLY/mods/textures/d000-0_00.dds" && parsed[1].size == 14, "entry 1");

    host_mod_xml_config_t inspected{};
    check(host_mod_inspect_options(archive.string().c_str(), &inspected) == 1, "inspect IROS mod.xml");
    check(std::strcmp(inspected.mod_name, "Synthetic IROS") == 0 && inspected.option_count == 1, "inspected IROS metadata");
}

void test_battle_dat_conversion(const std::filesystem::path &temp_dir) {
    /* Build a minimal clean-room FF8 battle .dat with 3 sections:
     *   Section 0: Skeleton (3 bones: 0=root(-1), 1->0, 2->1)
     *   Section 1: Geometry (1 object, 2 vertex groups on bones 0 & 2, 1 tri + 1 quad)
     *   Section 2: Texture table (1 embedded 4bpp 16x16 TIM) */
    std::vector<uint8_t> sec0;
    push_u16(sec0, 3u);   /* num_bones */
    push_u16(sec0, 100u); /* scale */
    for (int i = 0; i < 12; ++i) sec0.push_back(0);
    int16_t parents[3] = {-1, 0, 1};
    for (int b = 0; b < 3; ++b) {
        push_u16(sec0, uint16_t(parents[b]));
        for (int pad = 0; pad < 46; ++pad) sec0.push_back(0);
    }

    std::vector<uint8_t> sec1;
    push_u32(sec1, 1u); /* nobjs */
    push_u32(sec1, 8u); /* obj0 offset relative to sec1 */
    size_t obj_start = sec1.size();
    push_u16(sec1, 2u); /* ngroups = 2 */
    /* Group 0: bone 0, 3 vertices */
    push_u16(sec1, 0u);
    push_u16(sec1, 3u);
    int16_t g0[3][3] = {{0, 0, 0}, {16, 0, 0}, {0, 16, 0}};
    for (auto &v : g0) {
        push_u16(sec1, uint16_t(v[0]));
        push_u16(sec1, uint16_t(v[1]));
        push_u16(sec1, uint16_t(v[2]));
    }
    /* Group 1: bone 2, 1 vertex */
    push_u16(sec1, 2u);
    push_u16(sec1, 1u);
    push_u16(sec1, uint16_t(16));
    push_u16(sec1, uint16_t(16));
    push_u16(sec1, uint16_t(0));
    while ((sec1.size() - obj_start) % 4u != 0u) sec1.push_back(0);
    /* Poly header: 1 tri, 1 quad + 8 bytes padding */
    push_u16(sec1, 1u);
    push_u16(sec1, 1u);
    for (int i = 0; i < 8; ++i) sec1.push_back(0);
    /* 1 triangle (16 bytes) */
    push_u16(sec1, 0u); push_u16(sec1, 1u); push_u16(sec1, 2u);
    sec1.insert(sec1.end(), {0, 0, 8, 0, 0, 8}); /* u0,v0,u1,v1,u2,v2 */
    push_u16(sec1, 0u); /* tex_id */
    push_u16(sec1, 0u); /* pad */
    /* 1 quad (20 bytes) */
    push_u16(sec1, 0u); push_u16(sec1, 1u); push_u16(sec1, 2u); push_u16(sec1, 3u);
    sec1.insert(sec1.end(), {0, 0, 8, 0, 0, 8, 8, 8});
    push_u16(sec1, 0u);
    push_u16(sec1, 0u);

    /* Minimal valid 4bpp 16x16 TIM (clut + 4x16 halfwords) */
    std::vector<uint8_t> tim;
    push_u32(tim, 0x10u); /* magic */
    push_u32(tim, 0x08u); /* 4bpp + CLUT */
    push_u32(tim, 12u + 32u); /* clut bnum */
    push_u16(tim, 0u); push_u16(tim, 0u); push_u16(tim, 16u); push_u16(tim, 1u);
    for (int i = 0; i < 16; ++i) push_u16(tim, uint16_t(0x8000u | i));
    push_u32(tim, 12u + 128u); /* img bnum: 4 halfwords * 16 rows * 2 bytes */
    push_u16(tim, 320u); push_u16(tim, 0u); push_u16(tim, 4u); push_u16(tim, 16u);
    for (int i = 0; i < 64; ++i) push_u16(tim, 0x1234u);

    std::vector<uint8_t> sec2;
    push_u32(sec2, 1u); /* ntims */
    push_u32(sec2, 8u); /* tim0 offset */
    sec2.insert(sec2.end(), tim.begin(), tim.end());

    /* Assemble 3-section battle .dat */
    uint32_t header_sz = 4u + 4u * 4u; /* sec_count + 3 offsets + EOF */
    uint32_t off0 = header_sz;
    uint32_t off1 = off0 + uint32_t(sec0.size());
    uint32_t off2 = off1 + uint32_t(sec1.size());
    uint32_t eof = off2 + uint32_t(sec2.size());

    std::vector<uint8_t> dat;
    push_u32(dat, 3u);
    push_u32(dat, off0);
    push_u32(dat, off1);
    push_u32(dat, off2);
    push_u32(dat, eof);
    dat.insert(dat.end(), sec0.begin(), sec0.end());
    dat.insert(dat.end(), sec1.begin(), sec1.end());
    dat.insert(dat.end(), sec2.begin(), sec2.end());

    std::filesystem::path glb_path = temp_dir / "battle_d0c000.glb";
    std::vector<int> bone_remap;
    check(HostHD::convert_battle_dat_to_glb_file("d0c000", dat, glb_path.string(), &bone_remap),
          "convert_battle_dat_to_glb_file");
    check(bone_remap.size() == 2 && bone_remap[0] == 0 && bone_remap[1] == 2,
          "battle .dat bone_remap preserves used bones [0, 2]");

    std::ifstream in(glb_path, std::ios::binary | std::ios::ate);
    check(bool(in), "battle .glb readable");
    auto sz = in.tellg();
    check(sz > 64, "battle .glb non-empty");
    in.seekg(0);
    std::vector<uint8_t> glb(static_cast<size_t>(sz));
    in.read(reinterpret_cast<char *>(glb.data()), sz);
    check(std::memcmp(glb.data(), "glTF", 4) == 0, "battle .glb magic");
    uint32_t ver = uint32_t(glb[4]) | (uint32_t(glb[5]) << 8) | (uint32_t(glb[6]) << 16) | (uint32_t(glb[7]) << 24);
    check(ver == 2u, "battle .glb version 2");
}

} // namespace

int main(int argc, char **argv) {
    test_dds_decoders();
    test_mod_xml_evaluator();
    std::filesystem::path tmp = (argc > 1) ? std::filesystem::path(argv[1])
                                           : (std::filesystem::temp_directory_path() / "lh_mod_import_test");
    test_iros_parser(tmp);
    test_mod_registry_lifecycle(tmp);
    test_managed_import_preserves_existing_movies(tmp);
    test_battle_dat_conversion(tmp);
    std::printf("ok\n");
    return 0;
}
