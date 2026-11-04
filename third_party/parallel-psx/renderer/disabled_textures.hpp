// Host port: texture replacement is deliberately not linked to libretro.
#pragma once
#include "../atlas/atlas.hpp"
#include "../vulkan/device.hpp"
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
struct HdTexture { struct { int x=0,y=0,width=0,height=0; } vram_rect, texel_rect; Vulkan::ImageHandle texture; };
class Renderer;
struct TextureTracker {
    bool dump_enabled = false, hd_textures_enabled = false;
    void set_texture_uploader(Renderer *) {}
    TextureTrackerSaveState save_state() { return {}; }
    void load_state(const TextureTrackerSaveState &) {}
    void clearRegion(Rect) {}
    void notifyReadback(Rect, uint16_t *) {}
    void endFrame() {}
    void blit(Rect, Rect) {}
    void upload(Rect, uint16_t *) {}
    void on_queues_reset() {}
    HdTextureHandle get_hd_texture_index(Rect, UsedMode &, unsigned, unsigned, bool &, bool &) { return {}; }
    HdTexture get_hd_texture(HdTextureHandle) { return {}; }
};
}
