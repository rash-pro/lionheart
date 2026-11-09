// Temporary equivalence records consumed by check_against_python.py.
#pragma once
#include "equiv.h"
#include "numpy_random.h"
#include "oracles/image.h"
#include "../../../tools/devtools/render_quality.h"

LH_TEST(numpy_uint8_vectors_for_the_python_check) {
    for(uint64_t seed:{0ull,1ull,0x15ull,0x1820ull,0xffffffffull,0x100000007ull}) {
        lh::NumpyRandom random(seed);
        for(unsigned high:{9u,248u,256u}) for(size_t size:{1u,3u,4u,7u,1024u}) {
            const unsigned low=high==9?8:0;
            const auto data=random.integers_u8(low,high,size);
            lh::equiv::record(t,data);
        }
    }
}
LH_TEST(image_numeric_vectors_for_the_python_check) {
    for(int width:{7,320,640}) {
        lh::oracle::PngImage image{unsigned(width),224,{}};
        for(int y=0;y<224;++y) for(int x=0;x<width;++x) for(int c=0;c<3;++c) image.rgb+=char((x*7+y*3+c*51)&255);
        lh::oracle::Rect rect{1,2,width-2,220};
        const auto mean=lh::oracle::mean_linear(image,rect);const auto profile=lh::oracle::row_profile(image,rect);
        lh::equiv::record(t,image.rgb,mean,profile,lh::oracle::dominant_cycles(profile));
        const auto encoded=lh::oracle::write_png(image);
        LH_BYTES_EQ(lh::oracle::read_png(encoded).rgb,image.rgb);
        lh::equiv::record(t,uint64_t(encoded.size()),encoded);
    }
    for(const auto &scene:{"battle","world","dorm"}) {
        lh::oracle::PngImage off{1280,864,std::string(1280*864*3,char(80))},on=off;
        for(int x=100;x<300;++x) for(int c=0;c<3;++c) on.rgb[(500*1280+x)*3+c]=0;
        auto result=quality::compare(off,on,scene);lh::equiv::record(t,result.raw,result.interior,result.roi);
        off=on;for(int x=100;x<300;++x) for(int c=0;c<3;++c) on.rgb[(501*1280+x)*3+c]=0;
        result=quality::compare(off,on,scene);lh::equiv::record(t,result.raw,result.interior,result.roi);
    }
}
LH_TEST(numpy_movie_inputs_match_known_answers) {
    LH_EQ(lh::sha256(lh::NumpyRandom(0x1820).integers_u8(8,248,3*224*320*3)),"5e3ad6fc2f50e1550fb2e4b1893939a748c60836f732f45831cc30d29475711c");
    LH_EQ(lh::sha256(lh::NumpyRandom(0x15).integers_u8(8,248,224*320*3)),"581d2062b18836f47fb97914bad0de51ce01b83aa13ede318362dd3206991fb9");
}
