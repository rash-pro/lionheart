#ifdef _WIN32
#undef fopen
#undef open
#undef mkdir
#undef unlink
#undef remove
#undef rename
#undef getenv
#endif

#include "hd_models.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
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

using json = nlohmann::json;

constexpr uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

inline uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint16_t le16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}
inline float le_f32(const uint8_t *p) {
    uint32_t bits = le32(p);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
inline uint64_t fnv_u16(uint64_t h, uint16_t v) {
    h = (h ^ (v & 0xffu)) * kFnvPrime;
    h = (h ^ ((v >> 8) & 0xffu)) * kFnvPrime;
    return h;
}
inline uint64_t mix64(uint64_t h, uint64_t v) {
    h ^= v + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    return h * UINT64_C(0xff51afd7ed558ccd);
}

std::string normalize_id(std::string s) {
    for (char &ch : s) {
        if (ch == '\\') ch = '/';
        else if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
    }
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

bool parse_u64_hex_or_int(const json &v, uint64_t &out) {
    if (v.is_number_unsigned()) {
        out = v.get<uint64_t>();
        return true;
    }
    if (v.is_number_integer()) {
        int64_t iv = v.get<int64_t>();
        if (iv < 0) return false;
        out = uint64_t(iv);
        return true;
    }
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (s.empty()) return false;
        const char *p = s.c_str();
        if (s.size() > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        if (!*p) return false;
        char *end = nullptr;
        unsigned long long val = std::strtoull(p, &end, 16);
        if (!end || *end != '\0') return false;
        out = uint64_t(val);
        return true;
    }
    return false;
}

bool read_accessor_view(const json &root, int accessor_index, const uint8_t *bin, size_t bin_size,
                        const uint8_t *&data_out, size_t &count_out, size_t &stride_out,
                        int &component_type_out, std::string &type_out) {
    if (accessor_index < 0 || !root.contains("accessors") || !root.contains("bufferViews")) return false;
    const auto &accessors = root["accessors"];
    if (size_t(accessor_index) >= accessors.size()) return false;
    const auto &acc = accessors[size_t(accessor_index)];
    if (!acc.contains("bufferView") || !acc.contains("count") || !acc.contains("componentType") || !acc.contains("type"))
        return false;
    size_t bv_idx = acc["bufferView"].get<size_t>();
    const auto &views = root["bufferViews"];
    if (bv_idx >= views.size()) return false;
    const auto &bv = views[bv_idx];

    size_t bv_off = bv.value("byteOffset", size_t(0));
    size_t bv_len = bv.value("byteLength", size_t(0));
    size_t acc_off = acc.value("byteOffset", size_t(0));
    size_t count = acc["count"].get<size_t>();
    int ctype = acc["componentType"].get<int>();
    std::string type = acc["type"].get<std::string>();

    size_t comp_size = (ctype == 5121 || ctype == 5120) ? 1u :
                       (ctype == 5123 || ctype == 5122) ? 2u :
                       (ctype == 5125 || ctype == 5126) ? 4u : 0u;
    size_t comps = (type == "SCALAR") ? 1u : (type == "VEC2") ? 2u : (type == "VEC3") ? 3u : (type == "VEC4") ? 4u : 0u;
    if (!comp_size || !comps) return false;

    size_t elem_size = comp_size * comps;
    size_t stride = bv.value("byteStride", elem_size);
    if (stride < elem_size) return false;
    if (bv_off > bin_size || bv_len > bin_size - bv_off) return false;
    if (acc_off > bv_len) return false;
    if (count > 0 && (acc_off + (count - 1u) * stride + elem_size) > bv_len) return false;

    data_out = bin + bv_off + acc_off;
    count_out = count;
    stride_out = stride;
    component_type_out = ctype;
    type_out = std::move(type);
    return true;
}

void compute_smooth_normals(ModelPrimitive &mp) {
    for (auto &v : mp.vertices) {
        v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;
    }
    for (size_t i = 0; i + 2 < mp.indices.size(); i += 3) {
        auto &v0 = mp.vertices[mp.indices[i + 0]];
        auto &v1 = mp.vertices[mp.indices[i + 1]];
        auto &v2 = mp.vertices[mp.indices[i + 2]];
        float e1[3] = {v1.pos[0] - v0.pos[0], v1.pos[1] - v0.pos[1], v1.pos[2] - v0.pos[2]};
        float e2[3] = {v2.pos[0] - v0.pos[0], v2.pos[1] - v0.pos[1], v2.pos[2] - v0.pos[2]};
        float nx = e1[1] * e2[2] - e1[2] * e2[1];
        float ny = e1[2] * e2[0] - e1[0] * e2[2];
        float nz = e1[0] * e2[1] - e1[1] * e2[0];
        for (size_t k = 0; k < 3; ++k) {
            auto &v = mp.vertices[mp.indices[i + k]];
            v.normal[0] += nx;
            v.normal[1] += ny;
            v.normal[2] += nz;
        }
    }
    for (auto &v : mp.vertices) {
        float len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
        if (len > 1e-6f) {
            v.normal[0] /= len;
            v.normal[1] /= len;
            v.normal[2] /= len;
        } else {
            v.normal[0] = 0.0f;
            v.normal[1] = -1.0f;
            v.normal[2] = 0.0f;
        }
    }
}

const char *party_world_name(uint8_t char_id) {
    switch (char_id) {
    case 0: return "squall";
    case 1: return "zell";
    case 2: return "irvine";
    case 3: return "quistis";
    case 4: return "rinoa";
    case 5: return "selphie";
    case 6: return "seifer";
    case 7: return "edea";
    case 8: return "laguna";
    case 9: return "kiros";
    case 10: return "ward";
    default: return nullptr;
    }
}

} // namespace

void HdModel::local_transform(float rows[3][4]) const {
    float rx = rotation_euler[0] * kDegToRad;
    float ry = rotation_euler[1] * kDegToRad;
    float rz = rotation_euler[2] * kDegToRad;
    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);

    /* Intrinsic XYZ -> M = Rz * Ry * Rx, scaled by `scale`, translated by `offset`. */
    float r00 = cz * cy;
    float r01 = cz * sy * sx - sz * cx;
    float r02 = cz * sy * cx + sz * sx;

    float r10 = sz * cy;
    float r11 = sz * sy * sx + cz * cx;
    float r12 = sz * sy * cx - cz * sx;

    float r20 = -sy;
    float r21 = cy * sx;
    float r22 = cy * cx;

    rows[0][0] = r00 * scale; rows[0][1] = r01 * scale; rows[0][2] = r02 * scale; rows[0][3] = offset[0];
    rows[1][0] = r10 * scale; rows[1][1] = r11 * scale; rows[1][2] = r12 * scale; rows[1][3] = offset[1];
    rows[2][0] = r20 * scale; rows[2][1] = r21 * scale; rows[2][2] = r22 * scale; rows[2][3] = offset[2];
}

void ModelRegistry::configure(bool enabled, const std::string &models_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = (enabled_ != enabled) || (dir_ != models_dir);
    enabled_ = enabled;
    dir_ = models_dir;
    if (changed) {
        ++generation_;
        entries_.clear();
        tex_hash_to_id_.clear();
        run_key_to_id_.clear();
        cache_.clear();
        scan_locked();
        generation_atomic_.store(generation_, std::memory_order_relaxed);
    }
    enabled_atomic_.store(enabled_, std::memory_order_relaxed);
    active_atomic_.store(enabled_ && !entries_.empty(), std::memory_order_relaxed);
}

void ModelRegistry::reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    entries_.clear();
    tex_hash_to_id_.clear();
    run_key_to_id_.clear();
    cache_.clear();
    scan_locked();
    generation_atomic_.store(generation_, std::memory_order_relaxed);
    enabled_atomic_.store(enabled_, std::memory_order_relaxed);
    active_atomic_.store(enabled_ && !entries_.empty(), std::memory_order_relaxed);
}

const std::string &ModelRegistry::directory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dir_;
}

size_t ModelRegistry::discovered_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void ModelRegistry::scan_locked() {
    if (dir_.empty()) return;
    std::error_code ec;
    std::filesystem::path root(dir_);
    if (!std::filesystem::is_directory(root, ec)) return;

    /* 1. Optional manifest.json maps canonical FF8 IDs ("field/d000", "battle/c0m001",
     *    "world/squall", etc.) to .glb files plus optional transforms, bone_remap,
     *    vram_texture_hashes, and gte_run_keys. */
    std::filesystem::path manifest_path = root / "manifest.json";
    if (std::filesystem::is_regular_file(manifest_path, ec)) {
        std::ifstream in(manifest_path);
        if (in) {
            try {
                json doc = json::parse(in);
                if (doc.contains("models") && doc["models"].is_object()) {
                    for (auto it = doc["models"].begin(); it != doc["models"].end(); ++it) {
                        std::string id = normalize_id(it.key());
                        Entry e;
                        if (it.value().is_string()) {
                            e.path = (root / it.value().get<std::string>()).string();
                        } else if (it.value().is_object() && it.value().contains("file")) {
                            const auto &obj = it.value();
                            e.path = (root / obj["file"].get<std::string>()).string();
                            e.scale = obj.value("scale", 1.0f);
                            e.flip_winding = obj.value("flip_winding", false);
                            e.double_sided = obj.value("double_sided", false);
                            if (obj.contains("offset") && obj["offset"].is_array() && obj["offset"].size() == 3) {
                                for (size_t k = 0; k < 3; ++k)
                                    if (obj["offset"][k].is_number()) e.offset[k] = obj["offset"][k].get<float>();
                            }
                            if (obj.contains("rotation_euler") && obj["rotation_euler"].is_array() &&
                                obj["rotation_euler"].size() == 3) {
                                for (size_t k = 0; k < 3; ++k)
                                    if (obj["rotation_euler"][k].is_number())
                                        e.rotation_euler[k] = obj["rotation_euler"][k].get<float>();
                            }
                            if (obj.contains("bone_remap") && obj["bone_remap"].is_array()) {
                                for (const auto &v : obj["bone_remap"])
                                    if (v.is_number_integer()) e.bone_remap.push_back(v.get<int>());
                            }
                            if (obj.contains("vram_texture_hashes") && obj["vram_texture_hashes"].is_array()) {
                                for (const auto &v : obj["vram_texture_hashes"]) {
                                    uint64_t h = 0;
                                    if (parse_u64_hex_or_int(v, h) && h != 0) e.vram_texture_hashes.push_back(h);
                                }
                            }
                            if (obj.contains("gte_run_keys") && obj["gte_run_keys"].is_array()) {
                                for (const auto &v : obj["gte_run_keys"]) {
                                    uint64_t k = 0;
                                    if (parse_u64_hex_or_int(v, k) && k != 0) e.gte_run_keys.push_back(k);
                                }
                            }
                        }
                        if (!e.path.empty() && std::filesystem::is_regular_file(e.path, ec)) {
                            for (uint64_t h : e.vram_texture_hashes) tex_hash_to_id_.emplace(h, id);
                            for (uint64_t k : e.gte_run_keys) run_key_to_id_.emplace(k, id);
                            entries_[id] = std::move(e);
                        }
                    }
                }
            } catch (...) {
                /* Malformed manifest falls through to convention scan. */
            }
        }
    }

    /* 2. Convention discovery: any relative "<category>/<name>.glb" not already listed. */
    for (const auto &de : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (!de.is_regular_file(ec)) continue;
        std::string ext = de.path().extension().string();
        for (char &ch : ext) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
        if (ext != ".glb") continue;
        auto rel = std::filesystem::relative(de.path(), root, ec);
        if (ec) continue;
        rel.replace_extension("");
        std::string id = normalize_id(rel.generic_string());
        if (entries_.find(id) == entries_.end()) {
            Entry e;
            e.path = de.path().string();
            entries_[id] = std::move(e);
        }
    }
}

bool ModelRegistry::has_model(const std::string &id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_ && entries_.find(normalize_id(id)) != entries_.end();
}

bool ModelRegistry::has_explicit_match_keys(const std::string &id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return false;
    auto it = entries_.find(normalize_id(id));
    if (it == entries_.end()) return false;
    return !it->second.vram_texture_hashes.empty() || !it->second.gte_run_keys.empty();
}

std::string ModelRegistry::find_by_texture_hash(uint64_t tex_hash) const {
    if (!tex_hash) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return {};
    auto it = tex_hash_to_id_.find(tex_hash);
    return it != tex_hash_to_id_.end() ? it->second : std::string{};
}

std::string ModelRegistry::find_by_run_key(uint64_t run_key) const {
    if (!run_key) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return {};
    auto it = run_key_to_id_.find(run_key);
    return it != run_key_to_id_.end() ? it->second : std::string{};
}

std::shared_ptr<const HdModel> ModelRegistry::get_by_texture_hash(uint64_t tex_hash) {
    std::string id = find_by_texture_hash(tex_hash);
    return id.empty() ? std::shared_ptr<const HdModel>{} : get(id);
}

std::shared_ptr<const HdModel> ModelRegistry::get(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return {};
    std::string key = normalize_id(id);
    auto cached = cache_.find(key);
    if (cached != cache_.end()) return cached->second;

    auto it = entries_.find(key);
    if (it == entries_.end()) return {};

    auto model = std::make_shared<HdModel>();
    if (!load_glb(it->second.path, *model)) {
        cache_[key] = {};
        return {};
    }
    model->id = key;
    model->scale = it->second.scale;
    std::memcpy(model->offset, it->second.offset, sizeof(model->offset));
    std::memcpy(model->rotation_euler, it->second.rotation_euler, sizeof(model->rotation_euler));
    model->flip_winding = it->second.flip_winding;
    model->double_sided = it->second.double_sided;
    model->bone_remap = it->second.bone_remap;
    model->vram_texture_hashes = it->second.vram_texture_hashes;
    model->gte_run_keys = it->second.gte_run_keys;
    model->generation = generation_;
    cache_[key] = model;
    return model;
}

bool ModelRegistry::load_glb(const std::string &path, HdModel &out, std::string *error) {
    out = {};
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (error) *error = "cannot open GLB file";
        return false;
    }
    std::streamsize sz = in.tellg();
    if (sz < 20 || sz > 256 * 1024 * 1024) {
        if (error) *error = "invalid GLB size";
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> file(static_cast<size_t>(sz));
    if (!in.read(reinterpret_cast<char *>(file.data()), sz)) {
        if (error) *error = "cannot read GLB";
        return false;
    }

    if (std::memcmp(file.data(), "glTF", 4) != 0 || le32(file.data() + 4) != 2u ||
        le32(file.data() + 8) > file.size()) {
        if (error) *error = "unsupported GLB header";
        return false;
    }

    const uint8_t *json_ptr = nullptr;
    size_t json_len = 0;
    const uint8_t *bin_ptr = nullptr;
    size_t bin_len = 0;

    size_t offset = 12;
    while (offset + 8 <= file.size()) {
        uint32_t clen = le32(file.data() + offset);
        uint32_t ctype = le32(file.data() + offset + 4);
        offset += 8;
        if (clen > file.size() - offset) {
            if (error) *error = "truncated GLB chunk";
            return false;
        }
        if (ctype == 0x4E4F534Au && !json_ptr) { /* "JSON" */
            json_ptr = file.data() + offset;
            json_len = clen;
        } else if (ctype == 0x004E4942u && !bin_ptr) { /* "BIN\0" */
            bin_ptr = file.data() + offset;
            bin_len = clen;
        }
        offset += clen;
    }

    if (!json_ptr || !bin_ptr) {
        if (error) *error = "missing JSON or BIN chunk";
        return false;
    }

    json root;
    try {
        root = json::parse(json_ptr, json_ptr + json_len);
    } catch (...) {
        if (error) *error = "invalid glTF JSON";
        return false;
    }

    /* Decode embedded PNG/JPEG images (bufferView-backed). */
    if (root.contains("images") && root["images"].is_array() && root.contains("bufferViews")) {
        const auto &views = root["bufferViews"];
        for (const auto &img : root["images"]) {
            ModelTexture tex;
            if (img.contains("bufferView")) {
                size_t bv_idx = img["bufferView"].get<size_t>();
                if (bv_idx < views.size()) {
                    size_t off = views[bv_idx].value("byteOffset", size_t(0));
                    size_t len = views[bv_idx].value("byteLength", size_t(0));
                    if (off <= bin_len && len <= bin_len - off) {
                        int w = 0, h = 0, comp = 0;
                        unsigned char *px = stbi_load_from_memory(bin_ptr + off, int(len), &w, &h, &comp, 4);
                        if (px && w > 0 && h > 0) {
                            tex.width = unsigned(w);
                            tex.height = unsigned(h);
                            tex.rgba.assign(px, px + size_t(w) * size_t(h) * 4u);
                        }
                        if (px) stbi_image_free(px);
                    }
                }
            }
            out.textures.push_back(std::move(tex));
        }
    }

    if (!root.contains("meshes") || !root["meshes"].is_array()) {
        if (error) *error = "glTF contains no meshes";
        return false;
    }

    out.has_gltf_skin = root.contains("skins") && root["skins"].is_array() && !root["skins"].empty();

    /* Compute per-node world 4x4 column-major transforms so glTF root/node
     * orientations (such as Z_UP -> Y_UP conversions) are baked into vertices. */
    struct Mat4 {
        float m[16];
        static Mat4 identity() {
            Mat4 r{};
            r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
            return r;
        }
        static Mat4 mul(const Mat4 &a, const Mat4 &b) {
            Mat4 r{};
            for (int c = 0; c < 4; ++c) {
                for (int row = 0; row < 4; ++row) {
                    float s = 0.0f;
                    for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
                    r.m[c * 4 + row] = s;
                }
            }
            return r;
        }
    };
    auto node_local = [](const json &n) -> Mat4 {
        if (n.contains("matrix") && n["matrix"].is_array() && n["matrix"].size() == 16) {
            Mat4 r{};
            for (int i = 0; i < 16; ++i) r.m[i] = n["matrix"][size_t(i)].get<float>();
            return r;
        }
        float tx = 0.0f, ty = 0.0f, tz = 0.0f;
        float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
        float sx = 1.0f, sy = 1.0f, sz = 1.0f;
        if (n.contains("translation") && n["translation"].is_array() && n["translation"].size() == 3) {
            tx = n["translation"][0].get<float>();
            ty = n["translation"][1].get<float>();
            tz = n["translation"][2].get<float>();
        }
        if (n.contains("rotation") && n["rotation"].is_array() && n["rotation"].size() == 4) {
            qx = n["rotation"][0].get<float>();
            qy = n["rotation"][1].get<float>();
            qz = n["rotation"][2].get<float>();
            qw = n["rotation"][3].get<float>();
        }
        if (n.contains("scale") && n["scale"].is_array() && n["scale"].size() == 3) {
            sx = n["scale"][0].get<float>();
            sy = n["scale"][1].get<float>();
            sz = n["scale"][2].get<float>();
        }
        float xx = qx * qx, yy = qy * qy, zz = qz * qz;
        float xy = qx * qy, xz = qx * qz, yz = qy * qz;
        float wx = qw * qx, wy = qw * qy, wz = qw * qz;
        Mat4 r{};
        r.m[0] = (1.0f - 2.0f * (yy + zz)) * sx;
        r.m[1] = (2.0f * (xy + wz)) * sx;
        r.m[2] = (2.0f * (xz - wy)) * sx;
        r.m[3] = 0.0f;
        r.m[4] = (2.0f * (xy - wz)) * sy;
        r.m[5] = (1.0f - 2.0f * (xx + zz)) * sy;
        r.m[6] = (2.0f * (yz + wx)) * sy;
        r.m[7] = 0.0f;
        r.m[8] = (2.0f * (xz + wy)) * sz;
        r.m[9] = (2.0f * (yz - wx)) * sz;
        r.m[10] = (1.0f - 2.0f * (xx + yy)) * sz;
        r.m[11] = 0.0f;
        r.m[12] = tx; r.m[13] = ty; r.m[14] = tz; r.m[15] = 1.0f;
        return r;
    };

    std::vector<Mat4> mesh_world(root["meshes"].size(), Mat4::identity());
    if (root.contains("nodes") && root["nodes"].is_array()) {
        const auto &nodes = root["nodes"];
        size_t ncount = nodes.size();
        std::vector<int> parent(ncount, -1);
        for (size_t i = 0; i < ncount; ++i) {
            if (nodes[i].contains("children") && nodes[i]["children"].is_array()) {
                for (const auto &ch : nodes[i]["children"]) {
                    if (ch.is_number_integer()) {
                        size_t ci = size_t(ch.get<int>());
                        if (ci < ncount) parent[ci] = int(i);
                    }
                }
            }
        }
        std::vector<Mat4> world(ncount, Mat4::identity());
        std::vector<uint8_t> done(ncount, 0);
        auto resolve_world = [&](size_t idx) -> Mat4 {
            std::vector<size_t> chain;
            int cur = int(idx);
            while (cur >= 0 && chain.size() <= ncount) {
                if (done[size_t(cur)]) break;
                chain.push_back(size_t(cur));
                cur = parent[size_t(cur)];
            }
            Mat4 acc = (cur >= 0) ? world[size_t(cur)] : Mat4::identity();
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                acc = Mat4::mul(acc, node_local(nodes[*it]));
                world[*it] = acc;
                done[*it] = 1;
            }
            return world[idx];
        };
        std::vector<uint8_t> mesh_bound(root["meshes"].size(), 0);
        for (size_t i = 0; i < ncount; ++i) {
            if (nodes[i].contains("mesh") && nodes[i]["mesh"].is_number_integer()) {
                size_t mi = size_t(nodes[i]["mesh"].get<int>());
                if (mi < mesh_world.size() && !mesh_bound[mi]) {
                    mesh_world[mi] = resolve_world(i);
                    mesh_bound[mi] = 1;
                }
            }
        }
    }

    for (size_t mesh_idx = 0; mesh_idx < root["meshes"].size(); ++mesh_idx) {
        const auto &mesh = root["meshes"][mesh_idx];
        const Mat4 &wm = mesh_world[mesh_idx];
        if (!mesh.contains("primitives") || !mesh["primitives"].is_array()) continue;
        for (const auto &prim : mesh["primitives"]) {
            if (!prim.contains("attributes") || !prim["attributes"].contains("POSITION")) continue;
            const auto &attrs = prim["attributes"];

            const uint8_t *pos_p = nullptr;
            size_t vcount = 0, pos_stride = 0;
            int pos_ct = 0;
            std::string pos_t;
            if (!read_accessor_view(root, attrs["POSITION"].get<int>(), bin_ptr, bin_len,
                                    pos_p, vcount, pos_stride, pos_ct, pos_t) ||
                pos_ct != 5126 || pos_t != "VEC3" || vcount == 0)
                continue;

            ModelPrimitive mp;
            mp.vertices.resize(vcount);
            for (size_t i = 0; i < vcount; ++i) {
                const uint8_t *p = pos_p + i * pos_stride;
                auto &v = mp.vertices[i];
                float px = le_f32(p), py = le_f32(p + 4), pz = le_f32(p + 8);
                v.pos[0] = wm.m[0] * px + wm.m[4] * py + wm.m[8] * pz + wm.m[12];
                v.pos[1] = wm.m[1] * px + wm.m[5] * py + wm.m[9] * pz + wm.m[13];
                v.pos[2] = wm.m[2] * px + wm.m[6] * py + wm.m[10] * pz + wm.m[14];
                v.normal[0] = 0.0f; v.normal[1] = -1.0f; v.normal[2] = 0.0f;
                v.uv[0] = 0.0f; v.uv[1] = 0.0f;
                v.joints[0] = v.joints[1] = v.joints[2] = v.joints[3] = 0;
                v.weights[0] = 1.0f; v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
            }

            bool has_normals = false;
            if (attrs.contains("NORMAL")) {
                const uint8_t *np = nullptr; size_t nc = 0, ns = 0; int nct = 0; std::string nt;
                if (read_accessor_view(root, attrs["NORMAL"].get<int>(), bin_ptr, bin_len, np, nc, ns, nct, nt) &&
                    nct == 5126 && nt == "VEC3" && nc == vcount) {
                    has_normals = true;
                    for (size_t i = 0; i < vcount; ++i) {
                        const uint8_t *p = np + i * ns;
                        float nx = le_f32(p), ny = le_f32(p + 4), nz = le_f32(p + 8);
                        float tx = wm.m[0] * nx + wm.m[4] * ny + wm.m[8] * nz;
                        float ty = wm.m[1] * nx + wm.m[5] * ny + wm.m[9] * nz;
                        float tz = wm.m[2] * nx + wm.m[6] * ny + wm.m[10] * nz;
                        float len = std::sqrt(tx * tx + ty * ty + tz * tz);
                        if (len > 1e-6f) { tx /= len; ty /= len; tz /= len; }
                        mp.vertices[i].normal[0] = tx;
                        mp.vertices[i].normal[1] = ty;
                        mp.vertices[i].normal[2] = tz;
                    }
                }
            }

            if (attrs.contains("TEXCOORD_0")) {
                const uint8_t *tp = nullptr; size_t tc = 0, ts = 0; int tct = 0; std::string tt;
                if (read_accessor_view(root, attrs["TEXCOORD_0"].get<int>(), bin_ptr, bin_len, tp, tc, ts, tct, tt) &&
                    tct == 5126 && tt == "VEC2" && tc == vcount) {
                    for (size_t i = 0; i < vcount; ++i) {
                        const uint8_t *p = tp + i * ts;
                        mp.vertices[i].uv[0] = le_f32(p);
                        mp.vertices[i].uv[1] = le_f32(p + 4);
                    }
                }
            }

            if (attrs.contains("JOINTS_0")) {
                const uint8_t *jp = nullptr; size_t jc = 0, js = 0; int jct = 0; std::string jt;
                if (read_accessor_view(root, attrs["JOINTS_0"].get<int>(), bin_ptr, bin_len, jp, jc, js, jct, jt) &&
                    jt == "VEC4" && jc == vcount) {
                    for (size_t i = 0; i < vcount; ++i) {
                        const uint8_t *p = jp + i * js;
                        for (int k = 0; k < 4; ++k) {
                            mp.vertices[i].joints[k] = (jct == 5121) ? uint16_t(p[k]) :
                                                       (jct == 5123) ? le16(p + k * 2) : 0u;
                        }
                    }
                }
            }

            if (attrs.contains("WEIGHTS_0")) {
                const uint8_t *wp = nullptr; size_t wc = 0, ws = 0; int wct = 0; std::string wt;
                if (read_accessor_view(root, attrs["WEIGHTS_0"].get<int>(), bin_ptr, bin_len, wp, wc, ws, wct, wt) &&
                    (wct == 5126 || wct == 5121 || wct == 5123) && wt == "VEC4" && wc == vcount) {
                    for (size_t i = 0; i < vcount; ++i) {
                        const uint8_t *p = wp + i * ws;
                        float sum = 0.0f;
                        for (int k = 0; k < 4; ++k) {
                            float w = (wct == 5126) ? le_f32(p + k * 4) :
                                      (wct == 5121) ? float(p[k]) / 255.0f :
                                      float(le16(p + k * 2)) / 65535.0f;
                            mp.vertices[i].weights[k] = w;
                            sum += w;
                        }
                        if (sum > 1e-6f) {
                            for (int k = 0; k < 4; ++k) mp.vertices[i].weights[k] /= sum;
                        } else {
                            mp.vertices[i].weights[0] = 1.0f;
                            mp.vertices[i].weights[1] = mp.vertices[i].weights[2] = mp.vertices[i].weights[3] = 0.0f;
                        }
                    }
                }
            }

            if (prim.contains("indices")) {
                const uint8_t *ip = nullptr; size_t ic = 0, is = 0; int ict = 0; std::string it;
                if (read_accessor_view(root, prim["indices"].get<int>(), bin_ptr, bin_len, ip, ic, is, ict, it) &&
                    it == "SCALAR") {
                    mp.indices.reserve(ic);
                    for (size_t i = 0; i < ic; ++i) {
                        const uint8_t *p = ip + i * is;
                        uint32_t idx = (ict == 5121) ? uint32_t(p[0]) :
                                       (ict == 5123) ? uint32_t(le16(p)) :
                                       (ict == 5125) ? le32(p) : 0u;
                        if (idx < vcount) mp.indices.push_back(idx);
                    }
                }
            } else {
                mp.indices.resize(vcount);
                for (size_t i = 0; i < vcount; ++i) mp.indices[i] = uint32_t(i);
            }

            if (!has_normals && mp.indices.size() >= 3) {
                compute_smooth_normals(mp);
            }

            if (prim.contains("material") && root.contains("materials")) {
                size_t midx = prim["material"].get<size_t>();
                if (midx < root["materials"].size()) {
                    const auto &mat = root["materials"][midx];
                    mp.double_sided = mat.value("doubleSided", false);
                    mp.alpha_cutoff = mat.value("alphaCutoff", 0.5f);
                    std::string amode = mat.value("alphaMode", std::string("OPAQUE"));
                    if (amode == "MASK") mp.alpha_mode = AlphaMode::Mask;
                    else if (amode == "BLEND") mp.alpha_mode = AlphaMode::Blend;
                    else mp.alpha_mode = AlphaMode::Opaque;

                    if (mat.contains("pbrMetallicRoughness") && mat["pbrMetallicRoughness"].is_object()) {
                        const auto &pbr = mat["pbrMetallicRoughness"];
                        if (pbr.contains("baseColorFactor") && pbr["baseColorFactor"].is_array() &&
                            pbr["baseColorFactor"].size() == 4) {
                            for (size_t c = 0; c < 4; ++c)
                                if (pbr["baseColorFactor"][c].is_number())
                                    mp.base_color_factor[c] = pbr["baseColorFactor"][c].get<float>();
                        }
                        if (pbr.contains("baseColorTexture")) {
                            size_t tidx = pbr["baseColorTexture"].value("index", size_t(~0u));
                            if (root.contains("textures") && tidx < root["textures"].size()) {
                                int src = root["textures"][tidx].value("source", -1);
                                if (src >= 0 && size_t(src) < out.textures.size()) mp.texture_index = src;
                            }
                        }
                    }
                }
            }

            if (!mp.indices.empty()) out.primitives.push_back(std::move(mp));
        }
    }

    if (out.primitives.empty()) {
        if (error) *error = "glTF contains no valid primitives";
        return false;
    }
    return true;
}

GteBoneMatrix ModelRegistry::from_gte_setup(const uint32_t rotation[5], const int32_t translation[3]) {
    auto s16_lo = [](uint32_t w) -> float { return float(int16_t(w & 0xffffu)) / 4096.0f; };
    auto s16_hi = [](uint32_t w) -> float { return float(int16_t((w >> 16) & 0xffffu)) / 4096.0f; };

    GteBoneMatrix m{};
    m.r[0][0] = s16_lo(rotation[0]); m.r[0][1] = s16_hi(rotation[0]); m.r[0][2] = s16_lo(rotation[1]);
    m.r[1][0] = s16_hi(rotation[1]); m.r[1][1] = s16_lo(rotation[2]); m.r[1][2] = s16_hi(rotation[2]);
    m.r[2][0] = s16_lo(rotation[3]); m.r[2][1] = s16_hi(rotation[3]); m.r[2][2] = s16_lo(rotation[4]);
    m.t[0] = float(translation[0]);
    m.t[1] = float(translation[1]);
    m.t[2] = float(translation[2]);
    return m;
}

void ModelRegistry::evaluate_pose(const HdModel &model, const GteBoneMatrix *bones, size_t bone_count,
                                  float ofx, float ofy, float h,
                                  std::vector<std::vector<ProjectedVertex>> &out_primitives) {
    out_primitives.clear();
    if (!bones || bone_count == 0) return;

    float lm[3][4];
    model.local_transform(lm);

    out_primitives.resize(model.primitives.size());
    for (size_t pi = 0; pi < model.primitives.size(); ++pi) {
        const auto &prim = model.primitives[pi];
        auto &dst = out_primitives[pi];
        dst.resize(prim.vertices.size());

        for (size_t vi = 0; vi < prim.vertices.size(); ++vi) {
            const auto &v = prim.vertices[vi];
            float vx = lm[0][0] * v.pos[0] + lm[0][1] * v.pos[1] + lm[0][2] * v.pos[2] + lm[0][3];
            float vy = lm[1][0] * v.pos[0] + lm[1][1] * v.pos[1] + lm[1][2] * v.pos[2] + lm[1][3];
            float vz = lm[2][0] * v.pos[0] + lm[2][1] * v.pos[1] + lm[2][2] * v.pos[2] + lm[2][3];

            float wx = 0.0f, wy = 0.0f, wz = 0.0f;
            float wsum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                float w = v.weights[k];
                if (w <= 0.0f) continue;
                size_t bi = v.joints[k];
                if (bi < model.bone_remap.size() && model.bone_remap[bi] >= 0) {
                    bi = size_t(model.bone_remap[bi]);
                }
                if (bi >= bone_count) bi = 0;
                const auto &bm = bones[bi];
                wx += w * (bm.r[0][0] * vx + bm.r[0][1] * vy + bm.r[0][2] * vz + bm.t[0]);
                wy += w * (bm.r[1][0] * vx + bm.r[1][1] * vy + bm.r[1][2] * vz + bm.t[1]);
                wz += w * (bm.r[2][0] * vx + bm.r[2][1] * vy + bm.r[2][2] * vz + bm.t[2]);
                wsum += w;
            }
            if (wsum <= 1e-6f) {
                const auto &bm = bones[0];
                wx = bm.r[0][0] * vx + bm.r[0][1] * vy + bm.r[0][2] * vz + bm.t[0];
                wy = bm.r[1][0] * vx + bm.r[1][1] * vy + bm.r[1][2] * vz + bm.t[1];
                wz = bm.r[2][0] * vx + bm.r[2][1] * vy + bm.r[2][2] * vz + bm.t[2];
            }

            float sz = std::max(1.0f, wz);
            float inv = h / sz;
            dst[vi] = {ofx + wx * inv, ofy + wy * inv, sz, v.uv[0], v.uv[1]};
        }
    }
}

uint64_t ModelRegistry::hash_vram_rect(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *vram) {
    if (!vram || !w || !h) return 0;
    uint64_t hash = kFnvOffset;
    hash = fnv_u16(hash, uint16_t(w));
    hash = fnv_u16(hash, uint16_t(h));
    for (unsigned row = 0; row < h; ++row) {
        unsigned ry = (y + row) & 511u;
        for (unsigned col = 0; col < w; ++col) {
            unsigned rx = (x + col) & 1023u;
            hash = fnv_u16(hash, vram[ry * 1024u + rx]);
        }
    }
    return hash;
}

/* ---------------- HdSceneTracker ---------------- */

void HdSceneTracker::reset() {
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        live_capture_.clear();
    }
    snap_current_.clear();
    for (auto &h : snap_history_) h.clear();
    snapshot_pulled_ = false;
    vram_uploads_.clear();
    clusters_.clear();
    cached_poses_.clear();
    run_to_cluster_.clear();
    claimed_scene_ids_.clear();
    frame_max_sup_by_id_.clear();
    last_tint_by_id_.clear();
    pending_draws_.clear();
    last_scene_mode_ = SceneMode::Unknown;
    last_field_id_ = 0;
    frame_counter_ = 1;
    have_last_offset_ = false;
    suppressed_this_frame_ = 0;
}

void HdSceneTracker::invalidate_cached_poses_for_hash(uint64_t hash) {
    if (!hash || cached_poses_.empty()) return;
    /* Keep cached poses if another live VRAM upload still holds the same hash. */
    for (const auto &u : vram_uploads_) {
        if (u.hash == hash) return;
    }
    cached_poses_.erase(
        std::remove_if(cached_poses_.begin(), cached_poses_.end(),
                       [&](const CachedPose &cp) { return cp.tex_hashes.count(hash) != 0; }),
        cached_poses_.end());
}

void HdSceneTracker::record_vram_upload(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *vram) {
    if (!vram || !w || !h) return;
    /* Ignore tiny palette-only strips or huge full-VRAM snapshot uploads. */
    if (w < 4 || h < 4 || (w >= 512 && h >= 256)) return;
    uint64_t hash = ModelRegistry::hash_vram_rect(x, y, w, h, vram);
    if (!hash) return;

    std::vector<uint64_t> evicted_hashes;
    /* Remove entries completely covered by this upload. */
    vram_uploads_.erase(
        std::remove_if(vram_uploads_.begin(), vram_uploads_.end(),
                       [&](const VramUpload &u) {
                           bool covered = (u.x >= x && u.y >= y && (u.x + u.w) <= (x + w) && (u.y + u.h) <= (y + h));
                           if (covered && u.hash != hash) evicted_hashes.push_back(u.hash);
                           return covered;
                       }),
        vram_uploads_.end());

    if (vram_uploads_.size() >= 512) vram_uploads_.erase(vram_uploads_.begin());
    vram_uploads_.push_back({x, y, w, h, hash});
    for (uint64_t eh : evicted_hashes) invalidate_cached_poses_for_hash(eh);
    if (std::getenv("FF8_HD_DEBUG"))
        std::fprintf(stderr, "HD_MOD: vram_upload (%u,%u %ux%u) hash=%016llx\n", x, y, w, h, (unsigned long long)hash);
}

void HdSceneTracker::clear_vram_region(unsigned x, unsigned y, unsigned w, unsigned h) {
    std::vector<uint64_t> evicted_hashes;
    vram_uploads_.erase(
        std::remove_if(vram_uploads_.begin(), vram_uploads_.end(),
                       [&](const VramUpload &u) {
                           unsigned ox0 = std::max(u.x, x);
                           unsigned oy0 = std::max(u.y, y);
                           unsigned ox1 = std::min(u.x + u.w, x + w);
                           unsigned oy1 = std::min(u.y + u.h, y + h);
                           if (ox1 <= ox0 || oy1 <= oy0) return false;
                           uint64_t overlap = uint64_t(ox1 - ox0) * uint64_t(oy1 - oy0);
                           uint64_t area = uint64_t(u.w) * uint64_t(u.h);
                           bool evict = (overlap * 4u >= area);
                           if (evict) evicted_hashes.push_back(u.hash);
                           return evict;
                       }),
        vram_uploads_.end());
    for (uint64_t eh : evicted_hashes) invalidate_cached_poses_for_hash(eh);
}

uint64_t HdSceneTracker::lookup_vram_hash(unsigned tex_page_x, unsigned tex_page_y, unsigned tex_depth,
                                          int u0, int v0, int u1, int v1) const {
    if (vram_uploads_.empty()) return 0;
    unsigned shift = (tex_depth == 0) ? 2u : (tex_depth == 1u) ? 1u : 0u;
    unsigned rx0 = tex_page_x + (unsigned(std::max(0, u0)) >> shift);
    unsigned rx1 = tex_page_x + (unsigned(std::max(0, u1)) >> shift);
    unsigned ry0 = tex_page_y + unsigned(std::max(0, v0));
    unsigned ry1 = tex_page_y + unsigned(std::max(0, v1));

    /* Pass 1: Search newest uploads first for the tightest upload rect completely
     * containing the triangle's UV bounds. */
    uint64_t best_hash = 0;
    uint64_t best_area = UINT64_MAX;
    for (size_t i = vram_uploads_.size(); i-- > 0;) {
        const auto &u = vram_uploads_[i];
        if (rx0 >= u.x && ry0 >= u.y && rx1 < u.x + u.w && ry1 < u.y + u.h) {
            uint64_t area = uint64_t(u.w) * uint64_t(u.h);
            if (area < best_area) {
                best_area = area;
                best_hash = u.hash;
            }
        }
    }
    if (best_hash) return best_hash;

    /* Pass 2: Fallback for FF8 character sheets split across a 1-row CLUT strip
     * (e.g. VRAM y=240..247), where a head/neck triangle's V spans [238..248]. */
    unsigned best_y_overlap = 0;
    uint64_t best_fb_area = UINT64_MAX;
    for (size_t i = vram_uploads_.size(); i-- > 0;) {
        const auto &u = vram_uploads_[i];
        if (rx0 < u.x || rx1 >= u.x + u.w) continue;
        unsigned oy0 = std::max(ry0, u.y);
        unsigned oy1 = std::min(ry1 + 1u, u.y + u.h);
        if (oy1 <= oy0) continue;
        unsigned y_ov = oy1 - oy0;
        uint64_t area = uint64_t(u.w) * uint64_t(u.h);
        if (y_ov > best_y_overlap || (y_ov == best_y_overlap && area < best_fb_area)) {
            best_y_overlap = y_ov;
            best_fb_area = area;
            best_hash = u.hash;
        }
    }
    return best_hash;
}

void HdSceneTracker::record_gte_projection(const uint32_t rotation[5], const int32_t translation[3],
                                           int32_t ofx_q16, int32_t ofy_q16, uint32_t h,
                                           const int16_t input_v[3], uint32_t packed_sxy,
                                           float sx_subpx, float sy_subpx, uint32_t sz,
                                           const SceneContext &ctx) {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    /* Cap runaway frames so capture memory stays bounded. */
    if (live_capture_.runs.size() >= 8192) return;

    live_capture_.scene = ctx;
    bool same_run = false;
    if (!live_capture_.runs.empty()) {
        const auto &back = live_capture_.runs.back();
        same_run = (std::memcmp(back.raw_rot, rotation, sizeof(back.raw_rot)) == 0 &&
                    std::memcmp(back.raw_tr, translation, sizeof(back.raw_tr)) == 0 &&
                    back.raw_ofx == ofx_q16 && back.raw_ofy == ofy_q16 && back.raw_h == h);
    }

    if (!same_run) {
        GteTransformRun r{};
        std::memcpy(r.raw_rot, rotation, sizeof(r.raw_rot));
        std::memcpy(r.raw_tr, translation, sizeof(r.raw_tr));
        r.raw_ofx = ofx_q16;
        r.raw_ofy = ofy_q16;
        r.raw_h = h;
        r.bone = ModelRegistry::from_gte_setup(rotation, translation);
        r.ofx = float(ofx_q16) / 65536.0f;
        r.ofy = float(ofy_q16) / 65536.0f;
        r.h = float(h & 0xffffu);
        r.run_key = kFnvOffset;
        live_capture_.runs.push_back(r);
    }

    uint32_t run_idx = uint32_t(live_capture_.runs.size() - 1u);
    auto &run = live_capture_.runs[run_idx];
    run.vertex_count++;
    uint64_t packed_in = uint64_t(uint16_t(input_v[0])) |
                         (uint64_t(uint16_t(input_v[1])) << 16) |
                         (uint64_t(uint16_t(input_v[2])) << 32);
    run.run_key = mix64(run.run_key, packed_in);
    run.min_x = std::min(run.min_x, sx_subpx);
    run.min_y = std::min(run.min_y, sy_subpx);
    run.max_x = std::max(run.max_x, sx_subpx);
    run.max_y = std::max(run.max_y, sy_subpx);
    float fsz = float(sz & 0xffffu);
    run.min_sz = std::min(run.min_sz, fsz);
    run.max_sz = std::max(run.max_sz, fsz);
    run.sum_sz += fsz;

    uint32_t entry_idx = uint32_t(live_capture_.sxy_pool.size());
    uint32_t prev_head = UINT32_MAX;
    auto hit = live_capture_.sxy_head.find(packed_sxy);
    if (hit != live_capture_.sxy_head.end()) {
        /* Avoid duplicate entries for the exact same run_index at this pixel. */
        uint32_t cur = hit->second;
        int depth = 0;
        while (cur != UINT32_MAX && depth < 8) {
            if (live_capture_.sxy_pool[cur].run_index == run_idx) {
                live_capture_.sxy_pool[cur].sz = uint16_t(sz & 0xffffu);
                return;
            }
            cur = live_capture_.sxy_pool[cur].next;
            ++depth;
        }
        prev_head = hit->second;
    }
    if (live_capture_.sxy_pool.size() < 262144u) {
        live_capture_.sxy_pool.push_back(SxyRecord{run_idx, packed_sxy, uint16_t(sz & 0xffffu), prev_head});
        live_capture_.sxy_head[packed_sxy] = entry_idx;
    }
}

const HdSceneTracker::CaptureBuffer &HdSceneTracker::capture_for_slot(int slot) const {
    if (slot <= 0) return snap_current_;
    int idx = std::min(slot - 1, kHistorySlots - 1);
    return snap_history_[size_t(idx)];
}

void HdSceneTracker::ensure_snapshot_ready() {
    if (snapshot_pulled_) return;
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (!live_capture_.runs.empty()) {
        snap_current_ = live_capture_;
    }
    snapshot_pulled_ = true;
}

bool HdSceneTracker::find_projection(const CaptureBuffer &cap, int vx, int vy, int ox, int oy,
                                     SxyRecord &out, uint32_t prefer_run) const {
    if (cap.sxy_head.empty()) return false;
    int sx = vx - ox;
    int sy = vy - oy;
    uint32_t key = (uint32_t(sx) & 0xffffu) | ((uint32_t(sy) & 0xffffu) << 16);
    auto it = cap.sxy_head.find(key);
    if (it == cap.sxy_head.end()) return false;

    uint32_t best_idx = UINT32_MAX;
    uint32_t best_dist = UINT32_MAX;
    bool best_is_mesh = false;
    uint32_t cur = it->second;
    int depth = 0;
    while (cur != UINT32_MAX && depth < 16) {
        const auto &rec = cap.sxy_pool[cur];
        bool is_mesh = (rec.run_index < cap.runs.size() && cap.runs[rec.run_index].vertex_count >= 3u);
        uint32_t dist = (prefer_run == UINT32_MAX)
                            ? 0u
                            : (rec.run_index > prefer_run ? (rec.run_index - prefer_run) : (prefer_run - rec.run_index));
        if (best_idx == UINT32_MAX ||
            (is_mesh && !best_is_mesh) ||
            (is_mesh == best_is_mesh && dist < best_dist)) {
            best_idx = cur;
            best_dist = dist;
            best_is_mesh = is_mesh;
            if (is_mesh && dist == 0) break;
        }
        cur = rec.next;
        ++depth;
    }
    if (best_idx == UINT32_MAX) return false;
    out = cap.sxy_pool[best_idx];
    return true;
}

uint32_t HdSceneTracker::find_or_create_cluster(int cap_slot, uint32_t run_idx, float draw_ox, float draw_oy) {
    uint64_t rk = (uint64_t(uint32_t(cap_slot)) << 32) | uint64_t(run_idx);
    auto it = run_to_cluster_.find(rk);
    if (it != run_to_cluster_.end()) return it->second;

    const CaptureBuffer &cap = capture_for_slot(cap_slot);
    if (run_idx >= cap.runs.size()) return UINT32_MAX;

    /* Cluster contiguous mesh bone runs around run_idx that share the same camera
     * (ofx, ofy, h), separating real triangle mesh runs (>=3 verts) from 1-vertex
     * skeleton-propagation RTPS calls while preserving tiny synthetic test clusters. */
    const bool cluster_is_mesh = (cap.runs[run_idx].vertex_count >= 3u);
    auto same_skeleton = [&](const GteTransformRun &a, const GteTransformRun &b) -> bool {
        if (a.raw_ofx != b.raw_ofx || a.raw_ofy != b.raw_ofy || a.raw_h != b.raw_h) return false;
        if (cluster_is_mesh && (a.vertex_count < 3u || b.vertex_count < 3u)) return false;
        double dx = double(a.bone.t[0]) - double(b.bone.t[0]);
        double dy = double(a.bone.t[1]) - double(b.bone.t[1]);
        double dz = double(a.bone.t[2]) - double(b.bone.t[2]);
        double dist2 = dx * dx + dy * dy + dz * dz;
        /* Adjacent bones of one FF8 character sit within ~450 GTE units; separate NPCs are farther. */
        return dist2 <= (450.0 * 450.0);
    };

    uint32_t first = run_idx;
    while (first > 0 && (run_idx - first) < kMaxHdBones - 1 &&
           same_skeleton(cap.runs[first - 1], cap.runs[first])) {
        --first;
    }
    uint32_t last = run_idx;
    while (last + 1 < cap.runs.size() && (last - first) < kMaxHdBones - 1 &&
           same_skeleton(cap.runs[last], cap.runs[last + 1])) {
        ++last;
    }

    uint32_t cid = uint32_t(clusters_.size());
    ClusterState cs;
    cs.first_run = first;
    cs.run_count = last - first + 1u;
    cs.capture_slot = cap_slot;
    cs.draw_offset_x = draw_ox;
    cs.draw_offset_y = draw_oy;
    clusters_.push_back(std::move(cs));

    for (uint32_t r = first; r <= last; ++r) {
        uint64_t k = (uint64_t(uint32_t(cap_slot)) << 32) | uint64_t(r);
        run_to_cluster_[k] = cid;
    }
    return cid;
}

int HdSceneTracker::evaluate_run_compat(int cap_slot, uint32_t run_idx, const std::string &tri_model_id) const {
    if (tri_model_id.empty()) return 0;

    uint64_t rk = (uint64_t(uint32_t(cap_slot)) << 32) | uint64_t(run_idx);
    auto it = run_to_cluster_.find(rk);
    if (it != run_to_cluster_.end() && it->second < clusters_.size()) {
        const auto &cl = clusters_[it->second];
        if (cl.matched && !cl.model_id.empty()) {
            return (cl.model_id == tri_model_id) ? 2 : -2;
        }
    }

    if (cached_poses_.empty()) return 0;
    const CaptureBuffer &cap = capture_for_slot(cap_slot);
    if (run_idx >= cap.runs.size()) return 0;
    const auto &rt = cap.runs[run_idx].bone.t;

    double d2_same = 1e30;
    double d2_other = 1e30;
    for (const auto &cp : cached_poses_) {
        if (cp.bones.empty() || cp.model_id.empty()) continue;
        double best_cp = 1e30;
        for (const auto &b : cp.bones) {
            double dx = double(rt[0]) - double(b.t[0]);
            double dy = double(rt[1]) - double(b.t[1]);
            double dz = double(rt[2]) - double(b.t[2]);
            double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_cp) best_cp = d2;
        }
        if (cp.model_id == tri_model_id) {
            if (best_cp < d2_same) d2_same = best_cp;
        } else {
            if (best_cp < d2_other) d2_other = best_cp;
        }
    }

    if (d2_same < (260.0 * 260.0) && d2_same <= d2_other * 1.1) return 1;
    if (d2_other < (220.0 * 220.0) && d2_other * 1.4 < d2_same) return -1;
    return 0;
}

void HdSceneTracker::evaluate_cluster_match(ClusterState &cluster, uint64_t tex_hash, ModelRegistry &registry) {
    const CaptureBuffer &cap = capture_for_slot(cluster.capture_slot);

    /* Priority 1: Explicit VRAM texture hash match (can trigger even after initial evaluation
     * if earlier triangles of the same cluster were untextured). */
    if (tex_hash != 0 && !cluster.matched) {
        std::string id = registry.find_by_texture_hash(tex_hash);
        if (std::getenv("FF8_HD_DEBUG") && !id.empty())
            std::fprintf(stderr, "HD_MOD: tex_hash=%016llx -> '%s' runs=%u\n",
                         (unsigned long long)tex_hash, id.c_str(), cluster.run_count);
        if (!id.empty()) {
            auto m = registry.get(id);
            if (m) {
                /* Guard against spurious single-bone/effect overlaps (e.g. a save-point ring
                 * sharing screen pixels with a character's feet) stealing an articulated model. */
                size_t required_runs = 1;
                if (m->bone_remap.size() >= 6) required_runs = m->bone_remap.size() / 2;
                if (cluster.run_count < required_runs) return;
                cluster.model_id = std::move(id);
                cluster.model = std::move(m);
                cluster.matched = true;
                cluster.evaluated = true;
                return;
            }
        }
    }

    if (cluster.evaluated) return;
    cluster.evaluated = true;

    /* Priority 2: Explicit GTE run key match from manifest.json. */
    for (uint32_t i = 0; i < cluster.run_count; ++i) {
        const auto &r = cap.runs[cluster.first_run + i];
        std::string id = registry.find_by_run_key(r.run_key);
        if (!id.empty()) {
            auto m = registry.get(id);
            if (m) {
                cluster.model_id = std::move(id);
                cluster.model = std::move(m);
                cluster.matched = true;
                return;
            }
        }
    }

    /* Priority 3: Canonical FF8 scene entity ID candidates (Field / Battle / World).
     * Never hijack single-bone terrain/arena/sky/weapon clusters or full-screen
     * background geometry; articulated FF8 characters always span >= 2 bone runs
     * inside a compact screen-space bounding box. */
    if (cluster.run_count < 2) return;
    bool plausible_3d = false;
    uint32_t total_verts = 0;
    float c_min_x = 1e9f, c_min_y = 1e9f, c_max_x = -1e9f, c_max_y = -1e9f;
    for (uint32_t i = 0; i < cluster.run_count; ++i) {
        const auto &r = cap.runs[cluster.first_run + i];
        total_verts += r.vertex_count;
        if (r.avg_sz() > 8.0f) plausible_3d = true;
        c_min_x = std::min(c_min_x, r.min_x);
        c_min_y = std::min(c_min_y, r.min_y);
        c_max_x = std::max(c_max_x, r.max_x);
        c_max_y = std::max(c_max_y, r.max_y);
    }
    if (!plausible_3d || total_verts < 3) return;
    if ((c_max_x - c_min_x) > 260.0f || (c_max_y - c_min_y) > 220.0f) return;

    std::vector<std::string> candidates;
    const auto &sc = cap.scene;
    if (sc.mode == SceneMode::Field) {
        for (int slot = 0; slot < 3; ++slot) {
            uint8_t pid = sc.party[slot];
            if (pid < 16) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "field/d%03u", unsigned(pid));
                candidates.emplace_back(buf);
            }
        }
        candidates.emplace_back("field/d000");
        if (sc.field_id > 0) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "field/f%u_0", unsigned(sc.field_id));
            candidates.emplace_back(buf);
        }
    } else if (sc.mode == SceneMode::Battle) {
        static const char *const kBattlePartyIds[11][2] = {
            {"battle/d0c000", "battle/d0c001"}, /* 0: Squall */
            {"battle/d1c003", "battle/d1c004"}, /* 1: Zell */
            {"battle/d2c006", nullptr},         /* 2: Irvine */
            {"battle/d3c007", nullptr},         /* 3: Quistis */
            {"battle/d4c009", nullptr},         /* 4: Rinoa */
            {"battle/d5c011", "battle/d5c012"}, /* 5: Selphie */
            {"battle/d6c014", nullptr},         /* 6: Seifer */
            {"battle/d7c016", nullptr},         /* 7: Edea */
            {"battle/d8c017", "battle/d8c018"}, /* 8: Laguna */
            {"battle/d9c019", "battle/d9c020"}, /* 9: Kiros */
            {"battle/dac021", "battle/dac022"}, /* 10: Ward */
        };
        for (int slot = 0; slot < 3; ++slot) {
            uint8_t pid = sc.party[slot];
            if (pid < 11) {
                if (kBattlePartyIds[pid][0]) candidates.emplace_back(kBattlePartyIds[pid][0]);
                if (kBattlePartyIds[pid][1]) candidates.emplace_back(kBattlePartyIds[pid][1]);
            }
            if (pid < 16) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "battle/c0m%03u", unsigned(pid) + 1u);
                candidates.emplace_back(buf);
            }
        }
        candidates.emplace_back("battle/d0c000");
        candidates.emplace_back("battle/c0m001");
    } else if (sc.mode == SceneMode::World) {
        if (const char *nm = party_world_name(sc.party[0])) {
            candidates.emplace_back(std::string("world/") + nm);
        }
        candidates.emplace_back("world/squall");
        candidates.emplace_back("world/c0w000");
        candidates.emplace_back("world/balamb_garden");
        candidates.emplace_back("world/garden");
        candidates.emplace_back("world/ragnarok");
        candidates.emplace_back("world/chocobo");
        candidates.emplace_back("world/car");
        candidates.emplace_back("world/train");
    }
    /* Optional wildcard IDs for developer/user synthetic test packs. */
    candidates.emplace_back("default");
    candidates.emplace_back("any");

    for (const auto &cid : candidates) {
        if (claimed_scene_ids_.count(cid)) continue;
        if (!registry.has_model(cid) || registry.has_explicit_match_keys(cid)) continue;
        auto m = registry.get(cid);
        if (m) {
            claimed_scene_ids_.insert(cid);
            cluster.model_id = cid;
            cluster.model = std::move(m);
            cluster.matched = true;
            return;
        }
    }
}

bool HdSceneTracker::bboxes_overlap(int ax0, int ay0, int ax1, int ay1,
                                    int bx0, int by0, int bx1, int by1) {
    return ax0 <= bx1 && bx0 <= ax1 && ay0 <= by1 && by0 <= ay1;
}

void HdSceneTracker::cache_live_cluster(ClusterState &cluster, uint64_t tex_hash) {
    if (cluster.capture_slot < 0 || cluster.capture_slot >= kCachedSlot || !cluster.matched || !cluster.model) return;
    const CaptureBuffer &cap = capture_for_slot(cluster.capture_slot);
    if (cluster.first_run + cluster.run_count > cap.runs.size()) return;

    if (cluster.pose_cached && cluster.cached_index < cached_poses_.size()) {
        auto &cp = cached_poses_[cluster.cached_index];
        if (tex_hash != 0) cp.tex_hashes.insert(tex_hash);
        cp.last_seen_frame = frame_counter_;
        return;
    }

    CachedPose pose;
    pose.model_id = cluster.model_id;
    pose.model = cluster.model;
    pose.bones.reserve(cluster.run_count);
    for (uint32_t i = 0; i < cluster.run_count; ++i) {
        pose.bones.push_back(cap.runs[cluster.first_run + i].bone);
    }
    const auto &root_run = cap.runs[cluster.first_run];
    pose.ofx = root_run.ofx;
    pose.ofy = root_run.ofy;
    pose.h = root_run.h;
    for (uint64_t h : cluster.model->vram_texture_hashes) {
        if (h != 0) pose.tex_hashes.insert(h);
    }
    if (tex_hash != 0) pose.tex_hashes.insert(tex_hash);

    uint32_t run_end = cluster.first_run + cluster.run_count;
    for (const auto &rec : cap.sxy_pool) {
        if (rec.run_index >= cluster.first_run && rec.run_index < run_end) {
            int sx = int(int16_t(rec.packed_sxy & 0xffffu));
            int sy = int(int16_t((rec.packed_sxy >> 16) & 0xffffu));
            pose.min_sx = std::min(pose.min_sx, sx);
            pose.min_sy = std::min(pose.min_sy, sy);
            pose.max_sx = std::max(pose.max_sx, sx);
            pose.max_sy = std::max(pose.max_sy, sy);
            pose.sxy_to_sz[rec.packed_sxy] = rec.sz;
        }
    }
    if (pose.sxy_to_sz.size() < 3) return;

    pose.last_live_frame = frame_counter_;
    pose.last_seen_frame = frame_counter_;
    pose.frame_cluster_id = UINT32_MAX;

    /* Replace an existing cached pose for the same model from an earlier frame
     * (picking the closest root position if multiple NPCs share one model_id). */
    size_t replace_idx = SIZE_MAX;
    double best_dist2 = 1e30;
    for (size_t i = 0; i < cached_poses_.size(); ++i) {
        const auto &existing = cached_poses_[i];
        if (existing.model_id != pose.model_id) continue;
        if (existing.last_live_frame == frame_counter_) continue;
        double d2 = 0.0;
        if (!existing.bones.empty() && !pose.bones.empty()) {
            double dx = double(existing.bones[0].t[0]) - double(pose.bones[0].t[0]);
            double dy = double(existing.bones[0].t[1]) - double(pose.bones[0].t[1]);
            double dz = double(existing.bones[0].t[2]) - double(pose.bones[0].t[2]);
            d2 = dx * dx + dy * dy + dz * dz;
        }
        if (d2 < best_dist2) {
            best_dist2 = d2;
            replace_idx = i;
        }
    }

    if (replace_idx != SIZE_MAX) {
        /* Preserve any extra VRAM texture hashes observed on previous frames. */
        for (uint64_t h : cached_poses_[replace_idx].tex_hashes) pose.tex_hashes.insert(h);
        cached_poses_[replace_idx] = std::move(pose);
        cluster.cached_index = replace_idx;
    } else {
        if (cached_poses_.size() >= 32) {
            size_t oldest = 0;
            for (size_t i = 1; i < cached_poses_.size(); ++i) {
                if (cached_poses_[i].last_seen_frame < cached_poses_[oldest].last_seen_frame) oldest = i;
            }
            cached_poses_[oldest] = std::move(pose);
            cluster.cached_index = oldest;
        } else {
            cluster.cached_index = cached_poses_.size();
            cached_poses_.push_back(std::move(pose));
        }
    }
    cluster.pose_cached = true;
}

ObserveDecision HdSceneTracker::observe_triangle(const TriangleObservation &tri, ModelRegistry &registry) {
    ObserveDecision out{};
    if (!registry.has_any_active()) return out;

    /* Pull live GTE capture snapshot if new projections arrived since last pull. */
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (live_capture_.runs.size() != snap_current_.runs.size() ||
            live_capture_.sxy_pool.size() != snap_current_.sxy_pool.size()) {
            snap_current_ = live_capture_;
            snapshot_pulled_ = true;
        }
    }
    ensure_snapshot_ready();

    /* Detect scene/field transitions from live scene metadata and flush stale cached poses. */
    const SceneContext *active_scene = !snap_current_.runs.empty()      ? &snap_current_.scene
                                       : !snap_history_[0].runs.empty() ? &snap_history_[0].scene
                                                                        : nullptr;
    if (active_scene && active_scene->mode != SceneMode::Unknown) {
        if (last_scene_mode_ != SceneMode::Unknown &&
            (active_scene->mode != last_scene_mode_ || active_scene->field_id != last_field_id_)) {
            cached_poses_.clear();
            for (auto &h : snap_history_) h.clear();
        }
        last_scene_mode_ = active_scene->mode;
        last_field_id_ = active_scene->field_id;
    }

    int tri_x0 = std::min({tri.x[0], tri.x[1], tri.x[2]});
    int tri_y0 = std::min({tri.y[0], tri.y[1], tri.y[2]});
    int tri_x1 = std::max({tri.x[0], tri.x[1], tri.x[2]});
    int tri_y1 = std::max({tri.y[0], tri.y[1], tri.y[2]});

    /* When the GPU provides the exact GP0(E5) draw offset, only use that offset
     * (never match against bogus fallback offsets that shift the model on screen).
     * Fallback heuristics remain only for synthetic unit tests with has_draw_offset=false. */
    int draw_w = std::max(1, tri.clip_right - tri.clip_left + 1);
    int draw_h = std::max(1, tri.clip_bottom - tri.clip_top + 1);
    int cand_ox[5], cand_oy[5];
    int ncand = 0;
    if (tri.has_draw_offset) {
        cand_ox[ncand] = tri.draw_offset_x;
        cand_oy[ncand] = tri.draw_offset_y;
        ++ncand;
    } else {
        if (have_last_offset_) {
            cand_ox[ncand] = last_draw_ox_;
            cand_oy[ncand] = last_draw_oy_;
            ++ncand;
        }
        cand_ox[ncand] = tri.clip_left;
        cand_oy[ncand] = tri.clip_top;
        ++ncand;
        cand_ox[ncand] = tri.clip_left + draw_w / 2;
        cand_oy[ncand] = tri.clip_top + draw_h / 2;
        ++ncand;
        cand_ox[ncand] = 0;
        cand_oy[ncand] = 0;
        ++ncand;
    }

    /* Resolve the triangle's explicit VRAM texture hash and target model_id BEFORE
     * searching GTE snapshots so overlapping characters sharing integer screen pixels
     * never steal each other's live or historical skeletons. */
    uint64_t tex_hash = tri.explicit_tex_hash;
    if (!tex_hash && tri.textured) {
        int u0 = std::min({tri.u[0], tri.u[1], tri.u[2]});
        int v0 = std::min({tri.v[0], tri.v[1], tri.v[2]});
        int u1 = std::max({tri.u[0], tri.u[1], tri.u[2]});
        int v1 = std::max({tri.v[0], tri.v[1], tri.v[2]});
        tex_hash = lookup_vram_hash(tri.tex_page_x, tri.tex_page_y, tri.tex_depth, u0, v0, u1, v1);
    }
    const std::string tri_model_id = (tex_hash != 0) ? registry.find_by_texture_hash(tex_hash) : std::string();

    int hit_slot = -1;
    uint32_t hit_run = 0;
    size_t hit_cached_idx = SIZE_MAX;
    int hit_ox = 0, hit_oy = 0;
    float hit_sz_sum = 0.0f;
    int hit_corners = 0;
    bool hit_is_mesh = false;
    int hit_compat = -3;

    auto search_live_slots = [&](int slot_begin, int slot_end) {
        for (int slot = slot_begin; slot <= slot_end; ++slot) {
            const CaptureBuffer &cap = capture_for_slot(slot);
            if (cap.sxy_head.empty()) continue;
            for (int c = 0; c < ncand; ++c) {
                /* Try aligning all 3 corners to the same skeleton mesh cluster (>=3 verts),
                 * preferring candidates compatible with tri_model_id and real mesh RTPT runs
                 * over 1-vertex skeleton-propagation RTPS calls. */
                int best_matched = 0;
                bool best_is_mesh = false;
                bool best_all_close = false;
                int best_compat = -3;
                uint32_t best_first_r = 0;
                float best_sz_sum = 0.0f;

                /* Check every corner as a potential anchor so a colliding vertex at corner 0
                 * cannot hide a compatible skeleton present at corners 1 or 2. */
                for (int anchor = 0; anchor < 3; ++anchor) {
                    int sxa = tri.x[anchor] - cand_ox[c], sya = tri.y[anchor] - cand_oy[c];
                    uint32_t ka = (uint32_t(sxa) & 0xffffu) | ((uint32_t(sya) & 0xffffu) << 16);
                    auto ita = cap.sxy_head.find(ka);
                    if (ita == cap.sxy_head.end()) continue;
                    uint32_t cur = ita->second;
                    int depth = 0;
                    while (cur != UINT32_MAX && depth < 16) {
                        uint32_t cand_run = cap.sxy_pool[cur].run_index;
                        int compat = evaluate_run_compat(slot, cand_run, tri_model_id);
                        if (compat >= 0) {
                            bool cand_is_mesh = (cand_run < cap.runs.size() && cap.runs[cand_run].vertex_count >= 3u);
                            int matched = 1;
                            float sz_sum = float(cap.sxy_pool[cur].sz);
                            bool all_close = true;
                            for (int k = 0; k < 3; ++k) {
                                if (k == anchor) continue;
                                SxyRecord rec{};
                                if (find_projection(cap, tri.x[k], tri.y[k], cand_ox[c], cand_oy[c], rec, cand_run)) {
                                    uint32_t d = rec.run_index > cand_run ? (rec.run_index - cand_run) : (cand_run - rec.run_index);
                                    if (d > 24u) all_close = false;
                                    sz_sum += float(rec.sz);
                                    ++matched;
                                }
                            }
                            if (matched >= 2) {
                                bool better =
                                    (best_matched < 2) ||
                                    (cand_is_mesh && !best_is_mesh) ||
                                    (cand_is_mesh == best_is_mesh && compat > best_compat) ||
                                    (cand_is_mesh == best_is_mesh && compat == best_compat &&
                                     (matched > best_matched ||
                                      (matched == best_matched && all_close && !best_all_close)));
                                if (better) {
                                    best_matched = matched;
                                    best_is_mesh = cand_is_mesh;
                                    best_all_close = all_close;
                                    best_compat = compat;
                                    best_first_r = cand_run;
                                    best_sz_sum = sz_sum;
                                }
                            }
                        }
                        cur = cap.sxy_pool[cur].next;
                        ++depth;
                    }
                    if (best_is_mesh && best_matched == 3 && best_all_close &&
                        (tri_model_id.empty() || best_compat >= 1)) {
                        break;
                    }
                }

                if (best_matched >= 2) {
                    bool take =
                        (hit_slot < 0) ||
                        (best_is_mesh && !hit_is_mesh) ||
                        (best_is_mesh == hit_is_mesh && best_compat > hit_compat);
                    if (take) {
                        hit_slot = slot;
                        hit_run = best_first_r;
                        hit_ox = cand_ox[c];
                        hit_oy = cand_oy[c];
                        hit_sz_sum = best_sz_sum;
                        hit_corners = best_matched;
                        hit_is_mesh = best_is_mesh;
                        hit_compat = best_compat;
                    }
                    if (hit_is_mesh && (tri_model_id.empty() || hit_compat >= 1)) break;
                }
            }
            if (hit_slot >= 0 && hit_is_mesh && (tri_model_id.empty() || hit_compat >= 1)) break;
        }
    };

    /* Pass 1: Active double-buffered GTE frames (snap_current_ and 1-frame-lag snap_history_[0]). */
    search_live_slots(0, 1);

    /* Pass 2: Stationary field character cached poses (checked BEFORE stale history slots 2..6
     * so a stationary character never binds to a moving character's older history skeletons). */
    if ((hit_slot < 0 || !hit_is_mesh) && !cached_poses_.empty()) {
        int best_corners = 0;
        bool best_tex_ok = false;
        for (size_t ci = 0; ci < cached_poses_.size(); ++ci) {
            const auto &cp = cached_poses_[ci];
            if (!cp.model || cp.sxy_to_sz.empty()) continue;
            if (cp.last_live_frame == frame_counter_) continue;
            if (!tri_model_id.empty() && cp.model_id != tri_model_id) continue;
            bool tex_confirmed = (tex_hash != 0 && cp.tex_hashes.count(tex_hash) != 0);
            if (tri.textured && tex_hash != 0 && !cp.tex_hashes.empty() && !tex_confirmed) continue;

            for (int c = 0; c < ncand; ++c) {
                int sx[3], sy[3];
                for (int k = 0; k < 3; ++k) {
                    sx[k] = tri.x[k] - cand_ox[c];
                    sy[k] = tri.y[k] - cand_oy[c];
                }
                int bx0 = std::min({sx[0], sx[1], sx[2]});
                int by0 = std::min({sy[0], sy[1], sy[2]});
                int bx1 = std::max({sx[0], sx[1], sx[2]});
                int by1 = std::max({sy[0], sy[1], sy[2]});
                if (!bboxes_overlap(bx0, by0, bx1, by1, cp.min_sx, cp.min_sy, cp.max_sx, cp.max_sy)) continue;

                int matched = 0;
                float sz_sum = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    uint32_t key = (uint32_t(sx[k]) & 0xffffu) | ((uint32_t(sy[k]) & 0xffffu) << 16);
                    auto it = cp.sxy_to_sz.find(key);
                    if (it != cp.sxy_to_sz.end()) {
                        sz_sum += float(it->second);
                        ++matched;
                    }
                }
                int min_req = tex_confirmed ? 2 : 3;
                if (matched >= min_req &&
                    (matched > best_corners || (matched == best_corners && tex_confirmed && !best_tex_ok))) {
                    best_corners = matched;
                    best_tex_ok = tex_confirmed;
                    hit_slot = kCachedSlot;
                    hit_cached_idx = ci;
                    hit_ox = cand_ox[c];
                    hit_oy = cand_oy[c];
                    hit_sz_sum = sz_sum;
                    hit_corners = matched;
                    hit_is_mesh = true;
                }
            }
        }
    }

    /* Pass 3: Older history slots (2..kHistorySlots) only when neither active frames nor
     * cached stationary poses matched, and only if this model_id hasn't already matched live/cached. */
    if (hit_slot < 0 || !hit_is_mesh) {
        bool already_active_id = false;
        if (!tri_model_id.empty()) {
            for (const auto &cl : clusters_) {
                if (cl.matched && cl.model_id == tri_model_id &&
                    (cl.capture_slot <= 1 || cl.capture_slot == kCachedSlot)) {
                    already_active_id = true;
                    break;
                }
            }
        }
        if (!already_active_id) {
            search_live_slots(2, kHistorySlots);
        }
    }

    if (hit_slot >= 0) {
        have_last_offset_ = true;
        last_draw_ox_ = hit_ox;
        last_draw_oy_ = hit_oy;
        out.triangle_sz = hit_sz_sum / float(hit_corners);

        uint32_t cid = UINT32_MAX;
        if (hit_slot == kCachedSlot && hit_cached_idx < cached_poses_.size()) {
            auto &cp = cached_poses_[hit_cached_idx];
            cp.last_seen_frame = frame_counter_;
            if (cp.frame_cluster_id == UINT32_MAX || cp.frame_cluster_id >= clusters_.size()) {
                cid = uint32_t(clusters_.size());
                ClusterState cs;
                cs.capture_slot = kCachedSlot;
                cs.cached_index = hit_cached_idx;
                cs.run_count = uint32_t(cp.bones.size());
                cs.evaluated = true;
                cs.matched = true;
                cs.pose_cached = true;
                cs.model_id = cp.model_id;
                cs.model = cp.model;
                cs.draw_offset_x = float(hit_ox);
                cs.draw_offset_y = float(hit_oy);
                clusters_.push_back(std::move(cs));
                cp.frame_cluster_id = cid;
            } else {
                cid = cp.frame_cluster_id;
            }
        } else {
            cid = find_or_create_cluster(hit_slot, hit_run, float(hit_ox), float(hit_oy));
        }

        if (cid != UINT32_MAX) {
            auto &cluster = clusters_[cid];
            if (cluster.capture_slot < kCachedSlot) {
                if (!tri_model_id.empty()) {
                    uint32_t votes = ++cluster.model_votes[tri_model_id];
                    if (cluster.matched && !cluster.flushed && tri_model_id != cluster.model_id &&
                        votes > cluster.model_votes[cluster.model_id]) {
                        auto m = registry.get(tri_model_id);
                        size_t req_runs = (m && m->bone_remap.size() >= 6) ? (m->bone_remap.size() / 2) : 1u;
                        if (m && cluster.run_count >= req_runs) {
                            cluster.model_id = tri_model_id;
                            cluster.model = m;
                            if (cluster.pending_index < pending_draws_.size()) {
                                pending_draws_[cluster.pending_index].model_id = tri_model_id;
                                pending_draws_[cluster.pending_index].model = m;
                            }
                            if (cluster.pose_cached && cluster.cached_index < cached_poses_.size()) {
                                cached_poses_[cluster.cached_index].model_id = tri_model_id;
                                cached_poses_[cluster.cached_index].model = m;
                            }
                        }
                    }
                }
                evaluate_cluster_match(cluster, tex_hash, registry);
            }
            if (cluster.matched && cluster.model) {
                if (cluster.capture_slot < kCachedSlot) {
                    cache_live_cluster(cluster, tex_hash);
                }
                out.matched_cluster = cid;
                out.suppress_ps1_triangle = true;
                ++suppressed_this_frame_;

                /* Capture live PS1 GTE lighting/tint (128 = 1.0 neutral on PS1). */
                double tr = tri.raw ? 128.0 : (double(tri.r[0] + tri.r[1] + tri.r[2]) / 3.0);
                double tg = tri.raw ? 128.0 : (double(tri.g[0] + tri.g[1] + tri.g[2]) / 3.0);
                double tb = tri.raw ? 128.0 : (double(tri.b[0] + tri.b[1] + tri.b[2]) / 3.0);
                cluster.sum_r += tr;
                cluster.sum_g += tg;
                cluster.sum_b += tb;
                cluster.color_samples++;

                if (!cluster.flushed) {
                    if (cluster.pending_index == SIZE_MAX) {
                        HdModelDrawCall dc;
                        dc.model = cluster.model;
                        dc.model_id = cluster.model_id;
                        if (cluster.capture_slot == kCachedSlot && cluster.cached_index < cached_poses_.size()) {
                            const auto &cp = cached_poses_[cluster.cached_index];
                            dc.bones = cp.bones;
                            dc.ofx = cp.ofx;
                            dc.ofy = cp.ofy;
                            dc.h = cp.h;
                        } else {
                            const CaptureBuffer &cap = capture_for_slot(cluster.capture_slot);
                            dc.bones.reserve(cluster.run_count);
                            for (uint32_t i = 0; i < cluster.run_count; ++i) {
                                dc.bones.push_back(cap.runs[cluster.first_run + i].bone);
                            }
                            const auto &root_run = cap.runs[cluster.first_run];
                            dc.ofx = root_run.ofx;
                            dc.ofy = root_run.ofy;
                            dc.h = root_run.h;
                        }
                        dc.draw_offset_x = cluster.draw_offset_x;
                        dc.draw_offset_y = cluster.draw_offset_y;
                        dc.clip_left = tri.clip_left;
                        dc.clip_top = tri.clip_top;
                        dc.clip_right = tri.clip_right;
                        dc.clip_bottom = tri.clip_bottom;
                        auto prev_tint = last_tint_by_id_.find(cluster.model_id);
                        if (prev_tint != last_tint_by_id_.end()) {
                            dc.tint_rgb[0] = prev_tint->second[0];
                            dc.tint_rgb[1] = prev_tint->second[1];
                            dc.tint_rgb[2] = prev_tint->second[2];
                        }
                        if (std::getenv("FF8_HD_DEBUG"))
                            std::fprintf(stderr, "HD_DRAW: id='%s' slot=%d runs=%u of=(%.1f,%.1f) draw_off=(%.1f,%.1f) clip=(%d,%d,%d,%d) h=%.1f t0=(%.1f,%.1f,%.1f)\n",
                                         dc.model_id.c_str(), cluster.capture_slot, unsigned(dc.bones.size()),
                                         dc.ofx, dc.ofy, dc.draw_offset_x, dc.draw_offset_y,
                                         dc.clip_left, dc.clip_top, dc.clip_right, dc.clip_bottom,
                                         dc.h,
                                         dc.bones.empty() ? 0.0f : dc.bones[0].t[0],
                                         dc.bones.empty() ? 0.0f : dc.bones[0].t[1],
                                         dc.bones.empty() ? 0.0f : dc.bones[0].t[2]);
                        cluster.pending_index = pending_draws_.size();
                        pending_draws_.push_back(std::move(dc));
                    }

                    auto &dc = pending_draws_[cluster.pending_index];
                    dc.suppressed_triangles++;
                    dc.bbox_left = std::min(dc.bbox_left, tri_x0);
                    dc.bbox_top = std::min(dc.bbox_top, tri_y0);
                    dc.bbox_right = std::max(dc.bbox_right, tri_x1);
                    dc.bbox_bottom = std::max(dc.bbox_bottom, tri_y1);
                    dc.min_sz = std::min(dc.min_sz, out.triangle_sz);
                    dc.max_sz = std::max(dc.max_sz, out.triangle_sz);
                    dc.avg_sz = (dc.min_sz + dc.max_sz) * 0.5f;
                    dc.tint_rgb[0] = float(std::clamp(cluster.sum_r / (128.0 * double(cluster.color_samples)), 0.0, 2.0));
                    dc.tint_rgb[1] = float(std::clamp(cluster.sum_g / (128.0 * double(cluster.color_samples)), 0.0, 2.0));
                    dc.tint_rgb[2] = float(std::clamp(cluster.sum_b / (128.0 * double(cluster.color_samples)), 0.0, 2.0));
                    last_tint_by_id_[cluster.model_id] = {dc.tint_rgb[0], dc.tint_rgb[1], dc.tint_rgb[2]};
                }
                return out;
            }
        }
    }

    /* Non-suppressed primitive (foreground field mask, non-replaced character/enemy,
     * spell effect, or UI). Flush any pending HD model whose bounding box overlaps it
     * AND sits behind or at this primitive's Ordering Table position. */
    if (!pending_draws_.empty()) {
        for (const auto &dc : pending_draws_) {
            if (dc.bbox_right < dc.bbox_left) continue;
            if (!bboxes_overlap(tri_x0, tri_y0, tri_x1, tri_y1,
                                dc.bbox_left, dc.bbox_top, dc.bbox_right, dc.bbox_bottom)) {
                continue;
            }
            /* If 2D (hit_slot < 0: foreground field mask / HUD) OR closer/same depth
             * in OT order, flush the HD model before drawing this primitive on top. */
            if (hit_slot < 0 || out.triangle_sz <= dc.max_sz + 1.0f) {
                out.flush_pending_hd_models = true;
                break;
            }
        }
    }
    return out;
}

bool HdSceneTracker::should_flush_for_rect(int x, int y, int w, int h) const {
    if (pending_draws_.empty() || w <= 0 || h <= 0) return false;
    int x1 = x + w - 1;
    int y1 = y + h - 1;
    for (const auto &dc : pending_draws_) {
        if (dc.bbox_right < dc.bbox_left) continue;
        if (bboxes_overlap(x, y, x1, y1, dc.bbox_left, dc.bbox_top, dc.bbox_right, dc.bbox_bottom)) {
            return true;
        }
    }
    return false;
}

std::vector<HdModelDrawCall> HdSceneTracker::take_pending_draws() {
    for (auto &c : clusters_) {
        if (c.pending_index != SIZE_MAX) {
            c.flushed = true;
            c.pending_index = SIZE_MAX;
        }
    }
    std::vector<HdModelDrawCall> raw;
    raw.swap(pending_draws_);
    for (const auto &dc : raw) {
        auto &mx = frame_max_sup_by_id_[dc.model_id];
        if (dc.suppressed_triangles > mx) mx = dc.suppressed_triangles;
    }
    std::vector<HdModelDrawCall> result;
    result.reserve(raw.size());
    for (auto &dc : raw) {
        /* Drop tiny 1-3 triangle stray overlap duplicates when the same articulated
         * model already has a full body draw (>=10 triangles) on this frame. */
        bool articulated = (dc.model && dc.model->bone_remap.size() >= 6);
        if (articulated && dc.suppressed_triangles < 4u && frame_max_sup_by_id_[dc.model_id] >= 10u) {
            continue;
        }
        if (std::getenv("FF8_HD_DEBUG")) {
            std::fprintf(stderr, "HD_FLUSH: id='%s' sup=%u tint=(%.2f,%.2f,%.2f) bbox=(%d,%d,%d,%d) t0=(%.1f,%.1f,%.1f)\n",
                         dc.model_id.c_str(), dc.suppressed_triangles,
                         dc.tint_rgb[0], dc.tint_rgb[1], dc.tint_rgb[2],
                         dc.bbox_left, dc.bbox_top, dc.bbox_right, dc.bbox_bottom,
                         dc.bones.empty() ? 0.0f : dc.bones[0].t[0],
                         dc.bones.empty() ? 0.0f : dc.bones[0].t[1],
                         dc.bones.empty() ? 0.0f : dc.bones[0].t[2]);
        }
        result.push_back(std::move(dc));
    }
    return result;
}

void HdSceneTracker::begin_replay_pass() {
    clusters_.clear();
    run_to_cluster_.clear();
    claimed_scene_ids_.clear();
    frame_max_sup_by_id_.clear();
    pending_draws_.clear();
    for (auto &cp : cached_poses_) cp.frame_cluster_id = UINT32_MAX;
    suppressed_this_frame_ = 0;
}

void HdSceneTracker::end_frame() {
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!live_capture_.runs.empty()) {
            bool has_mesh = false;
            for (const auto &r : live_capture_.runs) {
                if (r.vertex_count >= 3u) {
                    has_mesh = true;
                    break;
                }
            }
            if (has_mesh || snap_history_[0].runs.empty()) {
                for (int i = kHistorySlots - 1; i > 0; --i) {
                    snap_history_[size_t(i)] = std::move(snap_history_[size_t(i - 1)]);
                }
                snap_history_[0] = std::move(live_capture_);
            }
            live_capture_.clear();
        }
    }
    snap_current_.clear();
    snapshot_pulled_ = false;
    clusters_.clear();
    run_to_cluster_.clear();
    claimed_scene_ids_.clear();
    frame_max_sup_by_id_.clear();
    pending_draws_.clear();
    ++frame_counter_;
    for (auto &cp : cached_poses_) cp.frame_cluster_id = UINT32_MAX;
    cached_poses_.erase(
        std::remove_if(cached_poses_.begin(), cached_poses_.end(),
                       [&](const CachedPose &cp) {
                           return frame_counter_ > cp.last_seen_frame &&
                                  (frame_counter_ - cp.last_seen_frame) > 120u;
                       }),
        cached_poses_.end());
    suppressed_this_frame_ = 0;
}

size_t HdSceneTracker::captured_run_count() const {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    return live_capture_.runs.size() + snap_current_.runs.size();
}

} // namespace HostHD
