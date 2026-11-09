// SPU reference vectors and DMA/MMIO integration without disc-derived fixtures.
#include "lh_test.h"

static const char *const kCases[] = {"deadlines", "adpcm", "gaussian", "adsr", "sample", "loops", "transfers", "dma", "stream_dma", "cd", "modulation", "reverb", "reverb_addresses", "volume_registers"};
LH_TEST_P(test_spu, kCases) {
    t.ok(t.harness("main").arg(p).timeout(30));
}

static unsigned little(const std::string &data, size_t at, size_t count) {
    LH_CHECK(at <= data.size() && count <= data.size() - at);
    unsigned value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= unsigned(static_cast<unsigned char>(data[at + i])) << (8 * i);
    return value;
}

static const char *const kModes[] = {"headless", "mute", "device"};
LH_TEST_P(test_audio_wav_is_lossless, kModes) {
    const auto path = t.temp() / (std::string(p) + ".wav");
    t.ok(t.harness("audio").args({p, path}).timeout(30).env("SDL_AUDIODRIVER", "dummy"));
    const auto wav = lh::read_file(path);
    LH_BYTES_EQ(wav.substr(0, 4), "RIFF");
    LH_BYTES_EQ(wav.substr(8, 4), "WAVE");
    bool format = false, samples = false;
    for (size_t at = 12; at + 8 <= wav.size();) {
        const auto size = little(wav, at + 4, 4);
        const auto start = at + 8;
        LH_CHECK(size <= wav.size() - start);
        const auto kind = wav.substr(at, 4);
        if (kind == "fmt ") {
            LH_CHECK(size >= 16);
            LH_EQ(little(wav, start, 2), 1u);
            LH_EQ(little(wav, start + 2, 2), 2u);
            LH_EQ(little(wav, start + 4, 4), 44100u);
            LH_EQ(little(wav, start + 14, 2), 16u);
            format = true;
        } else if (kind == "data") {
            LH_EQ(size / 4, 44100u);
            std::string expected;
            for (unsigned i = 0; i < 44100; ++i) expected.append("\x87\x13\x78\xec", 4);
            LH_BYTES_EQ(wav.substr(start, size), expected);
            samples = true;
        }
        at = start + size + (size & 1);
    }
    LH_CHECK(format && samples);
}

struct Clock { const char *id; lh::Tier tier; };
static const Clock kClocks[] = {{"paced", lh::Tier::realtime}, {"pal", lh::Tier::realtime},
    {"fast", lh::Tier::standard}, {"resample", lh::Tier::standard}, {"overrun", lh::Tier::standard}};
LH_TEST_P(test_audio_clock_and_fast_mode, kClocks) {
    const auto path = t.temp() / (std::string(p.id) + ".wav");
    t.ok(t.harness("audio").args({p.id, path}).timeout(15).env("SDL_AUDIODRIVER", "dummy"));
}
