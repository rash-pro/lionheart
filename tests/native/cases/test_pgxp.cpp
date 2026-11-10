// Clean-room projection-cache and GPU replay behavior, without disc fixtures.
#include "vulkan.h"
#include "rendering.h"
#include "oracles/png.h"
#include "../../../tools/devtools/render_quality.h"
#include <array>
namespace {
using lh::xy;
using lh::GpuStream;
LH_TEST(test_pgxp_cache_and_parser) {LH_CONTAINS(t.ok(t.harness("cache")).out,"contracts passed");}
static void vertex(GpuStream &s,int x,int y,int z) {
    s.row(7,xy(x,y));lh::little(s.data,x*65536+49152,8);lh::little(s.data,y*65536+32768,8);lh::little(s.data,z);
}
static void projection_stream(const lh::fs::path &path,bool gouraud=false,bool missing_fourth=false) {
    GpuStream s(2);s.row(1,0x03000000);s.gp0({0xe3000000,0xe407ffff,0xe1000400});
    s.gp0({0xa0000000,xy(512,256),xy(64,64)});std::vector<uint32_t> pixels;
    for(unsigned y=0;y<64;++y) for(unsigned x=0;x<64;++x) pixels.push_back((x/2)|((y/2)<<5)|(16<<10));
    for(size_t i=0;i<pixels.size();i+=2) s.row(0,pixels[i]|(pixels[i+1]<<16));
    vertex(s,20,20,100);vertex(s,180,20,400);vertex(s,20,180,200);
    if(gouraud) s.gp0({0x300000ff,xy(20,20),0x0000ff00,xy(180,20),0x00ff0000,xy(20,180)});
    else {
        s.gp0({missing_fourth?0x2d808080u:0x25808080u,xy(20,20),0,xy(180,20),(0x118<<16)|63,xy(20,180),63<<8});
        if(missing_fourth) s.gp0({xy(180,180),(63<<8)|63});
    }
    s.gp0({0xc0000000,xy(60,60),xy(2,1)});const unsigned native=7|(7<<5)|(16<<10);
    if(!gouraud) s.row(3,native|((native+1)<<16));
    s.row(2,0);s.save(path);
}
static std::string replay(lh::Case &t,const lh::fs::path &binary,const lh::fs::path &stream,const lh::fs::path &shot,const char *mode,const char *policy=nullptr) {
    auto csv=shot;csv.replace_extension(".csv");
    auto command=t.host(binary.string()).args({stream,"4",shot,csv,"0","1","--pgxp",mode}).timeout(120);
    if(policy) command.args({"--pgxp-policy",policy});
    auto run=t.ok(command);LH_EQ(lh::json_integer(run.out,"errors"),0);return run.out;
}
LH_TEST(test_precise_vertices_and_reciprocal_depth_texture_pixels) {
    auto binary=lh::vulkan_replay(t),stream=t.temp()/"plane.gpu";projection_stream(stream);std::map<std::string,lh::oracle::PngImage> images;
    for(auto mode:{"off","vertex","perspective"}) {
        auto shot=t.temp()/(std::string(mode)+".png");auto report=replay(t,binary,stream,shot,mode);LH_EQ(lh::json_integer(report,"reads"),1);
        if(std::string(mode)!="off") {LH_EQ(lh::json_integer(report,"precise"),1);LH_EQ(lh::json_integer(report,"candidates"),1);}
        images[mode]=lh::oracle::read_png(lh::read_file(shot));
    }
    LH_CHECK(images["off"].rgb!=images["vertex"].rgb);LH_CHECK(images["vertex"].rgb!=images["perspective"].rgb);
    size_t at=((60*4+2)*images["vertex"].width+60*4+2)*3;LH_EQ(uint8_t(images["vertex"].rgb[at]),57);LH_EQ(uint8_t(images["perspective"].rgb[at]),16);
}
LH_TEST(test_perspective_keeps_gouraud_colors_affine) {
    auto binary=lh::vulkan_replay(t),stream=t.temp()/"color.gpu";projection_stream(stream,true);
    replay(t,binary,stream,t.temp()/"vertex.png","vertex");replay(t,binary,stream,t.temp()/"perspective.png","perspective");LH_FILE_EQ(t.temp()/"vertex.png",t.temp()/"perspective.png");
}
LH_TEST(test_missing_quad_vertex_preserves_other_endpoints) {
    auto binary=lh::vulkan_replay(t),stream=t.temp()/"quad.gpu";projection_stream(stream,false,true);
    auto report=replay(t,binary,stream,t.temp()/"on.png","perspective");replay(t,binary,stream,t.temp()/"off.png","off");
    LH_EQ(lh::json_integer(report,"precise"),0);LH_EQ(lh::json_integer(report,"partial"),1);LH_CHECK(lh::read_file(t.temp()/"on.png")!=lh::read_file(t.temp()/"off.png"));
}
struct Payload {std::string id;unsigned size;};
static std::vector<Payload> payloads() {std::string id;for(int i=0;i<19;++i) id+="\\x00";return {{"",0},{id,19}};}
LH_TEST_P(test_truncated_projection_payload_is_rejected,payloads()) {
    auto binary=lh::vulkan_replay(t);GpuStream s(2);s.row(7,0);s.data.append(p.size,'\0');auto stream=t.temp()/"truncated.gpu";s.save(stream);
    LH_EQ(t.run(t.host(binary.string()).args({stream,"1"}).timeout(60)).rc,2);
}
static const int origins[]={0,1};
LH_TEST_P(test_recording_binds_before_projection_and_seeds_late_snapshots,origins) {
    auto binary=lh::vulkan_replay(t),stream=t.temp()/"capture.gpu";
    t.ok(t.harness("record").env("FF8_GPU_STREAM",stream.string()).env("FF8_GPU_FROM",std::to_string(p)));
    LH_CHECK(lh::read_file(stream).rfind("FF8GPUR2",0)==0);
    auto report=replay(t,binary,stream,t.temp()/"capture.png","perspective");LH_EQ(lh::json_integer(report,"precise"),1);LH_EQ(lh::json_integer(report,"candidates"),1);LH_EQ(lh::json_integer(report,"statuses"),1);
    auto strict=replay(t,binary,stream,t.temp()/"strict.png","vertex","strict");LH_EQ(lh::json_integer(strict,"precise"),0);LH_EQ(lh::json_integer(strict,"partial"),1);
}
LH_TEST(test_pgxp_host_initializes_lazy_hardware_and_keeps_guest_state) {
    lh::vulkan_replay(t);
    std::string header(2048,'\0');header.replace(0,8,"PS-X EXE");
    auto put=[&](size_t at,uint32_t value) {for(int i=0;i<4;++i) header[at+i]=char(value>>(8*i));};
    put(0x10,0x80010000);put(0x18,0x80010000);put(0x1c,2048);
    std::string text;lh::little(text,0x08004000);lh::little(text,0);text.append(2040,'\0');auto exe=t.temp()/"synthetic.exe";lh::write_file(exe,header+text);
    const auto folder=t.temp()/"synthetic-host";lh::host_contract(t,"lionheart-gen-spec",true,folder);
    const auto generated=folder/"gen";
    std::string hex;const char *digits="0123456789abcdef";for(uint8_t c:text) {hex+=digits[c>>4];hex+=digits[c&15];}
    dt::Json spec=dt::Json::Object{{"main",dt::Json::Object{{"data",hex},{"base",int64_t(0x80010000u)},{"entry",int64_t(0x80010000u)}}},{"mode","fast"},{"host_mode","precise"}};
    const auto description=t.temp()/"program.json";lh::write_file(description,spec.dump());
    t.ok(t.host((folder/"host/generator/lionheart-gen-spec").string()).args({description,generated}));
    // The empty catalog was only for building the generator; reload its emitted sources.
    t.ok(t.host("cmake").args({"-S",t.root(),"-B",folder/"host"}));
    auto binary=lh::host_contract(t,"ff8",true,folder);std::vector<std::string> states;
    for(auto mode:{"default","off","vertex","perspective"}) {
        auto state=t.temp()/(std::string(mode)+".state");
        auto command=t.host(binary.string()).args({exe,"--config","/dev/null","--headless","--mute","--fast","--stall","0","--max-instructions","0","--max-vblanks","2","--dump-state",state})
            .env("SDL_VIDEODRIVER","offscreen").env("SDL_AUDIODRIVER","dummy").timeout(30);
        if(std::string(mode)!="default") command.args({"--renderer","vulkan","--pgxp",mode});
        auto run=t.ok(command);LH_CONTAINS(run.out,"Stop: VBlank limit reached");LH_CONTAINS(run.out,"VBlanks=2 ");
        if(std::string(mode)!="default") {LH_CONTAINS(run.err,"Vulkan:");LH_NOT_CONTAINS(run.err,"falling back");}states.push_back(lh::read_file(state));
    }
    for(const auto &state:states) LH_BYTES_EQ(state,states[0]);
}
static void shared_edge_stream(const lh::fs::path &path) {
    GpuStream s(2);s.row(1,0x03000000);s.row(1,0x06000000|512|(3712<<12));s.gp0({0xe3000000,0xe407ffff,0xe1000400});
    for(auto v:{std::pair<int,int>{180,20},{20,180},{180,180}}) vertex(s,v.first,v.second,100);
    s.gp0({0x200000ff,xy(180,20),xy(20,180),xy(180,180),0x200000ff,xy(20,20),xy(180,20),xy(20,180)});s.row(2,0);s.save(path);
}
LH_TEST(test_adjacent_polygons_with_partial_provenance_have_no_black_crack) {
    auto binary=lh::vulkan_replay(t),stream=t.temp()/"shared.gpu";shared_edge_stream(stream);
    replay(t,binary,stream,t.temp()/"off.png","off");replay(t,binary,stream,t.temp()/"on.png","vertex");
    auto off=lh::oracle::read_png(lh::read_file(t.temp()/"off.png")),on=lh::oracle::read_png(lh::read_file(t.temp()/"on.png"));
    auto result=quality::compare(off,on,"battle");LH_EQ(result.interior,0);

}
LH_TEST(test_crack_detector_catches_interior_line_without_counting_silhouette_motion) {
    lh::oracle::PngImage off{1280,864,std::string(1280*864*3,char(80))},on=off;
    for(int x=100;x<300;++x) for(int c=0;c<3;++c) on.rgb[(500*1280+x)*3+c]=0;
    LH_EQ(quality::compare(off,on,"battle").interior,200);
    off=on;for(int x=100;x<300;++x) for(int c=0;c<3;++c) on.rgb[(501*1280+x)*3+c]=0;
    auto result=quality::compare(off,on,"battle");LH_EQ(result.raw,200);LH_EQ(result.interior,0);
}
} // namespace
