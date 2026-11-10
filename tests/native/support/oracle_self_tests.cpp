// Known answers for deterministic inputs and independent oracles.
#include "lh_test.h"
#include "oracles/png.h"
#include "py_random.h"
#include "values.h"

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string from_hex(const std::string &hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}


} // namespace

LH_TEST(py_random_known_answers_from_cpython) {
    LH_EQ(lh::PyRandom(0).random(), 0.8444218515250481);
    LH_EQ(lh::PyRandom(8).random(), 0.2267058593810488);
    LH_EQ(lh::PyRandom(0x100000007ull).random(), 0.22550888929893187);
    lh::PyRandom r(90210);
    LH_EQ(r.getrandbits(32), 2458402569u);
    LH_EQ(r.getrandbits(32), 3145459009u);
    LH_EQ(r.getrandbits(32), 3205711463u);
    LH_EQ(r.getrandbits(64), 14898022552378828633ull);
    LH_EQ(r.randrange(1, 1300), 1103);
    LH_EQ(r.randint(-32768, 32767), -7429);
    LH_EQ(lh::sha256(r.randbytes(14)), lh::sha256(from_hex("6772567ed388f4a5ac1df143a126")));
}

LH_TEST(inflate_reads_stored_and_compressed_zlib_streams) {
    const std::string compressed = from_hex("78dacb4b2cc92c4b55c82f4a4cce4955c84b4d4d2956c8cb57a8cac94c52c81b951b951b951b95a3400e00b9e09920");
    const std::string data = lh::oracle::Inflater(compressed).zlib();
    LH_EQ(data.size(), 1120u);
    LH_EQ(lh::sha256(data), std::string("740b7b50a38ad87ea34f74409aaf4ba966454180dbcc2a6e9153a656abc93a39"));

    // A stored block is the form the runtime's PNG writer produces.
    const std::string stored = from_hex("7801010300fcff616263024d0127");
    LH_TEXT_EQ(lh::oracle::Inflater(stored).zlib(), "abc");
}

LH_TEST(png_oracle_crc_matches_the_png_specification) { LH_EQ(lh::oracle::png_crc32("IEND"), 0xae426082u); }

#include "vulkan.h"
LH_TEST(vulkan_fixture_preserves_loader_selection) { LH_CHECK(lh::fs::is_regular_file(lh::vulkan_replay(t))); }



#include "player_probe_checks.h"

#include "rendering_oracle_checks.h"
