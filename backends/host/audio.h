#ifndef FF8_HOST_AUDIO_H
#define FF8_HOST_AUDIO_H
#include "spu.h"

/* Headless and mute disable the device, but never the WAV dump or the SPU. */
int host_audio_init(int headless, int mute, const char *wav_path);
int host_audio_pump(spu_t *);
int host_audio_stats(const char *csv_path);
void host_audio_pace(int pal, int fast);
/* Target speed multiplier for capped fast-forward (1.0 normal, 3.0 for 3x). */
void host_audio_speed(double scale);
/* While pacing waits for the guest's deadline: how to wait ms milliseconds
 * (at most), in place of sleeping; the deadline itself does not change. NULL
 * sleeps. Smooth motion presents on the display's clock from here. */
void host_audio_waiter(void (*wait)(double ms));
void host_audio_recover(void);
void host_audio_pause(void);
void host_audio_resume(void);
int host_audio_shutdown(spu_t *);
#endif
