#ifdef _WIN32
#undef fopen
#undef open
#undef mkdir
#undef unlink
#undef remove
#undef rename
#undef getenv
#endif

#include "hd_textures.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <zlib.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
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

constexpr uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);
constexpr size_t kMaxGpuCacheBytes = 256u * 1024u * 1024u;

inline uint64_t fnv_u16(uint64_t h, uint16_t v) {
    h = (h ^ (v & 0xffu)) * kFnvPrime;
    h = (h ^ ((v >> 8) & 0xffu)) * kFnvPrime;
    return h;
}

inline bool rects_overlap(const PSX::Rect &a, const PSX::Rect &b) {
    return a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

inline bool rect_contains(const PSX::Rect &outer, const PSX::Rect &inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

inline uint64_t rect_overlap_area(const PSX::Rect &a, const PSX::Rect &b) {
    unsigned x0 = std::max(a.x, b.x);
    unsigned y0 = std::max(a.y, b.y);
    unsigned x1 = std::min(a.x + a.width, b.x + b.width);
    unsigned y1 = std::min(a.y + a.height, b.y + b.height);
    if (x1 <= x0 || y1 <= y0) return 0;
    return uint64_t(x1 - x0) * uint64_t(y1 - y0);
}

/* Overlap test for a primitive's texture rect, which the renderer may report with
 * zero width for a one-word-wide sprite. */
inline bool rect_touches(const PSX::Rect &sheet, const PSX::Rect &prim) {
    PSX::Rect p = prim;
    if (!p.width) p.width = 1;
    if (!p.height) p.height = 1;
    return rects_overlap(sheet, p);
}

inline bool is_power_of_two(unsigned v) { return v != 0 && (v & (v - 1u)) == 0; }

inline std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
    return buf;
}

inline uint8_t expand5(unsigned c) { return uint8_t((c << 3) | (c >> 2)); }

void unpack_psx_pixel(uint16_t raw, uint8_t out[4]) {
    if (raw == 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[0] = expand5(raw & 31u);
    out[1] = expand5((raw >> 5) & 31u);
    out[2] = expand5((raw >> 10) & 31u);
    out[3] = 255;
}

void write_be32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(uint8_t((v >> 24) & 0xffu));
    out.push_back(uint8_t((v >> 16) & 0xffu));
    out.push_back(uint8_t((v >> 8) & 0xffu));
    out.push_back(uint8_t(v & 0xffu));
}

void write_png_chunk(std::vector<uint8_t> &out, const char type[4], const uint8_t *data, size_t len) {
    write_be32(out, uint32_t(len));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len && data) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + start, uInt(4 + len));
    write_be32(out, uint32_t(crc));
}

bool write_rgba_png(const std::string &path, unsigned w, unsigned h, const uint8_t *rgba) {
    if (!w || !h || !rgba) return false;
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (1u + size_t(w) * 4u));
    for (unsigned y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t *row = rgba + size_t(y) * size_t(w) * 4u;
        raw.insert(raw.end(), row, row + size_t(w) * 4u);
    }
    uLongf comp_cap = compressBound(uLong(raw.size()));
    std::vector<uint8_t> comp(comp_cap);
    if (compress2(comp.data(), &comp_cap, raw.data(), uLong(raw.size()), 1) != Z_OK) return false;

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> png(sig, sig + 8);
    uint8_t ihdr[13] = {
        uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
        uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h),
        8, 6, 0, 0, 0
    };
    write_png_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    write_png_chunk(png, "IDAT", comp.data(), comp_cap);
    write_png_chunk(png, "IEND", nullptr, 0);

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
    return bool(f);
}

} // namespace

TextureProvider::TextureProvider() { start_worker(); }
TextureProvider::~TextureProvider() { stop_worker(); }

void TextureProvider::start_worker() {
    if (worker_.joinable()) return;
    stop_ = false;
    worker_ = std::thread([this] { worker_loop(); });
}

void TextureProvider::stop_worker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void TextureProvider::configure(const TextureConfig &cfg) {
    bool pack_changed = (cfg_.textures_dir != cfg.textures_dir) ||
                        (cfg_.textures_enabled != cfg.textures_enabled) ||
                        (cfg_.internal_scale != cfg.internal_scale);
    cfg_ = cfg;
    font_.configure(cfg.font_mode, cfg.font_path);
    if (!active()) {
        uploads_.clear();
        sheets_.clear();
    }
    if (pack_changed) {
        ++pack_generation_;
        pending_or_failed_.clear();
        gpu_cache_.clear();
        gpu_bytes_ = 0;
        scan_pack_dir();
    }
    if (!cfg_.dump_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cfg_.dump_dir, ec);
    }
    if (renderer_) {
        bool need_track = active();
        renderer_->set_track_textures(need_track);
        renderer_->set_replace_textures(cfg_.font_mode != FontMode::Off || cfg_.textures_enabled);
        renderer_->set_dump_textures(!cfg_.dump_dir.empty());
    }
    /* Sheets uploaded while tracking was off are still in VRAM: find them now. */
    if (active() && last_vram_ && sheets_.empty()) rescan_sheets(PSX::Rect(0, 0, 1024, 512));
}

void TextureProvider::reload_pack() {
    ++pack_generation_;
    pending_or_failed_.clear();
    gpu_cache_.clear();
    gpu_bytes_ = 0;
    scan_pack_dir();
    if (active() && last_vram_ && sheets_.empty()) rescan_sheets(PSX::Rect(0, 0, 1024, 512));
}

bool TextureProvider::active() const {
    return cfg_.font_mode != FontMode::Off || cfg_.textures_enabled || !cfg_.dump_dir.empty();
}

void TextureProvider::set_uploader(PSX::Renderer *r) {
    renderer_ = r;
    if (renderer_) {
        bool need_track = active();
        renderer_->set_track_textures(need_track);
        renderer_->set_replace_textures(cfg_.font_mode != FontMode::Off || cfg_.textures_enabled);
        renderer_->set_dump_textures(!cfg_.dump_dir.empty());
    }
}

void TextureProvider::scan_pack_dir() {
    pack_index_.clear();
    if (!cfg_.textures_enabled || cfg_.textures_dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(cfg_.textures_dir, ec)) return;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(cfg_.textures_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        for (char &ch : ext) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
        if (ext != ".png") continue;
        std::string stem = entry.path().stem().string();
        for (char &ch : stem) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
        /* Match "<tex16>_<clut16>" or "<tex16>" (allowing optional suffix like "_256x256"). */
        if (stem.size() >= 33 && stem[16] == '_') {
            pack_index_[stem.substr(0, 33)] = entry.path().string();
        } else if (stem.size() >= 16) {
            pack_index_[stem.substr(0, 16)] = entry.path().string();
        }
    }
    if (std::getenv("FF8_HD_DEBUG"))
        std::fprintf(stderr, "HD_TEX: scanned '%s' -> %zu file(s)\n", cfg_.textures_dir.c_str(), pack_index_.size());
}

uint64_t TextureProvider::hash_vram_rect(const PSX::Rect &rect, const uint16_t *vram) {
    if (!vram || !rect.width || !rect.height) return 0;
    uint64_t h = kFnvOffset;
    h = fnv_u16(h, uint16_t(rect.width));
    h = fnv_u16(h, uint16_t(rect.height));
    for (unsigned y = 0; y < rect.height; ++y) {
        unsigned ry = (rect.y + y) & 511u;
        for (unsigned x = 0; x < rect.width; ++x) {
            unsigned rx = (rect.x + x) & 1023u;
            h = fnv_u16(h, vram[ry * 1024u + rx]);
        }
    }
    return h;
}

uint64_t TextureProvider::hash_clut(PSX::TextureMode mode, unsigned pal_x, unsigned pal_y,
                                    const uint16_t *vram) {
    if (!vram || mode == PSX::TextureMode::ABGR1555 || mode == PSX::TextureMode::None) return 0;
    unsigned count = (mode == PSX::TextureMode::Palette4bpp) ? 16u : 256u;
    uint64_t h = kFnvOffset;
    unsigned py = pal_y & 511u;
    for (unsigned i = 0; i < count; ++i) {
        unsigned px = (pal_x + i) & 1023u;
        h = fnv_u16(h, vram[py * 1024u + px]);
    }
    return h;
}

void TextureProvider::clear_region(PSX::Rect rect) {
    uploads_.erase(
        std::remove_if(uploads_.begin(), uploads_.end(),
                       [&](const UploadEntry &e) {
                           uint64_t overlap = rect_overlap_area(e.rect, rect);
                           if (!overlap) return false;
                           uint64_t area = uint64_t(e.rect.width) * uint64_t(e.rect.height);
                           /* Preserve large texture sheets (e.g. battlegrounds) when a tiny
                            * sub-rect (such as Victory/UI sprites or a CLUT strip) writes into
                            * an unused corner; newer uploads still win in reverse lookup(). */
                           bool evict = (overlap * 4u >= area);
                           if (std::getenv("FF8_HD_DEBUG") && pack_index_.count(hex64(e.tex_hash))) {
                               std::fprintf(stderr, "HD_TEX: clear_region(%u,%u %ux%u) vs pack tex=%016llx (%u,%u %ux%u) overlap=%llu/%llu -> %s\n",
                                            rect.x, rect.y, rect.width, rect.height,
                                            (unsigned long long)e.tex_hash,
                                            e.rect.x, e.rect.y, e.rect.width, e.rect.height,
                                            (unsigned long long)overlap, (unsigned long long)area,
                                            evict ? "EVICT" : "KEEP");
                           }
                           return evict;
                       }),
        uploads_.end());
    sheets_.erase(
        std::remove_if(sheets_.begin(), sheets_.end(),
                       [&](const FontSheet &e) { return rects_overlap(e.rect, rect); }),
        sheets_.end());
}

void TextureProvider::blit(PSX::Rect dst, PSX::Rect src) {
    clear_region(dst);
    /* Everything the copy carries whole moves with it. The VRAM mirror is written
     * after this call, so the copies are placed by geometry, not by re-reading. */
    const size_t upload_count = uploads_.size();
    for (size_t i = 0; i < upload_count; ++i) {
        if (!rect_contains(src, uploads_[i].rect)) continue;
        UploadEntry copy = uploads_[i];
        copy.rect.x = dst.x + (copy.rect.x - src.x);
        copy.rect.y = dst.y + (copy.rect.y - src.y);
        uploads_.push_back(copy);
    }
    const size_t sheet_count = sheets_.size();
    for (size_t i = 0; i < sheet_count; ++i) {
        if (!rect_contains(src, sheets_[i].rect)) continue;
        FontSheet copy = sheets_[i];
        copy.rect.x = dst.x + (copy.rect.x - src.x);
        copy.rect.y = dst.y + (copy.rect.y - src.y);
        sheets_.push_back(copy);
    }
}

void TextureProvider::rescan_sheets(const PSX::Rect &dirty) {
    if (!last_vram_) return;
    std::vector<SheetHit> hits;
    HdFont::find_sheets(last_vram_, dirty.x, dirty.y, dirty.width, dirty.height, hits);
    /* A sheet from another region build may not match the strict layout; accept a
     * lone upload that is exactly one sheet, as before. */
    if (hits.empty() && dirty.width == HdFont::kSheetWords &&
        HdFont::is_font_upload(dirty.x, dirty.y, dirty.width, dirty.height, last_vram_))
        hits.push_back({dirty.x, dirty.y, dirty.width, dirty.height, false});
    for (const SheetHit &hit : hits) {
        const PSX::Rect rect(hit.x, hit.y, hit.w, hit.h);
        sheets_.erase(
            std::remove_if(sheets_.begin(), sheets_.end(),
                           [&](const FontSheet &e) { return rects_overlap(e.rect, rect); }),
            sheets_.end());
        FontSheet sheet;
        sheet.rect = rect;
        sheet.tex_hash = hash_vram_rect(rect, last_vram_);
        sheet.icon = hit.icon;
        if (hit.icon) {
            for (unsigned i = 0; i < HdFont::kNameStripCount; ++i) {
                const IconTextInfo strip = HdFont::name_strip(i);
                sheet.name_hash[i] = hash_vram_rect(
                    PSX::Rect(rect.x + strip.x / 4u, rect.y + strip.y, strip.w / 4u, strip.h), last_vram_);
            }
        }
        sheets_.push_back(sheet);
    }
}

void TextureProvider::set_names(const std::vector<std::string> &names) {
    if (names == names_) return;
    names_ = names;
    ++names_generation_;
}

void TextureProvider::resync(const uint16_t *vram) {
    if (vram) last_vram_ = vram;
    sheets_.clear();
    if (!last_vram_ || !active()) return;
    rescan_sheets(PSX::Rect(0, 0, 1024, 512));
}

void TextureProvider::upload(PSX::Rect rect, const uint16_t *vram) {
    last_vram_ = vram;
    if (!vram || !rect.width || !rect.height) return;
    clear_region(rect);
    if (!active()) return;

    if (cfg_.textures_enabled || !cfg_.dump_dir.empty()) {
        UploadEntry entry;
        entry.rect = rect;
        entry.tex_hash = hash_vram_rect(rect, vram);
        if (uploads_.size() >= 512) uploads_.erase(uploads_.begin());
        uploads_.push_back(entry);
        /* Prefetch CLUT-independent pack textures immediately at VRAM upload time
         * (e.g. during loading screens before battle/field fade-in) so decoding
         * completes before the first visible frame renders. */
        if (cfg_.textures_enabled && !pack_index_.empty()) {
            auto pit = pack_index_.find(hex64(entry.tex_hash));
            if (pit != pack_index_.end()) {
                CacheKey key{entry.tex_hash, 0, pack_generation_};
                if (gpu_cache_.find(key) == gpu_cache_.end() &&
                    pending_or_failed_.find(key) == pending_or_failed_.end()) {
                    enqueue_decode(key, pit->second, rect.width, rect.height);
                }
            }
        }
    }
    /* Font sheets are looked for in VRAM itself: the host reports dirty regions,
     * which merge neighbouring uploads and split one upload along other writes. */
    if (font_.active()) rescan_sheets(rect);
}

void TextureProvider::end_frame() {
    ++frame_;
    drain_decoded();
    evict_if_needed();
}

void TextureProvider::on_queues_reset() {
    frame_handles_.clear();
}

void TextureProvider::drain_decoded() {
    if (!renderer_) return;
    std::deque<DecodedImage> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(decoded_ready_);
        has_decoded_ready_.store(false, std::memory_order_release);
    }
    for (auto &img : ready) {
        if (std::getenv("FF8_HD_DEBUG"))
            std::fprintf(stderr, "HD_TEX: decoded tex=%016llx clut=%016llx ok=%d size=%ux%u\n",
                         (unsigned long long)img.key.tex_hash, (unsigned long long)img.key.clut_hash,
                         int(img.ok), img.width, img.height);
        if (img.key.generation != pack_generation_) continue;
        if (!img.ok || img.rgba.empty() || !img.width || !img.height) continue;
        Vulkan::ImageHandle handle = renderer_->create_hd_texture(img.width, img.height, img.rgba.data(), true);
        if (!handle) continue;
        size_t bytes = size_t(img.width) * size_t(img.height) * 4u;
        gpu_cache_[img.key] = {std::move(handle), img.width, img.height, frame_, bytes};
        gpu_bytes_ += bytes;
        pending_or_failed_.erase(img.key);
    }
}

void TextureProvider::evict_if_needed() {
    while (gpu_bytes_ > kMaxGpuCacheBytes && !gpu_cache_.empty()) {
        auto oldest = gpu_cache_.begin();
        for (auto it = gpu_cache_.begin(); it != gpu_cache_.end(); ++it) {
            if (it->second.last_used < oldest->second.last_used) oldest = it;
        }
        gpu_bytes_ -= oldest->second.bytes;
        gpu_cache_.erase(oldest);
    }
}

void TextureProvider::enqueue_decode(const CacheKey &key, std::string file,
                                     unsigned expected_local_w, unsigned expected_local_h) {
    pending_or_failed_.insert(key);
    if (std::getenv("FF8_HD_DEBUG"))
        std::fprintf(stderr, "HD_TEX: enqueue decode '%s' tex=%016llx clut=%016llx expected=%ux%u\n",
                     file.c_str(), (unsigned long long)key.tex_hash, (unsigned long long)key.clut_hash,
                     expected_local_w, expected_local_h);
    DecodeRequest req{key, std::move(file), expected_local_w, expected_local_h};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decode_queue_.push_back(std::move(req));
    }
    wake_.notify_one();
}

TextureProvider::PackMatch TextureProvider::find_pack_file(uint64_t tex_hash, uint64_t clut_hash) const {
    if (pack_index_.empty()) return {};
    std::string full = hex64(tex_hash) + "_" + hex64(clut_hash);
    auto it = pack_index_.find(full);
    if (it != pack_index_.end()) return {it->second, true};
    it = pack_index_.find(hex64(tex_hash));
    if (it != pack_index_.end()) return {it->second, false};
    return {};
}

void TextureProvider::enqueue_dump(const UploadEntry &u, const PSX::UsedMode &mode, uint64_t clut_hash) {
    if (cfg_.dump_dir.empty() || !last_vram_) return;
    uint64_t dump_key = u.tex_hash ^ (clut_hash * UINT64_C(0x9e3779b97f4a7c15));
    if (!dumped_keys_.insert(dump_key).second) return;

    unsigned shift = (mode.mode == PSX::TextureMode::Palette4bpp) ? 2u :
                     (mode.mode == PSX::TextureMode::Palette8bpp) ? 1u : 0u;
    unsigned local_w = u.rect.width << shift;
    unsigned local_h = u.rect.height;
    if (!local_w || !local_h) return;

    DumpRequest req;
    req.width = local_w;
    req.height = local_h;
    req.rgba.resize(size_t(local_w) * size_t(local_h) * 4u);
    unsigned py = mode.palette_offset_y & 511u;

    for (unsigned y = 0; y < local_h; ++y) {
        unsigned vy = (u.rect.y + y) & 511u;
        for (unsigned x = 0; x < local_w; ++x) {
            uint16_t raw = 0;
            if (shift == 2u) {
                uint16_t word = last_vram_[vy * 1024u + ((u.rect.x + (x >> 2)) & 1023u)];
                unsigned idx = (word >> ((x & 3u) * 4u)) & 0xfu;
                raw = last_vram_[py * 1024u + ((mode.palette_offset_x + idx) & 1023u)];
            } else if (shift == 1u) {
                uint16_t word = last_vram_[vy * 1024u + ((u.rect.x + (x >> 1)) & 1023u)];
                unsigned idx = (word >> ((x & 1u) * 8u)) & 0xffu;
                raw = last_vram_[py * 1024u + ((mode.palette_offset_x + idx) & 1023u)];
            } else {
                raw = last_vram_[vy * 1024u + ((u.rect.x + x) & 1023u)];
            }
            unpack_psx_pixel(raw, &req.rgba[(size_t(y) * local_w + x) * 4u]);
        }
    }

    char name[96];
    std::snprintf(name, sizeof(name), "%016llx_%016llx_%ux%u.png",
                  (unsigned long long)u.tex_hash, (unsigned long long)clut_hash, local_w, local_h);
    req.path = (std::filesystem::path(cfg_.dump_dir) / name).string();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dump_queue_.size() < 256) dump_queue_.push_back(std::move(req));
    }
    wake_.notify_one();
}

PSX::HdTextureHandle TextureProvider::push_handle(const PSX::Rect &vram_rect, const CachedGpu &gpu) {
    /* Primitives sharing a texture share a handle, so the renderer can batch them. */
    const size_t scan = std::min<size_t>(frame_handles_.size(), 16);
    for (size_t i = 0; i < scan; ++i) {
        const size_t index = frame_handles_.size() - 1 - i;
        const PSX::HdTexture &h = frame_handles_[index];
        if (h.texture.get() == gpu.image.get() && h.vram_rect.x == int(vram_rect.x) &&
            h.vram_rect.y == int(vram_rect.y) && h.vram_rect.width == int(vram_rect.width) &&
            h.vram_rect.height == int(vram_rect.height))
            return PSX::HdTextureHandle{int(index)};
    }
    PSX::HdTexture hd;
    hd.vram_rect = {int(vram_rect.x), int(vram_rect.y), int(vram_rect.width), int(vram_rect.height)};
    hd.texel_rect = {0, 0, int(gpu.width), int(gpu.height)};
    hd.texture = gpu.image;
    int handle_index = int(frame_handles_.size());
    frame_handles_.push_back(std::move(hd));
    return PSX::HdTextureHandle{handle_index};
}

PSX::HdTextureHandle TextureProvider::font_handle(const FontSheet &sheet, const PSX::Rect &vram_rect,
                                                  const PSX::UsedMode &mode, uint64_t clut_hash,
                                                  bool &cache_hit) {
    const unsigned scale = cfg_.internal_scale >= 8 ? 8u : (cfg_.internal_scale >= 4 ? 4u : 2u);
    PSX::Rect hd_rect = sheet.rect;
    uint64_t tex_hash = sheet.tex_hash;
    uint64_t generation = font_.generation();
    int band = -1, name = -1;
    const FontSheet *sysfnt = nullptr;
    if (sheet.icon) {
        /* The icon sheet is mostly graphics. Only its text bands are replaced, each
         * by its own small texture; a primitive that touches none stays native. */
        const unsigned px = vram_rect.x > sheet.rect.x ? vram_rect.x - sheet.rect.x : 0u;
        const unsigned py = vram_rect.y > sheet.rect.y ? vram_rect.y - sheet.rect.y : 0u;
        const unsigned px_end = std::min(sheet.rect.width, vram_rect.x + std::max(1u, vram_rect.width) - sheet.rect.x);
        const unsigned py_end = std::min(sheet.rect.height, vram_rect.y + std::max(1u, vram_rect.height) - sheet.rect.y);
        if (px_end <= px || py_end <= py) return PSX::HdTextureHandle::make_none();
        band = HdFont::icon_band_at(px * 4u, py, (px_end - px) * 4u, py_end - py);
        IconTextInfo info;
        if (band >= 0) {
            info = HdFont::icon_band(size_t(band));
            tex_hash ^= (uint64_t(band) + 1u) * UINT64_C(0x9e3779b97f4a7c15);
        } else {
            /* A battle party name: recognised against the candidate names with
             * the sysfnt sheet's glyphs, so both have to be known. */
            if (names_.empty()) return PSX::HdTextureHandle::make_none();
            name = HdFont::name_strip_at(px * 4u, py, (px_end - px) * 4u, py_end - py);
            if (name < 0) return PSX::HdTextureHandle::make_none();
            for (auto it = sheets_.rbegin(); it != sheets_.rend() && !sysfnt; ++it)
                if (!it->icon) sysfnt = &*it;
            if (!sysfnt) return PSX::HdTextureHandle::make_none();
            info = HdFont::name_strip(size_t(name));
            tex_hash = sheet.name_hash[name] ^ UINT64_C(0xc2b2ae3d27d4eb4f);
            generation ^= names_generation_ << 32;
        }
        hd_rect = PSX::Rect(sheet.rect.x + info.x / 4u, sheet.rect.y + info.y, info.w / 4u, info.h);
    }

    CacheKey key{tex_hash, clut_hash, generation};
    auto it = gpu_cache_.find(key);
    if (it == gpu_cache_.end()) {
        if (pending_or_failed_.find(key) != pending_or_failed_.end()) return PSX::HdTextureHandle::make_none();
        uint16_t clut[16];
        unsigned py = mode.palette_offset_y & 511u;
        for (unsigned i = 0; i < 16; ++i)
            clut[i] = last_vram_[py * 1024u + ((mode.palette_offset_x + i) & 1023u)];
        FontRgba atlas;
        bool ok = false;
        if (name >= 0) {
            ok = font_.render_name_strip(size_t(name), clut, scale, atlas, last_vram_, sheet.rect.x, sheet.rect.y,
                                         sysfnt->rect.x, sysfnt->rect.y, names_);
        } else if (sheet.icon) {
            ok = font_.render_icon_band(size_t(band), clut, scale, atlas, last_vram_, sheet.rect.x, sheet.rect.y);
        } else {
            ok = font_.render_atlas(sheet.rect.width * 4u, sheet.rect.height, clut, scale, atlas,
                                    last_vram_, sheet.rect.x, sheet.rect.y);
        }
        Vulkan::ImageHandle img;
        if (ok) img = renderer_->create_hd_texture(atlas.width, atlas.height, atlas.pixels.data(), true);
        if (!img) {
            /* Nothing visible under this palette (or the upload failed): do not
             * rasterize again for every primitive that asks. */
            pending_or_failed_.insert(key);
            return PSX::HdTextureHandle::make_none();
        }
        size_t bytes = size_t(atlas.width) * size_t(atlas.height) * 4u;
        it = gpu_cache_.emplace(key, CachedGpu{std::move(img), atlas.width, atlas.height, frame_, bytes}).first;
        gpu_bytes_ += bytes;
    } else {
        cache_hit = true;
        it->second.last_used = frame_;
    }
    return push_handle(hd_rect, it->second);
}

PSX::HdTextureHandle TextureProvider::lookup(PSX::Rect vram_rect, const PSX::UsedMode &mode,
                                             unsigned tex_x, unsigned tex_y,
                                             bool &fastpath, bool &cache_hit) {
    (void)tex_x;
    (void)tex_y;
    fastpath = false;
    cache_hit = false;
    if (!renderer_ || !last_vram_ || !active()) return PSX::HdTextureHandle::make_none();

    const bool need_pack = cfg_.textures_enabled && !pack_index_.empty();
    const bool need_dump = !cfg_.dump_dir.empty();

    /* Fast path when only HD font is active: skip upload scanning and CLUT hashing
     * unless this 4bpp primitive actually touches one of the tracked font sheets. */
    if (!need_pack && !need_dump) {
        if (!font_.active() || sheets_.empty() || mode.mode != PSX::TextureMode::Palette4bpp)
            return PSX::HdTextureHandle::make_none();
        PSX::Rect probe = vram_rect;
        if (probe.height > 1) --probe.height;
        for (auto it = sheets_.rbegin(); it != sheets_.rend(); ++it) {
            if (!rect_touches(it->rect, probe)) continue;
            const uint64_t clut_hash = hash_clut(mode.mode, mode.palette_offset_x, mode.palette_offset_y, last_vram_);
            return font_handle(*it, probe, mode, clut_hash, cache_hit);
        }
        return PSX::HdTextureHandle::make_none();
    }

    /* Find the most recent VRAM upload covering this primitive's texture rect. */
    const UploadEntry *matched = nullptr;
    for (auto it = uploads_.rbegin(); it != uploads_.rend(); ++it) {
        if (rect_contains(it->rect, vram_rect)) {
            matched = &*it;
            break;
        }
    }

    const uint64_t clut_hash = hash_clut(mode.mode, mode.palette_offset_x, mode.palette_offset_y, last_vram_);
    if (matched && need_dump) enqueue_dump(*matched, mode, clut_hash);
    PackMatch pack_match;
    if (matched && need_pack) pack_match = find_pack_file(matched->tex_hash, clut_hash);
    const bool pack_hit = !pack_match.path.empty();

    /* 1. Built-in / custom HD font replacement (4bpp sysfnt + icon.tex text bands).
     *    A primitive only has to touch a sheet: polygons report loose texture rects,
     *    and the shader falls back to native texels outside the HD rect anyway. */
    if (font_.active() && mode.mode == PSX::TextureMode::Palette4bpp) {
        /* The renderer reports one row more than a sprite covers; without it a
         * texture stored right above a sheet would be taken for sheet text. */
        PSX::Rect probe = vram_rect;
        if (probe.height > 1) --probe.height;
        for (auto it = sheets_.rbegin(); it != sheets_.rend(); ++it) {
            if (!rect_touches(it->rect, probe)) continue;
            /* A user texture pack that replaces the icon sheet wins over built-in text. */
            if (it->icon && pack_hit) break;
            PSX::HdTextureHandle handle = font_handle(*it, probe, mode, clut_hash, cache_hit);
            if (handle != PSX::HdTextureHandle::make_none()) return handle;
            break;
        }
    }
    if (!matched) return PSX::HdTextureHandle::make_none();

    /* 2. General external HD Texture pack replacement (Phase 3). */
    if (pack_hit) {
        if (has_decoded_ready_.load(std::memory_order_acquire)) drain_decoded();
        const uint64_t cache_clut = pack_match.clut_specific ? clut_hash : 0;
        CacheKey key{matched->tex_hash, cache_clut, pack_generation_};
        auto it = gpu_cache_.find(key);
        if (it != gpu_cache_.end()) {
            unsigned shift = (mode.mode == PSX::TextureMode::Palette4bpp) ? 2u :
                             (mode.mode == PSX::TextureMode::Palette8bpp) ? 1u : 0u;
            unsigned local_w = matched->rect.width << shift;
            unsigned local_h = matched->rect.height;
            if (local_w && local_h &&
                it->second.width % local_w == 0 && it->second.height % local_h == 0 &&
                is_power_of_two(it->second.width / local_w) &&
                is_power_of_two(it->second.height / local_h)) {
                cache_hit = true;
                it->second.last_used = frame_;
                return push_handle(matched->rect, it->second);
            }
            return PSX::HdTextureHandle::make_none();
        }
        if (pending_or_failed_.find(key) == pending_or_failed_.end()) {
            unsigned shift = (mode.mode == PSX::TextureMode::Palette4bpp) ? 2u :
                             (mode.mode == PSX::TextureMode::Palette8bpp) ? 1u : 0u;
            enqueue_decode(key, std::move(pack_match.path), matched->rect.width << shift, matched->rect.height);
        }
    }
    return PSX::HdTextureHandle::make_none();
}

PSX::HdTexture TextureProvider::get(PSX::HdTextureHandle handle) {
    if (handle.index < 0 || size_t(handle.index) >= frame_handles_.size()) return {};
    return frame_handles_[size_t(handle.index)];
}

void TextureProvider::worker_loop() {
    for (;;) {
        DecodeRequest decode_req;
        DumpRequest dump_req;
        bool has_decode = false, has_dump = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !decode_queue_.empty() || !dump_queue_.empty(); });
            if (stop_) return;
            if (!decode_queue_.empty()) {
                decode_req = std::move(decode_queue_.front());
                decode_queue_.pop_front();
                has_decode = true;
            } else if (!dump_queue_.empty()) {
                dump_req = std::move(dump_queue_.front());
                dump_queue_.pop_front();
                has_dump = true;
            }
        }
        if (has_decode) {
            DecodedImage res;
            res.key = decode_req.key;
            int w = 0, h = 0, comp = 0;
            unsigned char *pixels = stbi_load(decode_req.path.c_str(), &w, &h, &comp, 4);
            if (pixels && w > 0 && h > 0 && decode_req.expected_local_w > 0 && decode_req.expected_local_h > 0) {
                unsigned uw = unsigned(w), uh = unsigned(h);
                /* parallel-psx hdtextures.h requires integer power-of-two scale relative to local texels. */
                bool valid_scale = (uw % decode_req.expected_local_w == 0) &&
                                   (uh % decode_req.expected_local_h == 0) &&
                                   is_power_of_two(uw / decode_req.expected_local_w) &&
                                   is_power_of_two(uh / decode_req.expected_local_h);
                if (valid_scale) {
                    res.width = uw;
                    res.height = uh;
                    res.rgba.assign(pixels, pixels + size_t(uw) * size_t(uh) * 4u);
                    /* parallel-psx hdtextures.h treats exact vec4(0) as transparent and exact
                     * (0,0,0,255) as the SD-fallthrough sentinel. Zero RGB on transparent pixels
                     * (e.g. DXT5 cutouts) so they never bleed as opaque dark fringes. */
                    for (size_t i = 0; i < res.rgba.size(); i += 4) {
                        if (res.rgba[i + 3] < 128) {
                            res.rgba[i] = res.rgba[i + 1] = res.rgba[i + 2] = res.rgba[i + 3] = 0;
                        } else {
                            res.rgba[i + 3] = 255;
                            if (res.rgba[i] == 0 && res.rgba[i + 1] == 0 && res.rgba[i + 2] == 0)
                                res.rgba[i] = res.rgba[i + 1] = res.rgba[i + 2] = 1;
                        }
                    }
                    res.ok = true;
                }
            }
            if (pixels) stbi_image_free(pixels);
            std::lock_guard<std::mutex> lock(mutex_);
            decoded_ready_.push_back(std::move(res));
            has_decoded_ready_.store(true, std::memory_order_release);
        } else if (has_dump) {
            write_rgba_png(dump_req.path, dump_req.width, dump_req.height, dump_req.rgba.data());
        }
    }
}

} // namespace HostHD
