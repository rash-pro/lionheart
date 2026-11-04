#include <SDL.h>
static Uint64 fake_clock;
static Uint64 audio_test_counter(void) {
    return fake_clock ? fake_clock : SDL_GetPerformanceCounter();
}
static Uint64 audio_test_frequency(void) {
    return fake_clock ? 1000000 : SDL_GetPerformanceFrequency();
}
static void audio_test_delay(Uint32 ms) {
    if (fake_clock) fake_clock += ms ? (Uint64)ms * 1000 : 100;
    else SDL_Delay(ms);
}
#define SDL_GetPerformanceCounter audio_test_counter
#define SDL_GetPerformanceFrequency audio_test_frequency
#define SDL_Delay audio_test_delay
#include "../backends/host/audio.c"
#undef SDL_GetPerformanceCounter
#undef SDL_GetPerformanceFrequency
#undef SDL_Delay
#include <assert.h>

int main(int argc, char **argv) {
    spu_t *s = spu_create();
    int16_t input[2048];
    unsigned i, frames = 0;
    int device;
    assert(argc == 3 && s);
    if (!strcmp(argv[1], "overrun")) {
        Uint64 before;
        fake_clock = 1000000;
        assert(host_audio_init(0, 1, NULL));
        before = fake_clock; host_audio_pace(0, 0);
        assert(fake_clock - before >= 16000 && fake_clock - before < 17000);
        fake_clock += 33000;
        before = fake_clock; host_audio_pace(0, 0);
        assert(fake_clock - before < 1000);
        before = fake_clock; host_audio_pace(0, 0);
        assert(fake_clock - before >= 16000 && fake_clock - before < 17000);
        assert(host_audio_shutdown(s)); spu_destroy(s); return 0;
    }
    if (!strcmp(argv[1], "resample")) {
        unsigned rate;
        for (rate = 0; rate < 3; ++rate) {
            int16_t output[2100];
            unsigned at, total = 0;
            double step = 1 / (rate == 0 ? 0.995 : rate == 1 ? 1 : 1.005);
            memset(&host_audio, 0, sizeof(host_audio)); host_audio.resample_step = step;
            for (i = 0; i < 1000; ++i) { input[i * 2] = (int16_t)(i * 30); input[i * 2 + 1] = (int16_t)-(int)(i * 30); }
            for (at = 0; at < 1000; at += 17) {
                unsigned count = 1000 - at < 17 ? 1000 - at : 17;
                total += resample(input + at * 2, count, output + total * 2);
            }
            assert(total >= 995 && total <= 1005);
            for (i = 0; i < total; ++i) {
                int expected = (int)(i * step * 30);
                assert(abs(output[i * 2] - expected) <= 1);
                assert(abs(output[i * 2 + 1] + expected) <= 1);
            }
        }
        spu_destroy(s); return 0;
    }
    if (!strcmp(argv[1], "paced") || !strcmp(argv[1], "pal") || !strcmp(argv[1], "fast") || !strcmp(argv[1], "long")) {
        int pal = !strcmp(argv[1], "pal"), fast = !strcmp(argv[1], "fast");
        unsigned count = !strcmp(argv[1], "long") ? 7200 : pal ? 125 : 150;
        uint64_t start;
        double elapsed, duration = count * (pal ? 1.0 / 50 : 1001.0 / 60000.0);
        assert(host_audio_init(0, 0, argv[2]));
        assert(host_audio.device);
        if (!strcmp(argv[1], "long")) {
            char path[4096];
            assert(snprintf(path, sizeof(path), "%s.csv", argv[2]) < (int)sizeof(path));
            assert(host_audio_stats(path));
        }
        start = SDL_GetPerformanceCounter();
        for (i = 1; i <= count; ++i) {
            uint64_t cycles = pal ? (uint64_t)i * 33868800 / 50 : (uint64_t)i * 33868800 * 1001 / 60000;
            spu_advance(s, cycles); assert(host_audio_pump(s)); host_audio_pace(pal, fast);
        }
        elapsed = (double)(SDL_GetPerformanceCounter() - start) / SDL_GetPerformanceFrequency();
        printf("Pacing: %u frames, %.6f wall seconds, %.6f guest seconds\n", count, elapsed, duration);
        if (fast) assert(elapsed < duration / 2);
        else {
            assert(elapsed >= duration * 0.99 && elapsed < duration + 3);
            /* Include a device period and scheduler jitter around the 60 ms target. */
            assert(host_audio.queue_max < SPU_RATE * 120 / 1000);
            assert(host_audio.queue_sum / host_audio.samples > SPU_RATE * 30 / 1000);
            assert(host_audio.underruns == 0);
        }
        assert(host_audio.dropped == 0);
        assert(host_audio_shutdown(s)); spu_destroy(s); return 0;
    }
    device = !strcmp(argv[1], "device");
    assert(host_audio_init(!strcmp(argv[1], "headless"), !strcmp(argv[1], "mute"), argv[2]));
    if (device) { assert(host_audio.device); SDL_PauseAudioDevice(host_audio.device, 1); }
    spu_write(s, 0x180, 0x3fff); spu_write(s, 0x182, 0x3fff);
    spu_write(s, 0x1b0, 0x4000); spu_write(s, 0x1b2, 0x4000); spu_write(s, 0x1aa, 1);
    for (i = 0; i < 1024; ++i) { input[i*2] = 10000; input[i*2+1] = -10000; }
    while (frames < 44100) {
        unsigned count = 44100 - frames < 1024 ? 44100 - frames : 1024;
        spu_cd_input(s, input, count); frames += count;
        spu_advance(s, (uint64_t)frames * 768);
        assert(host_audio_pump(s));
    }
    if (device) {
        assert(SDL_GetQueuedAudioSize(host_audio.device) == 44100 * 4);
        assert(host_audio.dropped == 0);
    }
    assert(host_audio_shutdown(s)); spu_destroy(s); return 0;
}
