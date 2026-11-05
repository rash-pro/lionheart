#ifndef HOST_PRESENTATION_HISTORY_HPP
#define HOST_PRESENTATION_HISTORY_HPP
#include "renderer/renderer.hpp"
#include "frame_tags.h"
#include "frame_geometry.hpp"
#include <array>
#include <algorithm>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace HostGPU {
// An ordered, owned construction journal. No entry borrows guest RAM or VRAM.
struct PresentationCommand {
    enum Kind { Triangle, Quad, Line, Fill, Copy, Upload, DisplayBarrier } kind;
    PSX::Renderer::HostState state{};
    std::array<PSX::Vertex,4> vertices{};
    std::array<frame_corner_t,3> corners{};
    PSX::Rect dst, src;
    uint32_t color=0;
    std::vector<uint16_t> pixels;
    bool geometry=false;
    bool preserve_scaled=false;
    uint64_t palette=0;
    void draw(PSX::Renderer &r,const GeometryPair *pair=nullptr,float alpha=1,float ox=0,float oy=0) const {
        r.host_state(state);
        switch (kind) {
        case DisplayBarrier: r.host_display_barrier(dst,color!=0); break;
        case Triangle:
            if (pair && pair->interpolate) {
                auto v=vertices;
                for (unsigned i=0;i<3;++i) {
                    if (pair->hold[i]) continue;
                    auto before=pair->before[i]; before.x+=ox; before.y+=oy;
                    auto p=interpolate_position(before,{v[i].x,v[i].y,v[i].w},alpha);
                    v[i].x=p.x; v[i].y=p.y; v[i].w=p.w;
                }
                r.host_draw_triangle(v.data());
            } else r.host_draw_triangle(vertices.data());
            break;
        case Quad: r.draw_quad(vertices.data()); break;
        case Line: r.draw_line(vertices.data()); break;
        case Fill: r.clear_rect(dst,color); break;
        case Copy: r.blit_vram(dst,src); break;
        case Upload: {
            auto buffer=r.copy_cpu_to_vram(dst,preserve_scaled);
            auto mapped=r.begin_copy(buffer);
            if (!mapped) throw std::runtime_error("Presentation upload mapping failed");
            std::copy(pixels.begin(),pixels.end(),mapped);
            r.end_copy(buffer);
            break;
        }
        }
    }
};

class PresentationHistory {
    std::unique_ptr<PSX::Renderer> construction_base, display_base, temporal;
    std::vector<PresentationCommand> construction, display;
    std::vector<GeometryTriangle> geometry;
    std::vector<size_t> geometry_commands;
    GeometryPairs pairs;
    bool initialized=false, complete=false;
    bool overflow=false;
    size_t owned_bytes=0;
    unsigned display_x=0, display_y=0;
    bool fixed_background=false;
    bool background_changed=false;
    std::vector<uint32_t> background;
    std::vector<GeometryPosition> background_positions;
public:
    uint64_t swap_frame=0, interval=0;
    PresentationHistory(Vulkan::Device &device,unsigned scale,unsigned msaa,bool fixed=false):fixed_background(fixed) {
        for (auto ptr:{&construction_base,&display_base,&temporal}) {
            ptr->reset(new PSX::Renderer(device,scale,msaa,nullptr));
            (*ptr)->set_adaptive_smoothing(false); (*ptr)->set_track_textures(false);
            (*ptr)->set_dump_textures(false); (*ptr)->set_replace_textures(false);
        }
    }
    bool needs_boundary(unsigned x,unsigned y) const {
        return !initialized || display_x!=x || display_y!=y;
    }
    void boundary(PSX::Renderer &native,unsigned x,unsigned y,unsigned width,unsigned height,uint64_t frame) {
        if (initialized && x==display_x && y==display_y) return;
        if (initialized) {
            display=std::move(construction);
            construction.clear();
            std::vector<GeometryTriangle> next;
            std::vector<uint32_t> next_background;
            std::vector<GeometryPosition> next_background_positions;
            geometry_commands.clear();
            for (size_t i=0;i<display.size();++i) {
                const auto &command=display[i];
                if (fixed_background && command.kind==PresentationCommand::Quad &&
                    command.state.render.draw_rect.intersects({x,y,width,height})) {
                    const auto &s=command.state.render;
                    const auto clip=s.draw_rect.scissor({x,y,width,height});
                    float left=command.vertices[0].x,right=left,top=command.vertices[0].y,bottom=top;
                    for (const auto &v:command.vertices) {
                        left=std::min(left,v.x); right=std::max(right,v.x);
                        top=std::min(top,v.y); bottom=std::max(bottom,v.y);
                    }
                    if (right<=x || left>=x+width || bottom<=y || top>=y+height) continue;
                    for (const auto &v:command.vertices) {
                        next_background_positions.push_back({v.x-float(x),v.y-float(y),v.w});
                        uint32_t bits; std::memcpy(&bits,&v.w,sizeof(bits)); next_background.push_back(bits);
                        next_background.insert(next_background.end(),{v.color,uint32_t(v.u)|(uint32_t(v.v)<<16)});
                    }
                    next_background.insert(next_background.end(),{
                        unsigned(s.texture_mode),s.texture_offset_x,s.texture_offset_y,
                        s.palette_offset_x,s.palette_offset_y,unsigned(command.palette),unsigned(command.palette>>32),
                        unsigned(s.semi_transparent),unsigned(s.mask_test),unsigned(s.force_mask_bit),
                        unsigned(s.texture_color_modulate),unsigned(s.dither_native_resolution),
                        clip.x-x,clip.y-y,clip.width,clip.height,
                        unsigned(s.texture_window.mask_x)|(unsigned(s.texture_window.mask_y)<<8)|
                        (unsigned(s.texture_window.or_x)<<16)|(unsigned(s.texture_window.or_y)<<24)});
                    next_background.push_back(unsigned(command.pixels.size()));
                    next_background.insert(next_background.end(),command.pixels.begin(),command.pixels.end());
                }
                if (command.kind!=PresentationCommand::Triangle ||
                    !command.state.render.draw_rect.intersects({x,y,width,height})) continue;
                GeometryTriangle t;
                t.corners=command.corners; t.eligible=command.geometry; t.palette=command.palette;
                const auto &s=command.state.render;
                t.surface={{unsigned(s.texture_mode),s.texture_offset_x,s.texture_offset_y,
                            s.palette_offset_x,s.palette_offset_y,
                            unsigned(s.texture_window.mask_x) | (unsigned(s.texture_window.mask_y)<<8) |
                            (unsigned(s.texture_window.or_x)<<16) | (unsigned(s.texture_window.or_y)<<24),
                            unsigned(s.mask_test),unsigned(s.force_mask_bit)}};
                for (unsigned v=0;v<3;++v) {
                    const auto &vertex=command.vertices[v];
                    t.positions[v]={vertex.x-float(x),vertex.y-float(y),vertex.w};
                    t.uv[v]=unsigned(vertex.u) | (unsigned(vertex.v)<<16);
                }
                next.push_back(t); geometry_commands.push_back(i);
            }
            background_changed=fixed_background && (next_background.empty() || background!=next_background ||
                                                      background_positions.size()!=next_background_positions.size());
            if (fixed_background && !background_changed) {
                float dx=next_background_positions[0].x-background_positions[0].x;
                float dy=next_background_positions[0].y-background_positions[0].y;
                background_changed=std::abs(dx)>16 || std::abs(dy)>16;
                for (size_t i=0;i<background_positions.size();++i)
                    if (next_background_positions[i].x-background_positions[i].x!=dx ||
                        next_background_positions[i].y-background_positions[i].y!=dy) background_changed=true;
                // Sprites and masks stay current. Express the old actor in the
                // current background's coordinates before interpolating motion.
                if (!background_changed) for (auto &t:geometry) for (auto &p:t.positions) { p.x+=dx; p.y+=dy; }
            }
            pairs=pair_geometry(geometry,next,!fixed_background); geometry=std::move(next);
            if (background_changed) {
                pairs.cut=true; pairs.matched=0;
                for (auto &pair:pairs.triangles) pair.interpolate=false;
            }
            background=std::move(next_background);
            background_positions=std::move(next_background_positions);
            display_base.swap(construction_base);
            interval=frame-swap_frame; swap_frame=frame; complete=!overflow;
            overflow=false; owned_bytes=0;
        }
        construction_base->host_copy_from(native);
        initialized=true; display_x=x; display_y=y;
    }
    void record(PresentationCommand command) {
        if (!initialized || overflow) return;
        owned_bytes+=sizeof(command)+command.pixels.size()*sizeof(uint16_t);
        if (owned_bytes>64*1024*1024) {
            construction.clear(); complete=false; overflow=true;
            return;
        }
        construction.push_back(std::move(command));
    }
    void exclude_effect() {
        // Feedback and full-screen fades use current native pixels. Discard
        // correspondence too, so the first image after the effect cannot blend
        // with geometry captured before it.
        complete=false; geometry.clear(); pairs={};
    }
    PSX::Renderer *replay(uint64_t frame,bool interpolate) {
        if (!complete) return nullptr;
        if (interpolate && (!pairs.matched || interval<2 || interval>6 || frame-swap_frame>=interval)) return nullptr;
        temporal->host_copy_from(*display_base);
        bool eligible=interpolate && interval>=2 && interval<=6 && frame-swap_frame<=6;
        float alpha=eligible ? std::min(1.0f,float(frame-swap_frame)/float(interval)) : 1;
        size_t polygon=0;
        for (size_t i=0;i<display.size();++i) {
            const GeometryPair *pair=nullptr;
            if (polygon<geometry_commands.size() && geometry_commands[polygon]==i) {
                if (eligible) pair=&pairs.triangles[polygon];
                ++polygon;
            }
            display[i].draw(*temporal,pair,alpha,float(display_x),float(display_y));
        }
        // Completion belongs to the current image, but never to its geometry
        // identity list. This includes current HUD writes and next-buffer work.
        for (const auto &command:construction) command.draw(*temporal);
        return temporal.get();
    }
    unsigned matched() const { return pairs.matched; }
    bool fixed_field() const { return fixed_background; }
    bool background_reset() const { return background_changed; }
    unsigned polygons() const { return unsigned(geometry.size()); }
    bool cut() const { return pairs.cut; }
    const GeometryPairs &diagnostics() const { return pairs; }
    float fraction(uint64_t frame) const {
        return interval>=2 && interval<=6 ? std::min(1.0f,float(frame-swap_frame)/float(interval)) : 1;
    }
    void abandon() {
        construction_base->abandon(); display_base->abandon(); temporal->abandon();
    }
};
}
#endif
