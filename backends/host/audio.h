#ifndef FF8_HOST_AUDIO_H
#define FF8_HOST_AUDIO_H
#include "spu.h"

/* Headless and mute disable the device, but never the WAV dump or the SPU. */
int host_audio_init(int headless, int mute, const char *wav_path);
int host_audio_pump(spu_t *);
int host_audio_stats(const char *csv_path);
void host_audio_pace(int pal, int fast);
void host_audio_recover(void);
void host_audio_pause(void);
void host_audio_resume(void);
int host_audio_shutdown(spu_t *);
#endif
