// TV geometry from GP1 timing: fixed line pitch, borders, and clipping.
#include "lh_test.h"
#include <array>
namespace {
struct Rect { int x,y,w,h; };
struct View { Rect frame,content; };
struct Timing { uint32_t horizontal,vertical,mode; };
Timing timing(unsigned mode,unsigned x1,unsigned x2,unsigned y1,unsigned y2) {
    return {x1|(x2<<12),y1|(y2<<10),mode};
}
void rect(Rect a, Rect b) { LH_EQ(a.x,b.x); LH_EQ(a.y,b.y); LH_EQ(a.w,b.w); LH_EQ(a.h,b.h); }
auto viewport(lh::Case &t) { return t.library("viewport").fn<View(Timing,int,int,int,int,int)>("host_viewport"); }
struct Mode { int mode,width,x1,x2,ax,aw,ix,iw; };
struct Height { int height,interlace,y1,y2,ay,ah,iy,ih; };
struct Row { std::string id; Mode m; Height h; };
std::vector<Row> rows() {
    std::vector<Row> out;
    const Mode modes[]={{0,256,590,3150,240,1440,320,1280},{1,320,600,3160,240,1440,320,1280},
        {64,368,595,3171,270,1380,347,1226},{2,512,615,3175,240,1440,320,1280},{3,640,620,3180,240,1440,320,1280}};
    const Height heights[]={{224,0,24,248,36,1008,92,896},{240,0,16,256,0,1080,60,960},
        {448,36,24,248,36,1008,92,896},{480,36,16,256,0,1080,60,960}};
    for (auto h:heights) for (auto m:modes) {
        std::string id;
        for (int n:{h.height,h.interlace,h.y1,h.y2,h.ay,h.ah,h.iy,h.ih,m.mode,m.width,m.x1,m.x2,m.ax,m.aw,m.ix,m.iw}) {
            if (!id.empty()) id+='-';
            id+=std::to_string(n);
        }
        out.push_back({id,m,h});
    }
    return out;
}
}
LH_TEST_P(test_tv_geometry, rows()) {
    auto v=viewport(t); auto m=p.m; auto h=p.h;
    auto s=timing(m.mode|h.interlace,m.x1,m.x2,h.y1,h.y2);
    auto aspect=v(s,m.width,h.height,1920,1080,0);
    rect(aspect.frame,{240,0,1440,1080}); rect(aspect.content,{m.ax,h.ay,m.aw,h.ah});
    auto integer=v(s,m.width,h.height,1920,1080,1);
    rect(integer.frame,{320,60,1280,960}); rect(integer.content,{m.ix,h.iy,m.iw,h.ih});
    LH_EQ(integer.content.h%h.height,0);
    rect(v(s,m.width,h.height,1920,1080,2).content,{0,0,1920,1080});
}
LH_TEST(test_narrow_and_shifted_ranges) {
    auto v=viewport(t);
    rect(v(timing(1,760,3000,24,248),280,224,1920,1080,0).content,{330,36,1260,1008});
    for (auto pair : {std::array<int,2>{680,285},std::array<int,2>{520,195}}) {
        auto a=v(timing(1,pair[0],pair[0]+2560,16,256),320,240,1920,1080,0);
        rect(a.frame,{240,0,1440,1080}); rect(a.content,{pair[1],0,1440,1080});
    }
    auto a=v(timing(64,539,3227,16,256),384,240,1920,1080,0);
    rect(a.content,a.frame); rect(a.frame,{240,0,1440,1080});
}
LH_TEST(test_short_range_retains_tv_line_pitch) {
    auto v=viewport(t);
    auto a=v(timing(1,600,3160,24,248),320,224,1920,1080,0);
    auto b=v(timing(1,600,3160,16,256),320,240,1920,1080,0);
    LH_EQ(a.content.h/224.0,4.5); LH_EQ(b.content.h/240.0,4.5);
}
LH_TEST(test_crop_preserves_pixel_aspect) {
    auto v=viewport(t)(timing(1,600,3160,24,248),320,224,1920,1080,3);
    rect(v.frame,v.content); rect(v.content,{189,0,1542,1080});
}
LH_TEST(test_pal_and_small_window) {
    auto v=viewport(t); auto s=timing(9,624,3184,35,291);
    rect(v(s,320,256,1920,1080,0).content,{240,60,1440,960});
    rect(v(s,320,256,1920,1080,1).content,{384,156,1152,768});
    s=timing(37,600,3160,24,248);
    auto a=v(s,320,448,200,100,1);
    rect(a.frame,{33,0,133,100}); rect(a.content,{33,3,133,94});
    rect(v(s,0,448,200,100,0).content,{0,0,0,0});
}
