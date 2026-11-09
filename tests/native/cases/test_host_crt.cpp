// CRT presentation presets: options, shaders, and synthetic patterns on Vulkan.
#include "presentation.h"
#include "host_fixtures.h"
#include "equiv.h"
static const char *shaders[]={"crt_fullscreen.vert","crt_lines.frag","crt_beam.frag","crt_glow.frag","crt_final.frag"};
LH_TEST_P(test_prebuilt_shader_matches_source_and_validates,shaders) {lh::shader_contract(t,p);}
static lh::Command options(lh::Case &t) {
    return t.harness("options").arg("unused.exe").env("FF8_TEST_CRT","1")
        .env("HOME",t.temp().string()).env("XDG_CONFIG_HOME",(t.temp()/"xdg").string());
}
LH_TEST(test_crt_option_parsing_and_classification) {
    auto run=t.ok(options(t).args({"--config","/dev/null"}));auto lines=lh::lines(run.out);
    LH_EQ(lines.back(),"crt=0 present=1920x1080 presented=");
    for(auto key:{"crt","dump-presented-frames","present-size"}) LH_CONTAINS(run.out,std::string(key)+" P\n");
    int index=0;
    for(auto name:{"off","pvm","tv","arcade"}) {
        run=t.ok(options(t).args({"--crt",name,"--present-size","3840x2160","--dump-presented-frames","/tmp/x","--config","/dev/null"}));
        LH_EQ(lh::lines(run.out).back(),"crt="+std::to_string(index++)+" present=3840x2160 presented=/tmp/x");
    }
    auto ini=t.temp()/"crt.ini";lh::write_file(ini,"crt=tv\npresent-size=1280x960\n");
    run=t.ok(t.harness("options").args({"unused.exe","--config",ini}).env("FF8_TEST_CRT","1"));
    LH_EQ(lh::lines(run.out).back(),"crt=2 present=1280x960 presented=");
    run=t.run(t.harness("options").args({"unused.exe","--config",ini,"--crt","off"}).env("FF8_TEST_CRT","1"));
    LH_CHECK(lh::lines(run.out).back().rfind("crt=0 ",0)==0);
}
struct Invalid {const char *id;std::vector<std::string> args;};
static const Invalid invalid[]={{"args0",{"--crt"}},{"args1",{"--crt","lcd"}},{"args2",{"--crt","PVM"}},
    {"args3",{"--present-size","1920"}},{"args4",{"--present-size","8x8"}},{"args5",{"--present-size","1920x1080x2"}},{"args6",{"--present-size","9000x100"}}};
LH_TEST_P(test_crt_invalid_settings,invalid) {auto command=options(t);for(const auto &arg:p.args) command.arg(arg);LH_EQ(t.run(command.args({"--config","/dev/null"})).rc,2);}
LH_TEST(test_off_presentation_is_the_nearest_blit) {
    auto tool=lh::crt_preview(t);auto view=lh::viewport(t,1280,960);
    LH_CHECK(lh::same(view.frame,{0,0,1280,960}));LH_CHECK(lh::same(view.content,{0,0,1280,960}));
    std::string pattern;for(int y=0;y<240;++y) for(int x=0;x<320;++x) {pattern+=char(x*7);pattern+=char(y*3);pattern+=char((x^y)*5);}
    auto out=lh::present(t,tool,pattern,1280,960,"off");std::string expected;
    for(int y=0;y<960;++y) for(int x=0;x<1280;++x) expected.append(pattern,(size_t(y/4)*320+x/4)*3,3);
    LH_BYTES_EQ(out.rgb,expected);
    view=lh::viewport(t,1920,1080);LH_CHECK(lh::same(view.frame,{240,0,1440,1080}));LH_CHECK(lh::same(view.content,view.frame));
    out=lh::present(t,tool,pattern,1920,1080,"off");
    LH_CHECK(lh::oracle::black_outside(out,view.frame));
    for(int y=0;y<1080;++y) {bool lit=false;for(int x=240;x<1680;++x) for(int c=0;c<3;++c) lit=lit || lh::oracle::channel(out,x,y,c);LH_CHECK(lit);}
}
struct Preset {std::string id,preset;int width,height;};
static std::vector<Preset> presets() {
    std::vector<Preset> result;int i=0;
    for(auto size:{std::pair<int,int>{1920,1080},{1280,960},{3840,2160}}) {
        for(auto preset:{"pvm","tv","arcade"}) result.push_back({"size"+std::to_string(i)+"-"+preset,preset,size.first,size.second});++i;
    }return result;
}
static int profile_cycles(lh::Case &t,const lh::oracle::PngImage &image,lh::Rect rect) {
    auto profile=lh::oracle::row_profile(image,rect);auto cycles=lh::oracle::dominant_cycles(profile);
    lh::equiv::record(t,int32_t(image.width),int32_t(image.height),rect.x,rect.y,rect.w,rect.h,image.rgb,profile,cycles);return cycles;
}
LH_TEST_P(test_preset_scanlines_borders_brightness_and_determinism,presets()) {
    auto tool=lh::crt_preview(t);auto view=lh::viewport(t,p.width,p.height);const auto c=view.content;
    const std::string grey(320*240*3,char(128));auto out=lh::present(t,tool,grey,p.width,p.height,p.preset);
    LH_BYTES_EQ(out.rgb,lh::present(t,tool,grey,p.width,p.height,p.preset).rgb);
    auto reference=lh::present(t,tool,grey,p.width,p.height,"off");
    lh::Rect inner{c.x+c.w/4,c.y+c.h/8,c.w/2,c.h-c.h/4};int cycles=profile_cycles(t,out,inner);
    LH_CHECK_MSG(std::abs(cycles/(double(inner.h)/c.h)-240)<=1,"scanline cycles="+std::to_string(cycles));
    LH_CHECK(lh::oracle::black_outside(out,view.frame));lh::Rect centre{c.x+c.w/4,c.y+c.h/4,c.w/2,c.h/2};
    const double mean=lh::oracle::mean_linear(out,centre),refmean=lh::oracle::mean_linear(reference,centre),ratio=mean/refmean;
    lh::equiv::record(t,centre.x,centre.y,centre.w,centre.h,reference.rgb,mean,refmean);
    LH_CHECK_MSG(std::abs(ratio-1)<=.08,"linear brightness ratio="+std::to_string(ratio));
}
LH_TEST(test_presets_draw_inside_the_scanout_rectangle) {
    auto tool=lh::crt_preview(t);lh::Timing timing{lh::ntsc.horizontal,40u|(232u<<10),1};auto view=lh::viewport(t,1920,1080,timing,320,192);
    LH_CHECK(lh::same(view.frame,{240,0,1440,1080}));LH_CHECK(view.content.y>view.frame.y);LH_CHECK(view.content.y+view.content.h<view.frame.y+view.frame.h);
    for(auto preset:{"pvm","tv","arcade"}) {
        auto out=lh::present(t,tool,std::string(320*192*3,char(200)),1920,1080,preset,timing,320,192);
        std::vector<int> lit;for(int y=0;y<1080;++y) {bool on=false;for(int x=0;x<1920;++x) for(int c=0;c<3;++c) on=on || lh::oracle::channel(out,x,y,c);if(on) lit.push_back(y);}
        LH_CHECK(!lit.empty());LH_CHECK(lit.front()>=view.content.y-1);LH_CHECK(lit.back()<view.content.y+view.content.h+1);
    }
}
LH_TEST(test_interlaced_scanout_uses_thinner_beams) {
    auto tool=lh::crt_preview(t);lh::Timing timing{lh::ntsc.horizontal,lh::ntsc.vertical,1|4|32};auto view=lh::viewport(t,1920,1080,timing,320,480);
    LH_CHECK(lh::same(view.content,view.frame));auto c=view.content;
    auto out=lh::present(t,tool,std::string(320*480*3,char(128)),1920,1080,"pvm",timing,320,480);
    lh::Rect inner{c.x+c.w/4,c.y+c.h/8,c.w/2,c.h-c.h/4};int cycles=profile_cycles(t,out,inner);
    LH_CHECK_MSG(std::abs(cycles/(double(inner.h)/c.h)-480)<=2,"interlaced scanline cycles="+std::to_string(cycles));
}
