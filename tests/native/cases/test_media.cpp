// Synthetic MDEC/CD vectors. No disc data or emulator source is an oracle.
#include "equiv.h"
#include "lh_test.h"
#include "oracles/media.h"
#include "py_random.h"
#include "values.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string pack(const int16_t *values, size_t count) {
    return {reinterpret_cast<const char *>(values), count * sizeof(int16_t)};
}

std::vector<int16_t> unpack(const std::string &bytes) {
    std::vector<int16_t> values(bytes.size() / sizeof(int16_t));
    std::memcpy(values.data(), bytes.data(), values.size() * sizeof(int16_t));
    return values;
}

std::string run_media(lh::Case &t, std::vector<lh::Arg> args, const std::string *input = nullptr) {
    lh::Command command = t.harness("media").timeout(30);
    for (lh::Arg &arg : args) command.arg(std::move(arg));
    if (input) command.input(*input);
    return t.ok(command).out;
}

const char *const kCases[] = {"mdec", "dma", "movie-overlap", "movie-priority", "movie-cancel", "movie-late-gpu"};

struct XaRow {
    const char *id;
    unsigned bits;
    bool stereo;
};
const XaRow kXaRows[] = {{"4-False", 4, false}, {"4-True", 4, true}, {"8-False", 8, false}, {"8-True", 8, true}};

struct InterleaveRow {
    const char *id;
    unsigned bits;
    bool stereo, half;
};
const InterleaveRow kInterleaveRows[] = {{"4-True-False", 4, true, false},
                                         {"4-False-True", 4, false, true},
                                         {"8-True-True", 8, true, true},
                                         {"8-False-False", 8, false, false}};

// Three sectors around the audio one: plain data, an audio sector for a file
// the filter must reject, and more data.
std::string data_sector() {
    std::string data(2352, '\0');
    data[15] = 2;
    data[18] = 8;
    data[24] = 0x44;
    data[25] = 0x33;
    data[26] = 0x22;
    data[27] = 0x11;
    return data;
}

} // namespace

LH_TEST_P(test_mdec_registers_and_dma, kCases) { run_media(t, {p}); }

LH_TEST(test_idct_fixed_point) {
    // Non-symmetric programmable matrices catch accidental transposition and
    // preserve the rounding between passes; large inputs exercise wrap/clamp.
    lh::PyRandom randomizer(0x1820);
    for (int iteration = 0; iteration < 24; ++iteration) {
        std::array<int16_t, 64> coeff{}, matrix{};
        for (int16_t &value : coeff) value = static_cast<int16_t>(randomizer.randrange(-16384, 16384));
        for (int16_t &value : matrix) value = static_cast<int16_t>(randomizer.randrange(-32768, 32768));
        const auto expected = lh::oracle::idct_reference(coeff, matrix);
        lh::equiv::record(t, coeff, matrix, expected);
        std::string input = pack(coeff.data(), coeff.size());
        input += pack(matrix.data(), matrix.size());
        const auto actual = unpack(run_media(t, {"idct"}, &input));
        LH_SEQ_EQ(actual, expected, lh::str("iteration=", iteration));
    }
}

LH_TEST_P(test_xa_predictor_arithmetic_and_history, kXaRows) {
    const std::string raw = lh::oracle::xa_sector(p.bits, p.stereo);
    lh::oracle::XaHistory history{};
    std::vector<int16_t> expected = lh::oracle::xa_reference(raw, history);
    const std::vector<int16_t> second = lh::oracle::xa_reference(raw, history);
    expected.insert(expected.end(), second.begin(), second.end());
    lh::equiv::record(t, raw, expected);
    const auto actual = unpack(run_media(t, {"xa"}, &raw));
    LH_SEQ_EQ(actual, expected, lh::str("bits=", p.bits, " stereo=", p.stereo));
}

LH_TEST_P(test_xa_interleave_resampling_and_volume, kInterleaveRows) {
    const std::string data = data_sector(), audio = lh::oracle::xa_sector(p.bits, p.stereo, p.half);
    std::string rejected = audio;
    rejected[17] = 3;
    const auto path = t.temp() / "synthetic.bin";
    lh::write_file(path, data + audio + rejected + data);
    const std::string plain = run_media(t, {"stream", path, "0xc8", 0});
    const size_t frames = (18 * (32 / p.bits) * 28 / (p.stereo ? 2 : 1)) * (p.half ? 2 : 1) * 7 / 6;
    LH_EQ(plain.size(), frames * 4);
    LH_CHECK(plain.find_first_not_of(std::string(1, '\0')) != std::string::npos);
    LH_BYTES_EQ(run_media(t, {"stream", path, "0xc0", 0}), plain); // Automatic channel selection without Setfilter mode.
    LH_BYTES_EQ(run_media(t, {"stream", path, "0x88", 0}), std::string()); // Filtering with XA disabled discards RT audio.
    LH_BYTES_EQ(run_media(t, {"stream", path, "0x80", 0}), std::string()); // Without either mode bit, all sectors reach the CPU.
    LH_BYTES_EQ(run_media(t, {"stream", path, "0xc8", 2}), plain); // Volume writes are latched only by CHNGATV.
    const std::string swapped = run_media(t, {"stream", path, "0xc8", 1});
    std::string expected;
    for (size_t i = 0; i < plain.size(); i += 4) expected += plain.substr(i + 2, 2) + plain.substr(i, 2);
    LH_BYTES_EQ(swapped, expected);
    LH_CHECK(run_media(t, {"stream", path, "0xc8", 3}).find_first_not_of(std::string(1, '\0')) == std::string::npos);
}

LH_TEST(test_xa_resampling_impulse) {
    const std::string data = data_sector();
    std::string audio(2352, '\0');
    const unsigned char audio_header[] = {2, 1, 2, 0x64, 1};
    std::memcpy(&audio[15], audio_header, sizeof(audio_header));
    audio[24 + 16] = 7; // Left impulse 28672, right silent, all predictors zero.
    std::string rejected = audio;
    rejected[17] = 3;
    const auto path = t.temp() / "impulse.bin";
    lh::write_file(path, data + audio + rejected + data);
    const std::string pcm = run_media(t, {"stream", path, "0xc8", 0});
    LH_CHECK(pcm.size() >= 28);
    const auto first = unpack(pcm.substr(0, 28));
    // First six inputs place the impulse at tap six. Seven filter coefficients
    // times 28672, floored after division by 32768, yield this exact vector.
    const std::array<int16_t, 7> left = {-2, 2, -5, 4, 8, 22, -60}, right = {};
    for (size_t i = 0; i < 7; ++i) {
        LH_EQ(first[i * 2], left[i]);
        LH_EQ(first[i * 2 + 1], right[i]);
    }
}
