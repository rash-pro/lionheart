// Count new clear-color holes, excluding one native pixel at existing edges.
#pragma once
#include "../../tests/native/oracles/image.h"
#include "json.h"
namespace quality {
using lh::oracle::PngImage;
struct Counts { int64_t raw = 0, interior = 0, roi = 0; };
inline Counts compare(const PngImage &off, const PngImage &on, const std::string &scene) {
    if (off.width != on.width || off.height != on.height) throw std::runtime_error("scanout dimensions differ");
    int x0, y0, x1, y1;
    if (scene == "battle") { x0=8; y0=80; x1=312; y1=172; }
    else if (scene == "world") { x0=8; y0=45; x1=312; y1=205; }
    else if (scene == "dorm") { x0=65; y0=45; x1=145; y1=205; }
    else throw std::runtime_error("unknown scene: " + scene);
    const int w=int(off.width), h=int(off.height), scale=w/320, side=2*scale+1;
    std::vector<int64_t> sums(size_t(w+1)*(h+1));
    auto at = [&](int x, int y) -> int64_t & { return sums[size_t(y)*(w+1)+x]; };
    auto opaque = [&](const PngImage &image, int x, int y, int threshold) {
        return std::max({lh::oracle::channel(image,x,y,0),lh::oracle::channel(image,x,y,1),lh::oracle::channel(image,x,y,2)}) > threshold;
    };
    for (int y=0; y<h; ++y) for (int x=0; x<w; ++x)
        at(x+1,y+1)=int(opaque(off,x,y,8))+at(x,y+1)+at(x+1,y)-at(x,y);
    Counts result;
    for (int y=0; y<h; ++y) for (int x=0; x<w; ++x) {
        if (x<x0*scale || x>=x1*scale || y<y0*scale || y>=y1*scale || (scene=="world" && y>=132*scale && x>=220*scale)) continue;
        ++result.roi;
        if (!opaque(off,x,y,8) || opaque(on,x,y,2)) continue;
        ++result.raw;
        const int left=std::max(0,x-scale), right=std::min(w,x+scale+1), top=std::max(0,y-scale), bottom=std::min(h,y+scale+1);
        if (at(right,bottom)-at(left,bottom)-at(right,top)+at(left,top)==int64_t(side)*side) ++result.interior;
    }
    return result;
}
inline dt::Json fields(Counts c) {
    return dt::Json::Object{{"raw_pixels",c.raw},{"interior_pixels",c.interior},{"roi_pixels",c.roi}};
}
}
