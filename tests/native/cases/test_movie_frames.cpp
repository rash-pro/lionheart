// The movie checker must reject misplaced tiles and accept decoder rounding.
#include "lh_test.h"
#include "numpy_random.h"
#include "../../../tools/devtools/movie_frames.h"
namespace {
using lh::oracle::PngImage;
static int64_t integer(const dt::Json &json,const char *key) {return std::get<int64_t>(json.at(key).value);}
static std::vector<PngImage> reference(lh::Case &t,uint64_t seed,int count) {
    lh::NumpyRandom random(seed);const auto bytes=random.integers_u8(8,248,size_t(count)*224*320*3);
    std::vector<PngImage> result;for(int i=0;i<count;++i) result.push_back({320,224,bytes.substr(size_t(i)*224*320*3,224*320*3)});return result;
}
static void write_png(const lh::fs::path &path,const PngImage &image) {lh::write_file(path,lh::oracle::write_png(image));}
static lh::fs::path frame(const lh::fs::path &path,int number) {
    std::ostringstream name;name<<"frame-"<<std::setfill('0')<<std::setw(6)<<number<<".png";return path/name.str();
}
static void record_report(lh::Case &t,const dt::Json &report) {
    const auto text=report.dump();
}
LH_TEST(test_movie_band_is_not_hidden_by_whole_frame_average) {
    auto ref=reference(t,0x1820,3);
    for(int number=0;number<12;++number) {auto displayed=ref[number/4];for(char &c:displayed.rgb) c=char(uint8_t(c)+1);write_png(frame(t.temp(),number),displayed);}
    auto report=movie_frames::compare_frames(ref,t.temp(),0,11);record_report(t,report);LH_EQ(integer(report,"bad_displayed_frames"),0);
    auto displayed=ref[1];for(unsigned y=0;y<32;++y) for(unsigned x=0;x<304;++x) for(int c=0;c<3;++c)
        displayed.rgb[(y*320+x)*3+c]=ref[1].rgb[(y*320+x+16)*3+c];
    write_png(frame(t.temp(),5),displayed);report=movie_frames::compare_frames(ref,t.temp(),0,11);record_report(t,report);
    LH_EQ(integer(report,"checked_vblanks"),12);LH_EQ(integer(report,"bad_displayed_frames"),1);
    const auto &failure=std::get<dt::Json::Array>(report.at("failures").value).at(0);LH_EQ(integer(failure,"reference_frame"),1);
    dt::Json expected=dt::Json::array();for(int y=0;y<2;++y) for(int x=0;x<19;++x) expected.push(dt::Json::Array{y,x});
    LH_EQ(failure.at("bad_tiles_yx").dump(),expected.dump());
    lh::fs::remove(frame(t.temp(),6));bool caught=false;
    try {movie_frames::compare_frames(ref,t.temp(),0,11);} catch(const lh::fs::filesystem_error &e) {caught=e.code()==std::errc::no_such_file_or_directory;}LH_CHECK(caught);
}
LH_TEST(test_movie_checker_accepts_integer_color_rounding) {
    PngImage ref{320,224,std::string(224*320*3,char(128))},displayed{320,224,std::string(224*320*3,char(129))};
    auto errors=movie_frames::tile_errors(displayed,ref);for(double error:errors) LH_EQ(error,1);
    bool caught=false;ref.height=16;ref.rgb.resize(16*320*3);
    try {movie_frames::tile_errors(ref,ref);} catch(const std::runtime_error &e) {caught=true;LH_CONTAINS(e.what(),"320x224");}LH_CHECK(caught);
}
LH_TEST(test_movie_switch_to_rgb15_does_not_hide_a_misplaced_band) {
    auto ref=reference(t,0x15,1);PngImage displayed{320,224,movie_frames::pack_rgb15(ref[0].rgb)};
    write_png(frame(t.temp(),0),displayed);auto report=movie_frames::compare_frames(ref,t.temp(),0,0);record_report(t,report);
    LH_EQ(integer(report,"bad_displayed_frames"),0);LH_EQ(integer(report,"rgb15_reference_vblanks"),1);
    const auto original=displayed.rgb;for(unsigned y=0;y<32;++y) for(unsigned x=0;x<304;++x) for(int c=0;c<3;++c)
        displayed.rgb[(y*320+x)*3+c]=original[(y*320+x+16)*3+c];
    write_png(frame(t.temp(),0),displayed);report=movie_frames::compare_frames(ref,t.temp(),0,0);record_report(t,report);LH_EQ(integer(report,"bad_displayed_frames"),1);
}
struct Anchor {const char *id;double left,right,lag;bool expected;};
static const Anchor anchors[]={{"0.84-0.93-0.0-True",.84,.93,0,true},{"0.99-0.99-0.1-False",.99,.99,.1,false},
    {"0.7-0.8-0.0-False",.70,.80,0,false},{"nan-0.99-0.0-False",std::numeric_limits<double>::quiet_NaN(),.99,0,false}};
LH_TEST_P(test_audio_anchor_requires_a_strong_match_and_stereo_agreement,anchors) {
    const bool actual=movie_frames::matched_anchor(p.left,p.right,1,1+p.lag);LH_EQ(actual,p.expected);
}
} // namespace
