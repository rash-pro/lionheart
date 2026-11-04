#define _POSIX_C_SOURCE 200809L
#include "states.h"
#include "savestate_internal.h"
#include "display.h"
#include "osd.h"
#include "audio.h"
#include "state_build.h"
#include "state_disc.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL.h>
#include <time.h>
#ifdef FF8_PLAYER
#include "disc_id.h"
#endif

static void (*recovery)(cpu_t *, unsigned);
void host_states_recovery(void (*callback)(cpu_t *, unsigned)) { recovery=callback; }

static struct {
    char *dir;
    const char *load, *save, **discs;
    unsigned count, disc, slot, requested_slot, action, failed, supported, full_disc, cards;
    uint64_t at, request_cycle;
    savestate_identity_t id;
} states;
static double milliseconds(uint64_t start) {
    return (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
}
static int directory(const char *path) {
    char *copy = strdup(path), *p;
    struct stat st;
    int ok = copy && *copy;
    if (!ok) { free(copy); return 0; }
    for (p = copy + 1; ; ++p) if (*p == '/' || !*p) {
        char saved = *p; *p = 0;
        if ((mkdir(copy,0700) && errno != EEXIST) || stat(copy,&st) || !S_ISDIR(st.st_mode)) ok = 0;
        *p = saved; if (!saved || !ok) break;
    }
    free(copy); return ok;
}
static int open_disc(cpu_t *c, unsigned disc, const uint8_t *expected, void *unused, char *error, size_t size) {
    uint8_t hash[20];
    (void)unused;
    if (!disc || disc > states.count || !states.discs[disc-1]) { snprintf(error,size,"save state needs --disc image %u",disc); return 0; }
#ifdef FF8_PLAYER
    {
        host_disc_runtime_t prepared; int ok;
        if (!host_disc_runtime_path(states.discs[disc-1],&prepared,error,size)) return 0;
        ok=cdrom_open(c,prepared.path,error,size);
        host_disc_runtime_cleanup(&prepared);
        if (!ok) return 0;
    }
#else
    if (!cdrom_open(c,states.discs[disc-1],error,size)) return 0;
#endif
    if (!host_state_disc_hash(&c->hw->cd,states.full_disc,hash) || memcmp(expected,hash,20)) {
        snprintf(error,size,"save state disc %u identity mismatch",disc); return 0;
    }
    return 1;
}
static char *slot_path(unsigned slot) {
    if (!states.dir) return NULL;
    size_t n = strlen(states.dir) + 32;
    char *p = malloc(n);
    if (p) snprintf(p,n,"%s/slot-%u.ff8s",states.dir,slot);
    return p;
}
unsigned host_states_slot(void) { return states.slot; }
int host_states_slot_available(unsigned slot,char *date,size_t capacity) {
    char *path=slot_path(slot); struct stat st; struct tm local;
    int ok=path && !stat(path,&st) && S_ISREG(st.st_mode);
    if (capacity) *date=0;
    if (ok && capacity && localtime_r(&st.st_mtime,&local)) strftime(date,capacity,"%Y-%m-%d %H:%M",&local);
    free(path); return ok;
}
void host_states_slot_step(int direction) {
    char date[32];
    states.slot=(states.slot+(direction<0 ? 9 : 1))%10;
    host_display_state_slot(states.slot);
    host_osd_event(HOST_OSD_SLOT,states.slot,host_states_slot_available(states.slot,date,sizeof(date)) ? NULL : "empty");
}
static int boundary(cpu_t *c, void *unused) {
    char error[256] = {0};
    char *allocated = NULL;
    const char *path;
    uint64_t start;
    (void)unused;
    if (states.action == 2) return 1;
    states.action = 0;
    host_display_state_pending(0);
    path = states.save ? states.save : (allocated = slot_path(states.requested_slot));
    if (!path || !directory(states.dir)) { fprintf(stderr,"Save state: cannot create state directory\n"); states.failed = 1; host_osd_event(HOST_OSD_SAVE_FAILED,states.requested_slot,"cannot create directory"); free(allocated); return 0; }
    start = SDL_GetPerformanceCounter();
    states.id.disc = states.disc;
    /* Hash the attached image, never the cue pathname or a host FILE handle. */
    if (states.disc && !host_state_disc_hash(&c->hw->cd,states.full_disc,states.id.disc_hash)) snprintf(error,sizeof(error),"cannot identify disc");
    if (*error || !savestate_write(c,path,&states.id,error,sizeof(error))) {
        fprintf(stderr,"Save state: %s\n",error); states.failed = 1;
        host_osd_event(HOST_OSD_SAVE_FAILED,states.requested_slot,error);
    } else {
        printf("Save state: %s requested=%llu actual=%llu cycle=%llu pc=%08x ",path,
            (unsigned long long)states.id.requested_vblank,(unsigned long long)c->hw->vblanks,
            (unsigned long long)c->cycles,c->pc);
#ifdef FF8_STATS
        printf("latency=%llu instructions ",(unsigned long long)(c_instructions(c) - states.id.requested_instructions));
#else
        printf("latency_cycles=%llu (instruction latency requires FF8_STATS) ",(unsigned long long)(c->cycles - states.request_cycle));
#endif
        printf("save_ms=%.3f\n",milliseconds(start));
        host_osd_event(HOST_OSD_SAVED,states.requested_slot,NULL);
    }
    states.save = NULL;
    free(allocated); return 0;
}
int host_states_init(cpu_t *c, const char *dir, const char *card, const char *load, const char *save,
                     uint64_t at, const char **discs, unsigned count, uint32_t exe, unsigned modes, int full_disc, int cards) {
    size_t n = strlen(dir ? dir : card ? card : "ff8") + 16;
    states.dir = malloc(n);
    if (!states.dir) return 0;
    if (dir) snprintf(states.dir,n,"%s",dir);
    else snprintf(states.dir,n,"%s.states",card ? card : "ff8");
    states.load = load; states.save = save; states.at = at; states.discs = discs; states.count = count; states.disc = count ? 1 : 0;
    states.full_disc = full_disc;
    states.cards = cards;
    memcpy(states.id.build,FF8_STATE_BUILD,64); states.id.exe = exe; states.id.modes = modes | (full_disc ? 16 : 0);
    states.supported = FF8_STATE_PRECISE && !(modes & 4);
    if ((load || save || at) && !states.supported) {
        fprintf(stderr,"Save states require exact precise host code and the full software SPU (omit --spu-control)\n"); return 0;
    }
    savestate_handler(boundary,NULL);
    host_states_disc(c,states.disc);
    host_display_state_slot(0);
    return 1;
}
void host_states_disc(cpu_t *c, unsigned disc) {
    states.disc = disc;
    /* Warm the identity once at image insertion, outside the operation timer. */
    if (disc && !host_state_disc_hash(&c->hw->cd,states.full_disc,states.id.disc_hash))
        fprintf(stderr,"Save states: cannot identify disc %u\n",disc);
}
void host_states_vblank(cpu_t *c) {
    unsigned action = host_input_state();
    if (states.action && c->cycles - states.request_cycle >= 2ull * HW_CPU_HZ) {
        fprintf(stderr,"%s state: timed out waiting for a safe boundary after 2 seconds of guest time (slot %u)\n",
                states.action == 1 ? "Save" : "Load",states.requested_slot);
        host_osd_event(states.action==1 ? HOST_OSD_SAVE_FAILED : HOST_OSD_LOAD_FAILED,states.requested_slot,"timed out");
        savestate_cancel(c); states.action = 0; states.save = NULL;
        host_display_state_pending(0);
    }
    if (action == 3 || action == 4) { host_states_slot_step(action==3 ? -1 : 1); return; }
    if (states.at && c->hw->vblanks >= states.at) { action = 1; states.at = 0; }
    if (!action) return;
    if (!states.supported) { fprintf(stderr,"Save states require exact precise host code and the full software SPU\n"); host_osd_event(HOST_OSD_UNAVAILABLE,0,NULL); return; }
    if (states.action) return;
    states.action = action; states.requested_slot = states.slot;
    host_display_state_pending(action);
    host_osd_event(action==1 ? HOST_OSD_SAVING : HOST_OSD_LOADING,states.requested_slot,NULL);
    states.id.requested_vblank = c->hw->vblanks; states.request_cycle = c->cycles;
#ifdef FF8_STATS
    states.id.requested_instructions = c_instructions(c);
#else
    states.id.requested_instructions = UINT64_MAX;
#endif
    savestate_request(c);
}
int host_states_loading(void) { return states.action == 2; }
static int publish_cards(cpu_t *c) {
    unsigned i;
    if (!directory(states.dir)) return 0;
    for (i = 0; i < 2; ++i) if (sio_card(c,i)) {
        size_t n = strlen(states.dir) + 48;
        char *path = malloc(n);
        int fd, ok;
        FILE *f;
        if (!path) return 0;
        snprintf(path,n,"%s/restored-card%u-XXXXXX",states.dir,i+1);
        fd = mkstemp(path);
        f = fd < 0 ? NULL : fdopen(fd,"w+b");
        ok = f && ss_card_publish(sio_card(c,i),f);
        if (!ok) { if (f) fclose(f); else if (fd >= 0) close(fd); unlink(path); free(path); return 0; }
        printf("Restored card %u (private copy): %s\n",i+1,path);
        free(path);
    }
    return 1;
}
static void live_cards(cpu_t *next, cpu_t *current) {
    unsigned i, changed = 0;
    for (i = 0; i < 2; ++i) {
        uint8_t saved[20], live[20];
        memcard_t *a = sio_card(next,i), *b = sio_card(current,i);
        ss_card_hash(a,saved); ss_card_hash(b,live);
        if (!!a != !!b || memcmp(saved,live,20)) {
            unsigned j;
            changed |= 1u << i;
            fprintf(stderr,"Warning: live memory card %u differs from save state (state=",i+1);
            if (a) for (j = 0; j < 20; ++j) fprintf(stderr,"%02x",saved[j]);
            else fprintf(stderr,"absent");
            fprintf(stderr,", live=");
            if (b) for (j = 0; j < 20; ++j) fprintf(stderr,"%02x",live[j]);
            else fprintf(stderr,"absent");
            fprintf(stderr,"); re-open the game's save/load menu to re-read it\n");
        }
    }
    ss_sio_live_cards(next,current,changed);
    if (changed) host_osd_event(HOST_OSD_CARD,0,NULL);
}
int host_states_apply(cpu_t **current, host_input_t *input) {
    cpu_t *next;
    char *allocated = NULL, error[256];
    const char *path = states.load;
    uint64_t start;
    int missing=0; struct stat st;
    savestate_identity_t id = states.id;
    if (!path && states.action != 2) return 0;
    if (!path) path = allocated = slot_path(states.requested_slot);
    states.action = 0;
    host_input_pause(1);
    start = SDL_GetPerformanceCounter();
    missing=path && stat(path,&st) && errno==ENOENT;
    next = path ? savestate_read(*current,path,&id,open_disc,NULL,error,sizeof(error)) : NULL;
    if (next && states.cards == HOST_CARDS_SNAPSHOT && !publish_cards(next)) {
        savestate_dispose(next); next = NULL; snprintf(error,sizeof(error),"cannot publish private card copies");
    }
    if (!next) {
        fprintf(stderr,"Load state: %s\n",path ? error : "out of memory");
        if (states.load) states.failed = 1;
        host_osd_event(missing ? HOST_OSD_NO_STATE : HOST_OSD_LOAD_FAILED,states.requested_slot,missing ? NULL : path ? error : "out of memory");
    } else {
        host_osd_event(HOST_OSD_LOADED,states.requested_slot,NULL);
        if (states.cards == HOST_CARDS_LIVE) live_cards(next,*current);
        host_audio_recover();
        savestate_dispose(*current); *current = next; states.disc = id.disc;
        if (recovery) recovery(next,id.disc);
        host_display_recover(next->hw->gpu,next->hw->vblanks);
        if (input) {
            unsigned port;
            ss_sio_input(next,input->analog_pad,&input->scripted);
            for (port = 0; port < 2; ++port) input->controller_id[port] = host_input_controller_id(port);
        }
        printf("Load state: %s VBlank=%llu cycle=%llu pc=%08x load_ms=%.3f native=%s\n",path,
            (unsigned long long)next->hw->vblanks,(unsigned long long)next->cycles,next->pc,milliseconds(start),lookup(next,next->pc) ? "yes" : "fallback");
    }
    host_input_pause(0);
    host_display_state_pending(0);
    free(allocated); states.load = NULL;
    return 1;
}
int host_states_failed(void) { return states.failed; }
void host_states_shutdown(void) { recovery=NULL; savestate_handler(NULL,NULL); host_state_disc_clear(); free(states.dir); memset(&states,0,sizeof(states)); }
