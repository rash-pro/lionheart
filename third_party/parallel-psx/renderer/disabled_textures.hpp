// Host port: clean-room texture replacement hook (decoupled from libretro).
#pragma once
#include "../atlas/atlas.hpp"
#include "../vulkan/device.hpp"
#include <cstdint>
#include <vector>

namespace PSX {
struct HdTextureHandle {
    int index = -1;
    static HdTextureHandle make_none() { return {}; }
    bool operator==(const HdTextureHandle &o) const { return index == o.index; }
    bool operator!=(const HdTextureHandle &o) const { return index != o.index; }
    bool operator>(const HdTextureHandle &o) const { return index > o.index; }
};
struct TextureTrackerSaveState {};
struct UsedMode { TextureMode mode; unsigned palette_offset_x, palette_offset_y; };
struct LoadedImage { std::vector<uint8_t> owned_data; int width, height; };
struct HdTexture {
    struct { int x = 0, y = 0, width = 0, height = 0; } vram_rect, texel_rect;
    Vulkan::ImageHandle texture;
};
class Renderer;

struct HdTextureProvider {
    virtual ~HdTextureProvider() = default;
    virtual void set_uploader(Renderer *r) = 0;
    virtual void clear_region(Rect rect) = 0;
    virtual void blit(Rect dst, Rect src) = 0;
    virtual void upload(Rect rect, const uint16_t *vram) = 0;
    virtual void end_frame() = 0;
    virtual void on_queues_reset() = 0;
    virtual HdTextureHandle lookup(Rect vram_rect, const UsedMode &mode,
                                   unsigned tex_x, unsigned tex_y,
                                   bool &fastpath, bool &cache_hit) = 0;
    virtual HdTexture get(HdTextureHandle handle) = 0;
};

struct TextureTracker {
    bool dump_enabled = false, hd_textures_enabled = false;
    HdTextureProvider *provider = nullptr;
    Renderer *uploader = nullptr;
    void set_texture_uploader(Renderer *r) {
        uploader = r;
        if (provider) provider->set_uploader(r);
    }
    void set_provider(HdTextureProvider *p) {
        provider = p;
        if (provider && uploader) provider->set_uploader(uploader);
    }
    TextureTrackerSaveState save_state() { return {}; }
    void load_state(const TextureTrackerSaveState &) {}
    void clearRegion(Rect rect) { if (provider) provider->clear_region(rect); }
    void notifyReadback(Rect, uint16_t *) {}
    void endFrame() { if (provider) provider->end_frame(); }
    void blit(Rect dst, Rect src) { if (provider) provider->blit(dst, src); }
    void upload(Rect rect, uint16_t *vram) { if (provider) provider->upload(rect, vram); }
    void on_queues_reset() { if (provider) provider->on_queues_reset(); }
    HdTextureHandle get_hd_texture_index(Rect rect, UsedMode &mode, unsigned tex_x, unsigned tex_y,
                                         bool &fastpath, bool &cache_hit) {
        if (!provider || !hd_textures_enabled) return HdTextureHandle::make_none();
        return provider->lookup(rect, mode, tex_x, tex_y, fastpath, cache_hit);
    }
    HdTexture get_hd_texture(HdTextureHandle handle) {
        if (!provider || handle == HdTextureHandle::make_none()) return {};
        return provider->get(handle);
    }
};
}
