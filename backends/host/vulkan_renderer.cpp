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
extern "C" {
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
struct Backend {
    gpu_raster_t *raster=nullptr;
    VkInstance instance=VK_NULL_HANDLE;
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    SDL_Window *window=nullptr;
    std::unique_ptr<HostGPU::Swapchain> wsi;
    std::vector<VkSurfaceKHR> old_surfaces;
    bool failed=false;
    bool acquired=false;
    uint32_t present_index=0;
    std::unique_ptr<Context> context;
    std::unique_ptr<Device> device;
    std::unique_ptr<Renderer> renderer;
    // Native VRAM is authoritative. Upscaled pixels never reach the guest.
    std::vector<uint8_t> rgb;
    unsigned output_width=1, output_height=1;
    std::array<uint64_t,16> dirty_rows[512] = {};
    bool dirty=false;
    // Framebuffer captures used as 15-bit feedback textures need the oracle's
    // per-primitive truncation. Track their provenance through copies and draws.
    std::array<uint64_t,16> effect_pixels[512] = {};
    unsigned effect_counts[32] = {};
    uint32_t effect_pages=0;
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
        if (!effect_pages) return false;
        for (unsigned row=0;row<h;) {
            unsigned yy=(y+row)&511;
            for (unsigned col=0;col<w;) {
                unsigned xx=(x+col)&1023;
                if (effect_pages&(uint32_t(1)<<((yy/256)*16+xx/64))) return true;
                col+=std::min(w-col,64-xx%64);
            }
            row+=std::min(h-row,256-yy%256);
        }
        return false;
    }
    bool effect_texture(const gpu_raster_t *s,gpu_paint_t p) const {
        return p.textured && ((s->mode>>7)&3)>=2 &&
            effect_source((s->mode&15)*64,((s->mode>>4)&1)*256,256,256);
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
    ImageHandle image; // GPU scanout; CPU mapping is only for explicit captures.
    ImageHandle scanouts[3];
    // CRT intermediates, the window's CRT target and the offscreen capture target,
    // recreated on size changes.
    ImageHandle crt_lines, crt_beam, crt_glow_h, crt_glow, output, capture_output;
    std::vector<uint8_t> presented;
    ImageHandle osd_image;
    unsigned osd_revision=0;
    Backend() = default;
    ~Backend() {
        if (renderer && failed) renderer->abandon();
        if (renderer) renderer->flush();
        if (device) device->wait_idle();
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
        osd_image.reset();
        movie_buffer.reset(); movie_frame.reset(); movie_patches.clear();
        first.reset(); last.reset(); present_first.reset(); present_last.reset(); osd_first.reset(); osd_last.reset();
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
    if (backend) backend->failed=true;
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
    auto emit=[&](Rect clip) { auto c=rect.scissor(clip); if (c.width && c.height) { r.set_draw_rect(c); draw(); } };
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
        }
        if (options.timings) { b->timings=std::fopen(options.timings,"w"); if (!b->timings) throw std::runtime_error("Cannot open GPU timings");
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
        } else
#endif
        if (!host_gpu_resume(g)) { backend.reset(); options.enabled=false; }
        return 1;
    } catch (const std::exception &e) { std::fprintf(stderr,"Vulkan: %s\n",e.what()); backend.reset(); options.enabled=false; std::fprintf(stderr,"Vulkan warning: using native soft presentation\n"); return 1; }
    catch (...) { degrade("initialization","unknown exception"); backend.reset(); return 1; }
}
extern "C" void host_gpu_stop(void) {
    if (backend) host_screenshot_flush();
    try { backend.reset(); } catch (...) { std::fprintf(stderr,"Vulkan cleanup failed\n"); }
    options={};
}
static int presentation_mode, presentation_linear;
extern "C" void host_gpu_presentation(int mode, int linear) {
    presentation_mode=mode; presentation_linear=linear;
}
extern "C" int host_gpu_resume(gpu_t *g) try {
    if (!backend || !g) return 0;
    backend->movie_patches.clear();
    backend->movie_frame.reset(); backend->movie_buffer.reset();
    backend->dirty=false;
    for (auto &row:backend->dirty_rows) row.fill(0);
    backend->raster=gpu_raster_state(g);
    backend->effect_pages=0;
    std::memset(backend->effect_counts,0,sizeof(backend->effect_counts));
    for (auto &row:backend->effect_pixels) row.fill(0);
    backend->upload({0,0,1024,512});
    return 1;
} catch (const std::exception &e) { degrade("host_gpu_resume",e.what()); return 0; }
catch (...) { degrade("host_gpu_resume","unknown backend exception"); return 0; }
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
    return options.scale==1 && !options.true_color && options.msaa==1 && !host_movies_active();
}

void dirty_bounds(gpu_raster_t *s,int x,int y,int w,int h,bool effect=false) {
    int l=std::max(x,s->left), r=std::min(x+w,s->right+1);
    int t=std::max(y,s->top), b=std::min(y+h,s->bottom+1);
    if (r>l) for (int row=t;row<b;++row) backend->mark(unsigned(l),unsigned(row),unsigned(r-l));
    if (effect && r>l && b>t) backend->effect(unsigned(l),unsigned(t),unsigned(r-l),unsigned(b-t),true);
}

}
extern "C" void __wrap_gpu_raster_triangle(gpu_raster_t *s,gpu_vertex_t a,gpu_vertex_t b,gpu_vertex_t c,gpu_paint_t p) {
    if (!active(s)) { __real_gpu_raster_triangle(s,a,b,c,p); return; }
#ifdef FF8_PGXP
    bool precise_draw=false;
#endif
    CpuTimer timer;
    if (backend->effect_texture(s,p)) {
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
        // Native output is replaced by the exact software pixels below. Keep
        // those writes pending until a draw that needs GPU pixels or scanout,
        // instead of drawing and uploading the same primitive twice.
        if (native_pixels()
#ifdef FF8_PGXP
            && !precise_draw
#endif
        ) return;
        state(s,p,PrimitiveType::Polygon);
        clipped(s,[&] { backend->renderer->draw_triangle(v); });
    }
    });
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
    if (backend->effect_texture(s,p)) {
        { SoftTimer soft; __real_gpu_raster_rectangle(s,a,w,h,p); }
        dirty_bounds(s,a.x,a.y,w,h,true);
        return;
    }
    gpu_work("rectangle",[&] {
    if (native_pixels()) return;
    if (w>0 && h>0) {
        state(s,p,PrimitiveType::Sprite);
        Vertex v[4]={vertex(a),vertex(a),vertex(a),vertex(a)};
        v[1].x+=w; v[2].y+=h; v[3].x+=w; v[3].y+=h;
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
        backend->renderer->set_UV_limits(uint16_t(std::min(u,u+du)),uint16_t(std::min(tex_v,tex_v+dv)),
                                         uint16_t(std::max(u,u+du)),uint16_t(std::max(tex_v,tex_v+dv)));
        clipped(s,[&] { backend->renderer->draw_quad(v); });
    }
    });
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
        if (skip && n) {
            v[0].x+=float(b.x-a.x)/float(n); v[0].y+=float(b.y-a.y)/float(n);
        }
        clipped(s,[&] { backend->renderer->draw_line(v); });
    }
    });
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
    gpu_work("fill uploads",[&] { backend->begin(); backend->uploads(); });
    // Clear ignores masks, field parity and the drawing area.
    gpu_work("fill field",[&] { backend->renderer->set_draw_field(-1); });
    { SoftTimer soft; __real_gpu_raster_fill(s,x,y,w,h,color); }
    gpu_work("fill",[&] {
    for (int row=0;row<h;) {
        unsigned yy=unsigned(y+row)&511, rows=std::min(unsigned(h-row),512-yy);
        for (int col=0;col<w;) {
            unsigned xx=unsigned(x+col)&1023, cols=std::min(unsigned(w-col),1024-xx);
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
        backend->renderer->set_mask_test(s->mask&2); backend->renderer->set_force_mask_bit(s->mask&1);
        backend->renderer->blit_vram({unsigned(dx),unsigned(dy),unsigned(w),unsigned(h)},
                                     {unsigned(sx),unsigned(sy),unsigned(w),unsigned(h)});
    }
    });
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
    auto &target=backend->scanouts[backend->device->get_current_frame_context()];
    target=backend->renderer->host_scanout({unsigned(x),unsigned(y),unsigned(std::max(nw,1)),unsigned(std::max(nh,1))},
        depth!=0,options.true_color,enabled && nw && nh,target);
    backend->image=target;
    backend->output_width=target->get_width(); backend->output_height=target->get_height();
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
namespace { int queue_present(); }
extern "C" int host_gpu_frame(gpu_t *g) try {
    if (!backend || backend->failed || backend->raster!=gpu_raster_state(g)) return 1;
    auto &b=*backend;
    inject(2);
    double cpu_start=thread_ms();
    b.begin(); b.uploads();
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
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=final_layout;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
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
void osd_present(Backend &b,Image &target,const host_osd_panel_t &panel) {
    if (!b.osd_image || b.osd_revision!=panel.revision) {
        auto info=ImageCreateInfo::immutable_2d_image(unsigned(panel.w),unsigned(panel.h),VK_FORMAT_R8G8B8A8_UNORM);
        ImageInitialData initial={panel.rgba,0,0};
        b.osd_image=b.device->create_image(info,&initial); b.osd_revision=panel.revision;
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
    cmd.set_texture(0,0,b.osd_image->get_view(),StockSampler::NearestClamp);
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
#endif
    if (preset) crt_present(b,target,extent,format,preset);
    else {
        ensure_image(*b.device,target,extent.width,extent.height,format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        blit_present(b,b.renderer->command_buffer_hack_fixme()->get_command_buffer(),target->get_image(),extent,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    if (host_screenshot_pending() && (!b.window || &target==&b.output)
#ifdef FF8_GUI
        && !host_gui_visible()
#endif
        ) screenshot_copy(b,target,extent);
#ifdef FF8_GUI
    if (host_gui_visible() || host_gui_toast_visible()) {
        auto &command=*b.renderer->command_buffer_hack_fixme();
        // Start after preceding composition commands complete, and end after GUI
        // writes/transitions. The result excludes retained-picture rendering.
        QueryPoolHandle first;
        if (b.context->get_gpu_props().limits.timestampComputeAndGraphics)
            first=command.write_timestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        bool drawn=HostGuiVulkan::render(command.get_command_buffer(),target->get_image(),
                                        target->get_view().get_view(),extent,format);
        if (drawn && first) {
            auto last=command.write_timestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            if (last) b.gui_samples.push_back({first,last});
        }
    }
#endif
    if (auto *panel=osd_panel(b,extent)) osd_present(b,*target,*panel);
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
extern "C" int host_gpu_present(void) {
    if (!backend || backend->failed) return 1;
    try {
        auto &b=*backend;
#ifdef FF8_GUI
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
        if (!b.window) return 1;
        int width,height; SDL_Vulkan_GetDrawableSize(b.window,&width,&height);
        if (width<=0 || height<=0 || (SDL_GetWindowFlags(b.window)&SDL_WINDOW_MINIMIZED)) return 1;
        if (b.wsi->surface_lost) {
            if (b.surface) { b.old_surfaces.push_back(b.surface); b.surface=VK_NULL_HANDLE; }
            if (!SDL_Vulkan_CreateSurface(b.window,b.instance,&b.surface)) {
                std::fprintf(stderr,"Vulkan surface unavailable; presentation will retry: %s\n",SDL_GetError());
                return 1;
            }
            b.wsi->surface=b.surface; b.wsi->surface_lost=false;
        }
        if (!b.wsi->prepare({unsigned(width),unsigned(height)},b.wsi_frame(),wall_milliseconds())) return 1;
        auto acquire=b.device->request_semaphore();
        uint32_t index=0;
        if (!b.wsi->acquire(acquire->get_semaphore(),index,b.wsi_frame(),wall_milliseconds())) return 1;
        acquire->signal_external();
        b.device->add_wait_semaphore(CommandBuffer::Type::Generic,acquire,VK_PIPELINE_STAGE_TRANSFER_BIT,false);
        b.acquired=true; b.present_index=index;
        if (!preset && !osd_panel(b,b.wsi->extent) && !host_screenshot_pending()
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
    b.renderer->command_buffer_hack_fixme();
    offscreen_present(b,std::max(1u,out_w),std::max(1u,out_h),preset>0 && preset<4 ? unsigned(preset) : 0u);
    download_output(b,out,w,h);
    return 1;
} catch (const std::exception &e) { degrade("host_gpu_present_image",e.what()); return 0; }
catch (...) { degrade("host_gpu_present_image","unknown backend exception"); return 0; }
namespace {
int queue_present() {
    auto &b=*backend;
    if (!b.acquired) return 1;
    b.acquired=false;
    b.wsi->present(b.present_index,b.wsi_frame(),wall_milliseconds());
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
