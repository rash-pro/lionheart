#include "hd_models.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

static void push_le32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(uint8_t(v & 0xffu));
    b.push_back(uint8_t((v >> 8) & 0xffu));
    b.push_back(uint8_t((v >> 16) & 0xffu));
    b.push_back(uint8_t((v >> 24) & 0xffu));
}
static void push_be32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(uint8_t((v >> 24) & 0xffu));
    b.push_back(uint8_t((v >> 16) & 0xffu));
    b.push_back(uint8_t((v >> 8) & 0xffu));
    b.push_back(uint8_t(v & 0xffu));
}
static void push_f32(std::vector<uint8_t> &b, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    push_le32(b, bits);
}
static void push_le16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(uint8_t(v & 0xffu));
    b.push_back(uint8_t((v >> 8) & 0xffu));
}

static void push_png_chunk(std::vector<uint8_t> &out, const char type[4], const uint8_t *data, size_t len) {
    push_be32(out, uint32_t(len));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len && data) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + start, uInt(4 + len));
    push_be32(out, uint32_t(crc));
}

static std::vector<uint8_t> make_2x2_png() {
    uint8_t raw[2 * (1 + 2 * 4)] = {
        0, 255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 0, 255, 255,   255, 255, 255, 128
    };
    uLongf comp_cap = compressBound(sizeof(raw));
    std::vector<uint8_t> comp(comp_cap);
    int zrc = compress2(comp.data(), &comp_cap, raw, sizeof(raw), 6);
    assert(zrc == Z_OK);
    comp.resize(comp_cap);

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> png(sig, sig + 8);
    uint8_t ihdr[13] = {0, 0, 0, 2, 0, 0, 0, 2, 8, 6, 0, 0, 0};
    push_png_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    push_png_chunk(png, "IDAT", comp.data(), comp.size());
    push_png_chunk(png, "IEND", nullptr, 0);
    return png;
}

static void write_synthetic_glb(const std::filesystem::path &path, bool with_texture_and_normals) {
    std::vector<uint8_t> bin;
    /* POSITION (offset 0, len 36) */
    float pos[9] = {0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f};
    for (float f : pos) push_f32(bin, f);
    /* TEXCOORD_0 (offset 36, len 24) */
    float uv[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    for (float f : uv) push_f32(bin, f);
    /* JOINTS_0 u8x4 (offset 60, len 12) */
    uint8_t joints[12] = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    bin.insert(bin.end(), joints, joints + 12);
    /* WEIGHTS_0 f32x4 (offset 72, len 48) */
    float weights[12] = {1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0};
    for (float f : weights) push_f32(bin, f);
    /* INDICES u16x3 (offset 120, len 6) */
    push_le16(bin, 0); push_le16(bin, 1); push_le16(bin, 2);
    while (bin.size() % 4 != 0) bin.push_back(0);

    size_t nrm_off = bin.size();
    size_t png_off = 0, png_len = 0;
    if (with_texture_and_normals) {
        float nrm[9] = {0, 0, -1, 0, 0, -1, 0, 0, -1};
        for (float f : nrm) push_f32(bin, f);
        png_off = bin.size();
        auto png = make_2x2_png();
        png_len = png.size();
        bin.insert(bin.end(), png.begin(), png.end());
        while (bin.size() % 4 != 0) bin.push_back(0);
    }

    std::string j;
    if (!with_texture_and_normals) {
        j = "{\"asset\":{\"version\":\"2.0\"},"
            "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],"
            "\"bufferViews\":["
            "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
            "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
            "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":12},"
            "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":48},"
            "{\"buffer\":0,\"byteOffset\":120,\"byteLength\":6}"
            "],"
            "\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
            "{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
            "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
            "{\"bufferView\":4,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
            "],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{"
            "\"POSITION\":0,\"TEXCOORD_0\":1,\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":4}]}]}";
    } else {
        j = "{\"asset\":{\"version\":\"2.0\"},"
            "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],"
            "\"bufferViews\":["
            "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
            "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
            "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":12},"
            "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":48},"
            "{\"buffer\":0,\"byteOffset\":120,\"byteLength\":6},"
            "{\"buffer\":0,\"byteOffset\":" + std::to_string(nrm_off) + ",\"byteLength\":36},"
            "{\"buffer\":0,\"byteOffset\":" + std::to_string(png_off) + ",\"byteLength\":" + std::to_string(png_len) + "}"
            "],"
            "\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
            "{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
            "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
            "{\"bufferView\":4,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
            "{\"bufferView\":5,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
            "],"
            "\"images\":[{\"bufferView\":6,\"mimeType\":\"image/png\"}],"
            "\"textures\":[{\"source\":0}],"
            "\"materials\":[{\"alphaMode\":\"MASK\",\"alphaCutoff\":0.4,\"doubleSided\":true,"
            "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.9,1.0,1.0],\"baseColorTexture\":{\"index\":0}}}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{"
            "\"POSITION\":0,\"TEXCOORD_0\":1,\"JOINTS_0\":2,\"WEIGHTS_0\":3,\"NORMAL\":5},"
            "\"indices\":4,\"material\":0}]}]}";
    }
    while (j.size() % 4 != 0) j.push_back(' ');

    std::vector<uint8_t> glb;
    glb.insert(glb.end(), {'g', 'l', 'T', 'F'});
    push_le32(glb, 2u);
    uint32_t total = uint32_t(12 + 8 + j.size() + 8 + bin.size());
    push_le32(glb, total);
    push_le32(glb, uint32_t(j.size()));
    push_le32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), j.begin(), j.end());
    push_le32(glb, uint32_t(bin.size()));
    push_le32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());

    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(glb.data()), std::streamsize(glb.size()));
    assert(bool(out));
}

static uint32_t pack_sxy(int x, int y) {
    return (uint32_t(x) & 0xffffu) | ((uint32_t(y) & 0xffffu) << 16);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    std::filesystem::path root(argv[1]);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    /* 1. Invalid GLB rejection tests */
    {
        std::filesystem::path bad = root / "bad.glb";
        std::filesystem::create_directories(root);
        {
            std::ofstream out(bad, std::ios::binary);
            out << "NOT_A_GLB_FILE_AT_ALL_0123456789";
        }
        HostHD::HdModel tmp;
        std::string err;
        assert(!HostHD::ModelRegistry::load_glb(bad.string(), tmp, &err));
        assert(!err.empty());
        std::filesystem::remove(bad, ec);
    }

    /* 2. Synthetic GLB packs (with textures, normals, alpha mode, bone_remap, offset, rotation_euler, VRAM hash) */
    write_synthetic_glb(root / "field/d000.glb", true);
    write_synthetic_glb(root / "custom/squall_battle.glb", true);
    write_synthetic_glb(root / "custom/hash_matched.glb", false);

    /* Compute a synthetic VRAM texture block hash just like PS1 VRAM upload. */
    std::vector<uint16_t> vram(1024u * 512u, 0u);
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x)
            vram[(256u + y) * 1024u + (320u + x)] = uint16_t((x * 17u + y * 31u) | 0x8000u);
    uint64_t expected_tex_hash = HostHD::ModelRegistry::hash_vram_rect(320, 256, 32, 32, vram.data());
    assert(expected_tex_hash != 0);
    char hex_hash[32];
    std::snprintf(hex_hash, sizeof(hex_hash), "%016llx", (unsigned long long)expected_tex_hash);

    {
        std::ofstream m(root / "manifest.json");
        m << "{\n"
             "  \"models\": {\n"
             "    \"battle/c0m001\": {\n"
             "      \"file\": \"custom/squall_battle.glb\",\n"
             "      \"scale\": 2.0,\n"
             "      \"offset\": [5.0, 0.0, 0.0],\n"
             "      \"rotation_euler\": [0.0, 0.0, 90.0],\n"
             "      \"flip_winding\": true,\n"
             "      \"double_sided\": true,\n"
             "      \"bone_remap\": [0, 1]\n"
             "    },\n"
             "    \"field/textured_npc\": {\n"
             "      \"file\": \"custom/hash_matched.glb\",\n"
             "      \"scale\": 1.0,\n"
             "      \"vram_texture_hashes\": [\"" << hex_hash << "\"]\n"
             "    }\n"
             "  }\n"
             "}\n";
    }

    HostHD::ModelRegistry reg;
    reg.configure(true, root.string());
    assert(reg.enabled());
    assert(reg.has_any_active());
    assert(reg.has_model("field/d000"));
    assert(reg.has_model("battle/c0m001"));
    assert(reg.has_model("field/textured_npc"));
    assert(!reg.has_model("field/missing"));
    assert(reg.find_by_texture_hash(expected_tex_hash) == "field/textured_npc");

    auto m = reg.get("battle/c0m001");
    assert(m != nullptr);
    assert(m->scale == 2.0f);
    assert(std::fabs(m->offset[0] - 5.0f) < 1e-5f);
    assert(std::fabs(m->rotation_euler[2] - 90.0f) < 1e-5f);
    assert(m->flip_winding);
    assert(m->double_sided);
    assert(m->primitives.size() == 1);
    assert(m->primitives[0].vertices.size() == 3);
    assert(m->primitives[0].indices.size() == 3);
    assert(m->primitives[0].texture_index == 0);
    assert(m->primitives[0].alpha_mode == HostHD::AlphaMode::Mask);
    assert(std::fabs(m->primitives[0].alpha_cutoff - 0.4f) < 1e-5f);
    assert(m->primitives[0].double_sided);
    assert(m->textures.size() == 1 && m->textures[0].width == 2 && m->textures[0].height == 2);
    assert(m->textures[0].rgba[0] == 255 && m->textures[0].rgba[1] == 0 && m->textures[0].rgba[3] == 255);
    assert(std::fabs(m->primitives[0].vertices[0].normal[2] - (-1.0f)) < 1e-5f);

    /* 3. GTE bone matrix extraction & skinned pose evaluation with scale + rotation_euler (90 deg Z) + offset */
    uint32_t rot[5] = {
        4096u | (0u << 16),
        0u | (0u << 16),
        4096u | (0u << 16),
        0u | (0u << 16),
        4096u
    };
    int32_t tr0[3] = {0, 0, 200};
    int32_t tr1[3] = {10, 0, 200};
    HostHD::GteBoneMatrix bones[2] = {
        HostHD::ModelRegistry::from_gte_setup(rot, tr0),
        HostHD::ModelRegistry::from_gte_setup(rot, tr1),
    };
    std::vector<std::vector<HostHD::ProjectedVertex>> projected;
    HostHD::ModelRegistry::evaluate_pose(*m, bones, 2, 160.0f, 120.0f, 200.0f, projected);
    assert(projected.size() == 1 && projected[0].size() == 3);
    /* Vertex 0: (0,0,0) -> rot90z*2 + offset(5,0,0) = (5,0,0) + tr0(0,0,200) -> screen (165, 120), sz=200 */
    assert(std::fabs(projected[0][0].x - 165.0f) < 0.01f);
    assert(std::fabs(projected[0][0].y - 120.0f) < 0.01f);
    assert(std::fabs(projected[0][0].sz - 200.0f) < 0.01f);
    /* Vertex 1: (10,0,0) -> rot90z=(0,10,0)*2=(0,20,0) + offset(5,0,0) = (5,20,0) + tr1(10,0,200) = (15,20,200) -> screen (175, 140) */
    assert(std::fabs(projected[0][1].x - 175.0f) < 0.01f);
    assert(std::fabs(projected[0][1].y - 140.0f) < 0.01f);

    /* 4. Live HdSceneTracker: VRAM texture hash matching, canonical scene matching,
     *    live GTE lighting tint capture, selective PS1 suppression, and foreground mask occlusion flushing! */
    HostHD::HdSceneTracker tracker;
    tracker.record_vram_upload(320, 256, 32, 32, vram.data());
    assert(tracker.lookup_vram_hash(320, 256, 2, 0, 0, 16, 16) == expected_tex_hash);

    /* Simulate Frame 1 in Field mode with two entities:
     *   - Entity A (Squall, bone runs 0..1 at Z=300, screen ~160,120) -> matches field/d000!
     *   - Entity B (distant NPC, run 2 at Z=4000, screen ~50,50, textured with expected_tex_hash) -> matches field/textured_npc!
     *   - Entity C (unmatched enemy/prop, run 3 at Z=8000, screen ~260,180) -> MUST NOT be suppressed! */
    HostHD::SceneContext field_ctx{};
    field_ctx.mode = HostHD::SceneMode::Field;
    field_ctx.field_id = 75;
    field_ctx.party[0] = 0; /* Squall -> field/d000 */

    int32_t tr_a0[3] = {0, 0, 300};
    int32_t tr_a1[3] = {0, -20, 300};
    int32_t tr_b[3] = {-100, -50, 4000};
    int32_t tr_c[3] = {300, 100, 8000};
    int16_t v_in[3][3] = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    int32_t ofx_q16 = 160 << 16;
    int32_t ofy_q16 = 120 << 16;

    /* Record GTE projections for Entity A (2 bones) */
    tracker.record_gte_projection(rot, tr_a0, ofx_q16, ofy_q16, 256, v_in[0], pack_sxy(160, 120), 160.0f, 120.0f, 300, field_ctx);
    tracker.record_gte_projection(rot, tr_a0, ofx_q16, ofy_q16, 256, v_in[1], pack_sxy(170, 120), 170.0f, 120.0f, 300, field_ctx);
    tracker.record_gte_projection(rot, tr_a1, ofx_q16, ofy_q16, 256, v_in[2], pack_sxy(160, 130), 160.0f, 130.0f, 300, field_ctx);

    /* Record GTE projections for Entity B (far away in view space -> separate cluster, matched by VRAM hash!) */
    tracker.record_gte_projection(rot, tr_b, ofx_q16, ofy_q16, 256, v_in[0], pack_sxy(50, 50), 50.0f, 50.0f, 4000, field_ctx);
    tracker.record_gte_projection(rot, tr_b, ofx_q16, ofy_q16, 256, v_in[1], pack_sxy(60, 50), 60.0f, 50.0f, 4000, field_ctx);
    tracker.record_gte_projection(rot, tr_b, ofx_q16, ofy_q16, 256, v_in[2], pack_sxy(50, 60), 50.0f, 60.0f, 4000, field_ctx);

    /* Record GTE projections for Entity C (unmatched!) */
    tracker.record_gte_projection(rot, tr_c, ofx_q16, ofy_q16, 256, v_in[0], pack_sxy(260, 180), 260.0f, 180.0f, 8000, field_ctx);
    tracker.record_gte_projection(rot, tr_c, ofx_q16, ofy_q16, 256, v_in[1], pack_sxy(270, 180), 270.0f, 180.0f, 8000, field_ctx);
    tracker.record_gte_projection(rot, tr_c, ofx_q16, ofy_q16, 256, v_in[2], pack_sxy(260, 190), 260.0f, 190.0f, 8000, field_ctx);

    /* Draw Entity A triangle with dimmed shadow lighting (r=64, g=64, b=96 => tint 0.5, 0.5, 0.75) */
    HostHD::TriangleObservation tri_a{};
    tri_a.x[0] = 160; tri_a.y[0] = 120;
    tri_a.x[1] = 170; tri_a.y[1] = 120;
    tri_a.x[2] = 160; tri_a.y[2] = 130;
    tri_a.r[0] = tri_a.r[1] = tri_a.r[2] = 64;
    tri_a.g[0] = tri_a.g[1] = tri_a.g[2] = 64;
    tri_a.b[0] = tri_a.b[1] = tri_a.b[2] = 96;
    tri_a.clip_left = 0; tri_a.clip_top = 0; tri_a.clip_right = 319; tri_a.clip_bottom = 239;

    auto dec_a = tracker.observe_triangle(tri_a, reg);
    assert(dec_a.suppress_ps1_triangle);
    assert(!dec_a.flush_pending_hd_models);
    assert(tracker.has_pending_draws());

    /* Draw Entity B triangle with matching VRAM texture coordinates -> matched by VRAM hash! */
    HostHD::TriangleObservation tri_b{};
    tri_b.x[0] = 50; tri_b.y[0] = 50;
    tri_b.x[1] = 60; tri_b.y[1] = 50;
    tri_b.x[2] = 50; tri_b.y[2] = 60;
    tri_b.textured = true;
    tri_b.tex_page_x = 320; tri_b.tex_page_y = 256; tri_b.tex_depth = 2;
    tri_b.u[0] = 0; tri_b.v[0] = 0; tri_b.u[1] = 16; tri_b.v[1] = 0; tri_b.u[2] = 0; tri_b.v[2] = 16;
    tri_b.r[0] = tri_b.r[1] = tri_b.r[2] = 128;
    tri_b.g[0] = tri_b.g[1] = tri_b.g[2] = 128;
    tri_b.b[0] = tri_b.b[1] = tri_b.b[2] = 128;
    tri_b.clip_left = 0; tri_b.clip_top = 0; tri_b.clip_right = 319; tri_b.clip_bottom = 239;

    auto dec_b = tracker.observe_triangle(tri_b, reg);
    assert(dec_b.suppress_ps1_triangle);

    /* Draw Entity C triangle (unmatched non-overlapping prop) -> MUST NOT suppress and MUST NOT flush A/B! */
    HostHD::TriangleObservation tri_c{};
    tri_c.x[0] = 260; tri_c.y[0] = 180;
    tri_c.x[1] = 270; tri_c.y[1] = 180;
    tri_c.x[2] = 260; tri_c.y[2] = 190;
    tri_c.clip_left = 0; tri_c.clip_top = 0; tri_c.clip_right = 319; tri_c.clip_bottom = 239;

    auto dec_c = tracker.observe_triangle(tri_c, reg);
    assert(!dec_c.suppress_ps1_triangle);
    assert(!dec_c.flush_pending_hd_models);

    /* Foreground field mask sprite overlapping Entity A's screen bbox -> MUST request HD flush first! */
    assert(tracker.should_flush_for_rect(155, 115, 20, 20));
    assert(!tracker.should_flush_for_rect(0, 0, 20, 20));

    auto draws = tracker.take_pending_draws();
    assert(draws.size() == 2);
    assert(draws[0].model_id == "field/d000");
    assert(draws[0].bones.size() == 2);
    assert(std::fabs(draws[0].tint_rgb[0] - 0.5f) < 0.01f);
    assert(std::fabs(draws[0].tint_rgb[1] - 0.5f) < 0.01f);
    assert(std::fabs(draws[0].tint_rgb[2] - 0.75f) < 0.01f);
    assert(draws[0].min_sz == 300.0f);
    assert(draws[1].model_id == "field/textured_npc");
    assert(draws[1].min_sz == 4000.0f);
    assert(!tracker.has_pending_draws());

    /* Additional suppressed triangle of already-flushed Entity A stays suppressed without re-queuing draw! */
    auto dec_a2 = tracker.observe_triangle(tri_a, reg);
    assert(dec_a2.suppress_ps1_triangle);
    assert(!tracker.has_pending_draws());

    /* 5. Static-pose vertex cache: simulate 10 frames where Entity B (stationary NPC)
     *    stops calling GTE while Entity C keeps calling GTE every frame and flushing all
     *    capture history slots. Entity B MUST stay matched via its cached pose! */
    for (int f = 0; f < 10; ++f) {
        tracker.end_frame();
        int32_t tr_c_move[3] = {300 + f * 5, 100, 8000};
        tracker.record_gte_projection(rot, tr_c_move, ofx_q16, ofy_q16, 256, v_in[0],
                                      pack_sxy(260 + f, 180), float(260 + f), 180.0f, 8000, field_ctx);
        tracker.record_gte_projection(rot, tr_c_move, ofx_q16, ofy_q16, 256, v_in[1],
                                      pack_sxy(270 + f, 180), float(270 + f), 180.0f, 8000, field_ctx);
        tracker.record_gte_projection(rot, tr_c_move, ofx_q16, ofy_q16, 256, v_in[2],
                                      pack_sxy(260 + f, 190), float(260 + f), 190.0f, 8000, field_ctx);
        auto dec_b_cached = tracker.observe_triangle(tri_b, reg);
        assert(dec_b_cached.suppress_ps1_triangle);
        auto cached_draws = tracker.take_pending_draws();
        assert(cached_draws.size() == 1);
        assert(cached_draws[0].model_id == "field/textured_npc");
        assert(cached_draws[0].bones.size() == 1);
    }

    /* 5b. Overlapping characters sharing integer screen pixels: Entity A (moving at z=300)
     *     and Entity B (textured NPC at z=4000) project to the exact same screen pixels.
     *     A textured triangle of Entity B MUST bind to Entity B's skeleton (never steal
     *     Entity A's skeleton or get hijacked by Entity A's older history snapshots). */
    {
        tracker.end_frame();
        /* Seed Entity A at (160,120,z=300) and Entity B at (80,60,z=4000) cleanly first. */
        tracker.record_gte_projection(rot, tr_b, ofx_q16, ofy_q16, 256, v_in[0], pack_sxy(80, 60), 80.0f, 60.0f, 4000, field_ctx);
        tracker.record_gte_projection(rot, tr_b, ofx_q16, ofy_q16, 256, v_in[1], pack_sxy(90, 60), 90.0f, 60.0f, 4000, field_ctx);
        tracker.record_gte_projection(rot, tr_b, ofx_q16, ofy_q16, 256, v_in[2], pack_sxy(80, 70), 80.0f, 70.0f, 4000, field_ctx);
        tracker.record_gte_projection(rot, tr0, ofx_q16, ofy_q16, 256, v_in[0], pack_sxy(160, 120), 160.0f, 120.0f, 300, field_ctx);
        tracker.record_gte_projection(rot, tr0, ofx_q16, ofy_q16, 256, v_in[1], pack_sxy(170, 120), 170.0f, 120.0f, 300, field_ctx);
        tracker.record_gte_projection(rot, tr0, ofx_q16, ofy_q16, 256, v_in[2], pack_sxy(160, 130), 160.0f, 130.0f, 300, field_ctx);
        tracker.record_gte_projection(rot, tr1, ofx_q16, ofy_q16, 256, v_in[0], pack_sxy(165, 125), 165.0f, 125.0f, 320, field_ctx);
        tracker.record_gte_projection(rot, tr1, ofx_q16, ofy_q16, 256, v_in[1], pack_sxy(175, 125), 175.0f, 125.0f, 320, field_ctx);
        tracker.record_gte_projection(rot, tr1, ofx_q16, ofy_q16, 256, v_in[2], pack_sxy(165, 135), 165.0f, 135.0f, 320, field_ctx);
        assert(tracker.observe_triangle(tri_a, reg).suppress_ps1_triangle);
        assert(tracker.observe_triangle(tri_b, reg).suppress_ps1_triangle);
        auto seed_draws = tracker.take_pending_draws();
        assert(seed_draws.size() == 2);

        /* Next frame: Entity A crosses right behind Entity B's screen pixels (80,60),
         * projecting AFTER Entity B so Entity A sits at the head of sxy_head. */
        tracker.end_frame();
        int32_t tr_b2[3] = {-100, -50, 4010};
        int32_t tr_a2[3] = {5, 0, 210};
        tracker.record_gte_projection(rot, tr_b2, ofx_q16, ofy_q16, 256, v_in[0], pack_sxy(80, 60), 80.0f, 60.0f, 4010, field_ctx);
        tracker.record_gte_projection(rot, tr_b2, ofx_q16, ofy_q16, 256, v_in[1], pack_sxy(90, 60), 90.0f, 60.0f, 4010, field_ctx);
        tracker.record_gte_projection(rot, tr_b2, ofx_q16, ofy_q16, 256, v_in[2], pack_sxy(80, 70), 80.0f, 70.0f, 4010, field_ctx);
        tracker.record_gte_projection(rot, tr_a2, ofx_q16, ofy_q16, 256, v_in[0], pack_sxy(80, 60), 80.0f, 60.0f, 210, field_ctx);
        tracker.record_gte_projection(rot, tr_a2, ofx_q16, ofy_q16, 256, v_in[1], pack_sxy(90, 60), 90.0f, 60.0f, 210, field_ctx);
        tracker.record_gte_projection(rot, tr_a2, ofx_q16, ofy_q16, 256, v_in[2], pack_sxy(80, 70), 80.0f, 70.0f, 210, field_ctx);

        auto ov_b = tracker.observe_triangle(tri_b, reg);
        assert(ov_b.suppress_ps1_triangle);
        auto ov_draws = tracker.take_pending_draws();
        assert(ov_draws.size() == 1);
        assert(ov_draws[0].model_id == "field/textured_npc");
        assert(ov_draws[0].bones.size() == 1 && ov_draws[0].bones[0].t[2] > 3000.0f);
    }

    /* CLUT-split UV straddling fallback: V spanning slightly outside upload rect still resolves. */
    assert(tracker.lookup_vram_hash(320, 256, 2, 0, 24, 16, 36) == expected_tex_hash);

    /* Field transition invalidates cached poses cleanly. */
    tracker.end_frame();
    HostHD::SceneContext next_field = field_ctx;
    next_field.field_id = 76;
    tracker.record_gte_projection(rot, tr_c, ofx_q16, ofy_q16, 256, v_in[0], pack_sxy(260, 180), 260.0f, 180.0f, 8000, next_field);
    tracker.record_gte_projection(rot, tr_c, ofx_q16, ofy_q16, 256, v_in[1], pack_sxy(270, 180), 270.0f, 180.0f, 8000, next_field);
    tracker.record_gte_projection(rot, tr_c, ofx_q16, ofy_q16, 256, v_in[2], pack_sxy(260, 190), 260.0f, 190.0f, 8000, next_field);
    auto dec_b_after_field = tracker.observe_triangle(tri_b, reg);
    assert(!dec_b_after_field.suppress_ps1_triangle);

    /* Real-time toggle OFF: zero suppression when disabled! */
    reg.configure(false, root.string());
    assert(!reg.has_any_active());
    tracker.end_frame();
    auto dec_off = tracker.observe_triangle(tri_a, reg);
    assert(!dec_off.suppress_ps1_triangle);
    assert(!dec_off.flush_pending_hd_models);

    /* Optional argv[2]: validate loading an externally generated pack (e.g. from tools/hd_models_import.py). */
    if (argc >= 3) {
        HostHD::ModelRegistry ext;
        ext.configure(true, argv[2]);
        assert(ext.has_any_active());
        assert(ext.has_model("field/d000"));
        assert(ext.has_model("battle/c0m001"));
        assert(ext.has_model("world/squall"));
        auto sq = ext.get("field/d000");
        assert(sq != nullptr);
        assert(sq->primitives.size() == 2);
        assert(sq->primitives[0].alpha_mode == HostHD::AlphaMode::Opaque);
        assert(sq->primitives[1].alpha_mode == HostHD::AlphaMode::Mask);
        assert(sq->textures.size() == 2 && sq->textures[0].width == 16 && sq->textures[1].width == 16);
    }

    std::puts("ok");
    return 0;
}
