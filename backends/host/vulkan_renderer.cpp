#ifdef _WIN32
#include <windows.h>
#endif
#include "vulkan_renderer.h"
#include "viewport.h"
#include "osd.h"
#include "screenshot.h"
#ifdef FF8_GUI
#include "gui_internal.h"
#include "gui_vulkan.h"
#endif
#include "vulkan_wsi.hpp"
#include "movies.h"
#include "hd_textures.hpp"
#include "hd_models.hpp"
#include "hd_model_renderer.hpp"
#include "ff8_ram_layout.h"
extern "C" {
#include "cpu.h"
#include "gpu_raster.h"
#ifdef FF8_PGXP
#include "pgxp.h"
#endif
}
#include "renderer/renderer.hpp"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace Vulkan;
using namespace PSX;
namespace {
#ifdef FF8_VULKAN_TESTING
unsigned fault_phase=0, test_maps=0;
PFN_vkWaitForFences real_wait=nullptr;
PFN_vkGetQueryPoolResults real_query=nullptr;
VKAPI_ATTR VkResult VKAPI_CALL fail_wait(VkDevice,uint32_t,const VkFence *,VkBool32,uint64_t) {
    vkWaitForFences=real_wait; fault_phase=0; return VK_ERROR_DEVICE_LOST;
}
VKAPI_ATTR VkResult VKAPI_CALL fail_query(VkDevice,VkQueryPool,uint32_t,uint32_t,size_t,void *,VkDeviceSize,VkQueryResultFlags) {
    vkGetQueryPoolResults=real_query; fault_phase=0; return VK_ERROR_DEVICE_LOST;
}
void inject(unsigned phase) {
    if (fault_phase==phase) { fault_phase=0; if (phase==2) throw 42; throw std::runtime_error("injected contract failure"); }
}
#else
void inject(unsigned) {}
#endif
struct Options {
    bool enabled=false, true_color=false;
    unsigned scale=1, msaa=1;
    const char *frames=nullptr, *timings=nullptr;
} options;
// Smooth motion paces presents itself (host_gpu_paced).
bool paced_wanted=false;
HostHD::TextureConfig hd_config;
std::vector<std::string> hd_names;
HostHD::ModelRegistry hd_models_registry;
HostHD::HdSceneTracker hd_scene;
#ifdef FF8_PGXP
void hd_models_gte_sink(void *, const cpu_t *c, uint32_t packed_sxy, int64_t x_q16, int64_t y_q16, uint32_t z) {
    if (!c || !hd_models_registry.has_any_active()) return;
    uint32_t rot[5] = {
        c->gte_ctrl[0], c->gte_ctrl[1], c->gte_ctrl[2], c->gte_ctrl[3], c->gte_ctrl[4]
    };
    int32_t tr[3] = {
        int32_t(c->gte_ctrl[5]), int32_t(c->gte_ctrl[6]), int32_t(c->gte_ctrl[7])
    };
    int32_t ofx = int32_t(c->gte_ctrl[24]);
    int32_t ofy = int32_t(c->gte_ctrl[25]);
    uint32_t h = c->gte_ctrl[26] & 0xffffu;
    int16_t in_v[3] = {
        int16_t(uint16_t(c->gte_data[0] & 0xffffu)),
        int16_t(uint16_t((c->gte_data[0] >> 16) & 0xffffu)),
        int16_t(uint16_t(c->gte_data[1] & 0xffffu))
    };
    HostHD::SceneContext ctx{};
    uint16_t mode_w = uint16_t(c->ram[FF8_MODE]) | uint16_t(uint16_t(c->ram[FF8_MODE + 1]) << 8);
    ctx.mode = static_cast<HostHD::SceneMode>(mode_w);
    ctx.field_id = uint16_t(c->ram[FF8_FIELD_ID]) | uint16_t(uint16_t(c->ram[FF8_FIELD_ID + 1]) << 8);
    ctx.party[0] = c->ram[FF8_SAVEMAP + FF8_PARTY + 0];
    ctx.party[1] = c->ram[FF8_SAVEMAP + FF8_PARTY + 1];
    ctx.party[2] = c->ram[FF8_SAVEMAP + FF8_PARTY + 2];
    hd_scene.record_gte_projection(rot, tr, ofx, ofy, h, in_v, packed_sxy,
                                   float(double(x_q16) / 65536.0),
                                   float(double(y_q16) / 65536.0),
                                   z, ctx);
}
void sync_hd_models_sink() {
    pgxp_record_sink(hd_models_registry.has_any_active() ? hd_models_gte_sink : nullptr, nullptr);
}
#else
void sync_hd_models_sink() {}
#endif
struct Backend {
    gpu_raster_t *raster=nullptr;
    VkInstance instance=VK_NULL_HANDLE;
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    SDL_Window *window=nullptr;
    std::unique_ptr<HostGPU::Swapchain> wsi;
    std::vector<VkSurfaceKHR> old_surfaces;
    bool failed=false;
    // The window's image is acquired (its wait semaphore not yet given to a
    // submission) and, once drawn into, filled: only then is it presented.
    bool acquired=false, filled=false;
    uint32_t present_index=0;
    Semaphore acquire_wait;
    std::unique_ptr<Context> context;
    std::unique_ptr<Device> device;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<HostHD::TextureProvider> hd_textures;
    std::unique_ptr<HostHD::HdModelRenderer> hd_model_renderer;
    // Native VRAM is authoritative. Upscaled pixels never reach the guest.
    std::vector<uint8_t> rgb;
    unsigned output_width=1, output_height=1;
    std::array<uint64_t,16> dirty_rows[512] = {};
    bool dirty=false;
    // Smooth motion draws the same commands again from a kept point: VRAM as
    // it stood there, and the tiles written since (Renderer::copy_vram).
    bool checkpoints=false;
    Renderer::VramCopy kept;
    uint64_t written[2]={0,0};
    // A picture between game frames is being drawn (host_gpu_between): above
    // 1x the software rasterizer draws only into the parts of VRAM the picture
    // before read software pixels from (read: what copies the renderer draws
    // natively and the textures of primitives drawn in software read), which
    // the pictures of a game frame share. What it left out is stale until
    // taken back from the GPU (fresh()).
    bool between=false;
    bool hd_frame_ended=false;
    // VRAM in blocks of 16x16 pixels: a palette next to the picture must not
    // share a block with it.
    struct Blocks {
        uint64_t bits[32]={};
        void clear() noexcept { std::fill(std::begin(bits),std::end(bits),uint64_t(0)); }
        template<class F> static void each(unsigned x,unsigned y,unsigned w,unsigned h,F f) noexcept {
            if (!w || !h) return;
            if (w>=1024) { x=0; w=1024; }
            if (h>=512) { y=0; h=512; }
            for (unsigned by=(y&511)/16, rows=0; rows<(h+(y&511)%16+15)/16; ++rows, by=(by+1)%32)
                for (unsigned bx=(x&1023)/16, cols=0; cols<(w+(x&1023)%16+15)/16; ++cols, bx=(bx+1)%64)
                    f(by*64+bx);
        }
        void add(unsigned x,unsigned y,unsigned w,unsigned h) noexcept {
            each(x,y,w,h,[&](unsigned bit) { bits[bit/64]|=uint64_t(1)<<(bit%64); });
        }
        bool meets(unsigned x,unsigned y,unsigned w,unsigned h) const noexcept {
            bool found=false;
            each(x,y,w,h,[&](unsigned bit) { found=found || (bits[bit/64]>>(bit%64)&1); });
            return found;
        }
    } read, reading, stale;
    // When the next present is to be shown, in seconds of SDL's counter (host_gpu_present_at).
    double present_target=0;
    void touch(unsigned x,unsigned y,unsigned w,unsigned h) noexcept {
        if (!checkpoints || !w || !h) return;
        if (w>=1024) { x=0; w=1024; }
        if (h>=512) { y=0; h=512; }
        unsigned tile=Renderer::VRAM_TILE;
        for (unsigned ty=(y&511)/tile, rows=0; rows<(h+(y&511)%tile+tile-1)/tile; ++rows, ty=(ty+1)%Renderer::VRAM_TILES_Y)
            for (unsigned tx=(x&1023)/tile, cols=0; cols<(w+(x&1023)%tile+tile-1)/tile; ++cols, tx=(tx+1)%Renderer::VRAM_TILES_X) {
                unsigned bit=ty*Renderer::VRAM_TILES_X+tx;
                written[bit/64]|=uint64_t(1)<<(bit%64);
            }
    }
    // Framebuffer captures used as 15-bit feedback textures need the oracle's
    // per-primitive truncation. Track their provenance through copies and draws.
    std::array<uint64_t,16> effect_pixels[512] = {};
    unsigned effect_counts[32] = {};
    uint32_t effect_pages=0;
    std::array<uint64_t,16> kept_effect_pixels[512] = {};
    unsigned kept_effect_counts[32] = {};
    uint32_t kept_effect_pages=0;
    void effect(unsigned x,unsigned y,unsigned w,unsigned h,bool value) {
        if (!value && !effect_pages) return;
        for (unsigned row=0;row<h;++row) {
            unsigned yy=(y+row)&511, xx=x, left=w;
            while (left) {
                xx&=1023;
                unsigned count=std::min({left,1024-xx,64-(xx&63)});
                uint64_t mask=count==64 ? ~uint64_t(0) : ((uint64_t(1)<<count)-1)<<(xx&63);
                auto &bits=effect_pixels[yy][xx/64];
                uint64_t next=value ? bits|mask : bits&~mask;
                unsigned page=(yy/256)*16+xx/64;
                if (next!=bits) {
                    if (value) effect_counts[page]+=__builtin_popcountll(next^bits);
                    else effect_counts[page]-=__builtin_popcountll(next^bits);
                    bits=next;
                    if (effect_counts[page]) effect_pages|=uint32_t(1)<<page;
                    else effect_pages&=~(uint32_t(1)<<page);
                }
                xx+=count; left-=count;
            }
        }
    }
    bool effect_source(unsigned x,unsigned y,unsigned w,unsigned h) const {
        if (!effect_pages || !w || !h) return false;
        for (unsigned row=0;row<h;++row) {
            unsigned yy=(y+row)&511, xx=x, left=w;
            while (left) {
                xx&=1023;
                unsigned count=std::min({left,1024-xx,64-(xx&63)});
                uint64_t mask=count==64 ? ~uint64_t(0) : ((uint64_t(1)<<count)-1)<<(xx&63);
                if (effect_pixels[yy][xx/64] & mask) return true;
                xx+=count; left-=count;
            }
        }
        return false;
    }
    bool effect_texture(const gpu_raster_t *s,gpu_paint_t p,unsigned u0,unsigned v0,unsigned w,unsigned h) const {
        if (!effect_pages || !p.textured || ((s->mode>>7)&3)<2) return false;
        unsigned tx=(s->mode&15)*64, ty=((s->mode>>4)&1)*256;
        if ((s->window&0x3ff)!=0) return effect_source(tx,ty,256,256);
        return effect_source(tx+u0,ty+v0,w ? w : 1u,h ? h : 1u);
    }
    std::vector<HostMovies::Patch> movie_patches;
    std::shared_ptr<const HostMovies::Frame> movie_frame;
    BufferHandle movie_buffer;
    bool overlaps_movie(unsigned x,unsigned y,unsigned words) const {
        x&=1023; y&=511;
        for (const auto &patch:movie_patches) {
            if (y<patch.y || y>=patch.y+patch.rows) continue;
            if (words>=1024 || (x<patch.x+patch.words && patch.x<x+words) ||
                (x+words>1024 && patch.x<x+words-1024)) return true;
        }
        return false;
    }
    void movie_span(unsigned x,unsigned y,const uint8_t *data,unsigned words) {
        HostMovies::Patch patch;
        if (!HostMovies::span(x,y,data,words,patch)) return;
        if (!movie_patches.empty()) {
            auto &last=movie_patches.back();
            if (last.x==x && last.y+last.rows==y && last.words==words && last.frame==patch.frame) {
                ++last.rows; return;
            }
        }
        movie_patches.push_back(std::move(patch));
    }
    FILE *timings=nullptr;
    uint64_t frame=0, prior_submits=0, prior_compile_ns=0, prior_compiles=0;
#ifdef FF8_GUI
    uint64_t presentation_serial=0;
    struct GuiSample { QueryPoolHandle first,last; };
    std::deque<GuiSample> gui_samples;
    std::deque<double> gui_times;
    void collect_gui() {
        while (!gui_samples.empty()) {
            const auto &sample=gui_samples.front();
            if (!sample.first->is_signalled() || !sample.last->is_signalled()) break;
            double first=sample.first->get_timestamp(),last=sample.last->get_timestamp();
            if (first>=0 && last>=first) {
                if (gui_times.size()==256) gui_times.pop_front();
                gui_times.push_back((last-first)*1000);
            }
            gui_samples.pop_front();
        }
    }
#endif
    uint64_t wsi_frame() const {
#ifdef FF8_GUI
        return presentation_serial;
#else
        return frame;
#endif
    }
    double cpu_ms=0, soft_ms=0, capture_ms=0;
    uint64_t upload_bytes=0;
    struct Sample {
        uint64_t frame, bytes, submits, backpressure;
        double cpu, soft, capture, submit, wall, submit_wall, pipeline_ms;
        uint64_t pipelines;
        unsigned draws, vertices, passes;
        QueryPoolHandle first, last, present_first, present_last, osd_first, osd_last;
    };
    std::deque<Sample> samples;
    Clock::time_point frame_start=Clock::now();
    QueryPoolHandle first, last, present_first, present_last, osd_first, osd_last;
    gpu_scanout_t scanout_timing{};
    int native_width=0, native_height=0;
    uint64_t scanout_revision=0;
    ImageHandle image; // GPU scanout; CPU mapping is only for explicit captures.
    ImageHandle scanouts[3];
    // CRT intermediates, the window's CRT target and the offscreen capture target,
    // recreated on size changes.
    ImageHandle crt_lines, crt_beam, crt_glow_h, crt_glow, output, capture_output;
#ifdef FF8_GUI
    struct BackdropKey {
        uint64_t scanout_revision=0;
        bool has_image=false;
        unsigned extent_width=0, extent_height=0;
        VkFormat format=VK_FORMAT_UNDEFINED;
        unsigned preset=0;
        int scale_mode=0, linear=0;
        unsigned scale=0;
        int native_width=0, native_height=0;
        unsigned output_width=0, output_height=0;
        gpu_scanout_t scanout_timing{};
        bool operator==(const BackdropKey &o) const noexcept {
            return scanout_revision==o.scanout_revision && has_image==o.has_image &&
                   extent_width==o.extent_width && extent_height==o.extent_height &&
                   format==o.format && preset==o.preset &&
                   scale_mode==o.scale_mode && linear==o.linear && scale==o.scale &&
                   native_width==o.native_width && native_height==o.native_height &&
                   output_width==o.output_width && output_height==o.output_height &&
                   scanout_timing.horizontal==o.scanout_timing.horizontal &&
                   scanout_timing.vertical==o.scanout_timing.vertical &&
                   scanout_timing.mode==o.scanout_timing.mode;
        }
    };
    struct OsdKey {
        bool active=false;
        unsigned revision=0;
        uint8_t opacity=0;
        int x=0, y=0, w=0, h=0;
        bool operator==(const OsdKey &o) const noexcept {
            return active==o.active && (!active || (revision==o.revision && opacity==o.opacity &&
                   x==o.x && y==o.y && w==o.w && h==o.h));
        }
    };
    struct ComposedGuiKey {
        BackdropKey backdrop{};
        uint64_t draw_hash=0;
        uint64_t font_gen=0;
        OsdKey osd{}, booster_osd{};
        VkImage target_image=VK_NULL_HANDLE;
        bool operator==(const ComposedGuiKey &o) const noexcept {
            return backdrop==o.backdrop && draw_hash==o.draw_hash &&
                   font_gen==o.font_gen && osd==o.osd && booster_osd==o.booster_osd &&
                   target_image==o.target_image;
        }
    };
    ImageHandle gui_backdrop;
    BackdropKey gui_backdrop_key{};
    bool gui_backdrop_valid=false;
    ComposedGuiKey gui_composed_output_key{}, gui_composed_capture_key{};
    bool gui_composed_output_valid=false, gui_composed_capture_valid=false;
    void invalidate_gui_composed() noexcept {
        gui_composed_output_valid=false;
        gui_composed_capture_valid=false;
    }
#endif
    std::vector<uint8_t> presented;
    ImageHandle osd_image, booster_osd_image;
    unsigned osd_revision=0, booster_osd_revision=0;
    Backend() = default;
    ~Backend() {
        if (renderer && failed) renderer->abandon();
        if (renderer) renderer->flush();
        if (device) device->wait_idle();
        kept=Renderer::VramCopy();
#ifdef FF8_GUI
        host_gui_vulkan_shutdown();
#endif
        if (wsi && wsi->needs_present_grace()) SDL_Delay(250); // Shutdown only, compatibility retirement grace.
        wsi.reset();
        for (auto old:old_surfaces) vkDestroySurfaceKHR(instance,old,nullptr);
        collect();
        samples.clear();
#ifdef FF8_GUI
        gui_samples.clear(); gui_times.clear();
#endif
        for (auto &scanout:scanouts) scanout.reset();
        crt_lines.reset(); crt_beam.reset(); crt_glow_h.reset(); crt_glow.reset(); output.reset(); capture_output.reset();
#ifdef FF8_GUI
        gui_backdrop.reset(); gui_backdrop_valid=false; invalidate_gui_composed();
#endif
        osd_image.reset(); booster_osd_image.reset();
        movie_buffer.reset(); movie_frame.reset(); movie_patches.clear();
        first.reset(); last.reset(); present_first.reset(); present_last.reset(); osd_first.reset(); osd_last.reset();
        hd_model_renderer.reset();
        image.reset(); renderer.reset(); device.reset(); context.reset();
        if (surface) vkDestroySurfaceKHR(instance,surface,nullptr);
        if (instance) vkDestroyInstance(instance,nullptr);
        if (timings) std::fclose(timings);
    }
    void collect() {
        while (!samples.empty()) {
            const auto &s=samples.front();
            if (s.first && (!s.first->is_signalled() || !s.last->is_signalled())) break;
            if (s.present_first && (!s.present_first->is_signalled() || !s.present_last->is_signalled())) break;
            if (s.osd_first && (!s.osd_first->is_signalled() || !s.osd_last->is_signalled())) break;
            auto span=[](const QueryPoolHandle &a,const QueryPoolHandle &b) {
                return a && b && a->get_timestamp()>=0 && b->get_timestamp()>=0 ? (b->get_timestamp()-a->get_timestamp())*1000 : -1;
            };
            double gpu=span(s.first,s.last), present=span(s.present_first,s.present_last);
            if (timings) std::fprintf(timings,"%llu,%.6f,%.6f,%.6f,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%llu\n",
                (unsigned long long)s.frame,s.cpu,gpu,s.wall,s.draws,s.vertices,s.passes,
                s.soft,s.cpu-s.soft-s.capture-s.submit,s.submit,s.capture,
                (unsigned long long)s.bytes,(unsigned long long)s.submits,(unsigned long long)s.backpressure,s.submit_wall,present,span(s.osd_first,s.osd_last),s.pipeline_ms,(unsigned long long)s.pipelines);
            samples.pop_front();
        }
    }
    void begin() {
        if (timings && !first) first=renderer->command_buffer_hack_fixme()->write_timestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    }
    void mark(unsigned x, unsigned y, unsigned n) noexcept {
        touch(x,y,n,1);
        dirty=true; y&=511;
        while (n) {
            x&=1023;
            unsigned count=std::min({n,1024-x,64-(x&63)});
            uint64_t mask=count==64 ? ~uint64_t(0) : ((uint64_t(1)<<count)-1)<<(x&63);
            dirty_rows[y][x/64]|=mask;
            x+=count; n-=count;
        }
    }
    void upload(Rect rect) {
        if (!rect.width || !rect.height) return;
        upload_bytes+=uint64_t(rect.width)*rect.height*2;
        renderer->set_mask_test(false); renderer->set_force_mask_bit(false);
        auto b=renderer->copy_cpu_to_vram(rect,options.scale>1 || options.true_color || options.msaa>1);
        uint16_t *p=renderer->begin_copy(b);
        if (!p) throw std::runtime_error("Native upload mapping failed");
        for (unsigned y=0;y<rect.height;++y)
            std::memcpy(p+y*rect.width,raster->vram+(rect.y+y)*1024+rect.x,rect.width*2);
        renderer->end_copy(b);
        renderer->notify_texture_upload(rect, raster->vram);
        if (hd_models_registry.has_any_active())
            hd_scene.record_vram_upload(rect.x, rect.y, rect.width, rect.height, raster->vram);
    }
    void uploads() {
        if (!dirty) return;
        dirty=false;
        for (unsigned y=0;y<512;) {
            auto mask=dirty_rows[y];
            if (std::all_of(mask.begin(),mask.end(),[](uint64_t v){ return v==0; })) { ++y; continue; }
            unsigned end=y+1;
            while (end<512 && dirty_rows[end]==mask) ++end;
            for (unsigned x=0;x<1024;) {
                if (!(mask[x/64]>>(x&63))) { x=(x/64+1)*64; continue; }
                if (!(mask[x/64]&(uint64_t(1)<<(x&63)))) { ++x; continue; }
                unsigned start=x++;
                while (x<1024 && (mask[x/64]&(uint64_t(1)<<(x&63)))) ++x;
                upload({start,y,x-start,end-y});
            }
            for (;y<end;++y) dirty_rows[y].fill(0);
        }
        for (const auto &patch:movie_patches) {
            if (movie_frame!=patch.frame) {
                BufferCreateInfo info;
                info.domain=BufferDomain::Host;
                info.size=patch.frame->rgba.size(); info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                movie_buffer=device->create_buffer(info,patch.frame->rgba.data());
                movie_frame=patch.frame;
            }
            touch(patch.x,patch.y,patch.words,patch.rows);
            renderer->host_movie_upload({patch.x,patch.y,patch.words,patch.rows},patch.origin_x,patch.origin_y,
                                        patch.rgb24,*movie_buffer,patch.frame->width,patch.frame->height);
        }
        movie_patches.clear();
    }
    void native_download(uint16_t *out) {
        // Explicit verification only: echo native bytes through a GPU buffer.
        // GPUREAD and DMA read raster->vram directly, without this round trip.
        BufferCreateInfo info;
        info.domain=BufferDomain::Device; info.size=1024*512*2;
        info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto native=device->create_buffer(info,raster->vram);
        info.domain=BufferDomain::CachedHost; info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto download=device->create_buffer(info,nullptr);
        auto cmd=device->request_command_buffer();
        cmd->full_barrier(); cmd->copy_buffer(*download,*native);
        cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
        Fence fence; device->submit(cmd,&fence);
        if (!fence || !fence->wait()) throw std::runtime_error("Native verification fence failed");
        const void *p=device->map_host_buffer(*download,MEMORY_ACCESS_READ_BIT);
        if (!p) throw std::runtime_error("Native verification mapping failed");
        std::memcpy(out,p,1024*512*2);
        device->unmap_host_buffer(*download,MEMORY_ACCESS_READ_BIT);
    }
} ;
std::unique_ptr<Backend> backend;
void degrade(const char *where, const char *reason) noexcept {
    if (!backend || !backend->failed)
        std::fprintf(stderr,"Vulkan warning (%s): %s; falling back to soft presentation\n",where,reason);
    if (backend) {
        backend->failed=true;
#ifdef FF8_GUI
        backend->gui_backdrop_valid=false;
        backend->invalidate_gui_composed();
#endif
    }
    options.enabled=false;
}
template<class Work> bool gpu_work(const char *where, Work work) noexcept {
    if (!backend || backend->failed) return false;
    try {
        if (!std::strcmp(where,"triangle")) inject(1);
        work(); return true;
    }
    catch (const std::exception &e) { degrade(where,e.what()); }
    catch (...) { degrade(where,"unknown backend exception"); }
    return false;
}

double thread_ms() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)) return 0;
    ULARGE_INTEGER k,u;
    k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
    return double(k.QuadPart+u.QuadPart)*1e-4;
#else
    timespec t={}; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
    return double(t.tv_sec)*1000+double(t.tv_nsec)*1e-6;
#endif
}
struct CpuTimer {
    double start=backend && backend->timings ? thread_ms() : 0;
    ~CpuTimer() { if (start) backend->cpu_ms+=thread_ms()-start; }
};
struct SoftTimer {
    double start=backend->timings ? thread_ms() : 0;
    ~SoftTimer() { if (start) backend->soft_ms+=thread_ms()-start; }
};
bool active(gpu_raster_t *s) { return backend && !backend->failed && backend->raster==s; }
// Textures are being replaced (HD font or a texture pack): primitives then
// report the texels they cover exactly. Otherwise nothing here changes.
bool hd_replacing() { return hd_config.font_mode!=HostHD::FontMode::Off || hd_config.textures_enabled; }
Vertex vertex(gpu_vertex_t v) {
    return {float(v.x),float(v.y),1.0f,unsigned(v.r)|(unsigned(v.g)<<8)|(unsigned(v.b)<<16),
            uint16_t(v.u),uint16_t(v.v)};
}
void state(gpu_raster_t *s, gpu_paint_t p, PrimitiveType type) {
    auto &r=*backend->renderer;
    backend->begin(); backend->uploads();
    static const TextureMode modes[]={TextureMode::Palette4bpp,TextureMode::Palette8bpp,TextureMode::ABGR1555,TextureMode::ABGR1555};
    static const SemiTransparentMode blends[]={SemiTransparentMode::Average,SemiTransparentMode::Add,SemiTransparentMode::Sub,SemiTransparentMode::AddQuarter};
    r.set_texture_mode(p.textured ? modes[(s->mode>>7)&3] : TextureMode::None);
    r.set_semi_transparent(p.semi ? blends[(s->mode>>5)&3] : SemiTransparentMode::None);
    r.set_texture_color_modulate(p.textured && !p.raw);
    r.set_texture_offset((s->mode&15)*64,((s->mode>>4)&1)*256);
    r.set_palette_offset((p.clut&63)*16,(p.clut>>6)&511);
    unsigned mx=(s->window&31)*8, my=((s->window>>5)&31)*8;
    r.set_texture_window({uint8_t(~mx),uint8_t(~my),uint8_t(((s->window>>10)&31)*8&mx),uint8_t(((s->window>>15)&31)*8&my)});
    r.set_mask_test(s->mask&2); r.set_force_mask_bit(s->mask&1);
    r.set_primitive_type(type);
    r.set_UV_limits(0,0,255,255);
    r.set_draw_field(s->interlaced && s->display_enabled && !(s->mode&1024) ? s->field : -1);
}
template<class Draw> void clipped(gpu_raster_t *s, Draw draw) {
    auto &r=*backend->renderer;
    if (s->right<s->left || s->bottom<s->top) return;
    Rect rect(unsigned(s->left),unsigned(s->top),unsigned(s->right-s->left+1),unsigned(s->bottom-s->top+1));
    auto emit=[&](Rect clip) { auto c=rect.scissor(clip); if (c.width && c.height) { backend->touch(c.x,c.y,c.width,c.height); r.set_draw_rect(c); draw(); } };
    if ((s->mode&1024) || !s->display_enabled) { emit(rect); return; }
    if (s->interlaced) {
        emit(rect);
    } else {
        // The common non-wrapping display rectangle needs at most four
        // scissors. Avoid allocating region vectors for every primitive.
        unsigned display_w=std::min(1024u,unsigned(s->display_w));
        unsigned display_h=std::min(512u,unsigned(s->display_h));
        unsigned display_x=unsigned(s->display_x), display_y=unsigned(s->display_y);
        if (display_x+display_w<=1024 && display_y+display_h<=512) {
            Rect hit=rect.scissor({display_x,display_y,display_w,display_h});
            if (!hit.width || !hit.height) { emit(rect); return; }
            if (hit.y>rect.y) emit({rect.x,rect.y,rect.width,hit.y-rect.y});
            unsigned end=hit.y+hit.height, bottom=rect.y+rect.height;
            if (end<bottom) emit({rect.x,end,rect.width,bottom-end});
            if (hit.x>rect.x) emit({rect.x,hit.y,hit.x-rect.x,hit.height});
            end=hit.x+hit.width; unsigned right=rect.x+rect.width;
            if (end<right) emit({end,hit.y,right-end,hit.height});
            return;
        }
        // Wrapped display rectangles retain the general subtraction path.
        std::vector<Rect> regions{rect};
        auto subtract=[&](Rect excluded) {
            std::vector<Rect> next;
            for (auto piece:regions) {
                Rect hit=piece.scissor(excluded);
                if (!hit.width || !hit.height) { next.push_back(piece); continue; }
                if (hit.y>piece.y) next.push_back({piece.x,piece.y,piece.width,hit.y-piece.y});
                unsigned end=hit.y+hit.height, bottom=piece.y+piece.height;
                if (end<bottom) next.push_back({piece.x,end,piece.width,bottom-end});
                if (hit.x>piece.x) next.push_back({piece.x,hit.y,hit.x-piece.x,hit.height});
                end=hit.x+hit.width; unsigned right=piece.x+piece.width;
                if (end<right) next.push_back({end,hit.y,right-end,hit.height});
            }
            regions.swap(next);
        };
        unsigned width=std::min(1024u,unsigned(s->display_w));
        unsigned height=std::min(512u,unsigned(s->display_h));
        unsigned x=unsigned(s->display_x),y=unsigned(s->display_y);
        unsigned first_w=std::min(width,1024-x),first_h=std::min(height,512-y);
        if (width && height) {
            subtract({x,y,first_w,first_h});
            if (width>first_w) subtract({0,y,width-first_w,first_h});
            if (height>first_h) subtract({x,0,first_w,height-first_h});
            if (width>first_w && height>first_h) subtract({0,0,width-first_w,height-first_h});
        }
        for (auto region:regions) emit(region);
    }
}
}
extern "C" int host_gpu_configure(const char *name,unsigned scale,int color,unsigned msaa,const char *frames,const char *timings) {
    if (backend || (std::strcmp(name,"soft") && std::strcmp(name,"vulkan")) || scale<1 || scale>8 ||
        (msaa!=1 && msaa!=2 && msaa!=4 && msaa!=8)) return 0;
    options={ !std::strcmp(name,"vulkan"),color!=0,scale,msaa,frames,timings };
    return options.enabled || (scale==1 && !color && msaa==1 && !frames && !timings);
}
extern "C" int host_gpu_enabled(void) { return options.enabled; }
extern "C" const char *host_gpu_scaled_frames(void) { return options.frames; }
extern "C" int host_gpu_start(gpu_t *g,SDL_Window *window) {
    if (!options.enabled) return 1;
#ifdef FF8_GUI
    if (backend && !backend->failed && !backend->raster && g && backend->window==window) return host_gpu_resume(g);
#endif
    try {
        auto b=std::unique_ptr<Backend>(new Backend);
        if (!Context::init_loader(nullptr)) throw std::runtime_error("Vulkan loader unavailable");
        unsigned count=0;
        if (window && !SDL_Vulkan_GetInstanceExtensions(window,&count,nullptr)) throw std::runtime_error(SDL_GetError());
        std::vector<const char *> extensions(count);
        if (window && !SDL_Vulkan_GetInstanceExtensions(window,&count,extensions.data())) throw std::runtime_error(SDL_GetError());
        bool present_fences=false;
        if (window) {
            unsigned available_count=0;
            vkEnumerateInstanceExtensionProperties(nullptr,&available_count,nullptr);
            std::vector<VkExtensionProperties> available(available_count);
            vkEnumerateInstanceExtensionProperties(nullptr,&available_count,available.data());
            auto has=[&](const char *name) { return std::any_of(available.begin(),available.end(),
                [&](const VkExtensionProperties &p){ return !std::strcmp(p.extensionName,name); }); };
            present_fences=has(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) && has(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            if (present_fences) {
                for (auto name:{VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME})
                    if (std::none_of(extensions.begin(),extensions.end(),[&](const char *p){ return !std::strcmp(p,name); })) extensions.push_back(name);
            }
        }
        VkApplicationInfo app={}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; app.pApplicationName="FF8 host"; app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci={}; ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo=&app;
        ci.enabledExtensionCount=unsigned(extensions.size()); ci.ppEnabledExtensionNames=extensions.data();
        if (vkCreateInstance(&ci,nullptr,&b->instance)!=VK_SUCCESS) throw std::runtime_error("Cannot create Vulkan instance");
        volkLoadInstance(b->instance);
        if (window && !SDL_Vulkan_CreateSurface(window,b->instance,&b->surface)) throw std::runtime_error(SDL_GetError());
        const char *swap=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkPhysicalDeviceFeatures features={};
        b->context.reset(new Context(b->instance,VK_NULL_HANDLE,b->surface,window?&swap:nullptr,window?1:0,nullptr,0,&features,present_fences));
        if (!b->context->is_valid()) throw std::runtime_error("Cannot create Vulkan device");
        const auto &limits=b->context->get_gpu_props().limits;
        if (1024*options.scale>limits.maxImageDimension2D) throw std::runtime_error("Requested scale exceeds device limit");
        b->device.reset(new Device); b->device->set_context(*b->context); b->device->init_frame_contexts(3);
        b->device->profile_pipeline_creation=options.timings!=nullptr;
        b->renderer.reset(new Renderer(*b->device,options.scale,options.msaa,nullptr));
        if (!b->renderer->is_valid()) throw std::runtime_error("Cannot create parallel-psx renderer");
        if (b->renderer->get_scaling()!=options.scale) throw std::runtime_error("Requested scale is unsupported");
        b->renderer->set_adaptive_smoothing(false); b->renderer->set_track_textures(false);
        b->renderer->set_dump_textures(false); b->renderer->set_replace_textures(false);
        b->hd_textures.reset(new HostHD::TextureProvider);
        b->renderer->set_hd_provider(b->hd_textures.get());
        hd_config.internal_scale=options.scale;
        b->hd_textures->configure(hd_config);
        b->hd_textures->set_names(hd_names);
        b->hd_model_renderer.reset(new HostHD::HdModelRenderer(*b->device, options.scale, b->renderer->get_msaa()));
        sync_hd_models_sink();
        auto warm_start=Clock::now();
        b->renderer->warmup();
        std::fprintf(stderr,"Vulkan: pipeline warmup %.3f ms\n",
            std::chrono::duration<double,std::milli>(Clock::now()-warm_start).count());
        b->prior_compile_ns=b->device->pipeline_compile_ns; b->prior_compiles=b->device->pipeline_compiles;
        b->prior_submits=b->device->queue_submissions;
        b->frame_start=Clock::now();
        b->window=window;
#ifdef FF8_GUI
        b->raster=g ? gpu_raster_state(g) : nullptr;
#else
        b->raster=gpu_raster_state(g);
#endif
        if (window) {
            b->wsi.reset(new HostGPU::Swapchain(b->context->get_gpu(),b->context->get_device(),
                b->context->get_graphics_queue(),b->context->get_enabled_device_features().supports_swapchain_maintenance1));
            b->wsi->surface=b->surface;
            b->wsi->latest_ready=b->context->get_enabled_device_features().supports_fifo_latest_ready;
            b->wsi->pace(paced_wanted);
            if (b->context->get_enabled_device_features().supports_present_wait)
                b->wsi->wait_present=reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(b->context->get_device(),"vkWaitForPresentKHR"));
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
            if (b->context->get_enabled_device_features().supports_present_timing) {
                VkDevice d=b->context->get_device();
                auto &api=b->wsi->timing_api;
                api.queue_size=reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(vkGetDeviceProcAddr(d,"vkSetSwapchainPresentTimingQueueSizeEXT"));
                api.properties=reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(vkGetDeviceProcAddr(d,"vkGetSwapchainTimingPropertiesEXT"));
                api.domains=reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(vkGetDeviceProcAddr(d,"vkGetSwapchainTimeDomainPropertiesEXT"));
                api.past=reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(vkGetDeviceProcAddr(d,"vkGetPastPresentationTimingEXT"));
                api.calibrated=reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(vkGetDeviceProcAddr(d,"vkGetCalibratedTimestampsKHR"));
                if (!api.calibrated) api.calibrated=reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(vkGetDeviceProcAddr(d,"vkGetCalibratedTimestampsEXT"));
                api.absolute=b->context->get_enabled_device_features().supports_present_at_absolute_time;
                if (!api.queue_size || !api.properties || !api.domains) api.past=nullptr;
            }
#endif
        }
        if (options.timings) {
#ifdef _WIN32
            b->timings=host_win_fopen(options.timings,"w");
#else
            b->timings=std::fopen(options.timings,"w");
#endif
            if (!b->timings) throw std::runtime_error("Cannot open GPU timings");
            std::fprintf(b->timings,"frame,cpu_ms,gpu_ms,wall_ms,draw_calls,vertices,render_passes,soft_ms,encode_ms,submit_ms,capture_ms,upload_bytes,submissions,backpressure,submit_wall_ms,present_ms,osd_ms,pipeline_ms,pipelines\n"); }
        std::fprintf(stderr,"Vulkan: %s, scale=%u, MSAA requested=%u active=%u, native readbacks=soft reference\n",b->context->get_gpu_props().deviceName,options.scale,options.msaa,b->renderer->get_msaa());
        backend=std::move(b);
#ifdef FF8_GUI
        HostGuiVulkan::configure(backend->instance,backend->context->get_gpu(),backend->context->get_device(),
            backend->context->get_graphics_queue_family(),backend->context->get_graphics_queue());
        if (!g) {
            const uint8_t black[4]={0,0,0,255};
            auto info=ImageCreateInfo::immutable_2d_image(1,1,VK_FORMAT_R8G8B8A8_UNORM);
            info.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT; info.initial_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            ImageInitialData initial={black,0,0};
            backend->image=backend->device->create_image(info,&initial);
            ++backend->scanout_revision;
        } else
#endif
        if (!host_gpu_resume(g)) { backend.reset(); options.enabled=false; }
        return 1;
    } catch (const std::exception &e) { std::fprintf(stderr,"Vulkan: %s\n",e.what()); backend.reset(); options.enabled=false; std::fprintf(stderr,"Vulkan warning: using native soft presentation\n"); return 1; }
    catch (...) { degrade("initialization","unknown exception"); backend.reset(); return 1; }
}
extern "C" void host_gpu_stop(void) {
#ifdef FF8_PGXP
    pgxp_record_sink(nullptr, nullptr);
#endif
    hd_scene.reset();
    if (backend) {
        host_screenshot_flush();
        if (backend->renderer) backend->renderer->set_hd_provider(nullptr);
        backend->hd_model_renderer.reset();
        backend->hd_textures.reset();
    }
    try { backend.reset(); } catch (...) { std::fprintf(stderr,"Vulkan cleanup failed\n"); }
    options={};
}
extern "C" void host_gpu_hd_assets(int font_mode, const char *font_path, int hd_textures,
                                   const char *textures_dir, const char *dump_textures) {
    hd_config.font_mode = font_mode == 1 ? HostHD::FontMode::Builtin :
                          font_mode == 2 ? HostHD::FontMode::Custom : HostHD::FontMode::Off;
    hd_config.font_path = font_path ? font_path : "";
    hd_config.textures_enabled = hd_textures != 0;
    hd_config.textures_dir = textures_dir ? textures_dir : "";
    hd_config.dump_dir = dump_textures ? dump_textures : "";
    hd_config.internal_scale = options.scale ? options.scale : 1u;
    if (backend && backend->hd_textures) {
        backend->hd_textures->configure(hd_config);
        // Font sheets loaded before the setting changed are found in the mirror.
        if (backend->raster) backend->hd_textures->resync(backend->raster->vram);
    }
}
extern "C" void host_gpu_hd_names(const char *names, size_t size) {
    std::vector<std::string> list;
    for (size_t at = 0; names && at < size;) {
        size_t len = 0;
        while (at + len < size && names[at + len]) ++len;
        if (len) list.emplace_back(names + at, len);
        at += len + 1;
    }
    if (list == hd_names) return;
    hd_names = std::move(list);
    if (backend && backend->hd_textures) backend->hd_textures->set_names(hd_names);
}
extern "C" void host_gpu_hd_models(int enabled, const char *models_dir) {
    uint64_t prev_gen = hd_models_registry.generation();
    hd_models_registry.configure(enabled != 0, models_dir ? models_dir : "");
    if (hd_models_registry.generation() != prev_gen) {
        hd_scene.reset();
        if (backend && backend->hd_model_renderer) backend->hd_model_renderer->clear_cache();
    }
    sync_hd_models_sink();
}
extern "C" void host_gpu_hd_models_rebind(void) {
    hd_scene.reset();
    sync_hd_models_sink();
}
extern "C" void host_gpu_hd_reload(void) {
    if (backend && backend->hd_textures) {
        backend->hd_textures->reload_pack();
        if (backend->raster) backend->hd_textures->resync(backend->raster->vram);
    }
    hd_models_registry.reload();
    hd_scene.reset();
    if (backend && backend->hd_model_renderer) backend->hd_model_renderer->clear_cache();
    sync_hd_models_sink();
}
extern "C" unsigned host_gpu_hd_model_count(void) {
    return unsigned(hd_models_registry.discovered_count());
}
static int presentation_mode, presentation_linear;
static bool checkpoints_wanted;
extern "C" void host_gpu_presentation(int mode, int linear) {
    presentation_mode=mode; presentation_linear=linear;
}
extern "C" int host_gpu_resume(gpu_t *g) try {
    if (!backend || !g) return 0;
    backend->movie_patches.clear();
    backend->movie_frame.reset(); backend->movie_buffer.reset();
    hd_scene.reset();
    backend->hd_frame_ended=false;
    backend->dirty=false;
    ++backend->scanout_revision;
#ifdef FF8_GUI
    backend->gui_backdrop.reset(); backend->gui_backdrop_valid=false; backend->invalidate_gui_composed();
#endif
    for (auto &row:backend->dirty_rows) row.fill(0);
    backend->raster=gpu_raster_state(g);
    backend->effect_pages=0;
    backend->stale.clear();
    std::memset(backend->effect_counts,0,sizeof(backend->effect_counts));
    for (auto &row:backend->effect_pixels) row.fill(0);
    backend->checkpoints=checkpoints_wanted;
    if (backend->renderer) backend->renderer->set_texture_memo(checkpoints_wanted);
    backend->upload({0,0,1024,512});
    // The GPU it now mirrors is where smooth motion keeps its point: all of
    // VRAM is kept again from here.
    if (backend->checkpoints) {
        backend->kept=Renderer::VramCopy();
        backend->written[0]=backend->written[1]=0;
        backend->renderer->copy_vram(backend->kept,backend->written,false);
        std::copy(std::begin(backend->effect_pixels),std::end(backend->effect_pixels),std::begin(backend->kept_effect_pixels));
        std::copy(std::begin(backend->effect_counts),std::end(backend->effect_counts),std::begin(backend->kept_effect_counts));
        backend->kept_effect_pages=backend->effect_pages;
    }
    return 1;
} catch (const std::exception &e) { degrade("host_gpu_resume",e.what()); return 0; }
catch (...) { degrade("host_gpu_resume","unknown backend exception"); return 0; }
extern "C" void host_gpu_checkpoints(int on) {
    checkpoints_wanted=on!=0;
    if (!backend) return;
    backend->checkpoints=on!=0;
    if (backend->renderer) backend->renderer->set_texture_memo(on!=0);
    if (!on) { backend->kept=Renderer::VramCopy(); backend->written[0]=backend->written[1]=0; }
}
extern "C" void host_gpu_checkpoint(gpu_t *g,int restore) try {
    if (!backend || backend->failed || !backend->checkpoints || backend->raster!=gpu_raster_state(g)) return;
    auto &b=*backend;
    CpuTimer timer;
    b.begin();
    if (!restore) {
        // Soft writes not yet on the GPU reach it before it is kept.
        b.uploads();
        b.renderer->copy_vram(b.kept,b.written,false);
        std::copy(std::begin(b.effect_pixels),std::end(b.effect_pixels),std::begin(b.kept_effect_pixels));
        std::copy(std::begin(b.effect_counts),std::end(b.effect_counts),std::begin(b.kept_effect_counts));
        b.kept_effect_pages=b.effect_pages;
    } else {
        b.stale.clear();
        hd_scene.begin_replay_pass();
        // What the drawing since left for upload lies in tiles put back now.
        b.dirty=false;
        for (auto &row:b.dirty_rows) row.fill(0);
        b.movie_patches.clear();
        b.renderer->copy_vram(b.kept,b.written,true);
        std::copy(std::begin(b.kept_effect_pixels),std::end(b.kept_effect_pixels),std::begin(b.effect_pixels));
        std::copy(std::begin(b.kept_effect_counts),std::end(b.kept_effect_counts),std::begin(b.effect_counts));
        b.effect_pages=b.kept_effect_pages;
    }
    b.written[0]=b.written[1]=0;
} catch (const std::exception &e) { degrade("host_gpu_checkpoint",e.what()); }
catch (...) { degrade("host_gpu_checkpoint","unknown backend exception"); }
extern "C" int host_gpu_download(gpu_t *g,uint16_t *out,int raw) try {
    if (!backend || backend->failed || backend->raster!=gpu_raster_state(g)) return 0;
    CpuTimer timer;
    if (raw) { backend->uploads(); backend->renderer->copy_vram_to_cpu_synchronous({0,0,1024,512},out); }
    else backend->native_download(out);
    return 1;
} catch (const std::exception &e) { degrade("host_gpu_download",e.what()); return 0; }
catch (...) { degrade("host_gpu_download","unknown backend exception"); return 0; }

extern "C" {
void __real_gpu_raster_triangle(gpu_raster_t *,gpu_vertex_t,gpu_vertex_t,gpu_vertex_t,gpu_paint_t);
void __real_gpu_raster_rectangle(gpu_raster_t *,gpu_vertex_t,int,int,gpu_paint_t);
void __real_gpu_raster_line(gpu_raster_t *,gpu_vertex_t,gpu_vertex_t,gpu_paint_t,int);
void __real_gpu_raster_fill(gpu_raster_t *,int,int,int,int,uint32_t);
void __real_gpu_raster_copy(gpu_raster_t *,int,int,int,int,int,int);
void __real_gpu_raster_store(gpu_raster_t *,int,int,uint16_t);
void __real_gpu_raster_store_span(gpu_raster_t *,unsigned,unsigned,const uint8_t *,unsigned);
}
namespace {
bool native_pixels() {
    return options.scale==1 && !options.true_color && options.msaa==1 && !host_movies_active() &&
           !hd_models_registry.has_any_active();
}

void flush_pending_hd_models(Backend &b) {
    if (!hd_scene.has_pending_draws()) return;
    auto draws = hd_scene.take_pending_draws();
    if (!draws.empty() && b.hd_model_renderer && b.renderer) {
        b.begin();
        b.uploads();
        b.hd_model_renderer->render_batch(*b.renderer, draws);
    }
}

void dirty_bounds(gpu_raster_t *s,int x,int y,int w,int h,bool effect=false) {
    int l=std::max(x,s->left), r=std::min(x+w,s->right+1);
    int t=std::max(y,s->top), b=std::min(y+h,s->bottom+1);
    if (r>l) for (int row=t;row<b;++row) backend->mark(unsigned(l),unsigned(row),unsigned(r-l));
    if (effect && r>l && b>t) backend->effect(unsigned(l),unsigned(t),unsigned(r-l),unsigned(b-t),true);
}

}
namespace {
// Smooth motion's pictures between game frames leave the software rasterizer
// out above 1x: nothing of them is kept, and the GPU draws them; except where
// the renderer itself needs software pixels (feedback effects, copies it draws
// natively), where the picture before needed them. A primitive drawn there
// is drawn in software too; elsewhere what it covers turns stale.
bool soft_left_out(gpu_raster_t *s,int x0,int y0,int x1,int y1) {
    if (!backend->checkpoints || !backend->between || native_pixels() || backend->raster!=s) return false;
    x0=std::max(x0,s->left); y0=std::max(y0,s->top); x1=std::min(x1,s->right); y1=std::min(y1,s->bottom);
    if (x1<x0 || y1<y0) return true;
    unsigned x=unsigned(x0), y=unsigned(y0), w=unsigned(x1-x0+1), h=unsigned(y1-y0+1);
    if (backend->read.meets(x,y,w,h)) return false;
    backend->stale.add(x,y,w,h);
    return true;
}
// The renderer reads software pixels from a rectangle: where drawing was left
// out there, VRAM is first taken back from the GPU.
void fresh(gpu_raster_t *s,int x,int y,int w,int h) {
    if (w<=0 || h<=0) return;
    backend->reading.add(unsigned(x),unsigned(y),unsigned(w),unsigned(h));
    if (!backend->stale.meets(unsigned(x),unsigned(y),unsigned(w),unsigned(h))) return;
    backend->begin(); backend->uploads();
    backend->renderer->copy_vram_to_cpu_synchronous({0,0,1024,512},s->vram);
    backend->stale.clear();
}
// A primitive drawn in software between game frames reads its texture page
// and palette.
void fresh_texture(gpu_raster_t *s,gpu_paint_t p) {
    unsigned depth=(s->mode>>7)&3;
    if (!p.textured || !backend->checkpoints || !backend->between || native_pixels()) return;
    fresh(s,int(s->mode&15)*64,int((s->mode>>4)&1)*256,depth>=2 ? 256 : 256>>(2-depth),256);
    if (depth<2) fresh(s,int(p.clut&63)*16,int((p.clut>>6)&511),depth ? 256 : 16,1);
}
}
extern "C" void __wrap_gpu_raster_triangle(gpu_raster_t *s,gpu_vertex_t a,gpu_vertex_t b,gpu_vertex_t c,gpu_paint_t p) {
    if (!active(s)) { __real_gpu_raster_triangle(s,a,b,c,p); return; }
#ifdef FF8_PGXP
    bool precise_draw=false;
#endif
    CpuTimer timer;
    unsigned u0=unsigned(std::min({a.u,b.u,c.u})), u1=unsigned(std::max({a.u,b.u,c.u}));
    unsigned v0=unsigned(std::min({a.v,b.v,c.v})), v1=unsigned(std::max({a.v,b.v,c.v}));
    if (backend->effect_texture(s,p,u0,v0,u1-u0,v1-v0)) {
        fresh_texture(s,p);
        { SoftTimer soft; __real_gpu_raster_triangle(s,a,b,c,p); }
        dirty_bounds(s,std::min({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),
            std::max({a.x,b.x,c.x})-std::min({a.x,b.x,c.x})+1,
            std::max({a.y,b.y,c.y})-std::min({a.y,b.y,c.y})+1,true);
        return;
    }
    gpu_work("triangle",[&] {
    if (std::max({a.x,b.x,c.x})-std::min({a.x,b.x,c.x})<1024 && std::max({a.y,b.y,c.y})-std::min({a.y,b.y,c.y})<512) {
        Vertex v[]={vertex(a),vertex(b),vertex(c)};
#ifdef FF8_PGXP
        pgxp_vertex_t precise[3];
        if (pgxp_triangle(s,precise)) {
            precise_draw=true;
            for (unsigned i=0;i<3;++i) {
                v[i].x=precise[i].x; v[i].y=precise[i].y; v[i].w=precise[i].w;
            }
        }
#endif
        // Smooth motion's fractions of a pixel (gpu_subpixel), and with PGXP
        // the depths that map textures in perspective (gpu_subpixel_depth).
        bool depths=s->cornered && s->corner_w[0]>0 && s->corner_w[1]>0 && s->corner_w[2]>0;
        for (unsigned i=0;i<3;++i) {
            v[i].x+=s->shift_x+(s->cornered ? s->corner[i][0] : 0.0f);
            v[i].y+=s->shift_y+(s->cornered ? s->corner[i][1] : 0.0f);
            if (depths) v[i].w=s->corner_w[i];
        }
        // Native output is replaced by the exact software pixels below. Keep
        // those writes pending until a draw that needs GPU pixels or scanout,
        // instead of drawing and uploading the same primitive twice.
        if (native_pixels()
#ifdef FF8_PGXP
            && !precise_draw
#endif
        ) return;
        if (hd_models_registry.has_any_active()) {
            HostHD::TriangleObservation obs{};
            obs.x[0]=a.x; obs.x[1]=b.x; obs.x[2]=c.x;
            obs.y[0]=a.y; obs.y[1]=b.y; obs.y[2]=c.y;
            obs.r[0]=a.r; obs.r[1]=b.r; obs.r[2]=c.r;
            obs.g[0]=a.g; obs.g[1]=b.g; obs.g[2]=c.g;
            obs.b[0]=a.b; obs.b[1]=b.b; obs.b[2]=c.b;
            obs.u[0]=a.u; obs.u[1]=b.u; obs.u[2]=c.u;
            obs.v[0]=a.v; obs.v[1]=b.v; obs.v[2]=c.v;
            obs.textured = p.textured != 0;
            obs.raw = p.raw != 0;
            obs.semi = p.semi != 0;
            obs.tex_page_x = (s->mode & 15u) * 64u;
            obs.tex_page_y = ((s->mode >> 4) & 1u) * 256u;
            obs.tex_depth = (s->mode >> 7) & 3u;
            obs.clip_left = s->left; obs.clip_top = s->top;
            obs.clip_right = s->right; obs.clip_bottom = s->bottom;
            obs.draw_offset_x = s->offset_x; obs.draw_offset_y = s->offset_y;
            obs.has_draw_offset = true;
            auto decision = hd_scene.observe_triangle(obs, hd_models_registry);
            if (decision.suppress_ps1_triangle) return;
            if (decision.flush_pending_hd_models) flush_pending_hd_models(*backend);
        }
        state(s,p,PrimitiveType::Polygon);
        // A replaced texture is found by, and its filter taps are kept on, the
        // texels this polygon covers: battle text is drawn as polygons, and the
        // whole page would name no glyph and let taps reach a neighbouring one.
        if (p.textured && hd_replacing()) {
            int u0=std::min({a.u,b.u,c.u}), u1=std::max({a.u,b.u,c.u});
            int v0=std::min({a.v,b.v,c.v}), v1=std::max({a.v,b.v,c.v});
            if (u0>=0 && v0>=0 && u1<=255 && v1<=255)
                backend->renderer->set_UV_limits(uint16_t(u0),uint16_t(v0),uint16_t(u1),uint16_t(v1));
        }
        clipped(s,[&] { backend->renderer->draw_triangle(v); });
    }
    });
    if (soft_left_out(s,std::min({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),std::max({a.x,b.x,c.x}),std::max({a.y,b.y,c.y}))) return;
    fresh_texture(s,p);
    { SoftTimer soft; __real_gpu_raster_triangle(s,a,b,c,p); }
    if (!backend->failed && native_pixels()
#ifdef FF8_PGXP
        && !precise_draw
#endif
    )
        dirty_bounds(s,std::min({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),
                     std::max({a.x,b.x,c.x})-std::min({a.x,b.x,c.x})+1,
                     std::max({a.y,b.y,c.y})-std::min({a.y,b.y,c.y})+1);
}
extern "C" void __wrap_gpu_raster_rectangle(gpu_raster_t *s,gpu_vertex_t a,int w,int h,gpu_paint_t p) {
    if (!active(s)) { __real_gpu_raster_rectangle(s,a,w,h,p); return; }
    CpuTimer timer;
    if (w>0 && h>0 && backend->effect_texture(s,p,unsigned(a.u),unsigned(a.v),unsigned(w),unsigned(h))) {
        fresh_texture(s,p);
        { SoftTimer soft; __real_gpu_raster_rectangle(s,a,w,h,p); }
        dirty_bounds(s,a.x,a.y,w,h,true);
        return;
    }
    if (backend->effect_pages && !p.textured && !p.semi && !(s->mask&2) && w>0 && h>0) {
        int l=std::max(a.x,s->left), r=std::min(a.x+w,s->right+1);
        int t=std::max(a.y,s->top), b=std::min(a.y+h,s->bottom+1);
        if (r>l && b>t) backend->effect(unsigned(l),unsigned(t),unsigned(r-l),unsigned(b-t),false);
    }
    gpu_work("rectangle",[&] {
    if (native_pixels()) return;
    if (w>0 && h>0) {
        if (hd_models_registry.has_any_active() && hd_scene.should_flush_for_rect(a.x,a.y,w,h))
            flush_pending_hd_models(*backend);
        state(s,p,PrimitiveType::Sprite);
        Vertex v[4]={vertex(a),vertex(a),vertex(a),vertex(a)};
        v[1].x+=w; v[2].y+=h; v[3].x+=w; v[3].y+=h;
        for (auto &vert:v) {
            vert.x+=s->shift_x+(s->cornered ? s->corner[0][0] : 0.0f);
            vert.y+=s->shift_y+(s->cornered ? s->corner[0][1] : 0.0f);
        }
        bool flip_x=(s->mode&0x1000)!=0, flip_y=(s->mode&0x2000)!=0;
        int du=flip_x?-w:w, dv=flip_y?-h:h;
        // Upscaled sample centers descend from the next texel boundary when
        // mirrored. Native single-sample shaders already add a half-texel bias.
        int bias=options.scale==1 && backend->renderer->get_msaa()==1 ? 0 : 1;
        int u=a.u+(flip_x?bias:0), tex_v=a.v+(flip_y?bias:0);
        int low=std::min(u,u+du);
        if (low<0) u+=((-low+255)/256)*256;
        low=std::min(tex_v,tex_v+dv);
        if (low<0) tex_v+=((-low+255)/256)*256;
        for (auto &vert:v) { vert.u=uint16_t(u); vert.v=uint16_t(tex_v); }
        v[1].u=uint16_t(u+du); v[3].u=v[1].u;
        v[2].v=uint16_t(tex_v+dv); v[3].v=v[2].v;
        int u_end=std::max(u,u+du), v_end=std::max(tex_v,tex_v+dv);
        // A sprite that ends on the page edge does not wrap. Past 255 the
        // renderer takes the whole page for its texture, which names no one
        // glyph of a replaced sheet (':' and the stat labels end on the edge).
        if (hd_replacing()) { if (u_end==256) u_end=255; if (v_end==256) v_end=255; }
        backend->renderer->set_UV_limits(uint16_t(std::min(u,u+du)),uint16_t(std::min(tex_v,tex_v+dv)),
                                         uint16_t(u_end),uint16_t(v_end));
        clipped(s,[&] { backend->renderer->draw_quad(v); });
    }
    });
    if (soft_left_out(s,a.x,a.y,a.x+w-1,a.y+h-1)) return;
    fresh_texture(s,p);
    { SoftTimer soft; __real_gpu_raster_rectangle(s,a,w,h,p); }
    if (!backend->failed && native_pixels()) dirty_bounds(s,a.x,a.y,w,h);
}
extern "C" void __wrap_gpu_raster_line(gpu_raster_t *s,gpu_vertex_t a,gpu_vertex_t b,gpu_paint_t p,int skip) {
    if (!active(s)) { __real_gpu_raster_line(s,a,b,p,skip); return; }
    CpuTimer timer;
    int n=std::max(std::abs(b.x-a.x),std::abs(b.y-a.y));
    gpu_work("line",[&] {
    if (native_pixels()) return;
    if ((!skip || n) && std::abs(b.x-a.x)<1024 && std::abs(b.y-a.y)<512) {
        state(s,p,PrimitiveType::Polygon);
        Vertex v[]={vertex(a),vertex(b)};
        for (auto &vert:v) { vert.x+=s->shift_x; vert.y+=s->shift_y; }
        if (skip && n) {
            v[0].x+=float(b.x-a.x)/float(n); v[0].y+=float(b.y-a.y)/float(n);
        }
        clipped(s,[&] { backend->renderer->draw_line(v); });
    }
    });
    if (soft_left_out(s,std::min(a.x,b.x)-1,std::min(a.y,b.y)-1,std::max(a.x,b.x)+1,std::max(a.y,b.y)+1)) return;
    { SoftTimer soft; __real_gpu_raster_line(s,a,b,p,skip); }
    if (!backend->failed && native_pixels())
        dirty_bounds(s,std::min(a.x,b.x)-1,std::min(a.y,b.y)-1,std::abs(b.x-a.x)+3,std::abs(b.y-a.y)+3);

}
extern "C" void __wrap_gpu_raster_store(gpu_raster_t *s,int x,int y,uint16_t pixel) {
    if (active(s) && backend->overlaps_movie(unsigned(x),unsigned(y),1))
        gpu_work("movie ordering",[&] { backend->begin(); backend->uploads(); });
    __real_gpu_raster_store(s,x,y,pixel);
    if (active(s)) {
        if (!(s->mask&2)) backend->effect(unsigned(x),unsigned(y),1,1,false);
        backend->mark(unsigned(x),unsigned(y),1);
    }
}
extern "C" void __wrap_gpu_raster_store_span(gpu_raster_t *s,unsigned x,unsigned y,const uint8_t *data,unsigned n) {
    if (active(s) && backend->overlaps_movie(x,y,n))
        gpu_work("movie ordering",[&] { backend->begin(); backend->uploads(); });
    __real_gpu_raster_store_span(s,x,y,data,n);
    if (active(s)) {
        if (!(s->mask&2)) backend->effect(x,y,n,1,false);
        backend->mark(x,y,n);
        gpu_work("movie upload",[&] { backend->movie_span(x,y,data,n); });
    }
}
extern "C" void __wrap_gpu_raster_fill(gpu_raster_t *s,int x,int y,int w,int h,uint32_t color) {
    if (!active(s)) { __real_gpu_raster_fill(s,x,y,w,h,color); return; }
    CpuTimer timer;
    backend->effect(unsigned(x),unsigned(y),unsigned(w),unsigned(h),false);
    gpu_work("fill uploads",[&] {
        backend->begin(); backend->uploads();
        if (hd_models_registry.has_any_active()) {
            if (hd_scene.should_flush_for_rect(x,y,w,h)) flush_pending_hd_models(*backend);
            hd_scene.clear_vram_region(unsigned(x),unsigned(y),unsigned(w),unsigned(h));
        }
    });
    // Clear ignores masks, field parity and the drawing area.
    gpu_work("fill field",[&] { backend->renderer->set_draw_field(-1); });
    { SoftTimer soft; __real_gpu_raster_fill(s,x,y,w,h,color); }
    gpu_work("fill",[&] {
    for (int row=0;row<h;) {
        unsigned yy=unsigned(y+row)&511, rows=std::min(unsigned(h-row),512-yy);
        for (int col=0;col<w;) {
            unsigned xx=unsigned(x+col)&1023, cols=std::min(unsigned(w-col),1024-xx);
            backend->touch(xx,yy,cols,rows);
            backend->renderer->clear_rect({xx,yy,cols,rows},color & 0xffffff);
            col+=int(cols);
        }
        row+=int(rows);
    }
    });
}
extern "C" void __wrap_gpu_raster_copy(gpu_raster_t *s,int sx,int sy,int dx,int dy,int w,int h) {
    if (!active(s)) { __real_gpu_raster_copy(s,sx,sy,dx,dy,w,h); return; }
    CpuTimer timer;
    bool capture=!options.true_color && (s->mask&1) && s->display_w>0 && s->display_h>0 && w==s->display_w && h==s->display_h;
    bool effect=capture || backend->effect_source(unsigned(sx),unsigned(sy),unsigned(w),unsigned(h));
    bool overlap=sx<dx+w && dx<sx+w && sy<dy+h && dy<sy+h;
    // Copies preserve presentation pixels even when their source was once used
    // for feedback. Only the initial capture and feedback color operations need
    // native truncation; HUD background save/restore must retain scaled detail.
    bool native=capture || overlap || sx+w>1024 || dx+w>1024 || sy+h>512 || dy+h>512;
    gpu_work("copy",[&] {
    if (!native) {
        backend->begin(); backend->uploads();
        if (hd_models_registry.has_any_active() &&
            (hd_scene.should_flush_for_rect(sx,sy,w,h) || hd_scene.should_flush_for_rect(dx,dy,w,h)))
            flush_pending_hd_models(*backend);
        backend->renderer->set_mask_test(s->mask&2); backend->renderer->set_force_mask_bit(s->mask&1);
        backend->touch(unsigned(dx),unsigned(dy),unsigned(w),unsigned(h));
        backend->renderer->blit_vram({unsigned(dx),unsigned(dy),unsigned(w),unsigned(h)},
                                     {unsigned(sx),unsigned(sy),unsigned(w),unsigned(h)});
    }
    });
    // A copy the renderer draws natively reads software pixels; one it draws
    // itself leaves the software copy of stale pixels stale.
    if (native) fresh(s,sx,sy,w,h);
    else if (backend->stale.meets(unsigned(sx),unsigned(sy),unsigned(w),unsigned(h)))
        backend->stale.add(unsigned(dx),unsigned(dy),unsigned(w),unsigned(h));
    { SoftTimer soft; __real_gpu_raster_copy(s,sx,sy,dx,dy,w,h); }
    if (effect || !(s->mask&2)) backend->effect(unsigned(dx),unsigned(dy),unsigned(w),unsigned(h),effect);
    if (!backend->failed && native) for (int row=0;row<h;++row) backend->mark(unsigned(dx),unsigned(dy+row),unsigned(w));
}
extern "C" int host_gpu_scanout(gpu_t *g) try {
    if (!backend || backend->failed || backend->raster!=gpu_raster_state(g)) return 0;
    CpuTimer timer;
    inject(4);
    int x,y,depth,enabled,nw,nh;
    gpu_display(g,&x,&y,&nw,&nh,&depth,&enabled);
    backend->scanout_timing=gpu_scanout(g); backend->native_width=nw; backend->native_height=nh;
    backend->begin(); backend->uploads();
    if (hd_models_registry.has_any_active()) {
        flush_pending_hd_models(*backend);
        hd_scene.end_frame();
        backend->hd_frame_ended=true;
    }
    if (backend->hd_textures) backend->hd_textures->end_frame();
    auto &target=backend->scanouts[backend->device->get_current_frame_context()];
    target=backend->renderer->host_scanout({unsigned(x),unsigned(y),unsigned(std::max(nw,1)),unsigned(std::max(nh,1))},
        depth!=0,options.true_color,enabled && nw && nh,target);
    backend->image=target;
    backend->output_width=target->get_width(); backend->output_height=target->get_height();
    ++backend->scanout_revision;
    return 1;
} catch (const std::exception &e) { degrade("host_gpu_scanout",e.what()); return 0; }
catch (...) { degrade("host_gpu_scanout","unknown backend exception"); return 0; }
extern "C" int host_gpu_rgb(gpu_t *g,const uint8_t **out,int *w,int *h) try {
    if (!host_gpu_scanout(g)) return 0;
    CpuTimer timer;
    double start=thread_ms();
    auto &b=*backend;
    auto image=b.image;
    unsigned iw=image->get_width(), ih=image->get_height();
    BufferCreateInfo info; info.domain=BufferDomain::CachedHost; info.size=size_t(iw)*ih*4;
    info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto buffer=b.device->create_buffer(info,nullptr);
    auto &cmd=b.renderer->command_buffer_hack_fixme();
    cmd->copy_image_to_buffer(*buffer,*image,0,{0,0,0},{iw,ih,1},0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1});
    cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    auto fence=b.renderer->flush_and_signal();
    if (!fence || !fence->wait()) throw std::runtime_error("Scanout capture fence failed");
#ifdef FF8_VULKAN_TESTING
    ++test_maps;
#endif
    auto pixels=static_cast<const uint8_t *>(b.device->map_host_buffer(*buffer,MEMORY_ACCESS_READ_BIT));
    if (!pixels) throw std::runtime_error("Scanout capture mapping failed");
    b.rgb.resize(size_t(iw)*ih*3);
    for (size_t i=0;i<size_t(iw)*ih;++i) std::memcpy(b.rgb.data()+i*3,pixels+i*4,3);
    b.device->unmap_host_buffer(*buffer,MEMORY_ACCESS_READ_BIT);
    b.capture_ms+=thread_ms()-start;
    *w=int(iw); *h=int(ih); *out=b.rgb.data(); return 1;
} catch (const std::exception &e) { degrade("host_gpu_rgb",e.what()); return 0; }
catch (...) { degrade("host_gpu_rgb","unknown backend exception"); return 0; }
// Presentation is appended to this frame's renderer command buffer. Submit once
// with its fence and presentation semaphore, then recycle a context two frames later.
namespace {
int queue_present();
double counter_seconds() { return double(SDL_GetPerformanceCounter())/double(SDL_GetPerformanceFrequency()); }
// Present timing reports and takes times on a host clock; the host keeps time
// in SDL's counter. The two are a fixed offset apart (sampled when needed:
// the clocks may be slewed against each other).
double domain_now(VkTimeDomainKHR domain) {
#ifndef _WIN32
    timespec now={};
    if (clock_gettime(domain==VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC,&now)) return -1;
    return double(now.tv_sec)+double(now.tv_nsec)*1e-9;
#else
    (void)domain; return -1;
#endif
}
double to_domain(VkTimeDomainKHR domain,double seconds) {
    double now=domain_now(domain);
    return now<0 ? -1 : seconds-counter_seconds()+now;
}
double from_domain(VkTimeDomainKHR domain,double time) {
    double now=domain_now(domain);
    return now<0 ? 0 : time-now+counter_seconds();
}
}
extern "C" int host_gpu_frame(gpu_t *g) try {
    if (!backend || backend->failed || backend->raster!=gpu_raster_state(g)) return 1;
    auto &b=*backend;
    inject(2);
    double cpu_start=thread_ms();
    b.begin(); b.uploads();
    if (hd_models_registry.has_any_active() && !b.hd_frame_ended) {
        flush_pending_hd_models(b);
        hd_scene.end_frame();
    }
    b.hd_frame_ended=false;
    if (b.timings) b.last=b.renderer->command_buffer_hack_fixme()->write_timestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    b.renderer->flush(false);
    uint64_t pressure=b.device->frame_backpressure;
    auto submit_wall_start=Clock::now();
    double submit_start=thread_ms();
    b.device->end_frame_context();
    if (!queue_present()) return 0;
    b.device->next_frame_context();
    if (b.device->get_error()!=VK_SUCCESS) throw std::runtime_error("Vulkan device lost or submission failed");
    double submit_ms=thread_ms()-submit_start;
    b.cpu_ms+=thread_ms()-cpu_start;
    b.samples.push_back({b.frame,b.upload_bytes,b.device->queue_submissions-b.prior_submits,
        b.device->frame_backpressure-pressure,b.cpu_ms,b.soft_ms,b.capture_ms,submit_ms,
        std::chrono::duration<double,std::milli>(Clock::now()-b.frame_start).count(),
        std::chrono::duration<double,std::milli>(Clock::now()-submit_wall_start).count(),
        (b.device->pipeline_compile_ns-b.prior_compile_ns)*1e-6,b.device->pipeline_compiles-b.prior_compiles,
        b.renderer->counters.draw_calls,b.renderer->counters.vertices,b.renderer->counters.render_passes,b.first,b.last,b.present_first,b.present_last,b.osd_first,b.osd_last});
    b.prior_submits=b.device->queue_submissions;
    b.prior_compile_ns=b.device->pipeline_compile_ns; b.prior_compiles=b.device->pipeline_compiles;
    b.collect();
    b.renderer->reset_counters();
    b.first.reset(); b.last.reset(); b.present_first.reset(); b.present_last.reset(); b.osd_first.reset(); b.osd_last.reset();
    b.cpu_ms=0; b.soft_ms=0; b.capture_ms=0; b.upload_bytes=0;
    b.frame_start=Clock::now(); ++b.frame;
#ifdef FF8_GUI
    ++b.presentation_serial;
#endif
    return 1;
} catch (const std::exception &e) { degrade("host_gpu_frame",e.what()); return 1; }
catch (...) { degrade("host_gpu_frame","unknown backend exception"); return 1; }


#ifdef FF8_GUI
extern "C" int host_gpu_gui_frame(void) try {
    if (!backend || backend->failed) return 0;
    auto &b=*backend;
    b.renderer->flush(false);
    b.device->end_frame_context();
    if (!queue_present()) return 0;
    b.device->next_frame_context();
    if (b.device->get_error()!=VK_SUCCESS) throw std::runtime_error("Vulkan GUI submission failed");
    ++b.presentation_serial;
    b.collect_gui();
    // GUI submissions are outside guest-frame telemetry, including WSI retirement.
    b.prior_submits=b.device->queue_submissions;
    b.frame_start=Clock::now();
    return 1;
} catch (const std::exception &e) { degrade("GUI frame",e.what()); return 0; }
catch (...) { degrade("GUI frame","unknown backend exception"); return 0; }
extern "C" double host_gpu_gui_timing(void) {
    if (!backend || backend->gui_times.empty()) return -1;
    double result=backend->gui_times.front(); backend->gui_times.pop_front(); return result;
}
extern "C" const char *host_gpu_device_name(void) {
    return backend && backend->context ? backend->context->get_gpu_props().deviceName : "";
}
#endif

namespace {
uint64_t wall_milliseconds() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}
// The crt=off path: clear the target and blit the scanout into the viewport.
// Window presentation and the offscreen capture share these exact commands.
void blit_present(Backend &b,VkCommandBuffer cmd,VkImage target,VkExtent2D extent,VkImageLayout final_layout) {
    VkImageMemoryBarrier barrier={}; barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=target; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkClearColorValue black={}; vkCmdClearColorImage(cmd,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&barrier.subresourceRange);
    unsigned sw=b.output_width,sh=b.output_height;
    auto final=b.image;
    auto view=host_viewport(b.scanout_timing,b.native_width,b.native_height,
                           int(extent.width),int(extent.height),presentation_mode);
    auto rect=view.content;
    // A blit cannot extend beyond its image. Clip to the TV aperture and
    // move source edges proportionally, preserving shifted/cropped scanout.
    int left=std::max({rect.x,view.frame.x,0}), top=std::max({rect.y,view.frame.y,0});
    int right=std::min({rect.x+rect.w,view.frame.x+view.frame.w,int(extent.width)});
    int bottom=std::min({rect.y+rect.h,view.frame.y+view.frame.h,int(extent.height)});
    VkImageBlit blit={}; blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    if (rect.w>0 && rect.h>0 && right>left && bottom>top) {
        blit.srcOffsets[0]={int(int64_t(left-rect.x)*sw/rect.w),int(int64_t(top-rect.y)*sh/rect.h),0};
        blit.srcOffsets[1]={int(int64_t(right-rect.x)*sw/rect.w),int(int64_t(bottom-rect.y)*sh/rect.h),1};
        blit.dstOffsets[0]={left,top,0}; blit.dstOffsets[1]={right,bottom,1};
        vkCmdBlitImage(cmd,final->get_image(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,presentation_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    bool for_transfer=final_layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=final_layout;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask=for_transfer ? VK_ACCESS_TRANSFER_READ_BIT : 0;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         for_transfer ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0,0,nullptr,0,nullptr,1,&barrier);
}

// CRT presets. Lengths are native pixels or scanline pitches unless noted, so
// the look is the same at every output size; masks are output-pixel patterns.
struct Preset {
    float h_sigma, chroma_sigma, chroma_delay; int composite;
    float beam_dark, beam_bright, beam_power;
    int mask; float mask_strength, mask_fade;
    float glow, glow_h, glow_v, glow_power;
    float curve_x, curve_y, corner, vignette;
    float saturation, convergence, peak;
    float period, period_4k; // mask triad period in output pixels below/above 2200 content pixels
};
const Preset presets[4]={
    {},
    {0.22f,0.0f,0.0f,0, 0.18f,0.30f,1.0f, 1,0.50f,0.30f, 0.10f,2.0f,3.0f,2.0f, 0.0f,0.0f,0.0f,0.0f, 1.00f,0.00f,1.15f, 3,3},
    {0.50f,1.60f,0.25f,1, 0.26f,0.40f,1.0f, 2,0.35f,0.40f, 0.20f,1.2f,1.5f,2.0f, 0.0f,0.0f,0.05f,0.20f, 1.00f,0.15f,1.10f, 3,6},
    {0.35f,0.0f,0.0f,0, 0.15f,0.30f,0.8f, 3,0.42f,0.30f, 0.55f,1.5f,1.5f,2.0f, 0.0f,0.0f,0.02f,0.10f, 1.15f,0.08f,1.30f, 6,6},
};
int crt_preset=0;
struct { const char *dir=nullptr; unsigned width=1920, height=1080; } capture;
const uint32_t crt_vert[]=
#include "crt_fullscreen.vert.inc"
;
const uint32_t crt_lines_frag[]=
#include "crt_lines.frag.inc"
;
const uint32_t crt_beam_frag[]=
#include "crt_beam.frag.inc"
;
const uint32_t crt_glow_frag[]=
#include "crt_glow.frag.inc"
;
const uint32_t crt_final_frag[]=
#include "crt_final.frag.inc"
;
const uint32_t osd_frag[]=
#include "osd.frag.inc"
;
void ensure_image(Device &device,ImageHandle &image,unsigned w,unsigned h,VkFormat format,VkImageUsageFlags usage) {
    if (image && image->get_width()==w && image->get_height()==h && image->get_format()==format) return;
#ifdef FF8_GUI
    if (image) HostGuiVulkan::forget_image(image->get_image());
#endif
    auto info=ImageCreateInfo::render_target(w,h,format);
    info.initial_layout=VK_IMAGE_LAYOUT_UNDEFINED; info.usage=usage;
    image=device.create_image(info);
}
#ifdef FF8_GUI
void ensure_backdrop_image(Device &device,ImageHandle &image,unsigned w,unsigned h,VkFormat format) {
    if (image && image->get_width()==w && image->get_height()==h && image->get_format()==format) return;
    auto info=ImageCreateInfo::render_target(w,h,format);
    info.initial_layout=VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image=device.create_image(info);
}
Backend::BackdropKey make_backdrop_key(const Backend &b,VkExtent2D extent,VkFormat format,unsigned preset) {
    Backend::BackdropKey key{};
    key.scanout_revision=b.scanout_revision;
    key.has_image=bool(b.image);
    key.extent_width=extent.width;
    key.extent_height=extent.height;
    key.format=format;
    key.preset=preset;
    key.scale_mode=presentation_mode;
    key.linear=presentation_linear;
    key.scale=options.scale;
    key.native_width=b.native_width;
    key.native_height=b.native_height;
    key.output_width=b.output_width;
    key.output_height=b.output_height;
    key.scanout_timing=b.scanout_timing;
    return key;
}
#endif
template<size_t N> Program &program(Device &device,const uint32_t (&frag)[N]) {
    return *device.request_program(crt_vert,sizeof(crt_vert),frag,sizeof(frag));
}
void run_pass(CommandBuffer &cmd,Image &target,Program &prog,std::initializer_list<const ImageView *> inputs,
              const void *push,size_t size,bool for_transfer) {
    cmd.image_barrier(target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    RenderPassInfo rp;
    rp.color_attachments[0]=&target.get_view(); rp.num_color_attachments=1; rp.store_attachments=1;
    cmd.begin_render_pass(rp);
    cmd.set_quad_state();
    cmd.set_program(prog);
    unsigned binding=0;
    for (auto *input:inputs) cmd.set_texture(0,binding++,*input,StockSampler::LinearClamp);
    cmd.push_constants(push,0,size);
    cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    cmd.draw(3);
    cmd.end_render_pass();
    cmd.image_barrier(target,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        for_transfer ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        for_transfer ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        for_transfer ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT);
}
// The final pass encodes sRGB itself, so it renders through a UNORM format of
// the same texel size; vkCmdCopyImage copies the bits into an sRGB swapchain.
VkFormat unorm_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return format;
    }
}
// Runs the preset's passes from the scanout image into target at the given
// size, leaving it in TRANSFER_SRC_OPTIMAL for the swapchain copy or readback.
// The scanout image returns to TRANSFER_SRC_OPTIMAL for later blits and reads.
void crt_present(Backend &b,ImageHandle &target,VkExtent2D extent,VkFormat format,unsigned preset) {
    const Preset &p=presets[preset];
    auto &cmd=*b.renderer->command_buffer_hack_fixme();
    const VkImageUsageFlags sampled=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    ensure_image(*b.device,target,extent.width,extent.height,format,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    auto view=host_viewport(b.scanout_timing,b.native_width,b.native_height,int(extent.width),int(extent.height),presentation_mode);
    unsigned lines=std::max(1u,b.output_height/options.scale), native_w=std::max(1u,b.output_width/options.scale);
    if (view.content.w<=0 || view.content.h<=0 || view.frame.w<=0 || view.frame.h<=0 || !b.image) {
        cmd.image_barrier(*target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue black={}; VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd.get_command_buffer(),target->get_image(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        cmd.image_barrier(*target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        return;
    }
    unsigned beam_w=std::min(4096u,unsigned(view.content.w)), glow_w=std::max(32u,beam_w/4);
    float px_per_native=float(view.content.w)/float(native_w);
    const VkFormat linear=VK_FORMAT_R16G16B16A16_SFLOAT;
    ensure_image(*b.device,b.crt_lines,b.output_width,lines,linear,sampled);
    ensure_image(*b.device,b.crt_beam,beam_w,lines,linear,sampled);
    ensure_image(*b.device,b.crt_glow_h,glow_w,lines,linear,sampled);
    ensure_image(*b.device,b.crt_glow,glow_w,lines,linear,sampled);
    cmd.image_barrier(*b.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    uint32_t lines_push[4]={options.scale,lines,b.output_width,0};
    run_pass(cmd,*b.crt_lines,program(*b.device,crt_lines_frag),{&b.image->get_view()},lines_push,sizeof(lines_push),false);
    float scale=float(options.scale);
    float beam_push[8]={float(b.output_width),float(lines),float(beam_w),0,
                        p.h_sigma*scale,p.chroma_sigma*scale,float(p.composite),p.chroma_delay*scale};
    run_pass(cmd,*b.crt_beam,program(*b.device,crt_beam_frag),{&b.crt_lines->get_view()},beam_push,sizeof(beam_push),false);
    float glow_h[8]={float(beam_w),float(lines),float(glow_w),float(lines),2,0,std::max(0.5f,p.glow_h*px_per_native*0.5f),p.glow_power};
    run_pass(cmd,*b.crt_glow_h,program(*b.device,crt_glow_frag),{&b.crt_beam->get_view()},glow_h,sizeof(glow_h),false);
    float glow_v[8]={float(glow_w),float(lines),float(glow_w),float(lines),0,1,std::max(0.3f,p.glow_v),1};
    run_pass(cmd,*b.crt_glow,program(*b.device,crt_glow_frag),{&b.crt_glow_h->get_view()},glow_v,sizeof(glow_v),false);
    float width_fade=std::min(1.0f,std::max(0.0f,(float(view.content.w)-640.0f)/560.0f));
    float period=view.content.w<2200 ? p.period : p.period_4k;
    float mask_mean=p.mask==3 ? 5.0f/18.0f : 1.0f/3.0f;
    float final_push[28]={float(view.content.x),float(view.content.y),float(view.content.w),float(view.content.h),
                          float(view.frame.x),float(view.frame.y),float(view.frame.w),float(view.frame.h),
                          float(lines),p.glow,p.convergence*px_per_native,p.saturation,
                          p.beam_dark,p.beam_bright,p.beam_power,mask_mean,
                          float(p.mask),p.mask_strength*width_fade,p.mask_fade,period,
                          p.curve_x,p.curve_y,p.corner,p.vignette,
                          p.peak,p.glow_power,0,0};
    run_pass(cmd,*target,program(*b.device,crt_final_frag),{&b.crt_beam->get_view(),&b.crt_glow->get_view()},final_push,sizeof(final_push),true);
    cmd.image_barrier(*b.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
}
const host_osd_panel_t *osd_panel(Backend &b,VkExtent2D extent) {
    auto view=host_viewport(b.scanout_timing,b.native_width,b.native_height,int(extent.width),int(extent.height),presentation_mode);
    return host_osd_panel(int(extent.width),int(extent.height),host_osd_area(view));
}
const host_osd_panel_t *osd_booster_panel(Backend &b,VkExtent2D extent) {
#ifdef FF8_GUI
    if (host_gui_visible()) return nullptr;
#endif
    auto view=host_viewport(b.scanout_timing,b.native_width,b.native_height,int(extent.width),int(extent.height),presentation_mode);
    return host_osd_booster_panel(int(extent.width),int(extent.height),host_osd_area(view));
}
void osd_present(Backend &b,Image &target,const host_osd_panel_t &panel,ImageHandle &image,unsigned &revision) {
    if (!image || revision!=panel.revision) {
        auto info=ImageCreateInfo::immutable_2d_image(unsigned(panel.w),unsigned(panel.h),VK_FORMAT_R8G8B8A8_UNORM);
        ImageInitialData initial={panel.rgba,0,0};
        image=b.device->create_image(info,&initial); revision=panel.revision;
    }
    auto &cmd=*b.renderer->command_buffer_hack_fixme();
    if (b.timings && !b.osd_first
#ifdef FF8_GUI
        && !host_gui_visible()
#endif
        ) b.osd_first=cmd.write_timestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    cmd.image_barrier(target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    RenderPassInfo rp;
    rp.color_attachments[0]=&target.get_view(); rp.num_color_attachments=1; rp.store_attachments=rp.load_attachments=1;
    cmd.begin_render_pass(rp); cmd.set_quad_state();
    cmd.set_program(program(*b.device,osd_frag));
    cmd.set_texture(0,0,image->get_view(),StockSampler::NearestClamp);
    VkViewport viewport={float(panel.x),float(panel.y),float(panel.w),float(panel.h),0,1};
    cmd.set_viewport(viewport);
    cmd.set_blend_enable(true);
    cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    float push[5]={float(panel.x),float(panel.y),float(panel.w),float(panel.h),panel.opacity/255.0f};
    cmd.push_constants(push,0,sizeof(push)); cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    cmd.draw(3); cmd.end_render_pass();
    cmd.image_barrier(target,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    if (b.timings && !b.osd_last
#ifdef FF8_GUI
        && !host_gui_visible()
#endif
        ) b.osd_last=cmd.write_timestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}
// Dedicated readback memory keeps the worker outside Granite's single-threaded
// allocators. Handles and the managed fence are retired by the game thread.
struct Screenshot {
    VkDevice device;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    Fence fence;
    bool coherent=false, mapped=false;
    ~Screenshot() {
        if (mapped) vkUnmapMemory(device,memory);
        if (buffer) vkDestroyBuffer(device,buffer,nullptr);
        if (memory) vkFreeMemory(device,memory,nullptr);
    }
};
const uint8_t *screenshot_read(void *data) {
    auto &s=*static_cast<Screenshot *>(data);
    VkFence fence=s.fence->get_fence(); void *pixels=nullptr;
    if (vkWaitForFences(s.device,1,&fence,VK_TRUE,UINT64_MAX)!=VK_SUCCESS ||
        vkMapMemory(s.device,s.memory,0,VK_WHOLE_SIZE,0,&pixels)!=VK_SUCCESS) return nullptr;
    s.mapped=true;
    if (!s.coherent) {
        VkMappedMemoryRange range={}; range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory=s.memory; range.size=VK_WHOLE_SIZE;
        if (vkInvalidateMappedMemoryRanges(s.device,1,&range)!=VK_SUCCESS) return nullptr;
    }
    return static_cast<const uint8_t *>(pixels);
}
void screenshot_release(void *data) { delete static_cast<Screenshot *>(data); }
void screenshot_copy(Backend &b,ImageHandle &image,VkExtent2D extent) {
    auto frame=host_viewport(b.scanout_timing,b.native_width,b.native_height,int(extent.width),int(extent.height),presentation_mode).frame;
    if (frame.w<=0 || frame.h<=0) { host_screenshot_fail("empty viewport"); return; }
    auto shot=std::make_unique<Screenshot>(); shot->device=b.device->get_device();
    VkBufferCreateInfo info={}; info.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size=size_t(frame.w)*frame.h*4; info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(shot->device,&info,nullptr,&shot->buffer)!=VK_SUCCESS) { host_screenshot_fail("cannot allocate"); return; }
    VkMemoryRequirements required; vkGetBufferMemoryRequirements(shot->device,shot->buffer,&required);
    auto &types=b.device->get_memory_properties(); int selected=-1,score=-1;
    for (unsigned i=0;i<types.memoryTypeCount;++i) {
        auto flags=types.memoryTypes[i].propertyFlags;
        int candidate=((flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 2 : 0)+((flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 1 : 0);
        if ((required.memoryTypeBits&(1u<<i)) && (flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && candidate>score) { selected=int(i); score=candidate; }
    }
    if (selected<0) { host_screenshot_fail("cannot allocate"); return; }
    VkMemoryAllocateInfo alloc={}; alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize=required.size; alloc.memoryTypeIndex=unsigned(selected);
    if (vkAllocateMemory(shot->device,&alloc,nullptr,&shot->memory)!=VK_SUCCESS ||
        vkBindBufferMemory(shot->device,shot->buffer,shot->memory,0)!=VK_SUCCESS) { host_screenshot_fail("cannot allocate"); return; }
    shot->coherent=(types.memoryTypes[selected].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    auto &cmd=b.renderer->command_buffer_hack_fixme();
    VkBufferImageCopy copy={}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageOffset={frame.x,frame.y,0}; copy.imageExtent={unsigned(frame.w),unsigned(frame.h),1};
    vkCmdCopyImageToBuffer(cmd->get_command_buffer(),image->get_image(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,shot->buffer,1,&copy);
    cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    shot->fence=b.renderer->flush_and_signal();
    if (!shot->fence) { host_screenshot_fail("cannot submit"); return; }
    int format;
    switch (image->get_format()) {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: format=HOST_SCREENSHOT_BGRA; break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: format=HOST_SCREENSHOT_RGBA; break;
    default: host_screenshot_fail("unsupported format"); return;
    }
    host_screenshot_submit(frame.w,frame.h,format,shot.release(),screenshot_read,screenshot_release);
}
void composed_present(Backend &b,ImageHandle &target,VkExtent2D extent,VkFormat format,unsigned preset) {
#ifdef FF8_GUI
    if (!host_gui_visible() && host_gui_toast_visible()) {
        auto view=host_viewport(b.scanout_timing,b.native_width,b.native_height,int(extent.width),int(extent.height),presentation_mode);
        host_gui_toast_frame(extent.width,extent.height,host_osd_area(view));
    }
    const auto *osd_p=osd_panel(b,extent);
    const auto *bosd_p=osd_booster_panel(b,extent);
    bool use_backdrop=host_gui_visible() && (&target==&b.output || !b.window);
    Backend::BackdropKey backdrop_key{};
    bool backdrop_hit=false;
    if (use_backdrop) {
        backdrop_key=make_backdrop_key(b,extent,format,preset);
        backdrop_hit=b.gui_backdrop_valid && b.gui_backdrop && b.gui_backdrop_key==backdrop_key;
    }
    bool is_output_target=(&target==&b.output);
    bool is_capture_target=(&target==&b.capture_output);
    Backend::ComposedGuiKey composed_key{};
    bool can_cache_composed=host_gui_visible() && !host_screenshot_pending() &&
                            (is_output_target || is_capture_target);
    if (can_cache_composed) {
        uint64_t dh=HostGuiVulkan::draw_hash();
        if (dh!=0) {
            composed_key.backdrop=use_backdrop ? backdrop_key : make_backdrop_key(b,extent,format,preset);
            composed_key.draw_hash=dh;
            composed_key.font_gen=HostGuiVulkan::font_generation();
            if (osd_p) composed_key.osd={true,osd_p->revision,osd_p->opacity,osd_p->x,osd_p->y,osd_p->w,osd_p->h};
            if (bosd_p) composed_key.booster_osd={true,bosd_p->revision,bosd_p->opacity,bosd_p->x,bosd_p->y,bosd_p->w,bosd_p->h};
            composed_key.target_image=target ? target->get_image() : VK_NULL_HANDLE;
            bool &slot_valid=is_output_target ? b.gui_composed_output_valid : b.gui_composed_capture_valid;
            Backend::ComposedGuiKey &slot_key=is_output_target ? b.gui_composed_output_key : b.gui_composed_capture_key;
            if (slot_valid && target && target->get_width()==extent.width && target->get_height()==extent.height &&
                target->get_format()==format && slot_key==composed_key) {
                if (b.context->get_gpu_props().limits.timestampComputeAndGraphics) {
                    auto &command=*b.renderer->command_buffer_hack_fixme();
                    auto first=command.write_timestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                    auto last=command.write_timestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                    if (first && last) b.gui_samples.push_back({first,last});
                }
                return;
            }
        } else {
            can_cache_composed=false;
        }
    }
    if (backdrop_hit) {
        ensure_image(*b.device,target,extent.width,extent.height,format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        auto &cmd=*b.renderer->command_buffer_hack_fixme();
        cmd.image_barrier(*target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        cmd.copy_image(*target,*b.gui_backdrop);
        cmd.image_barrier(*target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    } else
#endif
    {
        if (preset) crt_present(b,target,extent,format,preset);
        else {
            ensure_image(*b.device,target,extent.width,extent.height,format,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            blit_present(b,b.renderer->command_buffer_hack_fixme()->get_command_buffer(),target->get_image(),extent,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
#ifdef FF8_GUI
        if (use_backdrop) {
            ensure_backdrop_image(*b.device,b.gui_backdrop,extent.width,extent.height,format);
            if (b.gui_backdrop) {
                auto &cmd=*b.renderer->command_buffer_hack_fixme();
                cmd.image_barrier(*b.gui_backdrop,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
                cmd.copy_image(*b.gui_backdrop,*target);
                cmd.image_barrier(*b.gui_backdrop,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
                b.gui_backdrop_key=backdrop_key;
                b.gui_backdrop_valid=true;
            } else b.gui_backdrop_valid=false;
        }
#endif
    }
    if (host_screenshot_pending() && (!b.window || &target==&b.output)
#ifdef FF8_GUI
        && !host_gui_visible()
#endif
        ) screenshot_copy(b,target,extent);
#ifdef FF8_GUI
    bool gui_drawn=false;
    if (host_gui_visible() || host_gui_toast_visible()) {
        auto &command=*b.renderer->command_buffer_hack_fixme();
        // Start after preceding composition commands complete, and end after GUI
        // writes/transitions. The result excludes retained-picture rendering.
        QueryPoolHandle first;
        if (b.context->get_gpu_props().limits.timestampComputeAndGraphics)
            first=command.write_timestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        gui_drawn=HostGuiVulkan::render(command.get_command_buffer(),target->get_image(),
                                        target->get_view().get_view(),extent,format);
        if (gui_drawn && first) {
            auto last=command.write_timestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            if (last) b.gui_samples.push_back({first,last});
        }
    }
    if (is_output_target || is_capture_target) {
        bool &slot_valid=is_output_target ? b.gui_composed_output_valid : b.gui_composed_capture_valid;
        Backend::ComposedGuiKey &slot_key=is_output_target ? b.gui_composed_output_key : b.gui_composed_capture_key;
        if (can_cache_composed && gui_drawn && target) {
            composed_key.target_image=target->get_image();
            slot_key=composed_key;
            slot_valid=true;
        } else {
            slot_valid=false;
        }
    }
    if (osd_p) osd_present(b,*target,*osd_p,b.osd_image,b.osd_revision);
    if (bosd_p) osd_present(b,*target,*bosd_p,b.booster_osd_image,b.booster_osd_revision);
#else
    if (auto *panel=osd_panel(b,extent)) osd_present(b,*target,*panel,b.osd_image,b.osd_revision);
    if (auto *bpanel=osd_booster_panel(b,extent)) osd_present(b,*target,*bpanel,b.booster_osd_image,b.booster_osd_revision);
#endif
}
// Offscreen presentation for --dump-presented-frames and the preview tool. It
// has its own target, so a window presenting the same frame is unaffected.
void offscreen_present(Backend &b,unsigned width,unsigned height,unsigned preset) {
    composed_present(b,b.capture_output,{width,height},VK_FORMAT_R8G8B8A8_UNORM,preset);
}
void present_timestamp(Backend &b,QueryPoolHandle &slot,VkPipelineStageFlagBits stage) {
    if (b.timings && !slot
#ifdef FF8_GUI
        && !host_gui_visible()
#endif
        ) slot=b.renderer->command_buffer_hack_fixme()->write_timestamp(stage);
}
}
namespace {
// Acquires the window's next image, unless one is held: 1 when one is held,
// 0 when the swapchain has none free yet (FIFO frees one each refresh), -1
// when the window cannot be presented to now.
int acquire_window(Backend &b) {
    if (b.acquired) return 1;
    if (!b.window || !b.wsi) return -1;
    int width,height; SDL_Vulkan_GetDrawableSize(b.window,&width,&height);
    if (width<=0 || height<=0 || (SDL_GetWindowFlags(b.window)&SDL_WINDOW_MINIMIZED)) return -1;
    if (b.wsi->surface_lost) {
        if (b.surface) { b.old_surfaces.push_back(b.surface); b.surface=VK_NULL_HANDLE; }
        if (!SDL_Vulkan_CreateSurface(b.window,b.instance,&b.surface)) {
            std::fprintf(stderr,"Vulkan surface unavailable; presentation will retry: %s\n",SDL_GetError());
            return -1;
        }
        b.wsi->surface=b.surface; b.wsi->surface_lost=false;
    }
    if (!b.wsi->prepare({unsigned(width),unsigned(height)},b.wsi_frame(),wall_milliseconds())) return -1;
    auto acquire=b.device->request_semaphore();
    uint32_t index=0;
    if (!b.wsi->acquire(acquire->get_semaphore(),index,b.wsi_frame(),wall_milliseconds())) return 0;
    acquire->signal_external();
    b.acquire_wait=acquire;
    b.acquired=true; b.filled=false; b.present_index=index;
    return 1;
}
}
extern "C" int host_gpu_ready(void) try {
    if (!backend || backend->failed) return -1;
    return acquire_window(*backend);
} catch (const std::exception &e) { degrade("host_gpu_ready",e.what()); return -1; }
catch (...) { degrade("host_gpu_ready","unknown backend exception"); return -1; }
extern "C" int host_gpu_present(void) {
    if (!backend || backend->failed) return 1;
    try {
        auto &b=*backend;
#ifdef FF8_GUI
        if (!host_gui_visible()) {
            if (b.gui_backdrop) {
                b.gui_backdrop.reset();
                b.gui_backdrop_valid=false;
            }
            b.invalidate_gui_composed();
        }
        struct PreserveCounters {
            Renderer &renderer;
            decltype(Renderer::counters) saved;
            bool paused;
            ~PreserveCounters() { if (paused) renderer.counters=saved; }
        } counters{*b.renderer,b.renderer->counters,host_gui_visible()!=0};
#endif
        unsigned preset=unsigned(crt_preset);
#ifdef FF8_GUI
        if (!b.raster || !b.image) preset=0; // No retained PS1 picture in the shell or before first scanout.
#endif
        if (capture.dir || (!b.window && host_screenshot_pending())
#ifdef FF8_GUI
            || (host_gui_visible() && !b.window) || host_gui_test_capture()
#endif
            ) {
            present_timestamp(b,b.present_first,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            offscreen_present(b,capture.width,capture.height,preset);
            present_timestamp(b,b.present_last,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        if (!b.window || acquire_window(b)<=0) return 1;
        uint32_t index=b.present_index;
        // The image's wait goes with the submission that draws into it.
        if (b.acquire_wait) {
            b.device->add_wait_semaphore(CommandBuffer::Type::Generic,b.acquire_wait,VK_PIPELINE_STAGE_TRANSFER_BIT,false);
            b.acquire_wait.reset();
        }
        b.filled=true;
        if (!preset && !osd_panel(b,b.wsi->extent) && !osd_booster_panel(b,b.wsi->extent) && !host_screenshot_pending()
#ifdef FF8_GUI
            && !host_gui_visible() && !host_gui_toast_visible()
#endif
            ) {
            VkCommandBuffer cmd=b.renderer->command_buffer_hack_fixme()->get_command_buffer();
            blit_present(b,cmd,b.wsi->image(index),b.wsi->extent,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            b.device->add_signal_semaphore(CommandBuffer::Type::Generic,b.wsi->ready(index));
            return 1;
        }
        // CRT: render into an image of the swapchain's texel format, then copy it in.
        // The swapchain keeps its transfer-only usage, so crt=off is untouched.
        present_timestamp(b,b.present_first,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        composed_present(b,b.output,b.wsi->extent,unorm_format(b.wsi->format),preset);
        auto &cmd=*b.renderer->command_buffer_hack_fixme();
        VkImageMemoryBarrier barrier={}; barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=b.wsi->image(index); barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd.get_command_buffer(),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkImageCopy region={}; region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        region.extent={b.wsi->extent.width,b.wsi->extent.height,1};
        vkCmdCopyImage(cmd.get_command_buffer(),b.output->get_image(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       b.wsi->image(index),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
        vkCmdPipelineBarrier(cmd.get_command_buffer(),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        present_timestamp(b,b.present_last,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        // Screenshot readback may have submitted before OSD; signal only after
        // the final swapchain copy, so it cannot present that earlier submission.
        b.device->add_signal_semaphore(CommandBuffer::Type::Generic,b.wsi->ready(index));
        return 1;
    } catch (const std::exception &e) { degrade("present",e.what()); return 0; }
    catch (...) { degrade("present","unknown backend exception"); return 0; }
}
extern "C" void host_gpu_crt(int preset) { crt_preset=preset>=0 && preset<4 ? preset : 0; }
extern "C" int host_gpu_crt_preset(void) { return crt_preset; }
extern "C" void host_gpu_present_capture(const char *dir,unsigned width,unsigned height) {
    capture.dir=dir && *dir ? dir : nullptr;
    capture.width=std::max(1u,width); capture.height=std::max(1u,height);
}
extern "C" const char *host_gpu_presented_frames(void) { return capture.dir; }
namespace {
// Download b.capture_output (TRANSFER_SRC_OPTIMAL, RGBA) as tightly packed RGB.
void download_output(Backend &b,const uint8_t **out,int *w,int *h) {
    double start=thread_ms();
    unsigned iw=b.capture_output->get_width(), ih=b.capture_output->get_height();
    BufferCreateInfo info; info.domain=BufferDomain::CachedHost; info.size=size_t(iw)*ih*4;
    info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto buffer=b.device->create_buffer(info,nullptr);
    auto &cmd=b.renderer->command_buffer_hack_fixme();
    cmd->copy_image_to_buffer(*buffer,*b.capture_output,0,{0,0,0},{iw,ih,1},0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1});
    cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    auto fence=b.renderer->flush_and_signal();
    if (!fence || !fence->wait()) throw std::runtime_error("Presented capture fence failed");
    auto pixels=static_cast<const uint8_t *>(b.device->map_host_buffer(*buffer,MEMORY_ACCESS_READ_BIT));
    if (!pixels) throw std::runtime_error("Presented capture mapping failed");
    b.presented.resize(size_t(iw)*ih*3);
    for (size_t i=0;i<size_t(iw)*ih;++i) std::memcpy(b.presented.data()+i*3,pixels+i*4,3);
    b.device->unmap_host_buffer(*buffer,MEMORY_ACCESS_READ_BIT);
    b.capture_ms+=thread_ms()-start;
    *w=int(iw); *h=int(ih); *out=b.presented.data();
}
}
extern "C" int host_gpu_presented_rgb(const uint8_t **out,int *w,int *h) try {
    if (!backend || backend->failed || !capture.dir || !backend->capture_output) return 0;
    CpuTimer timer;
    download_output(*backend,out,w,h);
    return 1;
} catch (const std::exception &e) { degrade("host_gpu_presented_rgb",e.what()); return 0; }
catch (...) { degrade("host_gpu_presented_rgb","unknown backend exception"); return 0; }
#ifdef FF8_GUI
extern "C" int host_gpu_gui_rgb(const uint8_t **out,int *w,int *h) try {
    if (!backend || backend->failed || !backend->capture_output) return 0;
    double prior=backend->capture_ms;
    download_output(*backend,out,w,h);
    backend->capture_ms=prior;
    return 1;
} catch (const std::exception &e) { degrade("GUI readback",e.what()); return 0; }
catch (...) { degrade("GUI readback","unknown backend exception"); return 0; }
#endif
extern "C" int host_gpu_present_image(const uint8_t *rgb,int width,int height,gpu_scanout_t timing,int native_w,int native_h,
                                      unsigned out_w,unsigned out_h,int preset,const uint8_t **out,int *w,int *h) try {
    if (!backend || backend->failed || width<=0 || height<=0 || native_w<=0 || native_h<=0 ||
        unsigned(width)!=unsigned(native_w)*options.scale || unsigned(height)!=unsigned(native_h)*options.scale) return 0;
    auto &b=*backend;
    std::vector<uint8_t> rgba(size_t(width)*height*4,255);
    for (size_t i=0;i<size_t(width)*height;++i) std::memcpy(rgba.data()+i*4,rgb+i*3,3);
    auto info=ImageCreateInfo::immutable_2d_image(unsigned(width),unsigned(height),VK_FORMAT_R8G8B8A8_UNORM);
    info.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT; info.initial_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ImageInitialData initial={rgba.data(),0,0};
    b.image=b.device->create_image(info,&initial);
    b.output_width=unsigned(width); b.output_height=unsigned(height);
    b.scanout_timing=timing; b.native_width=native_w; b.native_height=native_h;
    ++b.scanout_revision;
    b.renderer->command_buffer_hack_fixme();
    offscreen_present(b,std::max(1u,out_w),std::max(1u,out_h),preset>0 && preset<4 ? unsigned(preset) : 0u);
    download_output(b,out,w,h);
    return 1;
} catch (const std::exception &e) { degrade("host_gpu_present_image",e.what()); return 0; }
catch (...) { degrade("host_gpu_present_image","unknown backend exception"); return 0; }
namespace {
int queue_present() {
    auto &b=*backend;
    // An image acquired ahead and not drawn into yet waits for the next present.
    if (!b.acquired || !b.filled) return 1;
    b.acquired=b.filled=false;
    uint64_t target=0;
    if (b.present_target>0 && b.wsi->targets()) {
        double at=to_domain(b.wsi->time_domain(),b.present_target);
        if (at>0) target=uint64_t(at*1e9);
    }
    b.present_target=0;
    b.wsi->present(b.present_index,b.wsi_frame(),wall_milliseconds(),target);
    return 1;
}
}

#ifdef FF8_VULKAN_TESTING
extern "C" void host_gpu_test_fault(unsigned phase) {
    fault_phase=phase;
    if (phase==3) { real_wait=vkWaitForFences; vkWaitForFences=fail_wait; }
    if (phase==5) { real_query=vkGetQueryPoolResults; vkGetQueryPoolResults=fail_query; }
}
extern "C" unsigned host_gpu_test_maps(void) { return test_maps; }
#endif
extern "C" uint64_t host_gpu_present_id(void) {
    return backend && !backend->failed && backend->wsi ? backend->wsi->last_id : 0;
}
extern "C" int host_gpu_present_seen(uint64_t *id,double *seconds) {
    if (!backend || backend->failed || !backend->wsi) return 0;
    if (backend->wsi->timed()) {
        uint64_t time=0;
        if (!backend->wsi->shown(*id,time)) return 0;
        *seconds=from_domain(backend->wsi->time_domain(),double(time)*1e-9);
        return 1;
    }
    if (!backend->wsi->wait_present) return 0;
    backend->wsi->watch(counter_seconds);
    return backend->wsi->seen(*id,*seconds) ? 1 : 0;
}
extern "C" int host_gpu_present_timing(void) {
    if (!backend || backend->failed || !backend->wsi || !backend->wsi->timed()) return 0;
    return backend->wsi->targets() ? 2 : 1;
}
extern "C" double host_gpu_refresh(void) {
    if (!backend || backend->failed || !backend->wsi || !backend->wsi->timed()) return 0;
    return double(backend->wsi->refresh())*1e-9;
}
extern "C" void host_gpu_present_at(double seconds) { if (backend) backend->present_target=seconds; }
extern "C" void host_gpu_paced(int on) {
    paced_wanted=on!=0;
    if (backend && backend->wsi) backend->wsi->pace(paced_wanted);
}
extern "C" void host_gpu_between(int on) {
    if (!backend) return;
    // What the picture just drawn read from the software rasterizer is what the
    // next will.
    backend->read=backend->reading;
    backend->reading.clear();
    backend->between=on!=0;
}
