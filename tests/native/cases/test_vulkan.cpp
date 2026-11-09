// Real Vulkan contracts using the selected ICD; no disc or display required.
#include "vulkan.h"
#include "rendering.h"
#include "host_fixtures.h"
#include "oracles/png.h"
#include <algorithm>
#include <array>
#include <cstdlib>

using lh::xy;
using lh::GpuStream;
static lh::oracle::PngImage png(const lh::fs::path &path) { return lh::oracle::read_png(lh::read_file(path)); }
static std::array<unsigned,3> pixel(const lh::oracle::PngImage &image, unsigned x, unsigned y) {
    size_t at=(size_t(y)*image.width+x)*3;
    return {uint8_t(image.rgb.at(at)),uint8_t(image.rgb.at(at+1)),uint8_t(image.rgb.at(at+2))};
}
struct Mode { std::string id; int scale, color, msaa; };
static const Mode reference_modes[]={{"1-0-1",1,0,1},{"2-0-1",2,0,1},{"3-0-1",3,0,1},{"4-0-1",4,0,1},
    {"5-0-1",5,0,1},{"6-0-1",6,0,1},{"7-0-1",7,0,1},{"8-0-1",8,0,1},{"4-1-4",4,1,4},{"1-1-1",1,1,1},{"1-0-4",1,0,4}};
LH_TEST_P(test_vulkan_reference_readbacks_and_scaled_scanout,reference_modes) {
    auto binary=lh::vulkan_replay(t), shot=t.temp()/"scanout.png", timing=t.temp()/"timing.csv";
    auto run=t.ok(t.host(binary.string()).args({"--self-test",p.scale,shot,timing,p.color,p.msaa})
        .env("SDL_VIDEODRIVER","offscreen").env("SDL_AUDIODRIVER","dummy").timeout(120));
    LH_EQ(lh::json_integer(run.out,"errors"),0); LH_EQ(lh::json_integer(run.out,"reads"),24);
    if(p.scale==4 && !p.color && p.msaa==1) LH_EQ(lh::json_integer(run.out,"shape_checks"),2);
    if(p.scale==1 && !p.color && p.msaa==1) LH_EQ(lh::json_integer(run.out,"raw_mismatch_pixels"),0);
    auto image=png(shot); LH_EQ(image.width,unsigned(320*p.scale)); LH_EQ(image.height,unsigned(240*p.scale));
    struct Sample { unsigned x,y; std::array<unsigned,3> expected; };
    for(auto sample : {Sample{5,5,{16,24,41}},Sample{21,5,{24,33,49}},Sample{15,100,{255,0,255}},Sample{15,108,{255,255,0}}})
        if(!p.color) LH_CHECK(pixel(image,sample.x*p.scale+p.scale/2,sample.y*p.scale+p.scale/2)==sample.expected);
    LH_EQ(lh::lines(lh::read_file(timing)).size(),size_t(10));
}
LH_TEST(test_vulkan_replay_rejects_truncated_stream) {
    auto binary=lh::vulkan_replay(t), stream=t.temp()/"bad.gpu";
    lh::write_file(stream,std::string("FF8GPUR1\0",9));
    LH_EQ(t.run(t.host(binary.string()).args({stream,"1"}).timeout(60)).rc,2);
}
static const int async_scales[]={1,4};
LH_TEST_P(test_vulkan_pipeline_keeps_guest_reads_native,async_scales) {
    auto binary=lh::vulkan_replay(t), timing=t.temp()/"async.csv";
    auto run=t.ok(t.host(binary.string()).args({"--self-test-async",p,t.temp()/"async.png",timing,"0","1"}).timeout(120));
    LH_EQ(lh::json_integer(run.out,"errors"),0); LH_EQ(lh::json_integer(run.out,"reads"),24);
    if(p==1) LH_EQ(lh::json_integer(run.out,"raw_mismatch_pixels"),0);
    auto rows=lh::csv_rows(timing); LH_EQ(rows.size(),size_t(9));
    for(const auto &row:rows) LH_CHECK(std::stod(row.at("gpu_ms"))>=0);
    for(size_t i=1;i<8;++i) { LH_EQ(std::stod(rows[i].at("capture_ms")),0); LH_EQ(std::stoi(rows[i].at("submissions")),1); }
}
LH_TEST(test_native_primitives_batch_their_exact_pixels) {
    auto binary=lh::vulkan_replay(t); GpuStream s;
    s.gp0({0xe3000000,0xe4000000|1023|(511<<10),0xe1000400});
    for(unsigned i=0;i<48;++i) {
        s.gp0({0x600000ff,xy(i,i),xy(32,32)});
        s.gp0({0x300000ff,xy(i,i),0x0000ff00,xy(i+30,i),0x00ff0000,xy(i,i+30)});
        s.gp0({0x4000ff00,xy(i,i),xy(i+30,i+30)});
    }
    s.gp0({0xe1000500,0x65000000,xy(128,0),0,xy(64,64)});
    s.gp0({0x60ff0000,xy(300,300),xy(4,4),0xc0000000,xy(300,300),xy(4,4)});
    for(int i=0;i<8;++i) s.row(3,0x7c007c00);
    s.display();s.row(2,0);
    auto stream=t.temp()/"native.gpu", timing=t.temp()/"timing.csv";s.save(stream);
    auto run=t.ok(t.host(binary.string()).args({stream,"1",t.temp()/"native.png",timing}).timeout(120));
    LH_EQ(lh::json_integer(run.out,"errors"),0);LH_EQ(lh::json_integer(run.out,"raw_mismatch_pixels"),0);
    auto rows=lh::csv_rows(timing);LH_CHECK(!rows.empty());LH_EQ(std::stoi(rows[0].at("draw_calls")),0);
}
LH_TEST(test_vulkan_wsi_recovery_and_retirement) {
    auto binary=lh::vulkan_replay(t).parent_path()/"gpu_wsi_contract";
    auto run=t.ok(t.host(binary.string()).timeout(30));LH_CHECK(lh::json_integer(run.out,"checks")>=40);
}
static const int phases[]={1,2,3,4,5};
LH_TEST_P(test_vulkan_c_boundaries_keep_native_state_after_failure,phases) {
    auto binary=lh::vulkan_replay(t);
    auto run=t.ok(t.host(binary.string()).args({"--self-test-fault",p,t.temp()/"unused.png",t.temp()/"fault.csv"}).timeout(120));
    LH_CONTAINS(run.out,"\"fallback\":\"pass\"");LH_CONTAINS(run.err,"falling back to soft");
}
static const char *overrides[]={"missing-driver","invalid-device"};
LH_TEST_P(test_vulkan_initialization_falls_back_to_soft,overrides) {
    auto binary=lh::vulkan_replay(t), shot=t.temp()/"native.png";
    auto command=t.host(binary.string()).args({"--self-test-fallback","4",shot}).timeout(120);
    if(std::string(p)=="missing-driver") command.env("VK_DRIVER_FILES",(t.temp()/"not-an-icd.json").string());
    else command.env("GRANITE_VULKAN_DEVICE_INDEX","9999");
    auto run=t.ok(command);LH_CONTAINS(run.out,"\"fallback\":\"pass\"");LH_CONTAINS(run.err,"soft presentation");
    auto image=png(shot);LH_EQ(image.width,320u);LH_EQ(image.height,240u);
}
LH_TEST(test_vulkan_unavailable_window_uses_sdl_soft_renderer) {
    auto binary=lh::vulkan_replay(t);
    auto run=t.ok(t.host(binary.string()).args({"--self-test-fallback-window","4",t.temp()/"fallback.png"})
        .env("SDL_VIDEODRIVER","offscreen").env("SDL_AUDIODRIVER","dummy").env("VK_DRIVER_FILES",(t.temp()/"missing-icd.json").string()).timeout(120));
    LH_CONTAINS(run.out,"\"fallback\":\"pass\"");
}
LH_TEST(test_vulkan_synchronization_validation_when_installed) {
    auto binary=lh::vulkan_replay(t);
    std::vector<lh::fs::path> folders={"/usr/share/vulkan/explicit_layer.d","/etc/vulkan/explicit_layer.d"};
    for(auto variable:{"VK_LAYER_PATH","VK_ADD_LAYER_PATH"}) {
        const char *value=std::getenv(variable);std::istringstream paths(value?value:"");std::string path;
        while(std::getline(paths,path,':')) if(!path.empty()) folders.emplace_back(path);
    }
    bool found=false;
    for(const auto &folder:folders) if(lh::fs::exists(folder)) for(const auto &entry:lh::fs::directory_iterator(folder))
        if(entry.path().extension()==".json" && lh::read_file(entry.path()).find("VK_LAYER_KHRONOS_validation")!=std::string::npos) found=true;
    if(!found) lh::skip("VK_LAYER_KHRONOS_validation is not installed");
    auto run=t.ok(t.host(binary.string()).args({"--self-test-async","4",t.temp()/"sync.png"})
        .env("VK_INSTANCE_LAYERS","VK_LAYER_KHRONOS_validation").env("VK_LAYER_ENABLES","VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT").timeout(120));
    LH_NOT_CONTAINS(run.out+run.err,"SYNC-HAZARD");LH_NOT_CONTAINS(run.out+run.err,"Validation Error");
}
struct Scale { std::string id; int scale,msaa; bool mask=false; };
static const Scale scales[]={{"1-1",1,1},{"4-1",4,1},{"4-4",4,4}};
static std::vector<Scale> mask_modes() {
    std::vector<Scale> result;
    for(bool mask:{false,true}) for(const auto &m:scales) result.push_back({std::string(mask?"True-":"False-")+m.id,m.scale,m.msaa,mask});
    return result;
}
static lh::oracle::PngImage run_stream(lh::Case &t,const lh::fs::path &binary,const GpuStream &s,int scale,int msaa,int color=0) {
    auto stream=t.temp()/"stream.gpu",shot=t.temp()/"scanout.png";s.save(stream);
    auto run=t.ok(t.host(binary.string()).args({stream,scale,shot,t.temp()/"timing.csv",color,msaa,"--benchmark"}).timeout(120));
    LH_EQ(lh::json_integer(run.out,"errors"),0);return png(shot);
}
LH_TEST_P(test_vram_copy_force_mask_is_used_by_textured_blending,mask_modes()) {
    auto binary=lh::vulkan_replay(t);GpuStream s;
    s.gp0({0xe3000000,0xe4000000|1023|(511<<10),0xe1000400});
    s.gp0({0x60400000,xy(640,256),xy(32,32),0xe6000000u|1u|(p.mask?2u:0u)});
    s.gp0({0x80000000,xy(640,256),xy(704,256),xy(32,32),0xe6000000});
    s.gp0({0x60000040,xy(0,0),xy(32,32),0xe100053b,0x66808080,xy(0,0),0,xy(32,32)});
    s.display();s.row(2,0);
    auto rgb=pixel(run_stream(t,binary,s,p.scale,p.msaa),8*p.scale,8*p.scale);
    LH_CHECK(rgb[0]>=50 && rgb[2]>=50 && rgb[1]==0);
}
LH_TEST_P(test_framebuffer_feedback_preserves_15bit_truncation,scales) {
    auto binary=lh::vulkan_replay(t);GpuStream s;s.display();
    s.gp0({0xe3000000,0xe4000000|1023|(511<<10),0xe1000400,0x60000008,xy(0,0),xy(320,240)});
    s.gp0({0xe6000001,0x80000000,xy(0,0),xy(640,256),xy(320,240),0xe6000000});
    for(int i=0;i<16;++i) {
        s.gp0({0xe100053a,0x66202020,xy(0,0),0,xy(320,240),0x80000000,xy(0,0),xy(640,256),xy(320,240)});s.row(2,0);
    }
    const std::array<unsigned,3> expected={8,0,0};
    LH_CHECK(pixel(run_stream(t,binary,s,p.scale,p.msaa),8*p.scale,8*p.scale)==expected);
}
LH_TEST(test_upload_releases_feedback_texture_for_scaled_geometry) {
    auto binary=lh::vulkan_replay(t);GpuStream s;s.display();
    s.gp0({0xe3000000,0xe4000000|1023|(511<<10),0xe1000400,0x60000008,xy(0,0),xy(320,240),0xe6000001});
    s.gp0({0x80000000,xy(0,0),xy(640,256),xy(320,240),0xe6000000,0xa0000000,xy(640,256),xy(256,256)});
    for(int i=0;i<256*256/2;++i) s.row(0,0x001f001f);
    s.gp0({0x02000000,xy(0,0),xy(320,240),0x25808080,xy(0,0),0,xy(8,0),0x11a<<16,xy(0,8),0});s.row(2,0);
    auto image=run_stream(t,binary,s,4,1);std::vector<unsigned> edge;
    for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) edge.push_back(pixel(image,3*4+x,4*4+y)[0]);
    LH_EQ(*std::min_element(edge.begin(),edge.end()),0u);LH_CHECK(*std::max_element(edge.begin(),edge.end())>200);
}
LH_TEST_P(test_hud_background_copy_keeps_scaled_pixels,scales) {
    auto binary=lh::vulkan_replay(t);GpuStream s;s.display();
    s.gp0({0xe3000000,0xe4000000|1023|(511<<10),0xe1000400,0xe6000001,0x80000000,xy(640,256),xy(0,0),xy(320,240),0xe6000000});
    s.gp0({0x200000f8,xy(0,0),xy(16,0),xy(0,16)});s.row(2,0);
    s.gp0({0xe6000001,0x80000000,xy(0,0),xy(384,256),xy(16,16),0x80000000,xy(384,256),xy(0,0),xy(16,16)});s.row(2,0);
    auto stream=t.temp()/"hud-copy.gpu",frames=t.temp()/"frames";s.save(stream);lh::fs::create_directory(frames);
    auto run=t.ok(t.host(binary.string()).args({stream,p.scale,t.temp()/"last.png",t.temp()/"timing.csv","0",p.msaa,"--benchmark","--dump-scaled-frames",frames}).timeout(120));
    LH_EQ(lh::json_integer(run.out,"errors"),0);
    auto a=png(frames/"frame-000001.png"),b=png(frames/"frame-000002.png");
    LH_EQ(a.width,b.width);LH_EQ(a.height,b.height);LH_BYTES_EQ(a.rgb,b.rgb);
}
struct Color { std::string id; bool masked,color;int scale,msaa; };
static std::vector<Color> color_modes() {
    std::vector<Color> result;
    for(bool masked:{false,true}) for(bool color:{false,true}) for(auto m:{Scale{"4-1",4,1},Scale{"1-4",1,4}})
        result.push_back({std::string(masked?"True-":"False-")+(color?"True-":"False-")+m.id,masked,color,m.scale,m.msaa});
    return result;
}
LH_TEST_P(test_vulkan_color_codes_survive_attachment_conversion,color_modes()) {
    auto binary=lh::vulkan_replay(t);GpuStream s;s.display();
    s.gp0({0xe3000000,0xe407ffff,0xe1000400,p.masked?0xe6000002u:0xe6000000u});
    for(unsigned value=0;value<256;++value) s.gp0({0x60000000|value*0x010101,xy(value,0),xy(1,4)});
    s.gp0({0xa0000000,xy(512,256),xy(32,1)});
    for(unsigned i=0;i<32;i+=2) s.row(0,(0x8000|i*0x421)|((0x8000|(i+1)*0x421)<<16));
    s.gp0({0xe1000518});
    for(auto row:{std::pair<unsigned,uint32_t>{8,0x65000000},{16,0x64808080},{24,0x64414141}}) s.gp0({row.second,xy(0,row.first),0,xy(32,1)});
    s.row(2,0);auto image=run_stream(t,binary,s,p.scale,p.msaa,p.color);
    for(unsigned y:{0u,8u,16u,24u}) for(unsigned x=0;x<(y?32u:256u);++x) {
        unsigned code=y==0?x:y==24?x*65/16:x*8,expected=p.color?code:(code&248)|(code>>5);
        auto rgb=pixel(image,x*p.scale+p.scale/2,y*p.scale+p.scale/2);
        LH_CHECK_MSG((rgb==std::array<unsigned,3>{expected,expected,expected}),"color at ("+std::to_string(x)+","+std::to_string(y)+"), expected "+std::to_string(expected));
    }
}
