#ifndef HOST_HOTKEY_BINDINGS_H
#define HOST_HOTKEY_BINDINGS_H
#include <SDL.h>
#include "sio.h"
typedef struct { unsigned pad_button, scancode; const char *label_id; int held; } host_gui_chord_t;
static const host_gui_chord_t host_chords[]={
    {PAD_R1,SDL_SCANCODE_F5,"hk.save",0}, {PAD_L1,SDL_SCANCODE_F7,"hk.load",0},
    {PAD_LEFT,SDL_SCANCODE_F6,"hk.slot",0}, {PAD_RIGHT,SDL_SCANCODE_F8,"hk.slot",0},
    {PAD_R2,SDL_SCANCODE_TAB,"hk.ff",1}, {PAD_TRIANGLE,SDL_SCANCODE_F9,"hk.crt",0},
    {PAD_SQUARE,SDL_SCANCODE_F12,"hk.shot",0}, {PAD_SELECT,SDL_SCANCODE_F1,"hk.disc",0},
    {PAD_START,SDL_SCANCODE_F11,"hk.fs",0}
};
static inline const host_gui_chord_t *host_display_chords(unsigned *count) {
    *count=(unsigned)(sizeof(host_chords)/sizeof(host_chords[0])); return host_chords;
}
#endif
