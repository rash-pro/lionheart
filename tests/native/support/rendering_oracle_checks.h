// Known movie inputs and synthetic PNG roundtrips protect independent oracles.
#pragma once
#include "lh_test.h"
#include "numpy_random.h"
#include "oracles/image.h"

LH_TEST(image_png_roundtrips) {
    for (int width : {7, 320, 640}) {
        lh::oracle::PngImage image{unsigned(width), 224, {}};
        for (int y = 0; y < 224; ++y)
            for (int x = 0; x < width; ++x)
                for (int c = 0; c < 3; ++c) image.rgb += char((x * 7 + y * 3 + c * 51) & 255);
        const auto encoded = lh::oracle::write_png(image);
        LH_BYTES_EQ(lh::oracle::read_png(encoded).rgb, image.rgb);
    }
}
LH_TEST(numpy_movie_inputs_match_known_answers) {
    LH_EQ(lh::sha256(lh::NumpyRandom(0x1820).integers_u8(8,248,3*224*320*3)),"5e3ad6fc2f50e1550fb2e4b1893939a748c60836f732f45831cc30d29475711c");
    LH_EQ(lh::sha256(lh::NumpyRandom(0x15).integers_u8(8,248,224*320*3)),"581d2062b18836f47fb97914bad0de51ce01b83aa13ede318362dd3206991fb9");
}
