#include "audio.h"
#include <SDL.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define TARGET_FRAMES (SPU_RATE * 60u / 1000u)
static struct {
    SDL_AudioDeviceID device;
    FILE *wav;
    uint32_t bytes;
    uint64_t dropped;
    int failed, initialized, headless, started;
    FILE *stats;
    uint64_t clock_start, clock_frequency, samples, queue_sum, queue_min, queue_max, underruns;
    double deadline, resample_step, resample_phase;
    void (*wait)(double ms);
    int16_t previous[2];
    unsigned have_previous;
} host_audio;

static void little(uint8_t *p, uint32_t n, unsigned size) {
    unsigned i;
    for (i = 0; i < size; ++i) p[i] = (uint8_t)(n >> (i * 8));
}
static int header(void) {
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4); little(h + 4, host_audio.bytes + 36, 4);
    memcpy(h + 8, "WAVEfmt ", 8); little(h + 16, 16, 4);
    little(h + 20, 1, 2); little(h + 22, 2, 2); little(h + 24, SPU_RATE, 4);
    little(h + 28, SPU_RATE * 4, 4); little(h + 32, 4, 2); little(h + 34, 16, 2);
    memcpy(h + 36, "data", 4); little(h + 40, host_audio.bytes, 4);
    return fseek(host_audio.wav, 0, SEEK_SET) == 0 && fwrite(h, 1, 44, host_audio.wav) == 44;
}

int host_audio_init(int headless, int mute, const char *wav_path) {
    SDL_AudioSpec wanted;
    memset(&host_audio, 0, sizeof(host_audio));
    host_audio.headless = headless;
    host_audio.clock_start = SDL_GetPerformanceCounter();
    host_audio.clock_frequency = SDL_GetPerformanceFrequency();
    host_audio.resample_step = 1;
    if (wav_path) {
        host_audio.wav = fopen(wav_path, "wb");
        if (!host_audio.wav || !header()) {
            fprintf(stderr, "Audio dump %s: %s\n", wav_path, strerror(errno));
            if (host_audio.wav) fclose(host_audio.wav);
            host_audio.wav = NULL; return 0;
        }
    }
    if (headless || mute) return 1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "Audio device unavailable: %s\n", SDL_GetError()); return 1;
    }
    host_audio.initialized = 1;
    SDL_zero(wanted); wanted.freq = SPU_RATE; wanted.format = AUDIO_S16SYS;
    wanted.channels = 2; wanted.samples = SPU_RATE / 100;
    host_audio.device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
    if (!host_audio.device) { fprintf(stderr, "Audio device unavailable: %s\n", SDL_GetError()); return 1; }
    /* Start after a short preroll, so the first frame cannot underrun. */
    return 1;
}

int host_audio_stats(const char *path) {
    if (!path) return 1;
    host_audio.stats = fopen(path, "w");
    if (!host_audio.stats) return 0;
    return fprintf(host_audio.stats, "frame,wall_seconds,queue_frames,speed,resample_ratio,throttled\n") > 0;
}

void host_audio_waiter(void (*wait)(double ms)) { host_audio.wait = wait; }

void host_audio_pace(int pal, int fast) {
    uint64_t now = SDL_GetPerformanceCounter();
    double frequency = (double)host_audio.clock_frequency;
    double period = frequency * (pal ? 1.0 / 50.0 : 1001.0 / 60000.0);
    unsigned queued = host_audio.device ? SDL_GetQueuedAudioSize(host_audio.device) / 4 : 0;
    double expected = SPU_RATE * (pal ? 1.0 / 50.0 : 1001.0 / 60000.0);
    double correction = host_audio.started ? ((double)queued - TARGET_FRAMES - expected) / SPU_RATE * 0.4 : 0;
    int throttled = !host_audio.headless && !fast;
    if (correction > 0.005) correction = 0.005;
    if (correction < -0.005) correction = -0.005;
    if (!throttled) correction = 0;
    host_audio.resample_step = 1 / (1 - correction);
    if (throttled) {
        if (!host_audio.deadline) host_audio.deadline = (double)now;
        if ((double)now - host_audio.deadline > period)
            host_audio.deadline = (double)now;
        else host_audio.deadline += period / (1 - correction);
        while ((double)now < host_audio.deadline) {
            double ms = (host_audio.deadline - (double)now) * 1000 / frequency;
            if (host_audio.wait) host_audio.wait(ms);
            else SDL_Delay(ms > 2 ? (Uint32)(ms - 1) : 0);
            now = SDL_GetPerformanceCounter();
        }
    } else host_audio.deadline = 0;
    queued = host_audio.device ? SDL_GetQueuedAudioSize(host_audio.device) / 4 : 0;
    if (host_audio.device && !host_audio.started && queued >= TARGET_FRAMES) {
        SDL_PauseAudioDevice(host_audio.device, 0);
        host_audio.started = 1;
    }
    if (!host_audio.samples || queued < host_audio.queue_min) host_audio.queue_min = queued;
    if (queued > host_audio.queue_max) host_audio.queue_max = queued;
    host_audio.queue_sum += queued;
    ++host_audio.samples;
    if (host_audio.started && !queued) ++host_audio.underruns;
    if (host_audio.stats && fprintf(host_audio.stats, "%llu,%.6f,%u,%.8f,%.8f,%d\n",
        (unsigned long long)host_audio.samples, (now - host_audio.clock_start) / frequency,
        queued, 1 - correction, 1 / host_audio.resample_step, throttled) < 0) host_audio.failed = 1;
}

static unsigned resample(const int16_t *pcm, unsigned frames, int16_t *output) {
    unsigned i, count = 0, channel;
    for (i = 0; i < frames; ++i) {
        if (!host_audio.have_previous) {
            output[count * 2] = pcm[i * 2]; output[count++ * 2 + 1] = pcm[i * 2 + 1];
            host_audio.have_previous = 1; host_audio.resample_phase = host_audio.resample_step;
        } else {
            while (host_audio.resample_phase <= 1) {
                for (channel = 0; channel < 2; ++channel)
                    output[count * 2 + channel] = (int16_t)(host_audio.previous[channel] +
                        (pcm[i * 2 + channel] - host_audio.previous[channel]) * host_audio.resample_phase);
                ++count; host_audio.resample_phase += host_audio.resample_step;
            }
            host_audio.resample_phase -= 1;
        }
        host_audio.previous[0] = pcm[i * 2]; host_audio.previous[1] = pcm[i * 2 + 1];
    }
    return count;
}

int host_audio_pump(spu_t *s) {
    int16_t pcm[2048];
    int16_t device_pcm[2064];
    uint8_t bytes[4096];
    unsigned frames, i;
    if (!s) return !host_audio.failed;
    while ((frames = spu_read_frames(s, pcm, 1024)) != 0) {
        if (host_audio.wav && !host_audio.failed) {
            for (i = 0; i < frames * 2; ++i) little(bytes + i * 2, (uint16_t)pcm[i], 2);
            if (host_audio.bytes > UINT32_MAX - 36u - frames * 4u ||
                fwrite(bytes, 4, frames, host_audio.wav) != frames) {
                fprintf(stderr, "Audio dump write failed or WAV size limit exceeded\n"); host_audio.failed = 1;
            } else host_audio.bytes += frames * 4;
        }
        if (host_audio.device) {
            unsigned send = resample(pcm, frames, device_pcm);
            if (SDL_QueueAudio(host_audio.device, device_pcm, send * 4)) {
                fprintf(stderr, "Audio queue failed: %s\n", SDL_GetError());
                host_audio.failed = 1;
            }
        }
    }
    return !host_audio.failed;
}

void host_audio_recover(void) {
    if (host_audio.device) {
        SDL_PauseAudioDevice(host_audio.device,1); SDL_ClearQueuedAudio(host_audio.device);
    }
    host_audio.started = 0; host_audio.deadline = 0; host_audio.resample_step = 1;
    host_audio.resample_phase = 0; host_audio.have_previous = 0;
    memset(host_audio.previous,0,sizeof(host_audio.previous));
    host_audio.clock_start = SDL_GetPerformanceCounter();
    host_audio.samples = host_audio.queue_sum = host_audio.queue_min = host_audio.queue_max = host_audio.underruns = 0;
}
void host_audio_pause(void) {
    if (host_audio.device) SDL_PauseAudioDevice(host_audio.device,1);
}
void host_audio_resume(void) {
    host_audio.deadline=0;
    if (host_audio.device && host_audio.started) SDL_PauseAudioDevice(host_audio.device,0);
}
int host_audio_shutdown(spu_t *s) {
    int ok = host_audio_pump(s);
    if (host_audio.stats) {
        if (fclose(host_audio.stats)) ok = 0;
        host_audio.stats = NULL;
    }
    if (host_audio.samples) printf("Audio queue: samples=%llu min=%llu max=%llu mean=%.3f frames (%.3f ms), empty=%llu dropped=%llu\n",
        (unsigned long long)host_audio.samples, (unsigned long long)host_audio.queue_min,
        (unsigned long long)host_audio.queue_max, (double)host_audio.queue_sum / host_audio.samples,
        (double)host_audio.queue_sum / host_audio.samples * 1000 / SPU_RATE,
        (unsigned long long)host_audio.underruns, (unsigned long long)host_audio.dropped);
    if (host_audio.wav) {
        if (!header()) ok = 0;
        if (fclose(host_audio.wav)) ok = 0;
        host_audio.wav = NULL;
        printf("Audio dump: %u stereo frames, 44100 Hz PCM16\n", host_audio.bytes / 4);
    }
    if (host_audio.device) { SDL_CloseAudioDevice(host_audio.device); host_audio.device = 0; }
    if (host_audio.initialized) { SDL_QuitSubSystem(SDL_INIT_AUDIO); host_audio.initialized = 0; }
    if (host_audio.dropped) printf("Audio device overflow: %llu frames dropped\n", (unsigned long long)host_audio.dropped);
    return ok;
}
