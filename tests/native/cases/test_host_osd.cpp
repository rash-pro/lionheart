// Presentation-only OSD: copy, cells, cached pixels, deterministic clock and composition.
#include "presentation.h"
#include "host_fixtures.h"
#include "equiv.h"
#include <set>
struct Panel {const uint8_t *rgba;int x,y,w,h,cw,ch;unsigned revision;uint8_t opacity;};
struct Snapshot {bool exists=false;Panel panel{};std::string pixels;};
class Osd {
    lh::Library lib;
public:
    explicit Osd(lh::Case &t):lib(t.library("osd")) {}
    void configure(int enabled=1,const char *script=nullptr) {lib.fn<void(int,const char *)>("host_osd_configure")(enabled,script);}
    void clock(double ms) {lib.fn<void(double)>("host_osd_clock")(ms);}
    void reset() {configure();clock(0);}
    void post(const char *label="CRT",const char *value="TV",int persistent=0) {lib.fn<void(const char *,const char *,int)>("host_osd_post")(label,value,persistent);}
    void event(unsigned event,unsigned number=0,const char *detail=nullptr) {lib.fn<void(unsigned,unsigned,const char *)>("host_osd_event")(event,number,detail);}
    void clear(unsigned event) {lib.fn<void(unsigned)>("host_osd_clear")(event);}
    void frame(int now,uint64_t frame) {lib.fn<void(int,uint64_t)>("host_osd_frame")(now,frame);}
    lh::Rect area(lh::View view) {return lib.fn<lh::Rect(lh::View)>("host_osd_area")(view);}
    Snapshot panel(int width=1920,int height=1080,lh::Rect frame={0,0,0,0}) {
        if(!frame.w && !frame.h) frame={0,0,width,height};
        const auto *p=lib.fn<const Panel *(int,int,lh::Rect)>("host_osd_panel")(width,height,frame);
        if(!p) return {};
        return {true,*p,std::string(reinterpret_cast<const char *>(p->rgba),size_t(p->w)*p->h*4)};
    }
};
static std::set<std::array<unsigned,4>> colors(const std::string &pixels) {
    std::set<std::array<unsigned,4>> result;
    for(size_t i=0;i<pixels.size();i+=4) result.insert({uint8_t(pixels[i]),uint8_t(pixels[i+1]),uint8_t(pixels[i+2]),uint8_t(pixels[i+3])});
    return result;
}
struct Area {const char *id;lh::Rect frame,content,area;};
static const Area areas[]={{"frame0-content0-area0",{240,0,1440,1080},{240,36,1440,1008},{240,36,1440,1008}},
    {"frame1-content1-area1",{240,0,1440,1080},{200,-20,1520,1120},{240,0,1440,1080}},
    {"frame2-content2-area2",{0,0,1920,1080},{0,0,0,0},{0,0,1920,1080}}};
LH_TEST_P(test_area_is_the_visible_picture,areas) {Osd osd(t);LH_CHECK(lh::same(osd.area({p.frame,p.content}),p.area));}
LH_TEST(test_font_header_matches_bdfs) {
    auto output=t.temp()/"font.h";t.ok(t.harness("font").args({"spleen",t.root(),output}));LH_FILE_EQ(output,t.root()/"backends/host/osd_font.h");
}
struct Cell {const char *id;int w,h,ch,cw;};
static const Cell cells[]={{"size0-16-8",1280,720,16,8},{"size1-24-12",1920,1080,24,12},{"size2-32-16",2560,1440,32,16},{"size3-48-24",3840,2160,48,24},{"size4-64-32",5120,2880,64,32}};
LH_TEST_P(test_cells_geometry_colors_and_cache,cells) {
    Osd osd(t);osd.reset();osd.post();osd.clock(120);auto a=osd.panel(p.w,p.h,{101,42,900,600});LH_CHECK(a.exists);
    LH_EQ(a.panel.ch,p.ch);LH_EQ(a.panel.cw,p.cw);LH_EQ(a.panel.x,119);LH_EQ(a.panel.y,60);
    // These preset cell heights make .75*height integral, including Python's ties-to-even rule.
    LH_EQ(a.panel.w,6*p.cw+2*int(std::nearbyint(.75*p.ch)));LH_EQ(a.panel.h,p.ch+2*int(.4*p.ch+.5));
    auto cs=colors(a.pixels);
    LH_CHECK(cs.count({16,16,20,199}));LH_CHECK(cs.count({165,165,175,255}));LH_CHECK(cs.count({242,242,242,255}));
    LH_EQ(uint8_t(a.pixels[3]),0);bool antialias=false;for(auto color:cs) if(color[3]>0 && color[3]<199) antialias=true;LH_CHECK(antialias);
    osd.clock(200);auto b=osd.panel(p.w,p.h);LH_CHECK(b.exists);LH_EQ(b.panel.revision,a.panel.revision);LH_BYTES_EQ(a.pixels,b.pixels);
    b=osd.panel(p.w+1,p.h);LH_CHECK(b.exists);LH_CHECK(b.panel.revision>a.panel.revision);
}
LH_TEST(test_utf8_latin1_and_fallback) {
    Osd osd(t);osd.reset();osd.post("éñÅ","");osd.clock(120);auto latin=osd.panel();LH_CHECK(latin.exists);LH_EQ(latin.panel.w,3*12+36);
    osd.post("???","");auto fallback=osd.panel();LH_CHECK(fallback.exists);LH_CHECK(latin.pixels!=fallback.pixels);
    osd.post("界🙂€","");auto unknown=osd.panel();LH_CHECK(unknown.exists);LH_BYTES_EQ(unknown.pixels,fallback.pixels);
    LH_CHECK(!colors(unknown.pixels).count({165,165,175,255}));
}
LH_TEST(test_clock_replacement_and_persistence) {
    Osd osd(t);osd.reset();osd.post();LH_CHECK(!osd.panel().exists);
    for(auto row:{std::pair<int,int>{60,128},{120,255},{1919,255},{1920,255},{2070,128}}) {osd.clock(row.first);auto s=osd.panel();LH_CHECK(s.exists);LH_EQ(unsigned(s.panel.opacity),unsigned(row.second));}
    osd.post("State saved","slot 3");LH_EQ(unsigned(osd.panel().panel.opacity),255u);
    osd.clock(2070+1920);LH_EQ(unsigned(osd.panel().panel.opacity),255u);osd.clock(2070+2220);LH_CHECK(!osd.panel().exists);
    osd.post("Saving","slot 3...",1);osd.clock(100000);LH_EQ(unsigned(osd.panel().panel.opacity),255u);
    osd.event(13);LH_EQ(unsigned(osd.panel().panel.opacity),255u);osd.clear(3);LH_CHECK(osd.panel().exists);osd.clear(13);LH_CHECK(!osd.panel().exists);
    osd.configure(0);osd.post();osd.clock(120);LH_CHECK(!osd.panel().exists);
}
LH_TEST(test_deterministic_diagnostic) {
    Osd osd(t);osd.configure(1,"3:CRT:TV\n100:State saved:slot 3\n");
    osd.frame(0,2);LH_CHECK(!osd.panel().exists);osd.frame(0,3);LH_CHECK(!osd.panel().exists);osd.frame(0,7);
    auto s=osd.panel();LH_CHECK(s.exists);LH_CHECK(s.panel.opacity>=140 && s.panel.opacity<=143);
    for(auto frame:{11,100}) {osd.frame(0,frame);LH_EQ(unsigned(osd.panel().panel.opacity),255u);}
    osd.frame(0,234);LH_CHECK(!osd.panel().exists);
}
struct Copy {const char *id;unsigned event,number;const char *detail,*label,*value;};
static const Copy copies[]={
    {"0-0-None-CRT-Off", 0, 0, nullptr, "CRT", "Off"},
    {"0-1-None-CRT-PVM", 0, 1, nullptr, "CRT", "PVM"},
    {"0-2-None-CRT-TV", 0, 2, nullptr, "CRT", "TV"},
    {"0-3-None-CRT-Arcade", 0, 3, nullptr, "CRT", "Arcade"},
    {"1-0-None-CRT-needs the Vulkan renderer", 1, 0, nullptr, "CRT", "needs the Vulkan renderer"},
    {"2-3-None-State slot-3", 2, 3, nullptr, "State slot", "3"},
    {"2-3-empty-State slot-3 (empty)", 2, 3, "empty", "State slot", "3 (empty)"},
    {"3-3-None-Saving-slot 3...", 3, 3, nullptr, "Saving", "slot 3..."},
    {"4-3-None-State saved-slot 3", 4, 3, nullptr, "State saved", "slot 3"},
    {"5-3-timed out-Save failed-slot 3: timed out", 5, 3, "timed out", "Save failed", "slot 3: timed out"},
    {"6-3-None-Loading-slot 3...", 6, 3, nullptr, "Loading", "slot 3..."},
    {"7-3-None-State loaded-slot 3", 7, 3, nullptr, "State loaded", "slot 3"},
    {"8-3-None-No state-in slot 3", 8, 3, nullptr, "No state", "in slot 3"},
    {"9-3-None-Load failed-slot 3", 9, 3, nullptr, "Load failed", "slot 3"},
    {"10-0-None-Memory card-changed: reopen the save menu", 10, 0, nullptr, "Memory card", "changed: reopen the save menu"},
    {"11-0-None-RA strict-loading states is off", 11, 0, nullptr, "RA strict", "loading states is off"},
    {"12-0-None-RA strict-fast-forward is off", 12, 0, nullptr, "RA strict", "fast-forward is off"},
    {"13-0-None-Fast forward-", 13, 0, nullptr, "Fast forward", ""},
    {"14-2-None-Disc-2", 14, 2, nullptr, "Disc", "2"},
    {"15-0-Pad \\xc3\\xa9-Controller-connected: Pad \\xe9", 15, 0, "Pad é", "Controller", "connected: Pad é"},
    {"16-0-None-Controller-disconnected", 16, 0, nullptr, "Controller", "disconnected"},
    {"17-0-None-Save states-not available", 17, 0, nullptr, "Save states", "not available"},
    {"18-0-None-Screenshot-saved", 18, 0, nullptr, "Screenshot", "saved"},
    {"19-0-cannot write-Screenshot failed-cannot write", 19, 0, "cannot write", "Screenshot failed", "cannot write"},
};
LH_TEST_P(test_event_copy,copies) {
    Osd osd(t);osd.reset();osd.event(p.event,p.number,p.detail);osd.clock(120);const auto actual=osd.panel();
    osd.post(p.label,p.value);const auto expected=osd.panel();LH_EQ(actual.exists,expected.exists);LH_BYTES_EQ(actual.pixels,expected.pixels);
}
static lh::Command options(lh::Case &t) {return t.harness("options").args({"unused","--config","/dev/null"}).env("FF8_TEST_OSD","1");}
static void ends(const std::string &text,const std::string &suffix) {LH_CHECK_MSG(text.size()>=suffix.size() && text.compare(text.size()-suffix.size(),suffix.size(),suffix)==0,text);}
LH_TEST(test_osd_options) {
    auto run=t.ok(options(t));ends(run.out,"osd=1 tests=");LH_CONTAINS(run.out,"osd P\n");LH_CONTAINS(run.out,"osd-test P\n");
    ends(t.ok(options(t).args({"--osd","0","--osd-test","3:CRT:TV","--osd-test","20:Fast forward"})).out,"osd=0 tests=3:CRT:TV\n20:Fast forward\n");
    for(auto args:{std::vector<std::string>{"--osd"},{"--osd","2"},{"--osd-test","x:X"},{"--osd-test","-1:X"},
        {"--osd-test","1:"},{"--osd-test","1::X"},{"--osd-test","18446744073709551616:X"}}) {
        auto command=options(t);for(const auto &arg:args) command.arg(arg);LH_EQ(t.run(command).rc,2);
    }
    auto ini=t.temp()/"ff8.ini";lh::write_file(ini,"osd=0\nosd-test=2:CRT:TV\n");
    ends(t.run(options(t).args({"--config",ini})).out,"osd=0 tests=2:CRT:TV\n");
}
struct Composition {const char *id;int w,h,age;};
static const Composition compositions[]={{"size0-120",1280,720,120},{"size1-60",1920,1080,60},{"size2-120",1920,1080,120},
    {"size3-2070",1920,1080,2070},{"size4-120",2560,1440,120},{"size5-120",3840,2160,120}};
LH_TEST_P(test_soft_vulkan_composition,compositions) {
    auto tool=lh::crt_preview(t),src=t.temp()/"in.rgb",vk=t.temp()/"vk.rgb",soft=t.temp()/"soft.rgb";
    lh::write_file(src,std::string(320*240*3,char(132)));const char *label="Controller",*value="connected: Pad é";
    t.ok(lh::dummy(t.host(tool.string()).args({src,"320","240","--scale","1","--native","320","240","--timing",lh::ntsc.horizontal,lh::ntsc.vertical,lh::ntsc.mode,
        "--size",std::to_string(p.w)+"x"+std::to_string(p.h),"--crt","off","--out",vk,"--filter","nearest","--osd-label",label,"--osd-value",value,"--osd-age",p.age})));
    t.ok(lh::dummy(t.harness("soft").args({p.w,p.h,soft,label,value,p.age})).env("SDL_RENDER_DRIVER","software"));
    const auto a=lh::read_file(vk),b=lh::read_file(soft);const int difference=lh::oracle::max_difference(a,b);
    lh::equiv::record(t,int32_t(p.w),int32_t(p.h),a,b,difference);
    LH_CHECK_MSG(difference<=2,"maximum RGB difference="+std::to_string(difference));
}
LH_TEST(test_osd_shader_matches_prebuilt) {lh::shader_contract(t,"osd.frag");}
LH_TEST(test_sdl_osd_event_hooks) {t.ok(lh::dummy(t.harness("input")).timeout(30));}
LH_TEST(test_vulkan_sdl_osd_events) {
    lh::crt_preview(t);t.ok(lh::dummy(t.host(lh::host_contract(t,"osd_input",false).string()).arg("vulkan")).timeout(30));
}
