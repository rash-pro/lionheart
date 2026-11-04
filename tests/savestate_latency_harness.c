/* Linked against the Release game's objects: time complete host operations,
 * including slot/directory work, title, card publication and SDL recovery. */
#include "states.h"
#include "savestate.h"
#include "hw.h"
#include "display.h"
#include "audio.h"
#include <SDL.h>
#include <assert.h>
#include <stdlib.h>
extern const entry_t generated_entries[];
extern const size_t generated_count, generated_overlay_count;
extern const code_region_t *const generated_regions[];
extern const overlay_t generated_overlays[];
extern const uint32_t generated_hash;
static double elapsed(uint64_t start) {
    return (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
}
static void request(cpu_t *c, SDL_Scancode code) {
    SDL_Event event; SDL_zero(event); event.type = SDL_KEYDOWN; event.key.keysym.scancode = code;
    assert(SDL_PushEvent(&event) == 1); host_present(c->hw->gpu); host_states_vblank(c);
    assert(savestate_pending == c);
    c->dispatch_depth = 1;
    assert(savestate_ready(c));
}
int main(int argc, char **argv) {
    cpu_t *c = malloc(sizeof(*c));
    host_input_t input = {0};
    char error[256];
    unsigned i, count;
    assert(argc == 5 && c); count = (unsigned)strtoul(argv[4],NULL,10); assert(count >= 5);
    cpu_init(c); hw_get(c); sio_init(c);
    c->entries = generated_entries; c->entry_count = generated_count; c->entry_regions = generated_regions;
    c->overlays = generated_overlays; c->overlay_count = generated_overlay_count;
    assert(cdrom_open(c,argv[1],error,sizeof(error)));
    assert(host_display_init(0,NULL,NULL) && host_audio_init(1,0,NULL));
    { const char *discs[] = {argv[1]};
      assert(host_states_init(c,argv[3],NULL,argv[2],NULL,0,discs,1,generated_hash,0,0,HOST_CARDS_SNAPSHOT));
      assert(host_states_apply(&c,&input) && !host_states_failed() && lookup(c,c->pc));
      for (i = 0; i < count; ++i) {
          uint64_t start;
          double save_ms, load_ms;
          request(c,SDL_SCANCODE_F5);
          start = SDL_GetPerformanceCounter(); assert(!savestate_boundary(c)); save_ms = elapsed(start);
          c->dispatch_depth = 0;
          request(c,SDL_SCANCODE_F7); assert(savestate_boundary(c)); c->dispatch_depth = 0;
          start = SDL_GetPerformanceCounter(); assert(host_states_apply(&c,&input)); load_ms = elapsed(start);
          assert(!host_states_failed() && lookup(c,c->pc));
          printf("Complete operation: save_ms=%.3f load_ms=%.3f\n",save_ms,load_ms);
      }
    }
    assert(host_audio_shutdown(c->hw->spu) && host_display_shutdown(c->hw->gpu));
    host_states_shutdown(); savestate_dispose(c); return 0;
}
