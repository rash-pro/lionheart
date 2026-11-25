#ifndef FF8_HOST_HD_TEXTURES_HPP
#define FF8_HOST_HD_TEXTURES_HPP

#include "hd_font.hpp"
#include "renderer/renderer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HostHD {

struct TextureConfig {
    FontMode font_mode = FontMode::Off;
    std::string font_path;
    bool textures_enabled = false;
    std::string textures_dir;
    std::string dump_dir;
    unsigned internal_scale = 4;
};

/* Clean-room MIT implementation of PSX::HdTextureProvider for parallel-psx.
 * Supports:
 *   1. Built-in OFL / custom HD font replacement (Phase 2)
 *   2. User-supplied HD texture packs keyed by VRAM+CLUT hash (Phase 3)
 *   3. Optional VRAM texture dumping for modders (--dump-textures) */
class TextureProvider final : public PSX::HdTextureProvider {
public:
    TextureProvider();
    ~TextureProvider() override;

    void configure(const TextureConfig &cfg);
    void reload_pack();
    bool active() const;
    /* Re-derives the tracked font sheets from the live VRAM mirror. The host calls
     * this when it (re)attaches a VRAM mirror or changes the HD settings, because
     * the sheets may have been uploaded before tracking was on. */
    void resync(const uint16_t *vram);
    /* Names the game can show for the battle party, in its own text encoding.
     * A name strip of the icon sheet is replaced only when it matches one. */
    void set_names(const std::vector<std::string> &names);

    /* PSX::HdTextureProvider interface */
    void set_uploader(PSX::Renderer *r) override;
    void clear_region(PSX::Rect rect) override;
    void blit(PSX::Rect dst, PSX::Rect src) override;
    void upload(PSX::Rect rect, const uint16_t *vram) override;
    void end_frame() override;
    void on_queues_reset() override;
    PSX::HdTextureHandle lookup(PSX::Rect vram_rect, const PSX::UsedMode &mode,
                                unsigned tex_x, unsigned tex_y,
                                bool &fastpath, bool &cache_hit) override;
    PSX::HdTexture get(PSX::HdTextureHandle handle) override;

    /* Deterministic 64-bit FNV-1a helpers (public for unit testing). */
    static uint64_t hash_vram_rect(const PSX::Rect &rect, const uint16_t *vram);
    static uint64_t hash_clut(PSX::TextureMode mode, unsigned pal_x, unsigned pal_y, const uint16_t *vram);

private:
    struct UploadEntry {
        PSX::Rect rect;
        uint64_t tex_hash = 0;
    };
    /* A sysfnt / icon sheet found in VRAM by structure. Tracked apart from the
     * upload list: the host reports dirty VRAM in merged or split rectangles, so
     * an upload's geometry says nothing about where a sheet starts or ends. */
    struct FontSheet {
        PSX::Rect rect;
        uint64_t tex_hash = 0;
        bool icon = false;
        /* Icon sheet only: content of each battle name strip. The game rewrites
         * the strips alone, so their textures are keyed apart from the sheet's. */
        uint64_t name_hash[HdFont::kNameStripCount] = {};
    };
    struct CacheKey {
        uint64_t tex_hash = 0;
        uint64_t clut_hash = 0;
        uint64_t generation = 0;
        bool operator==(const CacheKey &o) const {
            return tex_hash == o.tex_hash && clut_hash == o.clut_hash && generation == o.generation;
        }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey &k) const noexcept {
            uint64_t h = k.tex_hash ^ (k.clut_hash * UINT64_C(0x9e3779b97f4a7c15)) ^ (k.generation << 1);
            return size_t(h ^ (h >> 32));
        }
    };
    struct CachedGpu {
        Vulkan::ImageHandle image;
        unsigned width = 0;
        unsigned height = 0;
        uint64_t last_used = 0;
        size_t bytes = 0;
    };
    struct DecodeRequest {
        CacheKey key;
        std::string path;
        unsigned expected_local_w = 0;
        unsigned expected_local_h = 0;
    };
    struct DecodedImage {
        CacheKey key;
        unsigned width = 0;
        unsigned height = 0;
        std::vector<uint8_t> rgba;
        bool ok = false;
    };
    struct DumpRequest {
        std::string path;
        unsigned width = 0;
        unsigned height = 0;
        std::vector<uint8_t> rgba;
    };

    struct PackMatch {
        std::string path;
        bool clut_specific = false;
    };

    void start_worker();
    void stop_worker();
    void worker_loop();
    void scan_pack_dir();
    void drain_decoded();
    void evict_if_needed();
    void enqueue_decode(const CacheKey &key, std::string file, unsigned expected_local_w, unsigned expected_local_h);
    void enqueue_dump(const UploadEntry &u, const PSX::UsedMode &mode, uint64_t clut_hash);
    void rescan_sheets(const PSX::Rect &dirty);
    PSX::HdTextureHandle font_handle(const FontSheet &sheet, const PSX::Rect &vram_rect,
                                     const PSX::UsedMode &mode, uint64_t clut_hash, bool &cache_hit);
    PSX::HdTextureHandle push_handle(const PSX::Rect &vram_rect, const CachedGpu &gpu);
    PackMatch find_pack_file(uint64_t tex_hash, uint64_t clut_hash) const;

    PSX::Renderer *renderer_ = nullptr;
    const uint16_t *last_vram_ = nullptr;
    TextureConfig cfg_;
    HdFont font_;
    std::vector<std::string> names_;
    uint64_t names_generation_ = 0;
    uint64_t frame_ = 0;
    uint64_t pack_generation_ = 1;
    size_t gpu_bytes_ = 0;

    std::vector<UploadEntry> uploads_;
    std::vector<FontSheet> sheets_;
    std::vector<PSX::HdTexture> frame_handles_;
    std::unordered_map<CacheKey, CachedGpu, CacheKeyHash> gpu_cache_;
    std::unordered_map<std::string, std::string> pack_index_;
    std::unordered_set<CacheKey, CacheKeyHash> pending_or_failed_;
    std::unordered_set<uint64_t> dumped_keys_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<DecodeRequest> decode_queue_;
    std::deque<DecodedImage> decoded_ready_;
    std::deque<DumpRequest> dump_queue_;
    std::atomic<bool> has_decoded_ready_{false};
    bool stop_ = false;
    std::thread worker_;
};

} // namespace HostHD

#endif
