// Synthetic IROS, timestamp decoding, compositing and guest-read contracts.
#include "vulkan.h"
#include "rendering.h"
#include "../../../tools/devtools/iro_extract.h"
static void archive(const lh::fs::path &path, std::string name="CRF\\movies\\disc00_00h.avi", unsigned flags=0) {
    std::string data="IROS";
    auto put=[&](uint64_t v,int n) { for(int i=0;i<n;++i) data+=char(v>>(8*i)); };
    put(0x10002,4); put(0,4); put(16,4); put(1,4); put(name.size()*2+20,2); put(name.size()*2,2);
    for(unsigned char c:name) put(c,2);
    put(flags,4); put(40+name.size()*2,8); put(9,4); data+="owned-avi"; lh::write_file(path,data);
}
LH_TEST(test_iro_directory_and_atomic_extraction) {
    auto source=t.temp()/"owned.iroj", output=t.temp()/"cache"; archive(source);
    auto entries=iro::directory(source); LH_EQ(entries[0].name,"CRF/movies/disc00_00h.avi");
    LH_EQ(iro::extract(source,output,entries,t.root()),size_t(1));
    LH_BYTES_EQ(lh::read_file(output/"disc00_00h.avi"),"owned-avi");
    LH_EQ(iro::extract(source,output,entries,t.root()),size_t(1));
    lh::write_file(output/"disc00_00h.avi","owned-avj");
    LH_EQ(iro::extract(source,output,entries,t.root()),size_t(1));
    LH_BYTES_EQ(lh::read_file(output/"disc00_00h.avi"),"owned-avi");
}
struct BadPath { const char *id; const char *path; };
static const BadPath bad_paths[]={{"../escape.avi","../escape.avi"},{"/escape.avi","/escape.avi"},{"C:\\\\escape.avi","C:\\escape.avi"}};
LH_TEST_P(test_iro_rejects_paths,bad_paths) {
    auto source=t.temp()/"bad.iroj"; archive(source,p.path);
    bool caught=false; try { iro::directory(source); } catch(const std::runtime_error &e) { caught=true; LH_CONTAINS(e.what(),"unsafe"); }
    LH_CHECK(caught);
}
LH_TEST(test_iro_rejects_unknown_compression_before_extracting) {
    auto source=t.temp()/"compressed.iroj", output=t.temp()/"cache"; archive(source,"CRF\\movies\\disc00_00h.avi",1);
    bool caught=false; try { iro::extract(source,output,iro::directory(source),t.root()); } catch(const std::runtime_error &e) { caught=true; LH_CONTAINS(e.what(),"compressed"); }
    LH_CHECK(caught); LH_CHECK(!lh::fs::exists(output));
    auto bytes=lh::read_file(source); bytes.pop_back(); lh::write_file(source,bytes);
    caught=false; try { iro::directory(source); } catch(const std::runtime_error &e) { caught=true; LH_CONTAINS(e.what(),"beyond"); }
    LH_CHECK(caught);
}
struct Upload { const char *id; int rgb24,scale,msaa; };
static const Upload uploads[]={{"0-1-1",0,1,1},{"1-1-1",1,1,1},{"0-4-1",0,4,1},{"1-4-1",1,4,1},{"0-4-4",0,4,4},{"1-4-4",1,4,4}};
static lh::fs::path movie_contract(lh::Case &t) {
    lh::vulkan_replay(t);return lh::host_contract(t,"movie_contract");
}
LH_TEST_P(test_hd_upload_keeps_native_reads_and_draw_order,uploads) {
    auto tool=movie_contract(t);
    t.ok(t.host("ffmpeg").args({"-nostdin","-v","error","-f","lavfi","-i","color=0x00ff00:size=640x448:rate=30",
        "-t","1","-an","-vf","scale=out_color_matrix=bt709","-colorspace","bt709","-c:v","ffv1","-f","matroska",t.temp()/"disc00_00h.avi"}).timeout(30));
    auto run=t.ok(t.host(tool.string()).args({t.temp(),p.rgb24,t.temp()/"stats.csv",p.scale,p.msaa}).timeout(60));
    LH_CHECK(lh::json_boolean(run.out,"native_reads_exact"));LH_CONTAINS(lh::read_file(t.temp()/"stats.csv"),"ready");
}
LH_TEST(test_missing_hd_asset_leaves_original_available) {
    auto tool=movie_contract(t);
    auto run=t.ok(t.host(tool.string()).args({t.temp(),"2",t.temp()/"stats.csv"}).timeout(30));
    LH_CHECK(lh::json_boolean(run.out,"missing_falls_back"));LH_CONTAINS(lh::read_file(t.temp()/"stats.csv"),"missing");
}
LH_TEST(test_missing_leading_source_frame_uses_original) {
    auto tool=movie_contract(t);
    t.ok(t.host("ffmpeg").args({"-nostdin","-v","error","-f","lavfi","-i","color=green:size=640x448:rate=30",
        "-t","1","-an","-c:v","ffv1",t.temp()/"disc01_00h.avi"}).timeout(30));
    auto run=t.ok(t.host(tool.string()).args({t.temp(),"3",t.temp()/"stats.csv"}).timeout(30));
    LH_CHECK(lh::json_boolean(run.out,"negative_offset_fallback"));LH_CONTAINS(lh::read_file(t.temp()/"stats.csv"),"missing");
}
