// Image/payload hashing agrees with an independent SHA1 across padding boundaries.
#include "lh_test.h"
#include "equiv.h"
#include "oracles/sha1.h"

namespace {
struct Row { std::string id; unsigned size, prefix; };
std::vector<Row> rows() {
    std::vector<Row> out;
    for (unsigned prefix : {0,1,3})
        for (unsigned size : {0,1,3,55,56,57,63,64,65,119,120,127,128,129,511,65536,501056,1000000})
            out.push_back({std::to_string(prefix)+"-"+std::to_string(size), size, prefix});
    return out;
}
}
LH_TEST_P(test_image_sha1_padding_and_unaligned_input, rows()) {
    std::string data(p.size, '\0');
    for (size_t i=0; i<data.size(); ++i) data[i]=char(i & 255);
    const auto expected=lh::oracle::sha1(data);
    lh::equiv::record(t, uint32_t(p.size), uint32_t(p.prefix), data, expected);
    const auto path=t.temp()/"data";
    lh::write_file(path,data);
    LH_TEXT_EQ(t.ok(t.harness("sha1").args({path,p.prefix})).out, expected+"\n");
}
