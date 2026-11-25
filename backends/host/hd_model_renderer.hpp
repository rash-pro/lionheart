#ifndef FF8_HOST_HD_MODEL_RENDERER_HPP
#define FF8_HOST_HD_MODEL_RENDERER_HPP

#include "hd_models.hpp"
#include "renderer/renderer.hpp"
#include "vulkan/device.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace HostHD {

/* Clean-room Vulkan 3D HD model renderer built on parallel-psx's Granite
 * Vulkan::Device & CommandBuffer API.
 *
 * Presentation-only: renders skinned/rigid HD glTF 2.0 meshes directly into
 * parallel-psx's scaled VRAM plane with hardware Z-buffering, mipmapped
 * trilinear textures, alpha-mask & alpha-blend materials, and live PS1 GTE
 * lighting tint — without ever touching native software VRAM or guest state. */
class HdModelRenderer {
public:
    HdModelRenderer(Vulkan::Device &device, unsigned scaling, unsigned msaa);
    ~HdModelRenderer();

    /* Releases all cached GPU buffers/textures (safe to call on shutdown or pack reload). */
    void clear_cache();

    /* Renders a batch of matched HD 3D models into parallel-psx's scaled VRAM
     * framebuffer with a shared hardware depth buffer for true 3D occlusion. */
    void render_batch(PSX::Renderer &psx_renderer, const std::vector<HdModelDrawCall> &draws);

    size_t cached_model_count() const { return gpu_cache_.size(); }

private:
    struct GpuVertex {
        float pos[3];
        float normal[3];
        float uv[2];
        uint16_t joints[4];
        float weights[4];
    };
    static_assert(sizeof(GpuVertex) == 56, "GpuVertex layout must match hd_model.vert");

    struct GpuPrimitive {
        Vulkan::BufferHandle vbo;
        Vulkan::BufferHandle ibo;
        uint32_t index_count = 0;
        int texture_index = -1;
        AlphaMode alpha_mode = AlphaMode::Opaque;
        float alpha_cutoff = 0.5f;
        float base_color_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool double_sided = false;
    };

    struct GpuModel {
        uint64_t generation = 0;
        const HdModel *source_ptr = nullptr;
        std::vector<GpuPrimitive> primitives;
        std::vector<Vulkan::ImageHandle> textures;
    };

    const GpuModel *ensure_uploaded(const std::shared_ptr<const HdModel> &model);
    void ensure_resources();
    void draw_primitive(Vulkan::CommandBuffer &cmd, const HdModel &model, const GpuModel &gpu,
                        const GpuPrimitive &prim, const HdModelDrawCall &dc, bool blend_pass);

    Vulkan::Device &device_;
    unsigned scaling_ = 1;
    unsigned msaa_ = 1;
    uint64_t cache_generation_ = 0;
    Vulkan::Program *program_ = nullptr;
    Vulkan::ImageHandle white_texture_;
    std::unordered_map<std::string, std::unique_ptr<GpuModel>> gpu_cache_;
};

} // namespace HostHD

#endif
