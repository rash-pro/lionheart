#include "states.h"
#include "savestate.h"
#include "hw.h"
#include "display.h"
#include "osd.h"
#include "audio.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned osd_event, osd_slot;
static char osd_detail[512];
void __real_host_osd_event(unsigned,unsigned,const char *);
void __wrap_host_osd_event(unsigned event,unsigned slot,const char *detail) {
    osd_event=event; osd_slot=slot; snprintf(osd_detail,sizeof(osd_detail),"%s",detail ? detail : "");
    __real_host_osd_event(event,slot,detail);
}
static unsigned audio_clears;
static unsigned native_calls, recovered;
static void recovery(cpu_t *c,unsigned disc) { assert(c->r[12]==42 && disc==0); ++recovered; }
static void native_resume(cpu_t *c) { ++native_calls; c->pc = 0xffffffff; }
static const entry_t entries[] = {{0x80010000,native_resume}};
static SDL_Window *window;
static uint8_t presented[640 * 480 * 3];
static unsigned presents;
SDL_Window *__real_SDL_CreateWindow(const char *, int, int, int, int, Uint32);
SDL_Window *__wrap_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    window = __real_SDL_CreateWindow(title,x,y,w,h,flags); return window;
}
int __real_SDL_UpdateTexture(SDL_Texture *, const SDL_Rect *, const void *, int);
int __wrap_SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch) {
    Uint32 format; assert(!SDL_QueryTexture(texture,&format,NULL,NULL,NULL));
    if (format==SDL_PIXELFORMAT_RGB24) { assert(pitch > 0 && pitch <= 640 * 3); memcpy(presented,pixels,(size_t)pitch); ++presents; }
    return __real_SDL_UpdateTexture(texture,rect,pixels,pitch);
}
void __real_SDL_ClearQueuedAudio(SDL_AudioDeviceID);
void __wrap_SDL_ClearQueuedAudio(SDL_AudioDeviceID id) { ++audio_clears; __real_SDL_ClearQueuedAudio(id); }
static void key(cpu_t *c, SDL_Scancode code) {
    SDL_Event event; SDL_zero(event); event.type = SDL_KEYDOWN; event.key.keysym.scancode = code;
    assert(SDL_PushEvent(&event) == 1); host_present(c->hw->gpu); host_states_vblank(c);
    printf("SDL %s: %s\n",SDL_GetScancodeName(code),SDL_GetWindowTitle(window));
}
static int boundary(cpu_t *c) {
    unsigned before = native_calls;
    dispatch(c,0x80010000);
    assert(!savestate_pending && native_calls == before + !host_states_loading());
    return host_states_loading();
}
static void select_pad(cpu_t *c) {
    hw_write(c,0x1f80104a,2,0); hw_write(c,0x1f801048,2,0xd);
    hw_write(c,0x1f80104e,2,136); hw_write(c,0x1f80104a,2,0x1003);
}
static unsigned pad_byte(cpu_t *c, unsigned value) {
    hw_write(c,0x1f801040,1,value); c->cycles += 2000; hw_event(c); return hw_read(c,0x1f801040,1);
}
static void analog_pad(cpu_t *c) {
    static const unsigned enter[] = {1,0x43,0,1,0}, enable[] = {1,0x44,0,1,0,0,0,0,0};
    unsigned i;
    sio_set_pad(c,0,1,1); select_pad(c);
    for (i = 0; i < sizeof(enter)/sizeof(*enter); ++i) pad_byte(c,enter[i]);
    select_pad(c);
    for (i = 0; i < sizeof(enable)/sizeof(*enable); ++i) pad_byte(c,enable[i]);
    assert(sio_pad_output(c,0).analog);
}
static int exists(const char *path) { FILE *f = fopen(path,"rb"); if (!f) return 0; fclose(f); return 1; }
int main(int argc, char **argv) {
    cpu_t *c = malloc(sizeof(*c)), *old;
    host_input_t input = {0};
    char path[4096], error[256];
    uint8_t data[128];
    FILE *f;
    int controller;
    assert(argc == 3 && c);
    cpu_init(c); hw_get(c); sio_init(c); c->pc = 0x80010000; c->r[12] = 42;
    c->entries = entries; c->entry_count = 1;
    assert(sio_open_card(c,0,argv[2],error,sizeof(error)));
    assert(host_display_init(0,NULL,NULL)); assert(host_audio_init(0,0,NULL));
    controller = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,16,0);
    assert(controller >= 0); host_present(c->hw->gpu); assert(host_input_controller(0));
    input.dualshock = input.rumble = 1; host_input_dualshock(1); analog_pad(c);
    assert(host_states_init(c,argv[1],argv[2],NULL,NULL,0,NULL,0,0,8,0,HOST_CARDS_SNAPSHOT));
    host_states_recovery(recovery);
    assert(strstr(SDL_GetWindowTitle(window),"slot 0"));
    gpu_gp1(c->hw->gpu,0x03000000);
    gpu_gp0(c->hw->gpu,0x020000ff); gpu_gp0(c->hw->gpu,0); gpu_gp0(c->hw->gpu,0x01000100);
    c->hw->vblanks = 1;
    key(c,SDL_SCANCODE_F5);
    assert(osd_event==HOST_OSD_SAVING && osd_slot==0);
    assert(strstr(SDL_GetWindowTitle(window),"saving\xe2\x80\xa6"));
    /* Changing the selected slot while the save is pending keeps the request's slot. */
    key(c,SDL_SCANCODE_F8); assert(osd_event==HOST_OSD_SLOT && osd_slot==1 && *osd_detail); assert(strstr(SDL_GetWindowTitle(window),"slot 1"));
    assert(strstr(SDL_GetWindowTitle(window),"saving\xe2\x80\xa6")); assert(!boundary(c));
    assert(osd_event==HOST_OSD_SAVED && osd_slot==0);
    assert(!strstr(SDL_GetWindowTitle(window),"saving"));
    snprintf(path,sizeof(path),"%s/slot-0.ff8s",argv[1]); assert(exists(path));
    key(c,SDL_SCANCODE_F6); assert(osd_event==HOST_OSD_SLOT && osd_slot==0 && !*osd_detail); c->r[12] = 99;
    gpu_gp0(c->hw->gpu,0x02ff0000); gpu_gp0(c->hw->gpu,0); gpu_gp0(c->hw->gpu,0x01000100);
    key(c,SDL_SCANCODE_F7); assert(osd_event==HOST_OSD_LOADING && osd_slot==0); assert(boundary(c));
    assert(presented[2] > 200 && presented[0] == 0);
    { unsigned before = presents;
    old = c; assert(host_states_apply(&c,&input)); assert(osd_event==HOST_OSD_LOADED && osd_slot==0); assert(c != old && c->r[12] == 42 && audio_clears == 1 && recovered == 1);
      assert(presents == before + 1 && presented[0] > 200 && presented[2] == 0);
      printf("Immediate restored frame: RGB=%u,%u,%u VBlank=%llu presents=+1\n",presented[0],presented[1],presented[2],(unsigned long long)c->hw->vblanks);
      assert(c->hw->vblanks == 1 && !strstr(SDL_GetWindowTitle(window),"loading")); }
    { unsigned before = native_calls; dispatch(c,c->pc); assert(native_calls == before + 1 && c->pc == 0xffffffff); }
    assert(!host_states_failed() && host_display_running());
    assert(sio_pad_output(c,0).analog);
    host_input_sync(c,&input); assert(sio_pad_output(c,0).analog);
    memset(data,0xab,sizeof(data)); assert(memcard_write_sector(sio_card(c,0),100,data));
    f = fopen(argv[2],"rb"); assert(f && !fseek(f,100 * 128,SEEK_SET)); assert(fgetc(f) != 0xab); fclose(f);
    /* A failed live load keeps this exact running session and its card. */
    f = fopen(path,"r+b"); assert(f && !fseek(f,20,SEEK_SET)); { int byte = fgetc(f); assert(byte != EOF && !fseek(f,20,SEEK_SET)); assert(fputc(byte ^ 255,f) != EOF); } fclose(f);
    c->r[12] = 123; key(c,SDL_SCANCODE_F7); assert(boundary(c)); old = c;
    assert(host_states_apply(&c,&input)); assert(osd_event==HOST_OSD_LOAD_FAILED && *osd_detail); assert(c == old && c->r[12] == 123 && audio_clears == 1 && recovered == 1);
    assert(memcard_read_sector(sio_card(c,0),100,data) && data[0] == 0xab);
    key(c,SDL_SCANCODE_F6); key(c,SDL_SCANCODE_F5); assert(!boundary(c));
    snprintf(path,sizeof(path),"%s/slot-9.ff8s",argv[1]); assert(exists(path));
    /* A native loop can keep ticking video without returning to dispatch. */
    key(c,SDL_SCANCODE_F8); key(c,SDL_SCANCODE_F8); key(c,SDL_SCANCODE_F5);
    assert(strstr(SDL_GetWindowTitle(window),"slot 1") && savestate_pending == c);
    c->cycles += 2ull * HW_CPU_HZ - 1; host_states_vblank(c);
    assert(savestate_pending == c && !c->halted);
    ++c->cycles; host_states_vblank(c);
    assert(!savestate_pending && !savestate_ready(c) && !c->halted);
    assert(osd_event==HOST_OSD_SAVE_FAILED && osd_slot==1 && !strcmp(osd_detail,"timed out"));
    key(c,SDL_SCANCODE_F7); assert(boundary(c)); assert(host_states_apply(&c,&input));
    assert(osd_event==HOST_OSD_NO_STATE && osd_slot==1);
    assert(!strstr(SDL_GetWindowTitle(window),"saving"));
    snprintf(path,sizeof(path),"%s/slot-1.ff8s",argv[1]); assert(!exists(path));
    snprintf(path,sizeof(path),"%s/script.txt",argv[1]);
    f = fopen(path,"w"); assert(f); assert(fputs("0 CROSS\n10 -\n",f) >= 0); assert(!fclose(f));
    assert(sio_load_script(c,path,error,sizeof(error)));
    input.scripted = 1; host_input_sync(c,&input); assert(!input.analog_pad[0]);
    key(c,SDL_SCANCODE_F5); assert(!boundary(c));
    input.scripted = 0; key(c,SDL_SCANCODE_F7); assert(boundary(c));
    host_states_recovery(NULL);
    assert(host_states_apply(&c,&input));
    host_input_sync(c,&input);
    select_pad(c); assert(pad_byte(c,1) == 0xff); assert(pad_byte(c,0x43) == 0xff);
    assert(SDL_JoystickDetachVirtual(controller) == 0);
    host_states_shutdown();
    assert(host_states_init(c,argv[1],NULL,NULL,NULL,0,NULL,0,0,4,0,HOST_CARDS_LIVE));
    key(c,SDL_SCANCODE_F5); assert(osd_event==HOST_OSD_UNAVAILABLE);
    key(c,SDL_SCANCODE_F7); assert(osd_event==HOST_OSD_UNAVAILABLE);
    host_states_shutdown();
    assert(host_states_init(c,"/dev/null/states",NULL,NULL,NULL,0,NULL,0,0,0,0,HOST_CARDS_LIVE));
    key(c,SDL_SCANCODE_F5); assert(!boundary(c));
    assert(osd_event==HOST_OSD_SAVE_FAILED && strstr(osd_detail,"directory"));
    assert(host_audio_shutdown(c->hw->spu)); assert(host_display_shutdown(c->hw->gpu));
    host_states_shutdown(); savestate_dispose(c);
    puts("SDL F5/F6/F7/F8, pending title, 2s guest timeout, immediate restored red frame, private cards and input/audio recovery: OK"); return 0;
}
