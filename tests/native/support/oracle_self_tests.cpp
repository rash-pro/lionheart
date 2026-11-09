// Tests of the deterministic generator and the shared oracles. Their names do
// not start with test_, so they are labelled infra and never count toward
// parity with the Python suite. The check against Python itself lives in
// tests/native/check_against_python.py; these known answers outlive it.
#include "equiv.h"
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

const uint64_t kSeeds[] = {0, 1, 8, 90210, 0xadc0, 0x1820, 0x505358, 1400, 0xffffffffull, 0x100000000ull,
                           0x100000007ull, 0xfffffffffffffffeull};

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

// While LH_EQUIV_DIR is set, writes one record per generator method so the
// Python check can compare it with CPython itself, seed by seed.
LH_TEST(py_random_vectors_for_the_python_check) {
    if (!lh::equiv::stream(t)) return;
    for (uint64_t seed : kSeeds) {
        lh::PyRandom r(seed);
        for (int i = 0; i < 16; ++i) lh::equiv::record(t, r.random());
        for (unsigned k = 1; k <= 64; ++k) lh::equiv::record(t, r.getrandbits(k));
        for (int i = 0; i < 64; ++i) {
            // Braces, because the order function arguments are evaluated in is unspecified.
            const std::array<int64_t, 6> row = {r.randrange(1, 2), r.randrange(-98304, 98305), r.randrange(1, 1300),
                                                r.randrange(65536), r.randint(-32768, 32767), r.randrange(65537)};
            lh::equiv::record(t, row);
        }
        for (size_t size : {1u, 3u, 4u, 5u, 14u, 16u, 31u, 32u}) lh::equiv::record(t, r.randbytes(size));
    }
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

// Compares the reader with what Python's zlib wrote for the same payloads,
// when run from the Python check, which places vectors and expected digests here.
LH_TEST(png_oracle_vectors_from_the_python_check) {
    const char *dir = std::getenv("LH_EQUIV_DIR");
    if (!dir || !*dir) return;
    const lh::fs::path vectors = lh::fs::path(dir) / "png_vectors";
    if (!lh::fs::exists(vectors)) return;
    for (const auto &entry : lh::fs::directory_iterator(vectors)) {
        if (entry.path().extension() != ".png") continue;
        const auto image = lh::oracle::read_png(lh::read_file(entry.path()));
        const std::string expected = lh::read_file(lh::fs::path(entry.path()).replace_extension(".expected"));
        LH_EQ(lh::str(image.width, " ", image.height, " ", lh::sha256(image.rgb)), expected);
    }
}
