/* Queue preservation and preroll across a presentation-only modal pause. */
#include <SDL.h>
#include "../backends/host/audio.c"
#include <assert.h>

int main(void) {
    spu_t *spu=spu_create();
    uint64_t cycles=0; unsigned queued;
    assert(spu && host_audio_init(0,0,NULL) && host_audio.device);
    cycles+=564480; spu_advance(spu,cycles); assert(host_audio_pump(spu));
    queued=SDL_GetQueuedAudioSize(host_audio.device);
    assert(queued>0 && queued<TARGET_FRAMES*4);
    host_audio_pause(); SDL_Delay(100);
    assert(SDL_GetQueuedAudioSize(host_audio.device)==queued);
    host_audio_resume();
    assert(SDL_GetAudioDeviceStatus(host_audio.device)==SDL_AUDIO_PAUSED);
    /* Fill preroll while paused. The full-suite runner also compiles on other
     * cores, so wall-clock playback bounds belong to verify_pc16.py audio. */
    for (unsigned i=0;i<4;++i) {
        cycles+=564480; spu_advance(spu,cycles);
        assert(host_audio_pump(spu));
    }
    queued=SDL_GetQueuedAudioSize(host_audio.device)/4;
    assert(queued>=TARGET_FRAMES && queued<SPU_RATE*120/1000);
    assert(SDL_GetAudioDeviceStatus(host_audio.device)==SDL_AUDIO_PAUSED);
    host_audio_pace(0,1);
    assert(SDL_GetAudioDeviceStatus(host_audio.device)==SDL_AUDIO_PLAYING);
    host_audio_pause();
    cycles+=564480; spu_advance(spu,cycles); assert(host_audio_pump(spu));
    queued=SDL_GetQueuedAudioSize(host_audio.device);
    assert(queued>0);
    SDL_Delay(180);
    assert(SDL_GetAudioDeviceStatus(host_audio.device)==SDL_AUDIO_PAUSED);
    assert(SDL_GetQueuedAudioSize(host_audio.device)==queued);
    host_audio_resume();
    assert(SDL_GetAudioDeviceStatus(host_audio.device)==SDL_AUDIO_PLAYING);
    assert(host_audio.dropped==0);
    assert(host_audio_shutdown(spu)); spu_destroy(spu);
    puts("pause retained queued bytes; preroll stayed paused; resumed playback stayed started");
    return 0;
}
