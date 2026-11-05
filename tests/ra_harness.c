#include "ra.h"
#include "cpu.h"
#include "ra_store.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
static cpu_t cpu;
static uint64_t advance;
uint64_t __real_SDL_GetTicks64(void);
uint64_t __wrap_SDL_GetTicks64(void) { return __real_SDL_GetTicks64()+advance; }
void ra_test_advance(unsigned ms) { advance+=ms; }
void ra_test_reset(void) { host_ra_shutdown(); memset(&cpu,0,sizeof(cpu)); advance=0; }
void ra_test_frame(unsigned value) { cpu.ram[0x100]=(uint8_t)value; host_ra_frame(&cpu); }
int ra_test_state(void) { return host_ra_status()->state; }
unsigned ra_test_total(void) { return host_ra_status()->total; }
unsigned ra_test_unlocked(void) { return host_ra_status()->unlocked; }
int ra_test_failed(void) { return host_ra_status()->load_failed; }
int ra_test_unknown(void) { return host_ra_status()->unknown_game; }
const char *ra_test_error(void) { return host_ra_status()->error_id; }
const char *ra_test_hash(void) { return host_ra_status()->hash; }
int ra_test_badge(unsigned i) { host_ra_achievement_t a; return host_ra_achievement(i,&a) && a.badge!=NULL; }
int ra_test_pending(unsigned i) { host_ra_achievement_t a; return host_ra_achievement(i,&a) && a.pending; }
float ra_test_progress(unsigned i) { host_ra_achievement_t a; return host_ra_achievement(i,&a) ? a.percent : -1; }
const char *ra_test_progress_text(unsigned i) { host_ra_achievement_t a; return host_ra_achievement(i,&a) ? a.progress : ""; }
double ra_test_toast(double now) { host_ra_achievement_t a; float opacity; return host_ra_toast(&a,now,&opacity) ? opacity : -1; }
int ra_test_png(const void *bytes,size_t size) { uint8_t *image=host_ra_badge_decode(bytes,size); int ok=image!=NULL; free(image); return ok; }
