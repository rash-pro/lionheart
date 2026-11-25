#include "hd_model_renderer.hpp"

#include <algorithm>
#include <cstring>

namespace HostHD {
namespace {

const uint32_t hd_model_vert_spv[] =
#include "hd_model.vert.inc"
;

const uint32_t hd_model_frag_spv[] =
#include "hd_model.frag.inc"
;

struct HdModelUniforms {
    float local_row0[4];
    float local_row1[4];
    float local_row2[4];
    float proj_params[4]; /* x = draw_ox + ofx, y = draw_oy + ofy, z = h, w = float(bone_count) */
    float tint_params[4]; /* xyz = live GTE tint, w = alpha_cutoff */
    float base_color[4];
    uint32_t flags[4];    /* x = alpha_mode, y = has_texture */
    float bone_rows[kMaxHdBones * 3][4];
};

static_assert(sizeof(HdModelUniforms) <= Vulkan::VULKAN_UBO_SIZE,
              "HdModelUniforms must fit inside Granite VULKAN_UBO_SIZE");

} // namespace

HdModelRenderer::HdModelRenderer(Vulkan::Device &device, unsigned scaling, unsigned msaa)
    : device_(device), scaling_(std::max(1u, scaling)), msaa_(std::max(1u, msaa)) {}

HdModelRenderer::~HdModelRenderer() {
    clear_cache();
    white_texture_.reset();
}

void HdModelRenderer::clear_cache() {
    gpu_cache_.clear();
}

void HdModelRenderer::ensure_resources() {
    if (!program_) {
        program_ = device_.request_program(hd_model_vert_spv, sizeof(hd_model_vert_spv),
                                           hd_model_frag_spv, sizeof(hd_model_frag_spv));
    }
    if (!white_texture_) {
        static const uint8_t white[4] = {255, 255, 255, 255};
        auto info = Vulkan::ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        Vulkan::ImageInitialData initial = {white, 0, 0};
        white_texture_ = device_.create_image(info, &initial);
    }
}

const HdModelRenderer::GpuModel *HdModelRenderer::ensure_uploaded(const std::shared_ptr<const HdModel> &model) {
    if (!model) return nullptr;
    if (model->generation != cache_generation_) {
        clear_cache();
        cache_generation_ = model->generation;
    }

    auto it = gpu_cache_.find(model->id);
    if (it != gpu_cache_.end() && it->second &&
        it->second->generation == model->generation &&
        it->second->source_ptr == model.get()) {
        return it->second.get();
    }

    auto gpu = std::make_unique<GpuModel>();
    gpu->generation = model->generation;
    gpu->source_ptr = model.get();

    /* Upload RGBA8 textures with automatic mipmap generation for trilinear filtering. */
    gpu->textures.reserve(model->textures.size());
    for (const auto &tex : model->textures) {
        if (tex.width > 0 && tex.height > 0 &&
            tex.rgba.size() >= size_t(tex.width) * size_t(tex.height) * 4u) {
            bool mips = (tex.width > 1 || tex.height > 1);
            auto info = Vulkan::ImageCreateInfo::immutable_2d_image(
                tex.width, tex.height, VK_FORMAT_R8G8B8A8_UNORM, mips);
            Vulkan::ImageInitialData initial = {tex.rgba.data(), 0, 0};
            gpu->textures.push_back(device_.create_image(info, &initial));
        } else {
            gpu->textures.push_back({});
        }
    }

    /* Upload interleaved vertex buffers (applying model.bone_remap once at upload time)
     * and uint32 index buffers per primitive. */
    gpu->primitives.reserve(model->primitives.size());
    for (const auto &prim : model->primitives) {
        if (prim.vertices.empty() || prim.indices.size() < 3) continue;

        std::vector<GpuVertex> verts(prim.vertices.size());
        for (size_t i = 0; i < prim.vertices.size(); ++i) {
            const auto &src = prim.vertices[i];
            auto &dst = verts[i];
            std::memcpy(dst.pos, src.pos, sizeof(dst.pos));
            std::memcpy(dst.normal, src.normal, sizeof(dst.normal));
            std::memcpy(dst.uv, src.uv, sizeof(dst.uv));
            /* Standard glTF 2.0 rigged models carry glTF skeleton joint indices &
             * inverseBindMatrices that do not correspond to raw PS1 GTE bone runs
             * unless an explicit bone_remap is provided; bind rigidly to bone 0 so
             * they render intact rather than stretching across unrelated bones. */
            if (model->has_gltf_skin && model->bone_remap.empty()) {
                dst.joints[0] = dst.joints[1] = dst.joints[2] = dst.joints[3] = 0;
                dst.weights[0] = 1.0f;
                dst.weights[1] = dst.weights[2] = dst.weights[3] = 0.0f;
            } else {
                std::memcpy(dst.weights, src.weights, sizeof(dst.weights));
                for (int k = 0; k < 4; ++k) {
                    uint16_t j = src.joints[k];
                    if (size_t(j) < model->bone_remap.size() && model->bone_remap[j] >= 0) {
                        j = uint16_t(model->bone_remap[j]);
                    }
                    dst.joints[k] = j;
                }
            }
        }

        Vulkan::BufferCreateInfo vbo_info;
        vbo_info.domain = Vulkan::BufferDomain::Device;
        vbo_info.size = verts.size() * sizeof(GpuVertex);
        vbo_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        Vulkan::BufferCreateInfo ibo_info;
        ibo_info.domain = Vulkan::BufferDomain::Device;
        ibo_info.size = prim.indices.size() * sizeof(uint32_t);
        ibo_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

        GpuPrimitive gp;
        gp.vbo = device_.create_buffer(vbo_info, verts.data());
        gp.ibo = device_.create_buffer(ibo_info, prim.indices.data());
        gp.index_count = uint32_t(prim.indices.size());
        gp.texture_index = prim.texture_index;
        gp.alpha_mode = prim.alpha_mode;
        gp.alpha_cutoff = prim.alpha_cutoff;
        std::memcpy(gp.base_color_factor, prim.base_color_factor, sizeof(gp.base_color_factor));
        gp.double_sided = prim.double_sided || model->double_sided;
        if (gp.vbo && gp.ibo) gpu->primitives.push_back(std::move(gp));
    }

    const GpuModel *raw = gpu.get();
    gpu_cache_[model->id] = std::move(gpu);
    return raw;
}

void HdModelRenderer::draw_primitive(Vulkan::CommandBuffer &cmd, const HdModel &model, const GpuModel &gpu,
                                     const GpuPrimitive &prim, const HdModelDrawCall &dc, bool blend_pass) {
    auto *ubo = cmd.allocate_typed_constant_data<HdModelUniforms>(0, 1, 1);
    if (!ubo) return;

    float lm[3][4];
    model.local_transform(lm);
    std::memcpy(ubo->local_row0, lm[0], sizeof(ubo->local_row0));
    std::memcpy(ubo->local_row1, lm[1], sizeof(ubo->local_row1));
    std::memcpy(ubo->local_row2, lm[2], sizeof(ubo->local_row2));

    size_t bcount = std::min(dc.bones.size(), kMaxHdBones);
    if (bcount == 0) bcount = 1;
    ubo->proj_params[0] = dc.draw_offset_x + dc.ofx;
    ubo->proj_params[1] = dc.draw_offset_y + dc.ofy;
    ubo->proj_params[2] = dc.h;
    ubo->proj_params[3] = float(bcount);

    ubo->tint_params[0] = dc.tint_rgb[0];
    ubo->tint_params[1] = dc.tint_rgb[1];
    ubo->tint_params[2] = dc.tint_rgb[2];
    ubo->tint_params[3] = prim.alpha_cutoff;

    std::memcpy(ubo->base_color, prim.base_color_factor, sizeof(ubo->base_color));

    bool has_tex = (prim.texture_index >= 0 &&
                    size_t(prim.texture_index) < gpu.textures.size() &&
                    bool(gpu.textures[size_t(prim.texture_index)]));
    ubo->flags[0] = uint32_t(prim.alpha_mode);
    ubo->flags[1] = has_tex ? 1u : 0u;
    ubo->flags[2] = 0u;
    ubo->flags[3] = 0u;

    if (!dc.bones.empty()) {
        for (size_t bi = 0; bi < bcount; ++bi) {
            const auto &bm = dc.bones[bi];
            for (int r = 0; r < 3; ++r) {
                float *dst = ubo->bone_rows[bi * 3u + size_t(r)];
                dst[0] = bm.r[r][0];
                dst[1] = bm.r[r][1];
                dst[2] = bm.r[r][2];
                dst[3] = bm.t[r];
            }
        }
    } else {
        std::memset(ubo->bone_rows[0], 0, sizeof(float) * 12);
        ubo->bone_rows[0][0] = 1.0f;
        ubo->bone_rows[1][1] = 1.0f;
        ubo->bone_rows[2][2] = 1.0f;
    }

    const Vulkan::ImageView &tex_view = has_tex ? gpu.textures[size_t(prim.texture_index)]->get_view()
                                                : white_texture_->get_view();
    cmd.set_texture(0, 0, tex_view, Vulkan::StockSampler::TrilinearWrap);

    if (prim.double_sided) {
        cmd.set_cull_mode(VK_CULL_MODE_NONE);
    } else {
        cmd.set_cull_mode(VK_CULL_MODE_BACK_BIT);
        cmd.set_front_face(model.flip_winding ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    }

    if (blend_pass) {
        cmd.set_depth_test(true, false);
        cmd.set_depth_compare(VK_COMPARE_OP_LESS_OR_EQUAL);
        cmd.set_blend_enable(true);
        cmd.set_blend_op(VK_BLEND_OP_ADD, VK_BLEND_OP_ADD);
        cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ZERO,
                              VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE);
    } else {
        cmd.set_depth_test(true, true);
        cmd.set_depth_compare(VK_COMPARE_OP_LESS_OR_EQUAL);
        cmd.set_blend_enable(false);
    }

    int cl = std::clamp(dc.clip_left, 0, int(PSX::FB_WIDTH) - 1);
    int ct = std::clamp(dc.clip_top, 0, int(PSX::FB_HEIGHT) - 1);
    int cr = std::clamp(dc.clip_right, cl, int(PSX::FB_WIDTH) - 1);
    int cb = std::clamp(dc.clip_bottom, ct, int(PSX::FB_HEIGHT) - 1);
    VkRect2D scissor = {
        {int32_t(cl * int(scaling_)), int32_t(ct * int(scaling_))},
        {uint32_t(cr - cl + 1) * scaling_, uint32_t(cb - ct + 1) * scaling_}
    };
    cmd.set_scissor(scissor);

    cmd.set_vertex_binding(0, *prim.vbo, 0, sizeof(GpuVertex));
    cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, pos));
    cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, normal));
    cmd.set_vertex_attrib(2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv));
    cmd.set_vertex_attrib(3, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(GpuVertex, joints));
    cmd.set_vertex_attrib(4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, weights));
    cmd.set_index_buffer(*prim.ibo, 0, VK_INDEX_TYPE_UINT32);
    cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    cmd.draw_indexed(prim.index_count);
}

void HdModelRenderer::render_batch(PSX::Renderer &psx_renderer, const std::vector<HdModelDrawCall> &draws) {
    if (draws.empty()) return;
    ensure_resources();
    if (!program_ || !white_texture_) return;

    /* Lazily upload all models in this batch and compute union clip rect in PS1 VRAM coords. */
    std::vector<const GpuModel *> uploaded(draws.size(), nullptr);
    int min_x = int(PSX::FB_WIDTH), min_y = int(PSX::FB_HEIGHT);
    int max_x = -1, max_y = -1;
    bool any_valid = false;

    for (size_t i = 0; i < draws.size(); ++i) {
        const auto &dc = draws[i];
        if (!dc.model || dc.bones.empty()) continue;
        const GpuModel *gm = ensure_uploaded(dc.model);
        if (!gm || gm->primitives.empty()) continue;
        uploaded[i] = gm;
        any_valid = true;
        int cl = std::clamp(dc.clip_left, 0, int(PSX::FB_WIDTH) - 1);
        int ct = std::clamp(dc.clip_top, 0, int(PSX::FB_HEIGHT) - 1);
        int cr = std::clamp(dc.clip_right, cl, int(PSX::FB_WIDTH) - 1);
        int cb = std::clamp(dc.clip_bottom, ct, int(PSX::FB_HEIGHT) - 1);
        min_x = std::min(min_x, cl);
        min_y = std::min(min_y, ct);
        max_x = std::max(max_x, cr);
        max_y = std::max(max_y, cb);
    }

    if (!any_valid || max_x < min_x || max_y < min_y) return;

    PSX::Rect vram_rect(unsigned(min_x), unsigned(min_y),
                        unsigned(max_x - min_x + 1), unsigned(max_y - min_y + 1));

    /* Synchronize parallel-psx scaled VRAM framebuffer & flush PS1 background draws first. */
    Vulkan::ImageView &color_view = psx_renderer.begin_hd_model_pass(vram_rect);
    auto &cmd = *psx_renderer.command_buffer_hack_fixme();

    VkFormat depth_fmt = device_.get_default_depth_format();
    Vulkan::ImageView &depth_view = device_.get_transient_attachment(
        PSX::FB_WIDTH * scaling_, PSX::FB_HEIGHT * scaling_, depth_fmt, 0, msaa_, 1);

    Vulkan::RenderPassInfo rp = {};
    rp.color_attachments[0] = &color_view;
    rp.depth_stencil = &depth_view;
    rp.num_color_attachments = 1;
    rp.load_attachments = 1u << 0;
    rp.store_attachments = 1u << 0;
    rp.op_flags = Vulkan::RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT;
    rp.clear_depth_stencil = {1.0f, 0};
    rp.render_area = {
        {int32_t(vram_rect.x * scaling_), int32_t(vram_rect.y * scaling_)},
        {vram_rect.width * scaling_, vram_rect.height * scaling_}
    };

    cmd.begin_render_pass(rp);
    cmd.set_opaque_state();
    cmd.set_program(*program_);

    VkViewport vp = {
        0.0f, 0.0f,
        float(PSX::FB_WIDTH * scaling_), float(PSX::FB_HEIGHT * scaling_),
        0.0f, 1.0f
    };
    cmd.set_viewport(vp);

    /* Pass 1: Opaque and Alpha-Masked (MASK) primitives with depth write ON. */
    for (size_t i = 0; i < draws.size(); ++i) {
        if (!uploaded[i]) continue;
        const auto &dc = draws[i];
        const auto &gm = *uploaded[i];
        for (const auto &prim : gm.primitives) {
            if (prim.alpha_mode == AlphaMode::Blend) continue;
            draw_primitive(cmd, *dc.model, gm, prim, dc, false);
        }
    }

    /* Pass 2: Alpha-Blended (BLEND) primitives (hair strands, cloth edges) with depth test ON, depth write OFF. */
    for (size_t i = 0; i < draws.size(); ++i) {
        if (!uploaded[i]) continue;
        const auto &dc = draws[i];
        const auto &gm = *uploaded[i];
        for (const auto &prim : gm.primitives) {
            if (prim.alpha_mode != AlphaMode::Blend) continue;
            draw_primitive(cmd, *dc.model, gm, prim, dc, true);
        }
    }

    cmd.end_render_pass();
    psx_renderer.end_hd_model_pass();
}

} // namespace HostHD
