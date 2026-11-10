#include "toolchain.h"
#include "cpu.h"
#ifdef FF8_PLAYER
#include "player_catalog.h"
#include "player_paths.h"
#include "disc_identity.h"
#else
#include "generated.h"
#endif
#include "disc_id.h"
#include "hw.h"
#include "display.h"
#include "osd.h"
#include "screenshot.h"
#ifdef FF8_VULKAN
#include "vulkan_renderer.h"
#include "movies.h"
#endif
#include "sio.h"
#include "audio.h"
#include "input.h"
#include "options.h"
#ifdef FF8_GUI
#include "gui.h"
#define HOST_GUI_CLEANUP() host_gui_shutdown()
#ifndef FF8_PLAYER
#include "player_paths.h"
#endif
#else
#define HOST_GUI_CLEANUP() ((void)0)
#endif
#include "states.h"
#include "state_bridge.h"
#include "achievements.h"
#include "ra.h"
#include "player_paths.h"
#ifdef FF8_PGXP
#include "pgxp.h"
#endif
#include "native_modules.h"
#ifdef FF8_PLAYER
#include "native_player.h"
#endif
#include <errno.h>
#include <limits.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t max_vblanks;
static int log_events;
static int fast;
static const char *disc_paths[4];
static unsigned disc_count;
#ifndef FF8_PLAYER
static unsigned native_pace_count, native_pace_vblanks;
#else
/* Hidden: drive and observe the native build headless. */
static int native_off, native_status, native_wait, native_only;
static unsigned native_workers;
#endif
static host_input_t input;
static host_state_bridge_t *state_bridge;
static host_achievements_t *achievements;
static int pgxp_mode;
#ifdef FF8_PLAYER
static int player_disc_insert(cpu_t *c, const char *path, int swap, char *error, size_t size) {
    host_disc_runtime_t prepared;
    int ok;
    if (!host_disc_runtime_path(path,&prepared,error,size)) return 0;
    ok = swap ? cdrom_swap(c,prepared.path,error,size) : cdrom_open(c,prepared.path,error,size);
    host_disc_runtime_cleanup(&prepared);
    return ok;
}
#define HOST_DISC_INSERT(c,path,swap,error,size) player_disc_insert(c,path,swap,error,size)
#else
#define HOST_DISC_INSERT(c,path,swap,error,size) ((swap) ? cdrom_swap(c,path,error,size) : cdrom_open(c,path,error,size))
#endif
#ifdef FF8_GUI
typedef struct {
    host_options_t *options;
    int playing;
} gui_session_t;
static void gui_hotkeys(const host_options_t *o) {
    unsigned d, mask = 0, inserted = host_display_disc_value();
    for (d=0;d<4;++d) if (disc_paths[d]) mask |= 1u<<d;
    host_display_hotkeys(o->pad_hotkey,disc_count);
    host_display_disc_mask(mask); host_display_disc(inserted);
}
static void gui_changed(void *context, const char *key, int overlay) {
    gui_session_t *session = context;
    host_options_t *o = session->options;
    if (!strcmp(key,"fullscreen") || !strcmp(key,"scale-mode") || !strcmp(key,"filter"))
        host_display_configure(o->fullscreen,o->scale_mode,o->linear);
    else if (!strcmp(key,"crt")) host_display_crt(o->crt);
    else if (!strcmp(key,"osd")) host_osd_configure(o->osd,o->osd_tests);
    else if (!strcmp(key,"ra")) host_ra_enable(o->ra);
    else if (!strcmp(key,"ra-notify")) host_ra_notify(o->ra_notify);
    else if (!strcmp(key,"screenshot-dir")) host_screenshot_configure(o->screenshot_dir,o->screenshot_at);
    else if (!strcmp(key,"rumble")) input.rumble = o->rumble;
    else if (!strcmp(key,"pad-hotkey")) gui_hotkeys(o);
    else if (overlay && !strncmp(key,"disc",4) && key[4]>='2' && key[4]<='4' && !key[5]) {
        unsigned d, changed = (unsigned)(key[4]-'1');
        disc_paths[changed] = *o->discs[changed] ? o->discs[changed] : NULL;
        disc_count = 0;
        for (d=0;d<4;++d) if (disc_paths[d]) disc_count = d+1;
        gui_hotkeys(o);
    }
}
static void gui_action(void *context, unsigned scancode) {
    gui_session_t *session = context;
    host_display_gui_action(scancode);
    if (session->playing && (scancode==SDL_SCANCODE_F6 || scancode==SDL_SCANCODE_F8)) {
        unsigned action = host_input_state();
        if (action==3 || action==4) host_states_slot_step(action==3 ? -1 : 1);
    }
}
static unsigned gui_state_slot(void *context) {
    return ((gui_session_t *)context)->playing ? host_states_slot() : 0;
}
static int gui_state_available(void *context, unsigned slot, char *date, size_t size) {
    return ((gui_session_t *)context)->playing && host_states_slot_available(slot,date,size);
}
static unsigned gui_disc(void *context) { (void)context; return host_display_disc_value(); }
static host_gui_result_t gui_loop(void *context, int shell) { (void)context; return host_display_gui_loop(shell); }
static const char *gui_device(void *context) { (void)context; return host_display_gui_device(); }
static const char *gui_controller(void *context) { (void)context; return host_display_gui_controller(); }
static int gui_controller_type(void *context) { (void)context; return host_display_gui_controller_type(); }
static int gui_fullscreen_exclusive(void *context) { (void)context; return host_display_gui_fullscreen_exclusive(); }
static host_gui_services_t gui_services(gui_session_t *session) {
    host_gui_services_t services = {session,gui_changed,gui_action,gui_state_slot,gui_state_available,
        gui_disc,gui_loop,gui_device,gui_controller,gui_controller_type,gui_fullscreen_exclusive};
    return services;
}
static host_gui_result_t gui_shell_display(host_options_t *o) {
    host_gui_result_t result;
    do {
#ifdef FF8_VULKAN
        int vk = o->renderer;
        if (!host_gpu_configure(vk ? "vulkan" : "soft",vk ? (unsigned)o->internal_scale : 1,
                                vk ? o->true_color : 0,vk ? (unsigned)o->msaa : 1,NULL,NULL)) return HOST_GUI_ERROR;
        host_gpu_present_capture(NULL,(unsigned)o->present_size,(unsigned)(o->present_size>>32));
#else
        if (o->renderer) return HOST_GUI_ERROR;
#endif
        if (!host_display_init(0,NULL,NULL)) { host_display_shutdown(NULL); return HOST_GUI_ERROR; }
        host_display_gui_audio(host_audio_pause,host_audio_resume);
#ifdef FF8_VULKAN
        if (!host_display_gpu(NULL)) { host_display_shutdown(NULL); return HOST_GUI_ERROR; }
#endif
        host_display_present_capture(NULL,(unsigned)o->present_size,(unsigned)(o->present_size>>32));
        if (!host_display_configure(o->fullscreen,o->scale_mode,o->linear) || !host_display_gui_attach()) {
            host_display_shutdown(NULL); return HOST_GUI_ERROR;
        }
        host_display_crt(o->crt);
        result = host_gui_shell();
        if (!host_display_shutdown(NULL)) result = HOST_GUI_ERROR;
    } while (result==HOST_GUI_RECREATE);
    return result;
}
#endif
static void restored(cpu_t *c, unsigned disc) {
#ifdef FF8_PGXP
    pgxp_bind(c->hw->gpu,(pgxp_mode_t)pgxp_mode);
#else
    (void)pgxp_mode;
#endif
#ifdef FF8_VULKAN
    host_movies_restore(c,disc);
#else
    (void)disc;
#endif
    if (state_bridge) host_state_bridge_restore(state_bridge,c,c->hw->vblanks);
    if (disc) host_display_disc(disc);
#ifdef FF8_PLAYER
    if (disc) host_native_disc(disc);
#endif
    if (disc && disc<=disc_count && disc_paths[disc-1]) host_ra_disc(disc_paths[disc-1]);
    host_ra_reset();
    /* Local unlock progress lives outside the guest session and survives loads. */
}

static void trace(void *context, const char *line) {
    if (log_events && ((!strncmp(line, "MMIO ", 5) &&
        !strstr(line, "address=1f801040 ") &&
        !(line[5] == 'W' && strstr(line, "address=1f80104a "))) || !strncmp(line, "MISS ", 5) ||
        !strncmp(line, "BIOS B0:17 ", 11))) return;
    fprintf((FILE *)context, "%s\n", line);
}

static void present_frame(cpu_t *c, void *context) {
    (void)context;
    if (!host_audio_pump(c->hw->spu)) cpu_stop(c, "host audio dump failed");
    host_present(c->hw->gpu);
    host_audio_pace(!!(gpu_status(c->hw->gpu) & (1u << 20)), fast || host_input_fast());
    host_input_sync(c, &input);
    if (state_bridge) host_state_bridge_frame(state_bridge, c, c->hw->vblanks);
    if (achievements) host_achievements_frame(achievements, c);
    host_ra_frame(c);
    {
        unsigned disc = sio_input_disc(c), live = host_input_disc();
        char error[256];
        if (!disc) disc = live;
        if (disc) {
            if (!live) host_osd_event(HOST_OSD_DISC,disc,NULL);
            if (disc > disc_count || !disc_paths[disc - 1]) {
                char message[96];
                snprintf(message,sizeof(message),"Disc %u missing: supply its image",disc);
                host_osd_post("Disc required",message,1);
                fprintf(stderr, "Disc %u missing; waiting at the game's prompt\n", disc);
            }
            else if (!HOST_DISC_INSERT(c, disc_paths[disc - 1], 1, error, sizeof(error)))
                cpu_stop(c, "disc swap failed: %s", error);
            else {
                host_states_disc(c,disc);
#ifdef FF8_PLAYER
                host_native_disc(disc);
#endif
                host_display_disc(disc);
                host_ra_disc(disc_paths[disc-1]);
#ifdef FF8_VULKAN
                host_movies_bind(c,disc);
#endif
                printf("Disc swap: image=%u VBlank=%llu (lid open/close)\n", disc, (unsigned long long)c->hw->vblanks);
            }
        }
    }
    host_states_vblank(c);
    if (!host_display_running()) cpu_stop(c, "host display closed or failed");
    if (c->overlay_profile) c_profile_checkpoint(c, c->hw->vblanks);
    c_perf_checkpoint(c, c->hw->vblanks);
    if (c->hw->vblanks % 30 == 0 && getenv("FF8_PERF_STATS")) c_perf_summary(c, c->hw->vblanks);
    if (max_vblanks && c->hw->vblanks >= max_vblanks) cpu_stop(c, "VBlank limit reached");
#ifndef FF8_PLAYER
    if (native_pace_vblanks && !(c->hw->vblanks % native_pace_vblanks) && native_modules_remaining())
        native_modules_feed(native_pace_count);
#endif
}

#ifdef FF8_PLAYER
static int load_exe(cpu_t *c, const char *path) {
    uint8_t *data = NULL;
    size_t size = 0;
    char error[512];
    uint32_t stack;
    int ok;
    if (path) {
        FILE *file = fopen(path,"rb");
        if (!file) { fprintf(stderr,"%s: %s\n",path,strerror(errno)); return 0; }
        data = malloc(HOST_EXE_FILE_SIZE);
        if (!data) { fclose(file); fprintf(stderr,"Out of memory reading EXE %s\n",path); return 0; }
        ok = 1;
        if (ok) {
            size = fread(data,1,HOST_EXE_FILE_SIZE,file);
            ok = size == HOST_EXE_FILE_SIZE && fgetc(file) == EOF && !ferror(file);
        }
        fclose(file);
        if (!ok) { free(data); fprintf(stderr,"FF8 USA EXE file size/read check failed\n"); return 0; }
    } else if (!host_disc_exe(disc_paths[0],&data,&size,error,sizeof(error))) {
        fprintf(stderr,"%s\n",error); return 0;
    }
    if (!host_exe_check(data,size,error,sizeof(error))) { free(data); fprintf(stderr,"%s\n",error); return 0; }
    memcpy(c->ram + (HOST_EXE_BASE & 0x1fffffffu),data + 2048,HOST_EXE_SIZE);
    c->pc = read_le(data + 0x10); c->r[28] = read_le(data + 0x14);
    stack = read_le(data + 0x30);
    c->r[29] = stack ? stack + read_le(data + 0x34) : 0x801ffff0u;
    c->r[30] = c->r[29]; free(data);
    mem_write32(c,0x80001018,0x80001800);
    return 1;
}
#else
static int load_exe(cpu_t *c, const char *path) {
    uint8_t header[0x800];
    uint32_t base, size, i, hash = 2166136261u, physical, stack;
    FILE *file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return 0; }
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "PS-X EXE", 8)) {
        fprintf(stderr, "Invalid PS-X EXE header\n"); fclose(file); return 0;
    }
    base = read_le(header + 0x18); size = read_le(header + 0x1c); physical = base & 0x1fffffffu;
    if (physical >= RAM_SIZE || size > RAM_SIZE - physical || base != generated_base || size != generated_size) {
        fprintf(stderr, "EXE text does not match generated image range\n"); fclose(file); return 0;
    }
    if (fread(c->ram + physical, 1, size, file) != size) { fprintf(stderr, "Truncated EXE\n"); fclose(file); return 0; }
    fclose(file);
    for (i = 0; i < size; ++i) hash = (hash ^ c->ram[physical + i]) * 16777619u;
    if (hash != generated_hash) { fprintf(stderr, "EXE content does not match generated C; regenerate first\n"); return 0; }
    c->pc = read_le(header + 0x10); c->r[28] = read_le(header + 0x14);
    stack = read_le(header + 0x30);
    c->r[29] = stack ? stack + read_le(header + 0x34) : 0x801ffff0u;
    c->r[30] = c->r[29];
    /* Writable compatibility storage for Psy-Q's BIOS exception patches. */
    mem_write32(c, 0x80001018, 0x80001800);
    return 1;
}
#endif

static int by_count(const void *a, const void *b) {
    const mmio_stat_t *x = a, *y = b;
    uint64_t nx = x->reads + x->writes, ny = y->reads + y->writes;
    return nx < ny ? 1 : nx > ny ? -1 : x->address < y->address ? -1 : x->address != y->address;
}

static void summary(cpu_t *c) {
    unsigned i, t, n;
    static const char *tables[] = {"A0", "B0", "C0", "SYS"};
    printf("Stop: %s\nLast PC: %08x\n", c->stop_reason, c->pc);
    printf("Counted instructions: %llu\n", (unsigned long long)c_instructions(c));
    hw_summary(c, stdout);
    c_perf_summary(c, c->hw->vblanks);
    printf("Dispatch regions: main=%llu shared=%llu menu=%llu other=%llu\n",
           (unsigned long long)c->dispatch_regions[0], (unsigned long long)c->dispatch_regions[1],
           (unsigned long long)c->dispatch_regions[2], (unsigned long long)c->dispatch_regions[3]);
#ifndef FF8_STATS
    printf("Recompiler statistics disabled (build with FF8_STATS=ON to count)\n");
#endif
    printf("Instructions: interpreter=%llu recompiled=%llu\nRecompiled function entries: %llu\nDispatch misses: %llu\nGTE stubs: %llu\n",
           (unsigned long long)c->interp_instructions, (unsigned long long)c->recomp_instructions,
           (unsigned long long)c->recomp_functions, (unsigned long long)c->dispatch_misses,
           (unsigned long long)c->gte_commands);
    qsort(c->mmio_stats, c->mmio_count, sizeof(c->mmio_stats[0]), by_count);
    printf("MMIO (address width reads writes last):\n");
    for (i = 0; i < c->mmio_count; ++i) {
        mmio_stat_t *s = &c->mmio_stats[i];
        printf("  %08x %u %llu %llu %08x\n", s->address, s->width,
               (unsigned long long)s->reads, (unsigned long long)s->writes, s->last);
    }
    printf("BIOS (table:function count):\n");
    for (t = 0; t < 4; ++t) for (n = 0; n < 256; ++n)
        if (c->bios_counts[t][n]) printf("  %s:%02x %llu\n", tables[t], n, (unsigned long long)c->bios_counts[t][n]);
    printf("Call stack (entry return):\n");
    for (i = 0; i < c->depth; ++i) printf("  %08x %08x\n", c->stack[i].pc, c->stack[i].ret);
}

static int write_word(FILE *file, uint32_t value) {
    uint8_t bytes[4];
    unsigned i;
    for (i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
    return fwrite(bytes, 1, 4, file) == 4;
}

static int dump_state(cpu_t *c, const char *path) {
    unsigned i;
    int ok;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    ok = fwrite("FF8STATE1", 1, 9, file) == 9;
    ok &= write_word(file, c->pc);
    for (i = 0; i < 32; ++i) ok &= write_word(file, c->r[i]);
    ok &= write_word(file, c->hi); ok &= write_word(file, c->lo);
    ok &= write_word(file, c->load_reg); ok &= write_word(file, c->load_value);
    ok &= write_word(file, c->branch_pending);
    ok &= write_word(file, c->branch_pending ? c->branch_target : 0);
    for (i = 0; i < 32; ++i) ok &= write_word(file, c->cop0[i]);
    for (i = 0; i < 32; ++i) ok &= write_word(file, c->gte_data[i]);
    for (i = 0; i < 32; ++i) ok &= write_word(file, c->gte_ctrl[i]);
    ok &= fwrite(c->ram, 1, sizeof(c->ram), file) == sizeof(c->ram);
    ok &= fwrite(c->scratch, 1, sizeof(c->scratch), file) == sizeof(c->scratch);
    ok &= fwrite(c->mmio, 1, sizeof(c->mmio), file) == sizeof(c->mmio);
    if (fclose(file)) ok = 0;
    return ok;
}

static void run_guest(cpu_t *c) {
    c->depth = c->dispatch_depth = 0;
    c->stop_armed = 1;
    if (!setjmp(c->stop)) {
        c_enter(c, c->pc, 0xffffffffu);
        dispatch(c, c->pc);
        if (!c->halted && !host_states_loading()) cpu_stop(c, "entry point returned");
    }
    c->stop_armed = 0;
}

#ifdef FF8_PLAYER
#undef main
#define main host_player_main
#endif
static int toolchain_progress(void *user, host_tc_phase phase, uint64_t done, uint64_t total) {
    (void)user;
    fprintf(stderr,"toolchain %d %llu/%llu\n",(int)phase,(unsigned long long)done,(unsigned long long)total);
    return 0;
}
static int toolchain_command(int argc, char **argv) {
    char data[4096],exe[4096]; host_tc_error error;
    host_tc_manifest manifest=*host_tc_pinned();
    const char *url=NULL,*archive=NULL;
    if(!host_player_data(data,sizeof(data))) return 2;
    if(argc==4 && !strcmp(argv[2],"--url")) {url=argv[3];manifest.urls=&url;manifest.url_count=1;}
    else if(argc==3) archive=argv[2];
    else if(argc!=2) return 2;
    if(!host_tc_install(data,&manifest,archive,toolchain_progress,NULL,&error)) {
        fprintf(stderr,"toolchain: %s\n",error.reason);return 1;
    }
    if(!host_tc_executable(data,host_tc_pinned(),exe,sizeof(exe))) return 1;
    fprintf(stderr,"toolchain installed: %s\n",exe);return 0;
}
int main(int argc, char **argv) {
    if(argc>1 && !strcmp(argv[1],"--toolchain-install")) return toolchain_command(argc,argv);
    cpu_t *c;
    FILE *log;
    const char *log_path =
#ifdef _WIN32
        "NUL";
#else
        "/dev/null";
#endif
    const char *state_path = NULL;
    const char *card_paths[2] = {NULL, NULL}, *input_script = NULL;
    const char *frame_path = NULL, *vram_path = NULL;
    const char *audio_path = NULL;
    const char *audio_stats_path = NULL;
    const char *spu_path = NULL;
    const char *savestate_dir = NULL, *load_state = NULL, *save_state = NULL;
    uint64_t save_at = 0;
    int savestate_full_disc = 0;
    const char *bridge_port = NULL, *achievement_path = NULL, *achievement_progress = NULL;
    int mute = 0;
    int headless = 0;
    const char *renderer = "soft", *scaled_frames = NULL, *gpu_timings = NULL, *presented_frames = NULL;
    const char *movies = NULL, *movie_stats = NULL;
    unsigned internal_scale = 1, msaa = 1;
    int true_color = 0;
    const char *pgxp = "off";
    int legacy_overlays = 0;
    int spu_control = 0;
    overlay_t *legacy_registry = NULL;
    unsigned frame_step = 1;
    uint64_t frames_from = 0;
    int i, result = 0;
    host_options_t options;
    char data_dir[4096] = {0};
#ifdef FF8_GUI
    int gui_allowed = 0, gui_configured = 0, startup_problem = 0;
    host_gui_pane_t startup_pane = HOST_GUI_DISCS;
    char startup_detail[512] = {0};
    gui_session_t gui_session = {&options,0};
    host_gui_services_t services = gui_services(&gui_session);
#endif
    const char *exe_path = argc > 1 ? argv[1] : NULL;
    int option_start = 2;
#ifdef FF8_PLAYER
    char default_card[4096] = {0}, default_states[4096] = {0};
    if (argc < 2 || !strncmp(argv[1],"--",2)) { exe_path = NULL; option_start = 1; }
    else {
        size_t n = strlen(argv[1]);
        if (n >= 4 && (!SDL_strcasecmp(argv[1]+n-4,".cue") || !SDL_strcasecmp(argv[1]+n-4,".bin"))) {
            disc_paths[disc_count++] = argv[1]; exe_path = NULL;
        }
    }
#endif
    host_options_defaults(&options);
#ifndef FF8_PLAYER
    if (argc < 2) {
        fprintf(stderr, "Save states: F5 save, F7 load, F6/F8 slot; --savestate-dir DIR --load-state PATH --save-state PATH --save-at VBLANK\n");
        fprintf(stderr, "State cards: --savestate-cards live|snapshot (default live; snapshot uses private copies)\n");
        fprintf(stderr, "Disc identity: --savestate-verify-disc quick|full (default quick, cached at image insertion)\n");
        fprintf(stderr, "GPU options: --renderer soft|vulkan --internal-scale 1..8 --true-color --msaa 1|2|4|8 --pgxp off|vertex|perspective --dump-scaled-frames DIR --gpu-timings CSV\n");
        fprintf(stderr, "Movie options: --movies DIR --movie-stats CSV (Vulkan only; original guest audio)\n");
        fprintf(stderr, "Input options: --card1 PATH --card2 PATH --input-script FILE\n");
        fprintf(stderr, "Controller options: --pad digital|dualshock --rumble 0|1 (scripts and keyboard stay digital)\n");
        fprintf(stderr, "Presentation: --fullscreen [0|1] (F11) --windowed --scale-mode aspect|integer|stretch|aspect-crop --filter nearest|linear --config PATH\n");
        fprintf(stderr, "Controller hotkeys: --pad-hotkey guide|off (default guide); Guide + R1/L1 save/load, Left/Right slot, R2 fast-forward, Triangle CRT, Square screenshot, Select disc, Start fullscreen\n");
        fprintf(stderr, "Screenshots: F12 or Guide + Square; --screenshot-dir PATH (default XDG Pictures/FF8)\n");
        fprintf(stderr, "Diagnostic: --screenshot-at FRAME (repeatable, presentation frame starting at zero)\n");
        fprintf(stderr, "Notifications: --osd 0|1 (default 1)\n");
        fprintf(stderr, "CRT (Vulkan): --crt off|pvm|tv|arcade (F9 cycles) --dump-presented-frames DIR --present-size WxH\n");
        fprintf(stderr, "Local telemetry: --state-bridge PORT (0 picks a port), --achievements PATCH.json [--achievement-progress PATH]\n");
        fprintf(stderr, "Audio options: --mute --dump-audio PATH.wav --audio-stats PATH.csv --fast (or hold Tab)\n");
        fprintf(stderr, "SPU validation: --spu-control (exact guest state, raw CD output only)\n");
        fprintf(stderr, "Capture options: --max-vblanks N --frame-step N --frames-from N --log-events\n");
        fprintf(stderr, "Disc options: --disc PATH (up to four, in disc order); F1..F4 or scripted DISC1..DISC4 at the game's prompt\n");
        fprintf(stderr, "Usage: %s EXE [--disc CUE_OR_BIN] [--log PATH] [--stall N] [--max-instructions N] [--interpreter] [--dump-state PATH] [--headless] [--dump-frames DIR] [--dump-vram PATH]\n", argv[0]); return 2;
    }
#endif
#ifndef FF8_PLAYER
    const char *native_module_dir = NULL;
#endif
    if (!host_options_load(&options, argc, argv)) return 2;
    c = malloc(sizeof(*c));
    if (!c) { fprintf(stderr, "Cannot allocate guest CPU\n"); return 2; }
    cpu_init(c);
#ifdef FF8_PLAYER
    c->stall_limit = c->instruction_limit = 0;
    c->interpreter_only = 1;
#endif
    for (i = option_start; i < argc; ++i) {
        int setting = host_options_cli(&options, argc, argv, &i);
        if (setting < 0) { free(c); return 2; }
        if (setting) continue;
        if (!strcmp(argv[i], "--interpreter")) {
            c->interpreter_only = 1;
#ifdef FF8_PLAYER
            native_off = 1;
#endif
        }
#ifdef FF8_PLAYER
        else if (argv[i][0] != '-' && !exe_path) {
            size_t n = strlen(argv[i]);
            if (n >= 4 && (!SDL_strcasecmp(argv[i]+n-4,".cue") || !SDL_strcasecmp(argv[i]+n-4,".bin"))) {
                if (disc_count==4) { fprintf(stderr,"At most four --disc images are supported\n"); free(c); return 2; }
                disc_paths[disc_count++] = argv[i];
            } else exe_path = argv[i];
        }
#endif
#ifdef FF8_PLAYER
        else if (!strcmp(argv[i], "--native-off")) native_off = 1;
        else if (!strcmp(argv[i], "--native-status")) native_status = 1;
        else if (!strcmp(argv[i], "--native-wait")) native_wait = 1;
        else if (!strcmp(argv[i], "--native-build-only")) native_only = native_wait = 1;
        else if (!strcmp(argv[i], "--native-workers") && i + 1 < argc) {
            char *end;
            unsigned long count = strtoul(argv[++i], &end, 10);
            if (*end || !count || count > 256) { fprintf(stderr, "Invalid worker count: %s\n", argv[i]); free(c); return 2; }
            native_workers = (unsigned)count;
        }
#endif
#ifndef FF8_PLAYER
        else if (!strcmp(argv[i], "--native-modules") && i + 1 < argc) native_module_dir = argv[++i];
        else if (!strcmp(argv[i], "--native-modules-pace") && i + 1 < argc) {
            char *end;
            unsigned long count = strtoul(argv[++i], &end, 10), vblanks = 0;
            if (*end == ':') vblanks = strtoul(end + 1, &end, 10);
            if (*end || !count || !vblanks || count > UINT_MAX || vblanks > UINT_MAX) { fprintf(stderr, "Invalid module pace: %s\n", argv[i]); free(c); return 2; }
            native_pace_count = (unsigned)count; native_pace_vblanks = (unsigned)vblanks;
        }
#endif
        else if (!strcmp(argv[i], "--legacy-overlays")) legacy_overlays = 1;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-state") && i + 1 < argc) state_path = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) {
            if (disc_count == 4) { fprintf(stderr, "At most four --disc images are supported\n"); free(c); return 2; }
            disc_paths[disc_count++] = argv[++i];
        }
        else if (!strcmp(argv[i], "--card1") && i + 1 < argc) card_paths[0] = argv[++i];
        else if (!strcmp(argv[i], "--card2") && i + 1 < argc) card_paths[1] = argv[++i];
        else if (!strcmp(argv[i], "--input-script") && i + 1 < argc) input_script = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--mute")) mute = 1;
        else if (!strcmp(argv[i], "--spu-control")) spu_control = 1;
        else if (!strcmp(argv[i], "--fast")) fast = 1;
        else if (!strcmp(argv[i], "--log-events")) log_events = 1;
        else if (!strcmp(argv[i], "--dump-audio") && i + 1 < argc) audio_path = argv[++i];
        else if (!strcmp(argv[i], "--audio-stats") && i + 1 < argc) audio_stats_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-spu") && i + 1 < argc) spu_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-frames") && i + 1 < argc) frame_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc) vram_path = argv[++i];
        else if ((!strcmp(argv[i], "--stall") || !strcmp(argv[i], "--max-instructions") ||
                  !strcmp(argv[i], "--max-vblanks") || !strcmp(argv[i], "--frame-step") ||
                  !strcmp(argv[i], "--frames-from")) && i + 1 < argc) {
            const char *option = argv[i];
            char *end;
            unsigned long long value;
            errno = 0; value = strtoull(argv[++i], &end, 10);
            if (errno || !argv[i][0] || argv[i][0] == '-' || *end) { fprintf(stderr, "Invalid limit: %s\n", argv[i]); free(c); return 2; }
            if (!strcmp(option, "--stall")) c->stall_limit = value;
            else if (!strcmp(option, "--max-instructions")) c->instruction_limit = value;
            else if (!strcmp(option, "--max-vblanks")) max_vblanks = value;
            else if (!strcmp(option, "--frames-from")) frames_from = value;
            else {
                if (!value || value > UINT32_MAX) { fprintf(stderr, "Invalid frame step\n"); free(c); return 2; }
                frame_step = (unsigned)value;
            }
        } else { fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]); free(c); return 2; }
    }
#ifdef FF8_GUI
#ifdef FF8_PLAYER
    gui_allowed = !options.no_gui;
#else
    gui_allowed = !options.no_gui && (options.gui || *options.gui_test);
#endif
#else
    if (options.gui || *options.gui_test) { fprintf(stderr,"GUI support requires a build with -DFF8_GUI=ON\n"); free(c); return 2; }
#endif
#ifdef FF8_PLAYER
    {
        const char *supplied[4], *sorted[4] = {0};
        unsigned count = disc_count, d;
        int bad_disc = 0;
        char error[512] = {0};
        memcpy(supplied,disc_paths,sizeof(supplied));
        /* An explicit --disc list replaces the configured list. */
        if (!count) for (d=0;d<4;++d) if (*options.discs[d]) supplied[count++] = options.discs[d];
        for (d=0;d<count;++d) if (!host_disc_assign(sorted,supplied[d],options.verify_discs,error,sizeof(error))) {
#ifdef FF8_GUI
            if (gui_allowed && !headless) {
                if (!bad_disc) snprintf(startup_detail,sizeof(startup_detail),"%s",error);
                bad_disc = startup_problem = 1;
            } else
#endif
            { fprintf(stderr,"%s\n",error); free(c); return 2; }
        }
        if (!bad_disc) {
            char paths[4][512] = {{0}};
            const char *long_paths[4] = {0};
            for (d=0;d<4;++d) if (sorted[d]) {
                if (strlen(sorted[d]) >= sizeof(paths[d])) long_paths[d] = sorted[d];
                else strcpy(paths[d],sorted[d]);
            }
            memcpy(options.discs,paths,sizeof(paths));
            for (d=0;d<4;++d) disc_paths[d] = long_paths[d] ? long_paths[d] : *options.discs[d] ? options.discs[d] : NULL;
        } else {
            /* Keep rejected paths visible to the GUI worker for replacement. */
            char paths[4][512] = {{0}};
            for (d=0;d<count;++d) {
                if (strlen(supplied[d]) >= sizeof(paths[d])) { fprintf(stderr,"Disc path exceeds 511 bytes: %s\n",supplied[d]); free(c); return 2; }
                strcpy(paths[d],supplied[d]);
            }
            memcpy(options.discs,paths,sizeof(paths));
            for (d=0;d<4;++d) disc_paths[d] = *options.discs[d] ? options.discs[d] : NULL;
        }
        disc_count = 0;
        for (d=0;d<4;++d) if (disc_paths[d]) disc_count = d+1;
        if (disc_paths[0]) options.disc1_configured = 1;
        if (!disc_paths[0]) {
#ifdef FF8_GUI
            if (gui_allowed && !headless) startup_problem = 1;
            else
#endif
            {
                fprintf(stderr,"Provide FF8 USA disc 1 with --disc PATH or disc1=PATH in %s\n",
                        *options.config_path ? options.config_path : "a config file (none was found)");
                free(c); return 2;
            }
        }
        card_paths[0] = *options.cards[0] ? options.cards[0] : NULL;
        card_paths[1] = *options.cards[1] ? options.cards[1] : NULL;
        if (!host_player_data(data_dir,sizeof(data_dir)) && (!card_paths[0] || !*options.state_dir)) {
            fprintf(stderr,"No player data directory found; provide --card1 and --savestate-dir\n"); free(c); return 2;
        }
        if (!card_paths[0]) {
            int n = snprintf(default_card,sizeof(default_card),"%s/cards",data_dir);
            if (n < 0 || (size_t)n >= sizeof(default_card)) { fprintf(stderr,"Default card path exceeds 4095 bytes\n"); free(c); return 2; }
            if (!host_player_directory(default_card)) {
#ifdef FF8_GUI
                if (gui_allowed && !headless) {
                    startup_problem = 1; startup_pane = HOST_GUI_CARD;
                    snprintf(startup_detail,sizeof(startup_detail),"Cannot create default cards directory");
                } else
#endif
                { fprintf(stderr,"Cannot create default cards directory\n"); free(c); return 2; }
            }
            n = snprintf(default_card,sizeof(default_card),"%s/cards/card1.mcr",data_dir);
            if (n < 0 || (size_t)n >= sizeof(default_card)) { free(c); return 2; }
            card_paths[0] = default_card;
        }
        if (!*options.state_dir) {
            int n = snprintf(default_states,sizeof(default_states),"%s/states",data_dir);
            if (n < 0 || (size_t)n >= sizeof(default_states)) { free(c); return 2; }
        }
#ifdef FF8_GUI
        if (gui_allowed && !*options.cards[0] && card_paths[0] && strlen(card_paths[0])<sizeof(options.cards[0]))
            strcpy(options.cards[0],card_paths[0]);
        if (gui_allowed && !headless) {
            for (d=0;d<2;++d) if (card_paths[d]) {
                memcard_t *card = memcard_open(card_paths[d],error,sizeof(error));
                if (card) memcard_close(card);
                else { startup_problem = 1; startup_pane = HOST_GUI_CARD; snprintf(startup_detail,sizeof(startup_detail),"%s",error); }
            }
        }
#endif
    }
#endif
#if defined(FF8_GUI) && !defined(FF8_PLAYER)
    {
        unsigned d;
        for (d=0;d<2;++d) if (*options.cards[d]) card_paths[d] = options.cards[d];
        if (gui_allowed) {
            for (d=0;d<4;++d) if (disc_paths[d] && strlen(disc_paths[d])<sizeof(options.discs[d]))
                strcpy(options.discs[d],disc_paths[d]);
            for (d=0;d<2;++d) if (card_paths[d] && strlen(card_paths[d])<sizeof(options.cards[d]) && card_paths[d]!=options.cards[d])
                strcpy(options.cards[d],card_paths[d]);
        }
    }
#endif
    if (!*data_dir) host_player_data(data_dir,sizeof(data_dir));
    host_ra_configure(data_dir,options.ra,options.ra_notify);
    atexit(host_ra_shutdown);
#ifdef FF8_GUI
    if (gui_allowed) {
        if (!*data_dir) host_player_data(data_dir,sizeof(data_dir));
        if (!host_gui_configure(&options,data_dir,&services,options.gui_test)) { host_gui_shutdown(); free(c); return 2; }
        gui_configured = 1;
        if (!headless && (options.gui || startup_problem)) {
            host_gui_result_t shell;
            if (*startup_detail) host_gui_startup_error(startup_pane,startup_detail);
            if (options.gui) host_gui_hub();
            for (;;) {
                shell = gui_shell_display(&options);
                if (shell!=HOST_GUI_PLAY) {
                    result = shell==HOST_GUI_QUIT && !host_gui_test_failed() ? 0 : 2;
                    host_gui_shutdown(); free(c); return result;
                }
#ifdef FF8_PLAYER
                {
                    unsigned d, number;
                    char error[512] = {0};
                    host_gui_pane_t error_pane = HOST_GUI_DISCS;
                    for (d=0;d<4 && !*error;++d) if (*options.discs[d]) {
                        if (host_disc_identify(options.discs[d],options.verify_discs,&number,error,sizeof(error)) && number!=d+1)
                            snprintf(error,sizeof(error),"Expected Disc %u, found Disc %u",d+1,number);
                    }
                    for (d=0;d<2 && !*error;++d) if (*options.cards[d]) {
                        memcard_t *card = memcard_open(options.cards[d],error,sizeof(error));
                        if (card) memcard_close(card);
                        else error_pane = HOST_GUI_CARD;
                    }
                    if (*error) { host_gui_startup_error(error_pane,error); continue; }
                }
#endif
                break;
            }
#if defined(FF8_PLAYER)
            {
                unsigned d;
                disc_count = 0;
                for (d=0;d<4;++d) {
                    disc_paths[d] = *options.discs[d] ? options.discs[d] : NULL;
                    if (disc_paths[d]) disc_count = d+1;
                }
                card_paths[0] = *options.cards[0] ? options.cards[0] : default_card;
                card_paths[1] = *options.cards[1] ? options.cards[1] : NULL;
                if (!disc_paths[0]) { host_gui_shutdown(); free(c); return 2; }
            }
#else
            {
                unsigned d;
                /* GUI edits change selected media; untouched argv order stays intact. */
                for (d=0;d<4;++d) if (!disc_paths[d] || strlen(disc_paths[d])<sizeof(options.discs[d]) || *options.discs[d])
                    disc_paths[d] = *options.discs[d] ? options.discs[d] : NULL;
                for (d=0;d<2;++d) card_paths[d] = *options.cards[d] ? options.cards[d] : NULL;
                disc_count = 0;
                for (d=0;d<4;++d) if (disc_paths[d]) disc_count = d+1;
            }
#endif
        }
    }
#endif
    if (!host_options_ra_validate(&options,fast)) { free(c); HOST_GUI_CLEANUP(); return 2; }
    renderer = options.renderer ? "vulkan" : "soft";
    internal_scale = (unsigned)options.internal_scale; msaa = (unsigned)options.msaa;
    true_color = options.true_color;
    pgxp = options.pgxp == 1 ? "vertex" : options.pgxp == 2 ? "perspective" : "off";
    pgxp_mode = options.pgxp;
#ifdef FF8_GUI
    if (gui_allowed && !headless && !options.renderer) {
        internal_scale = msaa = 1; true_color = 0; pgxp = "off"; pgxp_mode = 0;
    }
#endif
#define PATH_OPTION(local, field) local = *options.field ? options.field : NULL
    PATH_OPTION(movies,movies); PATH_OPTION(movie_stats,movie_stats);
    PATH_OPTION(scaled_frames,scaled_frames); PATH_OPTION(gpu_timings,gpu_timings);
    PATH_OPTION(presented_frames,presented_frames);
    PATH_OPTION(bridge_port,bridge); PATH_OPTION(achievement_path,achievements);
    PATH_OPTION(achievement_progress,progress); PATH_OPTION(savestate_dir,state_dir);
    PATH_OPTION(load_state,load_state); PATH_OPTION(save_state,save_state);
#ifdef FF8_GUI
    if (gui_allowed && !headless && !options.renderer) movies = movie_stats = scaled_frames = gpu_timings = NULL;
#endif
#ifdef FF8_PLAYER
    if (!savestate_dir) savestate_dir = default_states;
#endif
#undef PATH_OPTION
    save_at = options.save_at; savestate_full_disc = options.full_disc;
    if (save_state && !save_at) { fprintf(stderr,"--save-state requires --save-at N (N > 0)\n"); free(c); HOST_GUI_CLEANUP(); return 2; }
    if (strcmp(pgxp, "off") && strcmp(pgxp, "vertex") && strcmp(pgxp, "perspective")) {
        fprintf(stderr, "Invalid PGXP mode: %s\n", pgxp); free(c); HOST_GUI_CLEANUP(); return 2;
    }
#ifdef FF8_PGXP
    if (strcmp(pgxp, "off") && strcmp(renderer, "vulkan")) {
        fprintf(stderr, "PGXP requires --renderer vulkan\n"); free(c); HOST_GUI_CLEANUP(); return 2;
    }
#else
    if (strcmp(pgxp, "off")) {
        fprintf(stderr, "PGXP support requires -DFF8_PGXP=ON\n"); free(c); HOST_GUI_CLEANUP(); return 2;
    }
#endif
#ifdef FF8_VULKAN
    if ((movies || movie_stats) && strcmp(renderer,"vulkan")) {
        fprintf(stderr,"Movie replacement requires --renderer vulkan\n"); free(c); HOST_GUI_CLEANUP(); return 2;
    }
    if (!host_gpu_configure(renderer, internal_scale, true_color, msaa, scaled_frames, gpu_timings)) {
        fprintf(stderr, "Invalid GPU options (enhancements require --renderer vulkan)\n"); free(c); HOST_GUI_CLEANUP(); return 2;
    }
    host_gpu_present_capture(presented_frames, (unsigned)(options.present_size & 0xffffffffu), (unsigned)(options.present_size >> 32));
#else
    if (strcmp(renderer, "soft") || internal_scale != 1 || true_color || msaa != 1 || scaled_frames || gpu_timings || movies || movie_stats) {
        fprintf(stderr, "Vulkan support requires a build with -DFF8_VULKAN=ON\n"); free(c); HOST_GUI_CLEANUP(); return 2;
    }
#endif
    if (!load_exe(c, exe_path)) { free(c); HOST_GUI_CLEANUP(); return 2; }
    if (disc_count) {
        char error[256];
        if (!HOST_DISC_INSERT(c, disc_paths[0], 0, error, sizeof(error))) {
            fprintf(stderr, "Disc %s: %s\n", disc_paths[0], error); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
        }
    }
    log = fopen(log_path, "w");
    if (!log) { fprintf(stderr, "%s: %s\n", log_path, strerror(errno)); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2; }
    c->log = strcmp(log_path, "/dev/null") ? trace : NULL; c->log_context = log;
    if (!host_display_init(headless, frame_path, vram_path)) {
        host_display_shutdown(NULL); fclose(log); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
    }
#ifdef FF8_GUI
    if (gui_allowed) host_display_gui_audio(host_audio_pause,host_audio_resume);
#endif
#ifdef FF8_PGXP
    pgxp_bind(hw_get(c)->gpu, !strcmp(pgxp, "vertex") ? PGXP_VERTEX : !strcmp(pgxp, "perspective") ? PGXP_PERSPECTIVE : PGXP_OFF);
#endif
#ifdef FF8_VULKAN
    if (!host_display_gpu(hw_get(c)->gpu)) {
        host_display_shutdown(NULL); fclose(log); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
    }
#endif
    if (!host_screenshot_configure(options.screenshot_dir,options.screenshot_at) ||
        !host_display_present_capture(presented_frames,(unsigned)options.present_size,(unsigned)(options.present_size>>32))) {
        fprintf(stderr,"Cannot configure screenshots/presented capture\n");
        host_display_shutdown(NULL); fclose(log); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
    }
    host_display_hotkeys(options.pad_hotkey,disc_count);
    {
        unsigned d, mask = 0;
        for (d=0;d<4;++d) if (disc_paths[d]) mask |= 1u << d;
        host_display_disc_mask(mask);
    }
    host_osd_configure(options.osd,options.osd_tests);
    host_osd_frame(!headless,0);
    host_display_capture(frame_step, frames_from);
    if (!host_display_configure(options.fullscreen, options.scale_mode, options.linear)) {
        host_display_shutdown(NULL); fclose(log); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
    }
    host_display_crt(options.crt);
#ifdef FF8_GUI
    if (gui_allowed && gui_configured && !host_display_gui_attach()) {
        host_gui_shutdown(); host_display_shutdown(NULL); fclose(log); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
    }
#endif
    input.dualshock = options.dualshock; input.rumble = options.rumble;
    host_input_dualshock(input.dualshock);
    input.scripted = input_script != NULL;
#ifdef FF8_VULKAN
    if (!host_movies_start(movies,internal_scale,movie_stats)) {
        fprintf(stderr,"Cannot configure movies directory/statistics\n");
        host_display_shutdown(NULL); fclose(log); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
    }
    host_movies_bind(c,1);
#endif
    hw_get(c)->present = present_frame;
    sio_init(c);
    for (i = 0; i < 2; ++i) if (card_paths[i]) {
        char error[256];
        if (!sio_open_card(c, (unsigned)i, card_paths[i], error, sizeof(error))) {
            fprintf(stderr, "Card %d: %s\n", i + 1, error);
            host_display_shutdown(NULL); fclose(log); sio_destroy(c); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
        }
    }
    if (input_script) {
        char error[256];
        if (!sio_load_script(c, input_script, error, sizeof(error))) {
            fprintf(stderr, "Input script: %s\n", error);
            host_display_shutdown(NULL); fclose(log); sio_destroy(c); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
        }
    }
    if (spu_control) {
        static const spu_backend_t sink = {NULL, NULL};
        spu_set_backend(c->hw->spu, &sink, NULL);
    }
    if (!host_audio_init(headless, mute, audio_path)) {
        sio_destroy(c);
        host_display_shutdown(NULL); fclose(log); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
    }
    if (!host_audio_stats(audio_stats_path)) {
        fprintf(stderr, "Cannot open audio statistics: %s\n", audio_stats_path);
        host_audio_shutdown(c->hw->spu); sio_destroy(c);
        host_display_shutdown(NULL); fclose(log); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return 2;
    }
    if (bridge_port) state_bridge = host_state_bridge_open(bridge_port);
    host_ra_frame(c);
    if (disc_count && disc_paths[0]) host_ra_disc(disc_paths[0]);
    if (!result && achievement_path) {
        achievements = host_achievements_open(achievement_path, card_paths[0] ? card_paths[0] : card_paths[1],
            achievement_progress, disc_count ? disc_paths[0] : NULL, host_state_bridge_event, state_bridge);
        if (!achievements) result = 2;
    }
    if (result) {
        host_state_bridge_close(state_bridge); host_audio_shutdown(c->hw->spu);
        sio_destroy(c); host_display_shutdown(NULL); fclose(log); hw_destroy(c); free(c); HOST_GUI_CLEANUP(); return result;
    }
    /* EXE startup occurs after the boot BIOS enabled the CD response IRQs. */
    c->hw->cd.irq_enable = 0x1f;
    c->entries = generated_entries; c->entry_count = generated_count;
#ifdef GENERATED_HAS_OVERLAYS
    c->entry_regions = generated_regions;
    c->overlays = generated_overlays; c->overlay_count = generated_overlay_count;
    if (legacy_overlays) {
        size_t n;
        legacy_registry = malloc(generated_overlay_count * sizeof(*legacy_registry));
        if (!legacy_registry) { fprintf(stderr, "Cannot allocate legacy overlay registry\n"); HOST_GUI_CLEANUP(); return 2; }
        c->overlay_count = 0;
        for (n = 0; n < generated_overlay_count; ++n)
            if (generated_overlays[n].index < 128) legacy_registry[c->overlay_count++] = generated_overlays[n];
        c->overlays = legacy_registry;
    }
#else
    if (!c->interpreter_only) fprintf(stderr, "Generated code has no validity metadata; using interpreter. Regenerate to enable recompilation.\n");
    c->interpreter_only = 1;
#endif
#ifndef FF8_PLAYER
    if (native_module_dir && generated_count) {
        /* Modules would silently shadow the linked tables; neither run would be meaningful. */
        fprintf(stderr, "--native-modules needs a host built from a tree generated with --shape modules\n");
        host_state_bridge_close(state_bridge); host_achievements_close(achievements);
        host_audio_shutdown(c->hw->spu); host_display_shutdown(NULL); fclose(log);
        sio_destroy(c); hw_destroy(c); free(legacy_registry); free(c); HOST_GUI_CLEANUP(); return 2;
    }
    if (native_module_dir) {
        /* The EXE text is intact only now; the loader keeps its own copy. */
        if (!native_modules_configure(generated_hash,generated_base,generated_size,c->ram + (generated_base & 0x1fffffffu)) ||
            !native_modules_scan(native_module_dir)) {
            fprintf(stderr, "Cannot read native modules from %s\n", native_module_dir);
            host_state_bridge_close(state_bridge); host_achievements_close(achievements);
            host_audio_shutdown(c->hw->spu); host_display_shutdown(NULL); fclose(log);
            sio_destroy(c); hw_destroy(c); free(legacy_registry); free(c); HOST_GUI_CLEANUP(); return 2;
        }
        /* Paced loading starts empty, so the opening frames are interpreted
         * like a player's first run before anything has been built. */
        if (!native_pace_vblanks) { native_modules_feed(SIZE_MAX); native_modules_commit(c); }
    }
#endif
#ifdef FF8_PLAYER
    /* The player starts on the interpreter and switches set by set as its own
     * modules arrive; without the compiler this starts nothing. */
    if (!native_off) {
        host_native_options_t native = {native_status, native_workers};
        if (!host_native_start(c, data_dir, disc_paths, 1, &native) && native_only) {
            fprintf(stderr, "No native compiler is installed\n");
            host_state_bridge_close(state_bridge); host_achievements_close(achievements);
            host_audio_shutdown(c->hw->spu); host_display_shutdown(NULL); fclose(log);
            sio_destroy(c); hw_destroy(c); free(legacy_registry); free(c); HOST_GUI_CLEANUP(); return 2;
        }
    }
    {
        /* Which code runs natively never changes guest state on the precise
         * host, so a player's states do not record it: the bit stays as the
         * interpreter-only player always wrote it, and a state saved before a
         * module arrived loads after it, and the other way round. */
        unsigned interpreted = 1;
#else
    {
        unsigned interpreted = c->interpreter_only;
#endif
    if (!host_states_init(c,savestate_dir,card_paths[0],load_state,save_state,save_at,
                          disc_paths,
#ifdef FF8_PLAYER
                          4,
#elif defined(FF8_GUI)
                          gui_allowed ? 4 : disc_count,
#else
                          disc_count,
#endif
                          generated_hash,
                          interpreted | (legacy_overlays << 1) | (spu_control << 2) | (options.dualshock << 3),savestate_full_disc,options.snapshot_cards)) {
#ifdef FF8_PLAYER
        host_native_stop(c, 0);
#endif
        host_state_bridge_close(state_bridge); host_achievements_close(achievements);
        host_audio_shutdown(c->hw->spu); host_display_shutdown(NULL); fclose(log);
        sio_destroy(c); hw_destroy(c); free(legacy_registry); free(c); host_states_shutdown(); HOST_GUI_CLEANUP(); return 2;
    }
    }
#ifdef FF8_GUI
    if (gui_allowed) { gui_session.playing = 1; host_gui_attach_game(); }
#endif
    host_states_recovery(restored);
#ifdef FF8_PLAYER
    if (native_only) {
        native_build_status_t status;
        host_native_stop(c, 1);
        native_build_status(&status);
        result = status.state == NATIVE_BUILD_DONE ? 0 : 1;
        snprintf(c->stop_reason, sizeof(c->stop_reason), "VBlank limit reached");
    } else
#endif
    {
        host_states_apply(&c,&input);
        if (!host_states_failed()) do { run_guest(c); } while (host_states_apply(&c,&input));
    }
    if (host_states_failed()) result = 2;
#ifdef FF8_GUI
    if (gui_allowed && host_gui_test_failed()) result = 2;
#endif
    /* Keep the runtime stop and its stdout summary intact for replay tools. */
    if (strcmp(c->stop_reason, "VBlank limit reached") &&
        strcmp(c->stop_reason, "instruction budget exhausted") &&
        (strcmp(c->stop_reason, "host display closed or failed") || host_display_failed())) {
        fprintf(stderr, "Stop: %s\nLast PC: %08x\n", c->stop_reason, c->pc);
        if (!result) result = 1;
    }
    hw_sync(c);
    summary(c);
    printf("Trace: %s\n", log_path);
    if (spu_path) {
        FILE *file = fopen(spu_path, "wb");
        int ok = file && fwrite(spu_ram(c->hw->spu), 1, 512 * 1024, file) == 512 * 1024;
        if (file && fclose(file)) ok = 0;
        if (!ok) { fprintf(stderr, "Cannot write SPU RAM: %s\n", spu_path); result = 2; }
    }
    if (state_path) {
        if (dump_state(c, state_path)) printf("Guest state: %s\n", state_path);
        else { fprintf(stderr, "Cannot write guest state: %s\n", state_path); result = 2; }
    }
    if (!host_audio_shutdown(c->hw ? c->hw->spu : NULL)) result = 2;
#ifdef FF8_VULKAN
    host_movies_stop();
#endif
#ifdef FF8_GUI
    if (gui_allowed) host_gui_shutdown();
#endif
    if (!host_display_shutdown(c->hw ? c->hw->gpu : NULL)) result = 2;
    if (!sio_cards_ok(c)) { fprintf(stderr, "Memory card write-back failed\n"); result = 2; }
    host_states_shutdown();
    if (!host_achievements_close(achievements)) result = 2;
    
    host_state_bridge_close(state_bridge);
#ifndef FF8_PLAYER
    if (native_module_dir) {
        fprintf(stderr, "Native modules: %zu registered\n", native_modules_registered());
        native_modules_shutdown(c);
    }
#else
    host_native_stop(c, native_wait);
#endif
    sio_destroy(c);
    fclose(log); hw_destroy(c); free(legacy_registry); free(c);
    HOST_GUI_CLEANUP(); return result;
}
