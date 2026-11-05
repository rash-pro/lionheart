#include "frame_geometry.hpp"
#include "frame_scene.h"
#include <cassert>
using namespace HostGPU;
static GeometryTriangle triangle(unsigned first=1,float x=0) {
    GeometryTriangle t;
    t.eligible=true;
    for (unsigned i=0;i<3;++i) {
        t.corners[i].vertex=first+i; t.corners[i].context=8;
        t.corners[i].input_xy=first+i; t.corners[i].input_z=100;
        t.positions[i]={x+float(first+i),float(i==2?10:0),100};
    }
    return t;
}
int main() {
    assert(frame_scene(3,0)==FRAME_SCENE_BATTLE);
    assert(frame_scene(1,227)==FRAME_SCENE_FIXED_FIELD);
    assert(frame_scene(1,226)==FRAME_SCENE_NONE && frame_scene(1,228)==FRAME_SCENE_NONE);
    assert(frame_scene(2,227)==FRAME_SCENE_NONE && frame_scene(6,227)==FRAME_SCENE_NONE);
    auto a=triangle(),b=triangle(1,8);
    auto p=pair_geometry({a},{b});
    assert(p.matched==1 && !p.cut);
    auto half=interpolate_position(p.triangles[0].before[0],b.positions[0],.25f);
    assert(half.x==3 && half.w==100);
    auto reject=[&](GeometryTriangle bad) { assert(pair_geometry({a},{bad}).matched==0); };
    auto bad=b; bad.corners[0].vertex=0; reject(bad);
    bad=b; bad.corners[0].context=9; reject(bad);
    bad=b; bad.corners[0].input_xy++; reject(bad);
    bad=b; bad.corners[0].generation++; reject(bad);
    bad=b; bad.corners[0].collection++; reject(bad);
    bad=b; bad.eligible=false; reject(bad);
    bad=b; bad.uv[1]++; reject(bad);
    bad=b; bad.palette++; reject(bad);
    bad=b; bad.surface[1]++; reject(bad);
    // Rejected faces stay current; adjacent accepted faces hold shared edges.
    auto adjacent=b; adjacent.corners[2].vertex=99;
    auto edge=pair_geometry({a},{b,adjacent});
    assert(edge.matched==1 && !edge.triangles[1].interpolate);
    assert(edge.triangles[0].hold[0] && edge.triangles[0].hold[1] && !edge.triangles[0].hold[2]);
    adjacent.corners[0]={}; // Missing identity must not open the shared endpoint.
    adjacent.positions[2].x+=10;
    edge=pair_geometry({a},{b,adjacent});
    assert(edge.matched==1 && !edge.triangles[1].interpolate);
    assert(edge.triangles[0].hold[0] && edge.triangles[0].hold[1] && !edge.triangles[0].hold[2]);
    bad=b; bad.positions[0].x++;
    auto alias=pair_geometry({a},{b,bad});
    assert(alias.triangles[0].hold[0] && alias.triangles[1].hold[0]);
    auto duplicate=a; duplicate.positions[0].x++;
    assert(pair_geometry({a,duplicate},{b}).matched==0);
    std::vector<GeometryTriangle> old,now;
    for (unsigned i=0;i<4;++i) { old.push_back(triangle(i*3+1)); now.push_back(triangle(i*3+1,80)); }
    auto cut=pair_geometry(old,now);
    assert(cut.cut && !cut.matched);
    now=old;
    for (auto &t:now) for (auto &c:t.corners) c.matrix[0]=1024;
    cut=pair_geometry(old,now);
    assert(cut.cut && !cut.matched); // A basis cut need not jump screen centers.
    assert(!pair_geometry(old,now,false).cut); // A verified static field background identifies its camera.
    now=old;
    for (auto &t:now) for (auto &p:t.positions) p.w=200;
    assert(pair_geometry(old,now).cut);
    // Culling/topology replacement cannot borrow a matching material's history.
    assert(pair_geometry({a},{triangle(100,8)}).matched==0);
}
