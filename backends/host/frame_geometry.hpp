#ifndef HOST_FRAME_GEOMETRY_HPP
#define HOST_FRAME_GEOMETRY_HPP
#include "frame_tags.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace HostGPU {
using CornerKey = std::array<uint64_t,6>;
inline CornerKey corner_key(const frame_corner_t &c) {
    return {{c.vertex,c.context,c.pc,c.occurrence,c.generation,c.collection}};
}
struct GeometryPosition { float x=0,y=0,w=1; };
struct GeometryTriangle {
    std::array<frame_corner_t,3> corners{};
    std::array<GeometryPosition,3> positions{};
    std::array<uint32_t,3> uv{};
    std::array<uint32_t,8> surface{};
    uint64_t palette=0;
    bool eligible=false;
};
struct GeometryPair {
    std::array<GeometryPosition,3> before{};
    bool interpolate=false;
    std::array<bool,3> hold{};
};
struct GeometryPairs {
    std::vector<GeometryPair> triangles;
    bool cut=false;
    unsigned matched=0;
    unsigned eligible=0, missing=0, changed_surface=0, changed_input=0, ambiguous=0, connected=0;
};
inline bool same_position(const GeometryPosition &a,const GeometryPosition &b) {
    return a.x==b.x && a.y==b.y && a.w==b.w;
}
inline bool same_input(const frame_corner_t &a,const frame_corner_t &b) {
    return a.input_xy==b.input_xy && a.input_z==b.input_z;
}
inline bool transform_jump(const frame_corner_t &a,const frame_corner_t &b) {
    // RT is signed Q12. A discontinuous basis can cut between views whose
    // projected centers happen to be nearby. Fast continuous turns also hold N.
    for (unsigned i=0;i<9;++i) {
        int before=int16_t(a.matrix[i/2]>>((i%2)*16));
        int after=int16_t(b.matrix[i/2]>>((i%2)*16));
        if (std::abs(before-after)>512) return true;
    }
    return false;
}

// Identities select correspondences. Texture state and UVs can only reject one.
// A rejected face stays at N. Its shared vertices also stay at N in adjacent
// accepted faces, so every copy of an edge has identical endpoint positions.
inline GeometryPairs pair_geometry(const std::vector<GeometryTriangle> &previous,
                                   const std::vector<GeometryTriangle> &current,bool check_basis=true) {
    using TriangleKey=std::array<CornerKey,3>;
    struct OldCorner { frame_corner_t tag; GeometryPosition position; bool ambiguous=false; };
    std::map<CornerKey,OldCorner> old_corners;
    std::map<TriangleKey,const GeometryTriangle *> old_triangles;
    auto keys=[](const GeometryTriangle &t) {
        return TriangleKey{{corner_key(t.corners[0]),corner_key(t.corners[1]),corner_key(t.corners[2])}};
    };
    for (const auto &t:previous) {
        if (!t.eligible) continue;
        auto inserted=old_triangles.emplace(keys(t),&t);
        if (!inserted.second && inserted.first->second &&
            (inserted.first->second->uv!=t.uv || inserted.first->second->surface!=t.surface))
            inserted.first->second=nullptr;
        for (unsigned i=0;i<3;++i) {
            const auto &tag=t.corners[i];
            auto entry=old_corners.emplace(corner_key(tag),OldCorner{tag,t.positions[i],false});
            if (!entry.second && (!same_input(entry.first->second.tag,tag) ||
                                 !same_position(entry.first->second.position,t.positions[i])))
                entry.first->second.ambiguous=true;
        }
    }
    GeometryPairs result;
    result.triangles.resize(current.size());
    std::set<CornerKey> held;
    std::set<std::array<float,3>> unknown_endpoints;
    std::map<CornerKey,OldCorner> new_corners;
    for (const auto &t:current) for (unsigned i=0;i<3;++i) {
        const auto &tag=t.corners[i];
        if (!tag.vertex || !tag.context) continue;
        auto entry=new_corners.emplace(corner_key(tag),OldCorner{tag,t.positions[i],false});
        if (!entry.second && (!same_input(entry.first->second.tag,tag) ||
                             !same_position(entry.first->second.position,t.positions[i])))
            held.insert(corner_key(tag));
    }
    unsigned shared=0, jumps=0;
    for (unsigned n=0;n<current.size();++n) {
        const auto &t=current[n];
        auto old=old_triangles.find(keys(t));
        bool valid=t.eligible && old!=old_triangles.end() && old->second &&
            old->second->uv==t.uv && old->second->surface==t.surface && old->second->palette==t.palette;
        if (t.eligible) {
            ++result.eligible;
            if (old==old_triangles.end() || !old->second) ++result.missing;
            else if (!valid) ++result.changed_surface;
        }
        for (unsigned i=0;i<3;++i) {
            auto corner=old_corners.find(corner_key(t.corners[i]));
            if (corner==old_corners.end()) { valid=false; continue; }
            if (corner->second.ambiguous || !same_input(corner->second.tag,t.corners[i])) {
                if (corner->second.ambiguous) ++result.ambiguous; else ++result.changed_input;
                valid=false; continue;
            }
            auto a=corner->second.position,b=t.positions[i];
            result.triangles[n].before[i]=a;
            if (t.eligible) {
                ++shared;
                if ((check_basis && transform_jump(corner->second.tag,t.corners[i])) ||
                    std::abs(a.x-b.x)>24 || std::abs(a.y-b.y)>24 ||
                    a.w<=0 || b.w<=0 || a.w>b.w*1.5f || b.w>a.w*1.5f) ++jumps;
            }
        }
        result.triangles[n].interpolate=valid;
        if (!valid) for (unsigned i=0;i<3;++i) {
            const auto &corner=t.corners[i];
            held.insert(corner_key(corner));
            if (!corner.vertex || !corner.context) {
                const auto &p=t.positions[i];
                unknown_endpoints.insert({p.x,p.y,p.w});
            }
        }
    }
    result.cut=shared>=12 && jumps*4>=shared;
    for (unsigned n=0;n<current.size();++n) {
        auto &pair=result.triangles[n];
        unsigned count=0;
        for (unsigned i=0;i<3;++i) {
            const auto &p=current[n].positions[i];
            // Current-space coincidence may only veto motion beside an unknown
            // endpoint. It never selects or constructs a temporal correspondence.
            pair.hold[i]=held.count(corner_key(current[n].corners[i]))!=0 ||
                         unknown_endpoints.count({p.x,p.y,p.w})!=0;
            count+=unsigned(pair.hold[i]);
        }
        if (pair.interpolate && count) ++result.connected;
        if (count==3) pair.interpolate=false;
        if (result.cut) pair.interpolate=false;
        if (pair.interpolate) ++result.matched;
    }
    return result;
}
inline GeometryPosition interpolate_position(GeometryPosition a,GeometryPosition b,float alpha) {
    alpha=std::max(0.0f,std::min(alpha,1.0f));
    return {a.x+(b.x-a.x)*alpha,a.y+(b.y-a.y)*alpha,a.w+(b.w-a.w)*alpha};
}
}
#endif
