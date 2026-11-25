#include "audio.h"
#include <SDL.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define TARGET_FRAMES (SPU_RATE * 60u / 1000u)
#define WSOLA_HOP     320u
#define WSOLA_WIN     (WSOLA_HOP * 2u)
#define WSOLA_SEARCH  160u
#define WSOLA_BUF     8192u

static struct {
    SDL_AudioDeviceID device;
    FILE *wav;
    uint32_t bytes;
    uint64_t dropped;
    int failed, initialized, headless, started, fast_mode;
    FILE *stats;
    uint64_t clock_start, clock_frequency, last_pace, samples, queue_sum, queue_min, queue_max, underruns;
    double deadline, resample_step, resample_phase, speed_scale, measured_speed;
    void (*wait)(double ms);
    int16_t previous[2];
    unsigned have_previous;
    /* Pitch-preserving WSOLA time-stretcher state (active only when speed_scale > 1.05). */
    int16_t wsola_in[WSOLA_BUF * 2];
    int16_t wsola_tail[WSOLA_HOP * 2];
    double wsola_fade[WSOLA_HOP];
    unsigned wsola_count;
    int wsola_primed;
    double wsola_pos;
} host_audio;

static void wsola_reset(void) {
    host_audio.wsola_count = 0;
    host_audio.wsola_primed = 0;
    host_audio.wsola_pos = 0;
    memset(host_audio.wsola_tail, 0, sizeof(host_audio.wsola_tail));
}

static void wsola_init_window(void) {
    unsigned i;
    for (i = 0; i < WSOLA_HOP; ++i) {
        double t = (double)i / (double)WSOLA_HOP;
        /* C2-continuous smootherstep: w(t) + w(1-t) == 1 identically, no libm needed. */
        host_audio.wsola_fade[i] = t * t * t * (t * (6.0 * t - 15.0) + 10.0);
    }
}

static int16_t clamp_pcm16(double v) {
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

static double wsola_score(unsigned cand) {
    unsigned k;
    double dot = 0.0, energy = 0.0;
    for (k = 0; k < WSOLA_HOP; k += 2) {
        double a = (double)host_audio.wsola_tail[k * 2] + (double)host_audio.wsola_tail[k * 2 + 1];
        double b = (double)host_audio.wsola_in[(cand + k) * 2] + (double)host_audio.wsola_in[(cand + k) * 2 + 1];
        dot += a * b;
        energy += b * b;
    }
    /* Monotonic with dot / sqrt(energy) without requiring sqrt()/libm. */
    return (dot >= 0.0 ? dot * dot : -(dot * dot)) / (energy + 1.0);
}

static unsigned wsola_stretch(const int16_t *pcm, unsigned frames, double rate,
                              int16_t *output, unsigned max_out) {
    unsigned out_count = 0;
    if (rate < 1.0) rate = 1.0;
    if (rate > 8.0) rate = 8.0;

    if (frames >= WSOLA_BUF) {
        pcm += (frames - WSOLA_BUF) * 2;
        frames = WSOLA_BUF;
        host_audio.wsola_count = 0;
        host_audio.wsola_pos = 0;
    } else if (host_audio.wsola_count + frames > WSOLA_BUF) {
        unsigned drop = host_audio.wsola_count + frames - WSOLA_BUF;
        memmove(host_audio.wsola_in, host_audio.wsola_in + drop * 2,
                (host_audio.wsola_count - drop) * 2 * sizeof(int16_t));
        host_audio.wsola_count -= drop;
        host_audio.wsola_pos = host_audio.wsola_pos > (double)drop ? host_audio.wsola_pos - (double)drop : 0.0;
    }
    memcpy(host_audio.wsola_in + host_audio.wsola_count * 2, pcm, frames * 2 * sizeof(int16_t));
    host_audio.wsola_count += frames;

    if (!host_audio.wsola_primed) {
        if (host_audio.wsola_count < WSOLA_WIN || max_out < WSOLA_HOP) return 0;
        memcpy(output, host_audio.wsola_in, WSOLA_HOP * 2 * sizeof(int16_t));
        memcpy(host_audio.wsola_tail, host_audio.wsola_in + WSOLA_HOP * 2, WSOLA_HOP * 2 * sizeof(int16_t));
        out_count = WSOLA_HOP;
        host_audio.wsola_pos = (double)WSOLA_HOP * rate;
        host_audio.wsola_primed = 1;
    }

    while (out_count + WSOLA_HOP <= max_out) {
        unsigned center = (unsigned)host_audio.wsola_pos;
        unsigned lo = center > WSOLA_SEARCH ? center - WSOLA_SEARCH : 0;
        unsigned hi = center + WSOLA_SEARCH;
        unsigned cand, best, fine_lo, fine_hi, k;
        double best_score;

        if (hi + WSOLA_WIN > host_audio.wsola_count) break;

        /* Coarse-to-fine waveform phase alignment (step 4, then step 1). */
        best = center;
        best_score = wsola_score(center);
        for (cand = lo; cand <= hi; cand += 4) {
            double sc = wsola_score(cand);
            if (sc > best_score) { best_score = sc; best = cand; }
        }
        fine_lo = best > lo + 4 ? best - 4 : lo;
        fine_hi = best + 4 < hi ? best + 4 : hi;
        for (cand = fine_lo; cand <= fine_hi; ++cand) {
            double sc = wsola_score(cand);
            if (sc > best_score) { best_score = sc; best = cand; }
        }

        /* Constant-gain C2 overlap-add between outgoing tail and aligned grain. */
        for (k = 0; k < WSOLA_HOP; ++k) {
            double w_in = host_audio.wsola_fade[k];
            double w_out = 1.0 - w_in;
            unsigned dst = (out_count + k) * 2;
            unsigned src = (best + k) * 2;
            output[dst] = clamp_pcm16((double)host_audio.wsola_tail[k * 2] * w_out +
                                      (double)host_audio.wsola_in[src] * w_in);
            output[dst + 1] = clamp_pcm16((double)host_audio.wsola_tail[k * 2 + 1] * w_out +
                                          (double)host_audio.wsola_in[src + 1] * w_in);
        }
        out_count += WSOLA_HOP;

        memcpy(host_audio.wsola_tail, host_audio.wsola_in + (best + WSOLA_HOP) * 2,
               WSOLA_HOP * 2 * sizeof(int16_t));
        host_audio.wsola_pos += (double)WSOLA_HOP * rate;

        if (host_audio.wsola_pos > (double)(WSOLA_SEARCH + 512u)) {
            unsigned drop = (unsigned)host_audio.wsola_pos - WSOLA_SEARCH;
            if (drop > host_audio.wsola_count) drop = host_audio.wsola_count;
            memmove(host_audio.wsola_in, host_audio.wsola_in + drop * 2,
                    (host_audio.wsola_count - drop) * 2 * sizeof(int16_t));
            host_audio.wsola_count -= drop;
            host_audio.wsola_pos -= (double)drop;
        }
    }
    return out_count;
}

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
    host_audio.speed_scale = 1;
    host_audio.measured_speed = 1;
    wsola_init_window();
    wsola_reset();
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
void host_audio_speed(double scale) {
    int was_boosted = host_audio.speed_scale > 1.05;
    int now_boosted;
    if (scale < 1.0) scale = 1.0;
    if (scale > 8.0) scale = 8.0;
    now_boosted = scale > 1.05;
    host_audio.speed_scale = scale;
    host_audio.measured_speed = scale;
    host_audio.resample_step = scale;
    host_audio.deadline = 0;
    host_audio.last_pace = 0;
    if (was_boosted != now_boosted) {
        wsola_reset();
        host_audio.have_previous = 0;
        host_audio.resample_phase = 0;
    }
}

void host_audio_pace(int pal, int fast) {
    uint64_t now = SDL_GetPerformanceCounter();
    double frequency = (double)host_audio.clock_frequency;
    double guest_period_s = pal ? 1.0 / 50.0 : 1001.0 / 60000.0;
    double speed = host_audio.speed_scale > 1.0 ? host_audio.speed_scale : 1.0;
    double period = frequency * guest_period_s / speed;
    unsigned queued = host_audio.device ? SDL_GetQueuedAudioSize(host_audio.device) / 4 : 0;
    double expected = SPU_RATE * guest_period_s / speed;
    int throttled = !host_audio.headless && !fast;
    double correction = 0.0;
    host_audio.fast_mode = fast;

    if (speed > 1.05) {
        /* Track actual achieved emulation speed so heavy scenes (e.g. battle effects)
         * that dip below 180 fps never starve the audio queue into underruns. */
        if (host_audio.last_pace && now > host_audio.last_pace) {
            double dt_s = (double)(now - host_audio.last_pace) / frequency;
            if (dt_s > 0.0005 && dt_s < 0.25) {
                double inst = guest_period_s / dt_s;
                if (inst < 0.8) inst = 0.8;
                if (inst > speed * 1.25) inst = speed * 1.25;
                host_audio.measured_speed = host_audio.measured_speed * 0.85 + inst * 0.15;
            }
        }
        host_audio.last_pace = now;
        if (throttled) {
            double q_err = host_audio.started ? ((double)queued - TARGET_FRAMES - expected) / (double)SPU_RATE : 0.0;
            double rate = host_audio.measured_speed * (1.0 + q_err * 2.5);
            if (rate < 1.0) rate = 1.0;
            if (rate > speed * 1.25) rate = speed * 1.25;
            host_audio.resample_step = rate;
            correction = q_err * 0.4;
            if (correction > 0.02) correction = 0.02;
            if (correction < -0.02) correction = -0.02;
        } else {
            host_audio.resample_step = speed;
        }
    } else {
        host_audio.last_pace = 0;
        host_audio.measured_speed = 1.0;
        correction = host_audio.started ? ((double)queued - TARGET_FRAMES - expected) / SPU_RATE * 0.4 : 0;
        if (correction > 0.005) correction = 0.005;
        if (correction < -0.005) correction = -0.005;
        if (!throttled) correction = 0;
        host_audio.resample_step = 1.0 / (1 - correction);
    }
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
    int16_t device_pcm[4096];
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
            unsigned send;
            if (host_audio.speed_scale > 1.05) {
                unsigned queued = SDL_GetQueuedAudioSize(host_audio.device) / 4;
                send = wsola_stretch(pcm, frames, host_audio.resample_step, device_pcm, 2048);
                if (host_audio.started && queued > TARGET_FRAMES * 4u) send = 0;
            } else {
                unsigned queued = SDL_GetQueuedAudioSize(host_audio.device) / 4;
                send = resample(pcm, frames, device_pcm);
                if (host_audio.started && host_audio.fast_mode && queued > TARGET_FRAMES * 4u) send = 0;
            }
            if (send && SDL_QueueAudio(host_audio.device, device_pcm, send * 4)) {
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
    host_audio.started = 0; host_audio.deadline = 0; host_audio.last_pace = 0;
    host_audio.measured_speed = host_audio.speed_scale > 1.0 ? host_audio.speed_scale : 1.0;
    host_audio.resample_step = host_audio.measured_speed;
    host_audio.resample_phase = 0; host_audio.have_previous = 0;
    memset(host_audio.previous,0,sizeof(host_audio.previous));
    wsola_reset();
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
