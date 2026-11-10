#define _POSIX_C_SOURCE 200809L
#include "gui_internal.h"
#include "gui_strings.h"
#include "display.h"
#include "osd.h"
#include "screenshot.h"
#include "ra.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef FF8_VERSION
#define FF8_VERSION "dev"
#endif

enum { MODAL_NONE,MODAL_BROWSER,MODAL_RA_WARNING,MODAL_QUIT,MODAL_WRITE,MODAL_NATIVE };
static struct {
    host_options_t *options; host_gui_services_t services;
    host_gui_mode_t mode; host_gui_pane_t pane,requested;
    host_gui_result_t result; host_gui_input_t input;
    int configured,game,want_open,modal,setup_step,ra_form,ra_error,ra_seen;
    int force_hub,first_focus,focus_rail,keyboard,keyboard_first,shift,symbols,pick_slot,drop_active;
    int card_imported[2];
    unsigned focus_epoch;
    unsigned generation; float width,height,rail;
    uint64_t started;
    char data[4096],error[512],startup_error[512],username[128],password[256];
    char *edit; size_t edit_capacity; int edit_password;
    char last_folder[512];
    gui_pick_t pick_kind;
    gui_disc_result_t discs[4];
    gui_browser_t browser;
    char browser_names[32][512],browser_paths[32][512]; unsigned browser_places;
    gui_disc_result_t browser_results[GUI_BROWSER_MAX]; unsigned browser_result_count;
    /* native_only: setup opened at the compiler step alone, for an
     * installation that finished setup before that step existed. */
    host_gui_native_t native; host_gui_failure_t failures[6]; unsigned failure_count;
    int native_offered,native_only,native_seen,native_local,after_browser;
    char crash_log[512],native_archive[512];
} gui;

const char *host_gui_string(const char *id) {
    for (size_t i=0;i<gui_string_count;++i) if (!strcmp(gui_strings[i].id,id)) return gui_strings[i].english;
    return "";
}
#define S(id) host_gui_string(id)
static gui_rect_t R(float x,float y,float w,float h) { gui_rect_t r={x,y,w,h}; return r; }
float gui_scale(unsigned height) {
    float s=(float)height/720.0f; return s<0.75f ? 0.75f : s>4.0f ? 4.0f : s;
}
static void text(const char *id,const char *value,float x,float y,float width,gui_font_t font,gui_colour_t colour) {
    gui_ui_text(id,value,R(x,y,width,32),font,colour);
}
static void label(const char *id,float x,float y,float width,gui_font_t font,gui_colour_t colour) { text(id,S(id),x,y,width,font,colour); }
/* A row's base observation is its selected value; its caption has a separate ID. */
static void control_label(const char *id,const char *title,float x,float y,float width,gui_font_t font,gui_colour_t colour) {
    char caption[128]; snprintf(caption,sizeof(caption),"%s.label",id); text(caption,S(title),x,y,width,font,colour);
}
static int button(const char *id,float x,float y,float width,unsigned flags) { return gui_ui_button(id,S(id),R(x,y,width,42),flags); }
static const char *base_name(const char *path) {
    const char *p=strrchr(path,'/');
#ifdef _WIN32
    const char *q=strrchr(path,'\\'); if (q && (!p || q>p)) p=q;
#endif
    return p ? p+1 : path;
}
static void parent_folder(const char *path,char *out,size_t capacity) {
    const char *slash=strrchr(path,'/');
#ifdef _WIN32
    const char *back=strrchr(path,'\\'); if (back && (!slash || back>slash)) slash=back;
#endif
    if (slash) snprintf(out,capacity,"%.*s",(int)(slash==path ? 1 : slash-path),path);
    else snprintf(out,capacity,".");
#ifdef _WIN32
    for (char *p=out;*p;++p) if (*p=='\\') *p='/';
    if (strlen(out)==2 && out[1]==':' && capacity>3) { out[2]='/'; out[3]=0; }
#endif
}
static int overlay(void) { return gui.mode==HOST_GUI_OVERLAY; }
static void request_focus(void) { gui.first_focus=1; ++gui.focus_epoch; }
static void close_form(void) { memset(gui.password,0,sizeof(gui.password)); gui.ra_form=gui.ra_error=0; gui.keyboard=0; gui.edit=NULL; }
static void persist_ui(void) { gui_state_save(gui.data,gui.last_folder,gui.input,gui.native_offered); }
static void changed(const char *key,const char *value) {
    char error[512];
    if (!host_options_set(gui.options,key,value)) return;
    if (!host_options_write(gui.options,key,value,error,sizeof(error))) {
        snprintf(gui.error,sizeof(gui.error),"%s",error); gui.modal=MODAL_WRITE; request_focus();
    }
    if (gui.services.changed) gui.services.changed(gui.services.context,key,overlay());
}
static void changed_number(const char *key,unsigned value) { char number[32]; snprintf(number,sizeof(number),"%u",value); changed(key,number); }
static void change_pane(host_gui_pane_t pane) { gui.pane=pane; gui.focus_rail=0; request_focus(); gui.keyboard=0; gui.edit=NULL; }
static void section(int direction) {
    static const host_gui_pane_t panes[]={HOST_GUI_GAME,HOST_GUI_ACHIEVEMENTS,HOST_GUI_DISCS,HOST_GUI_CARD,HOST_GUI_VIDEO,HOST_GUI_CONTROLS,HOST_GUI_RA};
    for (unsigned i=0;i<7;++i) if (gui.pane==panes[i]) { change_pane(panes[(i+(direction>0 ? 1 : 6))%7]); return; }
}
int host_gui_enabled(void) { return gui.configured; }
int host_gui_visible(void) { return gui.configured && gui.mode!=HOST_GUI_HIDDEN; }
host_gui_mode_t host_gui_mode(void) { return gui.mode; }
host_gui_pane_t host_gui_pane(void) { return gui.pane; }
host_gui_input_t host_gui_last_input(void) { return gui.input; }
host_gui_result_t host_gui_result(void) { return gui.result; }
int host_gui_requested(void) { return gui.want_open; }
void host_gui_request(host_gui_pane_t pane) { if (gui.configured) { gui.requested=pane; gui.want_open=1; } }
void host_gui_close(void) {
    gui.result=overlay() ? HOST_GUI_PLAY : HOST_GUI_QUIT;
    gui.mode=HOST_GUI_HIDDEN; gui.want_open=0; gui.modal=MODAL_NONE;
    close_form(); persist_ui();
}
void host_gui_edit_focus(char *edit,size_t capacity,int password) {
    gui.edit=edit; gui.edit_capacity=capacity; gui.edit_password=password;
    if (gui.input==HOST_GUI_PAD && !gui.keyboard) { gui.keyboard=1; gui.keyboard_first=1; }
}
void host_gui_startup_error(host_gui_pane_t pane,const char *detail) {
    snprintf(gui.startup_error,sizeof(gui.startup_error),"%s",detail ? detail : ""); gui.pane=pane; gui.force_hub=1;
}
void host_gui_hub(void) { gui.force_hub=1; }
int host_gui_configure(host_options_t *options,const char *data,const host_gui_services_t *services,const char *script) {
    if (gui.configured) host_gui_shutdown();
    memset(&gui,0,sizeof(gui)); gui.options=options; gui_achievements_reset();
    if (services) gui.services=*services;
    if (data) snprintf(gui.data,sizeof(gui.data),"%s",data);
    gui.input=HOST_GUI_KEYBOARD; gui.mode=HOST_GUI_HIDDEN; gui.result=HOST_GUI_RUNNING;
    gui.generation=1; gui.started=SDL_GetTicks64();
    { int last=gui.input; gui_state_load(gui.data,gui.last_folder,sizeof(gui.last_folder),&last,&gui.native_offered); gui.input=(host_gui_input_t)last; }
    if (gui.services.native) gui.services.native(gui.services.context,&gui.native);
    gui.native_seen=(int)gui.native.compiler;
    if (!gui_media_start()) return 0;
    gui.configured=1;
    for (unsigned i=0;i<4;++i) if (*options->discs[i]) {
        snprintf(gui.discs[i].path,sizeof(gui.discs[i].path),"%s",options->discs[i]);
        gui.discs[i].disc=i+1; gui.discs[i].state=GUI_DISC_CHECKING;
        gui_media_identify(options->discs[i],0,gui.generation,3);
    }
    if (!host_gui_test_load(script)) { gui_media_stop(); gui.configured=0; return 0; }
    return 1;
}
void host_gui_attach_game(void) { gui.game=1; gui.mode=HOST_GUI_HIDDEN; gui.started=SDL_GetTicks64(); }
static int native_offer(void);
void host_gui_previous_crash(const char *log) { if (gui.configured) snprintf(gui.crash_log,sizeof(gui.crash_log),"%s",log ? log : ""); }
int host_gui_wants_shell(void) { return gui.configured && (*gui.crash_log || native_offer()); }
host_gui_result_t host_gui_shell(void) {
    gui.native_only=!gui.force_hub && gui.options->disc1_configured && native_offer();
    gui.mode=(!gui.force_hub && !gui.options->disc1_configured) || gui.native_only ? HOST_GUI_SETUP : HOST_GUI_HUB;
    gui.result=HOST_GUI_RUNNING; request_focus(); gui.setup_step=gui.native_only ? 2 : 0;
    if (gui.services.loop) gui.result=gui.services.loop(gui.services.context,1);
    return gui.result;
}
void host_gui_open(void) {
    if (!gui.configured) return;
    gui.mode=HOST_GUI_OVERLAY; gui.result=HOST_GUI_RUNNING; gui.want_open=0;
    gui.pane=gui.requested; request_focus(); gui.focus_rail=0;
    if (gui.services.loop) gui.result=gui.services.loop(gui.services.context,0);
    gui.mode=HOST_GUI_HIDDEN;
}
void host_gui_shutdown(void) {
    if (!gui.configured) return;
    persist_ui(); gui_media_stop(); host_gui_test_shutdown(); close_form(); memset(&gui,0,sizeof(gui));
}
static void set_result(host_gui_result_t result) { gui.result=result; }
static int ready_to_play(void) {
#ifdef FF8_PLAYER
    return gui.discs[0].state==GUI_DISC_VERIFIED && *gui.options->cards[0];
#else
    return 1;
#endif
}
static void play(void) {
    if (overlay()) { host_gui_close(); return; }
    if (ready_to_play()) set_result(HOST_GUI_PLAY);
}
/* The browser can open over the compiler dialog and goes back to it. */
static void close_browser(void) { gui.modal=gui.after_browser; gui.after_browser=MODAL_NONE; request_focus(); }
static void finish_setup(void);
static void back(int controller) {
    if (gui.keyboard) { gui.keyboard=0; gui.edit=NULL; return; }
    if (gui.modal==MODAL_BROWSER) {
        if (controller && gui_browser_up(&gui.browser)) return;
        close_browser(); return;
    }
    if (gui.modal!=MODAL_NONE) { gui.modal=MODAL_NONE; request_focus(); return; }
    if (gui.pane==HOST_GUI_ACHIEVEMENTS && gui_achievements_back()) return;
    if (gui.ra_form) { close_form(); request_focus(); return; }
    if (gui.mode==HOST_GUI_SETUP && gui.setup_step) {
        if (gui.native_only) finish_setup();
        else { --gui.setup_step; request_focus(); }
        return;
    }
    if (!gui.focus_rail && gui.mode!=HOST_GUI_SETUP) { gui.focus_rail=1; gui_ui_focus(overlay() ? "nav.game" : "nav.play"); return; }
    if (overlay()) host_gui_close();
    else { gui.modal=MODAL_QUIT; request_focus(); }
}
static void pick(gui_pick_t kind,unsigned slot);
static void setup_next(void);
static void selected(const char *path,gui_pick_t kind,unsigned slot) {
    char folder[512],destination[512],error[512];
    parent_folder(path,folder,sizeof(folder)); snprintf(gui.last_folder,sizeof(gui.last_folder),"%s",folder); persist_ui();
    if (kind==GUI_PICK_DISC) {
        if (overlay() && slot==0) return;
        ++gui.generation;
        for (unsigned i=0;i<4;++i) if (*gui.options->discs[i]) gui_media_identify(gui.options->discs[i],0,gui.generation,3);
        gui.discs[slot].state=GUI_DISC_CHECKING; snprintf(gui.discs[slot].path,sizeof(gui.discs[slot].path),"%s",path);
        if (gui.mode==HOST_GUI_SETUP || gui.pane==HOST_GUI_DISCS) {
            char focus[64]; snprintf(focus,sizeof(focus),S("ui.disc.action"),slot+1,"replace"); gui_ui_focus(focus);
        }
        gui_media_identify(path,0,gui.generation,0);
    } else if (kind==GUI_PICK_CARD) {
        if (gui_card_import(path,gui.data,destination,sizeof(destination),error,sizeof(error))) {
            char key[16]; snprintf(key,sizeof(key),"card%u",slot+1); changed(key,destination);
            gui.card_imported[slot]=1; gui.error[0]=0;
        } else snprintf(gui.error,sizeof(gui.error),"%s",error);
    } else if (kind==GUI_PICK_ARCHIVE) {
        snprintf(gui.native_archive,sizeof(gui.native_archive),"%s",path); gui.native_local=1;
        if (gui.services.native_install) gui.services.native_install(gui.services.context,path);
        request_focus();
    } else changed(kind==GUI_PICK_MOVIES ? "movies" : "screenshot-dir",path);
}
static void pick(gui_pick_t kind,unsigned slot) {
    char path[512]; int result=-1;
    gui.pick_kind=kind; gui.pick_slot=(int)slot;
    if (gui.input!=HOST_GUI_PAD && !(gui.services.fullscreen_exclusive && gui.services.fullscreen_exclusive(gui.services.context))) result=gui_ui_native_pick(kind,path,sizeof(path));
    if (result==1) { selected(path,kind,slot); return; }
    if (!result) return;
    memset(&gui.browser,0,sizeof(gui.browser));
    const char *folder=*gui.last_folder ? gui.last_folder : getenv(
#ifdef _WIN32
    "USERPROFILE"
#else
    "HOME"
#endif
    );
    if (!folder || !gui_browser_open(&gui.browser,folder,kind)) gui_browser_open(&gui.browser,
#ifdef _WIN32
    "C:/"
#else
    "/"
#endif
    ,kind);
    gui.browser_places=gui_browser_places(gui.browser_names,gui.browser_paths,32,gui.last_folder);
    gui.browser_result_count=0; gui.after_browser=gui.modal==MODAL_NATIVE ? MODAL_NATIVE : MODAL_NONE;
    gui.modal=MODAL_BROWSER; request_focus();
}
static void remove_disc(unsigned slot) {
    if (slot>=4 || (overlay() && !slot)) return;
    char key[16]; snprintf(key,sizeof(key),"disc%u",slot+1); changed(key,"");
    memset(gui.discs+slot,0,sizeof(gui.discs[slot])); ++gui.generation;
    if (gui.mode==HOST_GUI_SETUP && !slot) gui_ui_focus("btn.browse");
    else { char focus[64]; snprintf(focus,sizeof(focus),S("ui.disc.action"),slot+1,"add"); gui_ui_focus(focus); }
    for (unsigned i=0;i<4;++i) if (*gui.options->discs[i] && gui.discs[i].state==GUI_DISC_CHECKING) gui_media_identify(gui.options->discs[i],0,gui.generation,3);
}
static void drop(const char *path) {
    struct stat st; size_t length=strlen(path);
    if (stat(path,&st)) { snprintf(gui.error,sizeof(gui.error),S("disc.err.missing"),path); return; }
    if (S_ISDIR(st.st_mode)) { if (gui.pane==HOST_GUI_VIDEO) changed("movies",path); return; }
    if (length>=4 && !strcasecmp(path+length-4,".zip")) {
        int asked=(gui.mode==HOST_GUI_SETUP && gui.setup_step==2) || gui.modal==MODAL_NATIVE;
        if (asked && gui.native.compiler!=HOST_GUI_COMPILER_READY && gui.native.compiler!=HOST_GUI_COMPILER_INSTALLING) selected(path,GUI_PICK_ARCHIVE,0);
        return;
    }
    if (length>=4 && (!strcasecmp(path+length-4,".cue") || !strcasecmp(path+length-4,".bin"))) {
        unsigned slot=0;
        while (slot<4 && *gui.options->discs[slot]) ++slot;
        if (gui.mode==HOST_GUI_SETUP) slot=0;
        if (slot==4) slot=overlay() ? 1 : 0;
        selected(path,GUI_PICK_DISC,slot); return;
    }
    if (st.st_size==131072) selected(path,GUI_PICK_CARD,0);
    else snprintf(gui.error,sizeof(gui.error),"%s",S("card.invalid"));
}
void host_gui_event(const SDL_Event *event) {
    if (!gui.configured) return;
    int keyboard_was_open=gui.keyboard;
    if (event->type==SDL_KEYDOWN || event->type==SDL_TEXTINPUT) { gui.input=HOST_GUI_KEYBOARD; gui.keyboard=0; }
    if (event->type==SDL_MOUSEBUTTONDOWN || event->type==SDL_MOUSEMOTION || event->type==SDL_MOUSEWHEEL) gui.input=HOST_GUI_MOUSE;
    if (event->type==SDL_CONTROLLERBUTTONDOWN || event->type==SDL_CONTROLLERBUTTONUP) gui.input=HOST_GUI_PAD;
    if (event->type==SDL_CONTROLLERAXISMOTION && ((event->caxis.axis<=SDL_CONTROLLER_AXIS_RIGHTY && abs(event->caxis.value)>8000) || (event->caxis.axis>SDL_CONTROLLER_AXIS_RIGHTY && event->caxis.value>8000))) gui.input=HOST_GUI_PAD;
    if (!host_gui_visible()) return;
    if (gui.pane==HOST_GUI_ACHIEVEMENTS && !gui.modal && !gui.keyboard) gui_achievements_event(event);
    gui_ui_event(event);
    if (event->type==SDL_KEYDOWN || event->type==SDL_TEXTINPUT) {
        gui.input=HOST_GUI_KEYBOARD; gui.keyboard=0;
        if (event->type==SDL_KEYDOWN && !event->key.repeat) {
            SDL_Keycode key=event->key.keysym.sym;
            if (key==SDLK_ESCAPE && !keyboard_was_open) back(0);
            if (!gui.modal && !gui.keyboard && !gui_ui_wants_text() && (key==SDLK_q || key==SDLK_e)) section(key==SDLK_e ? 1 : -1);
        }
    }
    if (event->type==SDL_MOUSEBUTTONDOWN || event->type==SDL_MOUSEMOTION || event->type==SDL_MOUSEWHEEL) gui.input=HOST_GUI_MOUSE;
    if (event->type==SDL_CONTROLLERBUTTONDOWN || event->type==SDL_CONTROLLERBUTTONUP || event->type==SDL_CONTROLLERAXISMOTION) {
        if (event->type==SDL_CONTROLLERBUTTONDOWN) {
            unsigned button=event->cbutton.button;
            if (button==SDL_CONTROLLER_BUTTON_B) back(1);
            if (gui.keyboard && button==SDL_CONTROLLER_BUTTON_Y) gui.shift=!gui.shift;
            if (gui.keyboard && button==SDL_CONTROLLER_BUTTON_X && gui.edit && *gui.edit) {
                size_t n=strlen(gui.edit)-1; while (n && ((unsigned char)gui.edit[n]&0xc0)==0x80) --n; gui.edit[n]=0;
            }
            if (!gui.keyboard && button==SDL_CONTROLLER_BUTTON_Y) {
                if (gui.modal==MODAL_BROWSER) gui_ui_focus("browser.place.0");
                else if (!gui.modal && gui.pane==HOST_GUI_DISCS) {
                    unsigned slot=0; if (sscanf(gui_ui_focused(),"disc.%u.",&slot)==1 && slot) remove_disc(slot-1);
                }
            }
            if (!gui.modal && !gui.keyboard && (button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) section(button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ? 1 : -1);
            if (button==SDL_CONTROLLER_BUTTON_START && !gui.modal) {
                if (gui.mode==HOST_GUI_SETUP) setup_next();
                else play();
            }
        }
        if (event->type==SDL_CONTROLLERBUTTONUP && event->cbutton.button==SDL_CONTROLLER_BUTTON_GUIDE && overlay()) host_gui_close();
    }
    if (event->type==SDL_DROPBEGIN) gui.drop_active=1;
    if (event->type==SDL_DROPCOMPLETE) gui.drop_active=0;
    if (event->type==SDL_DROPFILE && event->drop.file) { gui.drop_active=0; drop(event->drop.file); }
    if (event->type==SDL_QUIT) set_result(HOST_GUI_QUIT);
}
static void apply_results(void) {
    gui_disc_result_t result;
    while (gui_media_result(&result)) {
        unsigned pending=4;
        if (result.generation!=gui.generation) continue;
        if (result.scan==4) {
            size_t folder_length=strlen(gui.browser.folder);
            if (!folder_length || strncmp(result.path,gui.browser.folder,folder_length) || (gui.browser.folder[folder_length-1]!='/' && result.path[folder_length]!='/')) continue;
            unsigned index;
            for (index=0;index<gui.browser_result_count;++index) if (!strcmp(gui.browser_results[index].path,result.path)) break;
            if (index<GUI_BROWSER_MAX) { gui.browser_results[index]=result; if (index==gui.browser_result_count) ++gui.browser_result_count; }
            continue;
        }
        for (unsigned i=0;i<4;++i) if (!strcmp(gui.discs[i].path,result.path)) { pending=i; break; }
        if (result.scan==3 && pending==4) continue;
        if (result.state!=GUI_DISC_VERIFIED) {
            if (result.scan==1) continue;
            if (pending<4) gui.discs[pending]=result;
            continue;
        }
        unsigned slot=result.disc-1;
        if (slot>=4 || (overlay() && slot==0 && result.scan!=3)) continue;
        /* A rescan can relocate missing media, but never replace a verified
         * selection with another copy of the same disc. */
        int relocated=result.scan==1 && gui.discs[slot].state==GUI_DISC_MISSING;
        if (*gui.options->discs[slot] && strcmp(gui.options->discs[slot],result.path) && pending!=slot && !relocated) {
            if (!result.scan && pending<4) {
                gui.discs[pending]=result; gui.discs[pending].state=GUI_DISC_ERROR;
                snprintf(gui.discs[pending].detail,sizeof(gui.discs[pending].detail),S("disc.err.duplicate"),slot+1);
            }
            continue;
        }
        if (pending<4 && pending!=slot) {
            memset(gui.discs+pending,0,sizeof(gui.discs[pending]));
            if (*gui.options->discs[pending]) {
                snprintf(gui.discs[pending].path,sizeof(gui.discs[pending].path),"%s",gui.options->discs[pending]);
                gui.discs[pending].state=GUI_DISC_CHECKING;
                gui_media_identify(gui.options->discs[pending],0,gui.generation,3);
            }
        }
        gui.discs[slot]=result;
        if (!result.scan) {
            if (gui.mode==HOST_GUI_SETUP) gui_ui_focus(slot ? "btn.browse" : "btn.continue");
            else if (gui.pane==HOST_GUI_DISCS) {
                char focus[64]; snprintf(focus,sizeof(focus),S("ui.disc.action"),slot+1,"replace"); gui_ui_focus(focus);
            }
        }
        if (!slot && gui.pane==HOST_GUI_GAME && gui.mode==HOST_GUI_HUB) request_focus();
        if (result.scan!=3) {
            char key[16]; snprintf(key,sizeof(key),"disc%u",slot+1); changed(key,result.path);
            if (!slot && result.scan==0) { char folder[512]; parent_folder(result.path,folder,sizeof(folder)); gui_media_scan(folder,gui.generation); }
        }
    }
}
static void new_card(unsigned slot) {
    char path[512],error[512],key[16];
    if (gui_card_new(gui.data,slot,path,sizeof(path),error,sizeof(error))) {
        snprintf(key,sizeof(key),"card%u",slot+1); changed(key,path); gui.card_imported[slot]=0; gui.error[0]=0;
    } else snprintf(gui.error,sizeof(gui.error),"%s",error);
}
static int has_native(void) { return gui.services.native!=NULL; }
static int native_offer(void) { return has_native() && !gui.native_offered && gui.native.compiler!=HOST_GUI_COMPILER_READY; }
static void finish_setup(void) {
    int only=gui.native_only;
    if (has_native() && !gui.native_offered) { gui.native_offered=1; persist_ui(); }
    gui.native_only=0; gui.mode=HOST_GUI_HUB; gui.pane=HOST_GUI_GAME; request_focus();
    /* Offered on its own, the step stood between the player and the game. */
    if (only && !*gui.crash_log && ready_to_play()) set_result(HOST_GUI_PLAY);
}
static void setup_next(void) {
    if (!gui.setup_step) {
        if (gui.discs[0].state==GUI_DISC_VERIFIED) { gui.setup_step=1; request_focus(); if (!*gui.options->cards[0]) new_card(0); }
    } else if (gui.setup_step==1) {
        if (!*gui.options->cards[0]) return;
        if (has_native()) { gui.setup_step=2; request_focus(); }
        else finish_setup();
    } else finish_setup();
}
static void rail(void) {
    static const char *const ids[]={"nav.play","nav.discs","nav.card","nav.video","nav.controls","nav.ra","nav.achievements"};
    static const unsigned order[]={HOST_GUI_GAME,HOST_GUI_ACHIEVEMENTS,HOST_GUI_DISCS,HOST_GUI_CARD,HOST_GUI_VIDEO,HOST_GUI_CONTROLS,HOST_GUI_RA};
    gui_ui_panel(R(0,0,gui.rail,gui.height-56),0x101016ff,0);
    gui_ui_emblem(R(28,26,48,48)); label("app.name",86,32,gui.rail-98,GUI_TITLE,GUI_TEXT);
#ifdef _WIN32
    const char *platform=S("platform.windows");
#else
    const char *platform=S("platform.linux");
#endif
    char version[128]; snprintf(version,sizeof(version),S("app.version"),FF8_VERSION,platform); text("app.version",version,32,88,gui.rail-48,GUI_SUB,GUI_MUTED);
    for (unsigned row=0;row<7;++row) {
        unsigned i=order[row];
        const char *id=i==0 && overlay() ? "nav.game" : ids[i];
        if (gui_ui_row(id,S(id),NULL,R(0,142+(float)row*46,gui.rail,44),gui.pane==(host_gui_pane_t)i ? GUI_SELECTED : 0)) change_pane((host_gui_pane_t)i);
    }
    if (gui_ui_row("nav.quit",S("nav.quit"),NULL,R(0,492,gui.rail,44),0)) { gui.modal=MODAL_QUIT; request_focus(); }
}
static void hints(void) {
    int type=gui.services.controller_type ? gui.services.controller_type(gui.services.context) : SDL_CONTROLLER_TYPE_XBOXONE;
    const char *select="hint.key.select",*back_key="hint.key.back",*section="hint.key.section",*secondary="hint.pad.xbox.secondary",*backspace="hint.pad.xbox.backspace";
    if (gui.input==HOST_GUI_PAD) {
        if (type==SDL_CONTROLLER_TYPE_PS3 || type==SDL_CONTROLLER_TYPE_PS4 || type==SDL_CONTROLLER_TYPE_PS5) { select="hint.pad.ps.select"; back_key="hint.pad.ps.back"; secondary="hint.pad.ps.secondary"; backspace="hint.pad.ps.backspace"; }
        else if (type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO) { select="hint.pad.nintendo.select"; back_key="hint.pad.nintendo.back"; secondary="hint.pad.nintendo.secondary"; backspace="hint.pad.nintendo.backspace"; }
        else { select="hint.pad.xbox.select"; back_key="hint.pad.xbox.back"; }
        section="hint.pad.section";
    }
    gui_ui_panel(R(0,gui.height-56,gui.width,56),0x101016ff,0);
    gui_ui_line(0,gui.height-56,gui.width,gui.height-56,0xffffff14,1);
    if (gui.keyboard) {
        gui_ui_keycap("hint.type",S(select),S("hint.type"),R(48,gui.height-40,200,24));
        gui_ui_keycap("hint.backspace",S(backspace),S("hint.backspace"),R(260,gui.height-40,230,24));
        gui_ui_keycap("hint.shift",S(secondary),S("hint.shift"),R(510,gui.height-40,200,24));
        gui_ui_keycap("hint.done",S(back_key),S("hint.done"),R(730,gui.height-40,200,24));
    } else {
        if (gui.pane==HOST_GUI_ACHIEVEMENTS) gui_ui_keycap("hint.filter",S(gui.input==HOST_GUI_PAD ? "hint.pad.filter" : "hint.key.filter"),S("hint.filter"),R(48,gui.height-40,230,24));
        else gui_ui_keycap("hint.select",S(select),S("hint.select"),R(48,gui.height-40,230,24));
        gui_ui_keycap("hint.back",S(back_key),S(gui.modal==MODAL_BROWSER && gui.input==HOST_GUI_PAD ? "hint.up" : "hint.back"),R(310,gui.height-40,300,24));
        if (gui.input==HOST_GUI_PAD && gui.modal==MODAL_BROWSER) gui_ui_keycap("hint.places",S(secondary),S("hint.places"),R(gui.width-310,gui.height-40,260,24));
        else if (gui.mode!=HOST_GUI_SETUP && !gui.modal) gui_ui_keycap("hint.section",S(section),S("hint.section"),R(gui.width-350,gui.height-40,300,24));
    }
}

static void pane_header(const char *title,const char *subtitle) {
    label(title,gui.rail+48,32,gui.width-gui.rail-96,GUI_TITLE,GUI_TEXT);
    if (subtitle) text("pane.subtitle",subtitle,gui.rail+48,72,gui.width-gui.rail-96,GUI_SUB,GUI_DIM);
}
static void card_status(unsigned slot,float x,float y,float width) {
    char summary[256]; gui_card_summary_t card;
    const char *path=gui.options->cards[slot];
    if (*path && gui_card_summary(path,&card)) {
        snprintf(summary,sizeof(summary),S("card.summary"),card.blocks_used,card.ff8_saves);
        text(slot ? "card.2.summary" : "card.1.summary",summary,x,y,width,GUI_SUB,GUI_DIM);
    } else label("card.empty",x,y,width,GUI_SUB,GUI_MUTED);
}
static void disc_status_data(const gui_disc_result_t *disc,unsigned slot,char *out,size_t capacity) {
    if (disc->state==GUI_DISC_EMPTY) snprintf(out,capacity,"%s",S("disc.not_added"));
    else if (disc->state==GUI_DISC_CHECKING) snprintf(out,capacity,"%s",S("disc.checking"));
    else if (disc->state==GUI_DISC_VERIFIED) {
        static const char *const serials[]={"disc.serial.1","disc.serial.2","disc.serial.3","disc.serial.4"};
        const char *serial=S(serials[disc->disc>=1 && disc->disc<=4 ? disc->disc-1 : slot]);
        if (disc->full) snprintf(out,capacity,S("disc.full"),serial,disc->elapsed);
        else snprintf(out,capacity,S("disc.verified"),serial);
    } else {
        const char *id=gui_disc_error_id(disc->detail);
        const char *duplicate=S("disc.err.duplicate"),*placeholder=strstr(duplicate,"%u");
        if (placeholder && !strncmp(disc->detail,duplicate,(size_t)(placeholder-duplicate))) snprintf(out,capacity,"%s",disc->detail);
        else if (!strcmp(id,"disc.err.missing")) {
            /* The identifier names the failing BIN, which can differ from
             * the selected CUE. Preserve that cause in the friendly row. */
            const char *separator=strstr(disc->detail,": ");
            if (separator) {
                char path[512]; snprintf(path,sizeof(path),"%.*s",(int)(separator-disc->detail),disc->detail);
                snprintf(out,capacity,S(id),path);
            } else snprintf(out,capacity,S(id),disc->path);
        }
        else if (!strcmp(id,"disc.err.size") || !strcmp(id,"disc.err.img_hash")) {
            char image[32]; snprintf(image,sizeof(image),S("disc.image"),slot+1);
            snprintf(out,capacity,S(id),strstr(disc->detail,"IMG") ? image : base_name(disc->path));
        }
        else if (!strcmp(id,"disc.err.other_game")) {
            const char *serial=strchr(disc->detail,':'); snprintf(out,capacity,S(id),serial ? serial+2 : S("ui.dash"));
        } else snprintf(out,capacity,"%s",S(id));
        if (disc->full) {
            char detail[512]; snprintf(detail,sizeof(detail),"%s",out);
            snprintf(out,capacity,S("disc.verify.elapsed"),detail,disc->elapsed);
        }
    }
}
static void disc_status(unsigned slot,char *out,size_t capacity) { disc_status_data(gui.discs+slot,slot,out,capacity); }
static void draw_disc_row(unsigned slot,float x,float y,float width,float height) {
    char id[64],title[64],status[512]; const gui_disc_result_t *disc=gui.discs+slot;
        snprintf(title,sizeof(title),S("disc.n"),slot+1); snprintf(id,sizeof(id),"disc.%u.title",slot+1);
        gui_ui_panel(R(x,y,width,height),0x13131aff,7);
        gui_ui_panel(R(x+14,y+15,34,34),disc->state==GUI_DISC_VERIFIED ? 0x9fb4d8ff : 0x1e1e26ff,8);
        char number[16]; snprintf(number,sizeof(number),S("ui.number"),slot+1); text(id,number,x+24,y+21,24,GUI_MONO,disc->state==GUI_DISC_VERIFIED ? GUI_MUTED : GUI_DIM);
        snprintf(id,sizeof(id),"disc.%u.name",slot+1); text(id,*disc->path ? base_name(disc->path) : title,x+64,y+12,width-(disc->state==GUI_DISC_MISSING ? 420 : 320),GUI_REGULAR,GUI_TEXT);
        disc_status(slot,status,sizeof(status)); snprintf(id,sizeof(id),"disc.%u.status",slot+1);
        text(id,status,x+64,y+43,width-(disc->state==GUI_DISC_MISSING ? 420 : 320),GUI_SUB,disc->state==GUI_DISC_VERIFIED ? GUI_OK : disc->state==GUI_DISC_ERROR || disc->state==GUI_DISC_MISSING ? GUI_ERROR : GUI_DIM);
        if (overlay() && slot==0) { label("tag.in_use",x+width-126,y+28,120,GUI_SUB,GUI_MUTED); return; }
        const char *verb=disc->state==GUI_DISC_EMPTY ? "add" : disc->state==GUI_DISC_MISSING ? "locate" : "replace";
        snprintf(id,sizeof(id),S("ui.disc.action"),slot+1,verb);
        if (gui_ui_button(id,S(disc->state==GUI_DISC_EMPTY ? "btn.add" : disc->state==GUI_DISC_MISSING ? "btn.locate" : "btn.replace"),R(x+width-228,y+20,108,38),0)) pick(GUI_PICK_DISC,slot);
        if (gui.first_focus && slot==(overlay() ? 1u : 0u)) gui_ui_focus(id);
        if (disc->state==GUI_DISC_MISSING) {
            snprintf(id,sizeof(id),S("ui.disc.action"),slot+1,"rescan");
            if (gui_ui_button(id,S("btn.rescan"),R(x+width-346,y+20,108,38),0)) {
                char folder[512]; parent_folder(disc->path,folder,sizeof(folder)); gui_media_scan(folder,gui.generation);
            }
        }
        snprintf(id,sizeof(id),S("ui.disc.action"),slot+1,"remove");
        if (disc->state!=GUI_DISC_EMPTY && gui_ui_button(id,S("btn.remove"),R(x+width-110,y+20,100,38),GUI_DANGER)) {
            remove_disc(slot);
        }
    }
/* ---- native code ---- */
static unsigned megabytes(unsigned long long bytes) { return (unsigned)((bytes+1048575)/1048576); }
const char *gui_native_reason_id(const char *reason) {
    /* The installer's reasons are its own words; these are the player's. */
    static const struct { const char *needle,*id; } map[]={
        {"Cancelled","native.err.cancelled"},{"Download failed","native.err.download"},
        {"No download URL","native.err.no_url"},{"Archive size","native.err.archive"},
        {"Archive SHA-256","native.err.archive"},{"Cannot read archive","native.err.read"},
        {"Invalid ZIP","native.err.zip"},{"self-check","native.err.check"},{"cannot be found","native.err.check"},
        {"Another installer","native.err.busy"},{"Cannot start","native.err.other"},{"Cannot","native.err.disk"}};
    for (size_t i=0;reason && i<sizeof(map)/sizeof(map[0]);++i) if (strstr(reason,map[i].needle)) return map[i].id;
    return "native.err.other";
}
static const char *native_step_id(host_gui_step_t step) {
    static const char *const ids[]={"native.step.download","native.step.verify","native.step.extract","native.step.check","native.step.install"};
    return ids[(unsigned)step<5 ? (unsigned)step : 4];
}
/* Bytes are counted only while downloading and checking the archive; the
 * later steps count entries and checks, which say nothing as megabytes. */
static int native_amount(const host_gui_native_t *n,char *out,size_t capacity) {
    if ((n->step!=HOST_GUI_STEP_DOWNLOAD && n->step!=HOST_GUI_STEP_VERIFY) || !n->total) return 0;
    snprintf(out,capacity,S("native.amount"),(unsigned)(n->done/1048576),megabytes(n->total)); return 1;
}
static void native_append(char *out,size_t capacity,const char *part) {
    size_t n=strlen(out);
    if (n<capacity) snprintf(out+n,capacity-n,"%s%s",n ? S("native.separator") : "",part);
}
static void native_left(int seconds,char *out,size_t capacity) {
    unsigned minutes=(unsigned)(seconds+59)/60;
    /* Coarser the further off: a closer finish earns finer steps. */
    if (minutes>60) minutes=(minutes+9)/10*10;
    else if (minutes>10) minutes=(minutes+4)/5*5;
    if (!minutes) minutes=1;
    if (minutes<60) snprintf(out,capacity,S("native.minutes"),minutes);
    else snprintf(out,capacity,S("native.hours"),minutes/60,minutes%60);
}
void gui_native_summary(const host_gui_native_t *n,char *out,size_t capacity,int *alert) {
    char part[128];
    *alert=0; *out=0;
    if (n->compiler==HOST_GUI_COMPILER_INSTALLING) {
        native_append(out,capacity,S(native_step_id(n->step)));
        if (native_amount(n,part,sizeof(part))) native_append(out,capacity,part);
    } else if (n->compiler==HOST_GUI_COMPILER_FAILED) {
        *alert=1; snprintf(out,capacity,"%s",S(gui_native_reason_id(n->reason)));
    } else if (n->compiler==HOST_GUI_COMPILER_MISSING) snprintf(out,capacity,"%s",S("native.missing"));
    else if (n->build==HOST_GUI_BUILD_OFF || !n->sets) snprintf(out,capacity,"%s",S("native.ready"));
    else {
        snprintf(out,capacity,S("native.count"),n->ready,n->sets);
        if (n->failed) { *alert=1; snprintf(part,sizeof(part),S("native.part.failed"),n->failed); native_append(out,capacity,part); }
        if (n->build==HOST_GUI_BUILD_STOPPED) { *alert=1; native_append(out,capacity,S("native.part.stopped")); }
        else if (n->build==HOST_GUI_BUILD_RUNNING && n->seconds_left>=0) {
            char left[64]; native_left(n->seconds_left,left,sizeof(left));
            snprintf(part,sizeof(part),S("native.part.left"),left); native_append(out,capacity,part);
        }
    }
}
static void native_install(const char *archive) {
    gui.native_local=archive!=NULL;
    if (gui.services.native_install) gui.services.native_install(gui.services.context,archive);
    request_focus();
}
/* The compiler's last words: the line before its count of errors, when it
 * ends with one. */
static const char *last_words(const char *words,char *out,size_t capacity) {
    const char *end=words+strlen(words),*start;
    for (int pass=0;pass<2;++pass) {
        while (end>words && (end[-1]=='\n' || end[-1]=='\r')) --end;
        start=end; while (start>words && start[-1]!='\n') --start;
        snprintf(out,capacity,"%.*s",(int)(end-start),start);
        if (!strstr(out," generated.") || start==words) break;
        end=start;
    }
    return out;
}
static void open_native(void) {
    if (gui.native.compiler!=HOST_GUI_COMPILER_READY) { gui.modal=MODAL_NATIVE; request_focus(); return; }
    if (!gui.native.failed && gui.native.build!=HOST_GUI_BUILD_STOPPED) return;
    gui.failure_count=gui.services.native_failures ? gui.services.native_failures(gui.services.context,gui.failures,6) : 0;
    gui.modal=MODAL_NATIVE; request_focus();
}
static void native_row(float x,float y,float width,float height) {
    char sub[256]; int alert;
    if (!has_native()) return;
    gui_native_summary(&gui.native,sub,sizeof(sub),&alert);
    if (gui_ui_row("hub.native",S("nav.native"),sub,R(x,y,width,height),alert ? GUI_ALERT : 0)) open_native();
}
/* The compiler step, on the setup page and in the dialog the hub opens. */
static void draw_native_install(float x,float y,float width) {
    const host_gui_native_t *n=&gui.native;
    int busy=n->compiler==HOST_GUI_COMPILER_INSTALLING,ready=n->compiler==HOST_GUI_COMPILER_READY;
    float half=(width-20)/2,status=y+284;
    char size[64],archive[160],line[160];
    label("setup.native.title",x,y,width,GUI_TITLE,GUI_TEXT);
    label("setup.native.sub",x,y+46,width,GUI_SUB,GUI_DIM);
    snprintf(size,sizeof(size),S("native.size"),megabytes(n->bytes));
    if (*n->archive) snprintf(archive,sizeof(archive),"%s",n->archive);
    else snprintf(archive,sizeof(archive),S("native.zip"),megabytes(n->bytes));
    if (gui_ui_row("setup.native.download",S("setup.native.download"),n->downloadable ? size : S("native.unavailable"),R(x,y+106,half,150),
                   !n->downloadable || busy || ready ? GUI_DISABLED : 0)) native_install(NULL);
    if (gui_ui_row("setup.native.archive",S("setup.native.archive"),archive,R(x+half+20,y+106,half,150),busy || ready ? GUI_DISABLED : 0))
        pick(GUI_PICK_ARCHIVE,0);
    if (busy) {
        float bar=width-200,share=n->total ? (float)((double)n->done/(double)n->total) : 0;
        text("native.step",S(native_step_id(n->step)),x+18,status,bar,GUI_REGULAR,GUI_TEXT);
        if (native_amount(n,line,sizeof(line))) {
            gui_ui_panel(R(x+18,status+38,bar,6),0x1e1e26ff,3);
            gui_ui_panel(R(x+18,status+38,bar*(share>1 ? 1 : share),6),0x9fb4d8ff,3);
            text("native.amount",line,x+18,status+52,bar,GUI_SUB,GUI_DIM);
        }
        if (gui_ui_button("native.cancel",S("btn.cancel"),R(x+width-150,status-4,150,42),0) && gui.services.native_cancel)
            gui.services.native_cancel(gui.services.context);
    } else if (n->compiler==HOST_GUI_COMPILER_FAILED) {
        text("native.error",S(gui_native_reason_id(n->reason)),x+18,status+6,width-200,GUI_SUB,GUI_ERROR);
        if (gui_ui_button("native.retry",S("btn.retry"),R(x+width-150,status-4,150,42),0)) native_install(gui.native_local ? gui.native_archive : NULL);
    } else if (ready) label("native.installed",x+18,status,width-36,GUI_REGULAR,GUI_OK);
    if (gui.first_focus && gui.modal!=MODAL_BROWSER)
        gui_ui_focus(ready ? (gui.modal==MODAL_NATIVE ? "btn.close" : "btn.finish") : busy ? "native.cancel" :
                     n->compiler==HOST_GUI_COMPILER_FAILED ? "native.retry" : n->downloadable ? "setup.native.download" : "setup.native.archive");
}
static void draw_native_details(float x,float y,float width) {
    char sub[256],id[64],words[512],more[64]; int alert; unsigned shown=0;
    float row=y+96;
    label("nav.native",x,y,width,GUI_TITLE,GUI_TEXT);
    gui_native_summary(&gui.native,sub,sizeof(sub),&alert);
    text("native.summary",sub,x,y+46,width,GUI_SUB,alert ? GUI_ERROR : GUI_DIM);
    if (gui.native.build==HOST_GUI_BUILD_STOPPED && *gui.native.error) { text("native.stop",gui.native.error,x,row,width,GUI_MONO,GUI_DIM); row+=40; }
    for (;shown<gui.failure_count && shown<6;++shown,row+=36) {
        snprintf(id,sizeof(id),"native.failure.%u",shown); text(id,gui.failures[shown].name,x,row,180,GUI_MONO,GUI_TEXT);
        snprintf(id,sizeof(id),"native.failure.%u.words",shown);
        text(id,last_words(gui.failures[shown].words,words,sizeof(words)),x+190,row,width-190,GUI_SUB,GUI_DIM);
    }
    if (gui.native.failed>shown) { snprintf(more,sizeof(more),S("native.more"),gui.native.failed-shown); text("native.more",more,x,row,width,GUI_SUB,GUI_MUTED); }
    if (gui.first_focus) gui_ui_focus("btn.close");
}
static void open_log(void) {
    char url[1100];
    snprintf(url,sizeof(url),"file://%s%s",gui.crash_log[0]=='/' ? "" : "/",gui.crash_log);
    /* A scripted run must not start the desktop's viewer. */
    if (host_gui_test_active()) fprintf(stderr,"GUI test: open %s\n",url);
    else SDL_OpenURL(url);
}
static void draw_setup(void) {
    float x=(gui.width-800)/2;
    gui_ui_emblem(R(48,24,44,44)); label("app.name",106,30,200,GUI_TITLE,GUI_TEXT);
    if (has_native()) {
        label("setup.step1",gui.width-480,38,120,GUI_SUB,gui.setup_step==0 ? GUI_TEXT : GUI_MUTED);
        label("setup.step2",gui.width-340,38,150,GUI_SUB,gui.setup_step==1 ? GUI_TEXT : GUI_MUTED);
        label("setup.step3",gui.width-170,38,130,GUI_SUB,gui.setup_step==2 ? GUI_TEXT : GUI_MUTED);
    } else {
        label("setup.step1",gui.width-340,38,150,GUI_SUB,gui.setup_step ? GUI_MUTED : GUI_TEXT);
        label("setup.step2",gui.width-190,38,160,GUI_SUB,gui.setup_step ? GUI_TEXT : GUI_MUTED);
    }
    if (gui.setup_step==2) {
        int going=gui.native.compiler==HOST_GUI_COMPILER_READY || gui.native.compiler==HOST_GUI_COMPILER_INSTALLING;
        draw_native_install(x,120,800);
        if (!gui.native_only && button("btn.back",48,gui.height-124,120,0)) { gui.setup_step=1; request_focus(); }
        /* An installation still running goes on in the background. */
        if (button(going ? "btn.finish" : "btn.not_now",gui.width-198,gui.height-124,150,going ? GUI_PRIMARY : 0)) finish_setup();
    } else if (!gui.setup_step) {
        if (gui.discs[0].state==GUI_DISC_EMPTY) {
            label("setup.disc1.title",x,120,800,GUI_TITLE,GUI_TEXT);
            label("setup.disc1.supported",x,164,800,GUI_SUB,GUI_DIM);
            label("setup.disc1.unsupported",x,192,800,GUI_SUB,GUI_MUTED);
            if (button("btn.browse",x,238,150,GUI_PRIMARY)) pick(GUI_PICK_DISC,0);
            if (gui.first_focus) gui_ui_focus("btn.browse");
            unsigned line=gui.drop_active ? 0x9fb4d8ff : 0x3a3a46ff;
            gui_ui_panel(R(x,306,800,116),gui.drop_active ? 0x9fb4d814 : 0x13131aff,10);
            for (int i=0;i<800;i+=14) { gui_ui_line(x+i,306,x+i+7,306,line,1); gui_ui_line(x+i,422,x+i+7,422,line,1); }
            for (int i=0;i<116;i+=14) { gui_ui_line(x,306+i,x,313+i,line,1); gui_ui_line(x+800,306+i,x+800,313+i,line,1); }
            gui_ui_text_centred("setup.disc1.drop",S(gui.drop_active ? "setup.disc1.drop.active" : "setup.disc1.drop"),R(x+20,306,760,116),GUI_SUB,gui.drop_active ? GUI_TEXT : GUI_MUTED);
        } else {
            label("nav.discs",x,100,800,GUI_TITLE,GUI_TEXT);
            label("hub.subtitle",x,148,800,GUI_SUB,GUI_DIM);
            for (unsigned i=0;i<4;++i) draw_disc_row(i,x,200+(float)i*82,800,76);
        }
        label("setup.discs.later",48,gui.height-128,gui.width-300,GUI_SUB,GUI_DIM);
        if (button("btn.continue",gui.width-198,gui.height-124,150,gui.discs[0].state==GUI_DISC_VERIFIED ? GUI_PRIMARY : GUI_DISABLED)) setup_next();
    } else {
        label("setup.card.title",x,120,800,GUI_TITLE,GUI_TEXT); label("setup.card.sub",x,166,800,GUI_SUB,GUI_DIM);
        if (gui_ui_row("setup.card.new",S("setup.card.new"),NULL,R(x,226,390,150),gui.card_imported[0] ? 0 : GUI_SELECTED)) new_card(0);
        if (gui_ui_row("setup.card.import",S("setup.card.import"),S("setup.card.import.help"),R(x+410,226,390,150),gui.card_imported[0] ? GUI_SELECTED : 0)) pick(GUI_PICK_CARD,0);
        card_status(0,x+18,404,764);
        if (*gui.options->cards[0]) text("card.1.path",base_name(gui.options->cards[0]),x+18,440,764,GUI_MONO,GUI_DIM);
        if (button("btn.back",48,gui.height-124,120,0)) { gui.setup_step=0; request_focus(); }
        const char *next=has_native() ? "btn.continue" : "btn.finish";
        if (button(next,gui.width-198,gui.height-124,150,*gui.options->cards[0] ? GUI_PRIMARY : GUI_DISABLED)) setup_next();
        if (gui.first_focus) gui_ui_focus(*gui.options->cards[0] ? next : "setup.card.new");
    }
    if (*gui.error) text("setup.error",gui.error,x,530,800,GUI_SUB,GUI_ERROR);
}
static void draw_game(void) {
    float x=gui.rail+48,width=gui.width-gui.rail-96; char sub[256],time[64]; unsigned slot=gui.services.state_slot ? gui.services.state_slot(gui.services.context) : 0;
    if (overlay()) {
        uint64_t minutes=(SDL_GetTicks64()-gui.started)/60000;
        snprintf(time,sizeof(time),S("game.time"),(unsigned)(minutes/60),(unsigned)(minutes%60));
        snprintf(sub,sizeof(sub),S("game.sub"),gui.services.disc ? gui.services.disc(gui.services.context) : 1,time);
        pane_header("game.paused",sub);
        if (button("btn.resume",x,126,170,GUI_PRIMARY)) host_gui_close();
        if (gui.first_focus) gui_ui_focus("btn.resume");
        gui_ui_control_row(R(x,200,width,52));
        int next=gui_ui_stepper("game.slot",R(x+width-196,200,196,52),(int)slot,0,9,1);
        control_label("game.slot","game.slot",x+18,212,240,GUI_REGULAR,GUI_TEXT);
        if (next!=(int)slot && gui.services.action) gui.services.action(gui.services.context,next==(int)((slot+9)%10) ? SDL_SCANCODE_F6 : SDL_SCANCODE_F8);
        char date[64]; int available=gui.services.state_available && gui.services.state_available(gui.services.context,slot,date,sizeof(date));
        if (available) snprintf(sub,sizeof(sub),S("game.slot.saved"),slot,date);
        else snprintf(sub,sizeof(sub),S("game.slot.empty"),slot);
        text("game.slot.status",sub,x+18,260,width-36,GUI_SUB,GUI_DIM);
        if (gui_ui_row("game.save",S("game.save"),NULL,R(x,316,width,58),0)) { host_gui_close(); if (gui.services.action) gui.services.action(gui.services.context,SDL_SCANCODE_F5); }
        if (gui_ui_row("game.load",S("game.load"),available ? NULL : S("game.load.empty"),R(x,378,width,64),available ? 0 : GUI_DISABLED)) { host_gui_close(); if (gui.services.action) gui.services.action(gui.services.context,SDL_SCANCODE_F7); }
        if (gui_ui_row("game.shot",S("game.shot"),NULL,R(x,446,width,58),0)) { host_gui_close(); if (gui.services.action) gui.services.action(gui.services.context,SDL_SCANCODE_F12); }
        if (gui_ui_row("btn.view_ach",S("btn.view_ach"),NULL,R(x,514,width,58),0)) change_pane(HOST_GUI_ACHIEVEMENTS);
        native_row(x,582,width,58);
    } else {
        pane_header("nav.play",S("hub.subtitle"));
        if (button("btn.play",x,126,170,ready_to_play() ? GUI_PRIMARY : GUI_DISABLED)) play();
        if (gui.first_focus) gui_ui_focus(ready_to_play() ? "btn.play" : gui.discs[0].state==GUI_DISC_CHECKING ? "nav.play" : "hub.locate");
        if (!ready_to_play()) {
            label("disc.needed",x+190,138,width-360,GUI_SUB,GUI_ERROR);
            if (gui_ui_button("hub.locate",S("btn.locate"),R(x+width-150,126,150,42),gui.discs[0].state==GUI_DISC_CHECKING ? GUI_DISABLED : 0)) pick(GUI_PICK_DISC,0);
        } else if (*gui.crash_log) {
            char line[640]; snprintf(line,sizeof(line),S("crash.line"),base_name(gui.crash_log));
            text("crash.line",line,x+190,138,width-360,GUI_SUB,GUI_ERROR);
            if (gui_ui_button("crash.open",S("btn.open_log"),R(x+width-150,126,150,42),0)) open_log();
        }
        /* One more row when native code has one: the same rows, a little closer. */
        float top=has_native() ? 196 : 210,step=has_native() ? 68 : 72,tall=has_native() ? 64 : 68;
        unsigned added=0; for (unsigned i=0;i<4;++i) if (gui.discs[i].state==GUI_DISC_VERIFIED) ++added;
        snprintf(sub,sizeof(sub),S("hub.discs"),added);
        if (gui_ui_row("hub.discs",S("nav.discs"),sub,R(x,top,width,tall),0)) change_pane(HOST_GUI_DISCS);
        if (gui_ui_row("hub.card",S("nav.card"),*gui.options->cards[0] ? base_name(gui.options->cards[0]) : S("card.empty"),R(x,top+step,width,tall),0)) change_pane(HOST_GUI_CARD);
        snprintf(sub,sizeof(sub),S("hub.video"),S(gui.options->renderer ? "val.vulkan" : "val.soft"),(unsigned)gui.options->internal_scale,S(gui.options->crt==0 ? "val.off" : gui.options->crt==1 ? "val.pvm" : gui.options->crt==2 ? "val.tv" : "val.arcade"));
        if (gui_ui_row("hub.video",S("nav.video"),sub,R(x,top+2*step,width,tall),0)) change_pane(HOST_GUI_VIDEO);
        if (gui_ui_row("hub.controls",S("nav.controls"),S(gui.options->dualshock ? "val.dualshock" : "val.digital"),R(x,top+3*step,width,tall),0)) change_pane(HOST_GUI_CONTROLS);
        if (gui_ui_row("hub.ra",S("nav.ra"),S(gui.options->ra ? "tag.casual" : "ra.off"),R(x,top+4*step,width,tall),0)) change_pane(HOST_GUI_RA);
        native_row(x,top+5*step,width,tall);
        label("app.wip",x,has_native() ? 618 : 606,width,GUI_SUB,GUI_MUTED);
    }
}
static void draw_discs(void) {
    float x=gui.rail+48,width=gui.width-gui.rail-96;
    pane_header("nav.discs",S("setup.discs.later"));
    for (unsigned i=0;i<4;++i) draw_disc_row(i,x,126+(float)i*92,width,84);
    label("disc.verify.title",x+18,526,width-36,GUI_REGULAR,GUI_TEXT);
    char verify[256]; snprintf(verify,sizeof(verify),S("disc.verify.sub"),3u); text("disc.verify.sub",verify,x+18,558,width-240,GUI_SUB,GUI_DIM);
    if (button("btn.verify_all",x+width-166,522,166,0)) for (unsigned i=0;i<4;++i) if (*gui.options->discs[i]) {
        gui.discs[i].state=GUI_DISC_CHECKING; gui_media_identify(gui.options->discs[i],1,gui.generation,3);
    }
    if (button("btn.rescan",x,602,170,0)) {
        char folder[512];
        if (*gui.options->discs[0]) parent_folder(gui.options->discs[0],folder,sizeof(folder));
        else snprintf(folder,sizeof(folder),"%s",*gui.last_folder ? gui.last_folder : ".");
        gui_media_scan(folder,gui.generation);
    }
}
static void draw_cards(void) {
    float x=gui.rail+48,width=gui.width-gui.rail-96; char slot[64],id[64];
    pane_header("nav.card",overlay() ? S("tag.next_launch") : S("setup.card.import.help"));
    for (unsigned i=0;i<2;++i) {
        float y=132+(float)i*210;
        gui_ui_panel(R(x,y,width,180),0x13131aff,7);
        snprintf(slot,sizeof(slot),S("card.slot"),i+1); snprintf(id,sizeof(id),"card.%u.title",i+1); text(id,slot,x+18,y+16,width-36,GUI_REGULAR,GUI_TEXT);
        snprintf(id,sizeof(id),"card.%u.path",i+1); text(id,*gui.options->cards[i] ? gui.options->cards[i] : S("card.empty"),x+18,y+52,width-36,GUI_MONO,GUI_DIM);
        card_status(i,x+18,y+83,width-36);
        snprintf(id,sizeof(id),S("ui.card.action"),i+1,"new");
        if (gui_ui_button(id,S("btn.new_card"),R(x+18,y+124,144,38),0)) new_card(i);
        if (gui.first_focus && !i) gui_ui_focus(id);
        snprintf(id,sizeof(id),S("ui.card.action"),i+1,"import");
        if (gui_ui_button(id,S("btn.import"),R(x+174,y+124,124,38),0)) pick(GUI_PICK_CARD,i);
        snprintf(id,sizeof(id),S("ui.card.action"),i+1,"clear");
        if (i && *gui.options->cards[i] && gui_ui_button(id,S("btn.clear"),R(x+310,y+124,104,38),GUI_DANGER)) changed("card2","");
        if (overlay()) label("tag.next_launch",x+width-180,y+132,160,GUI_SUB,GUI_MUTED);
    }
    if (*gui.error) text("card.error",gui.error,x,584,width,GUI_SUB,GUI_ERROR);
}
static int segments(const char *id,const char *title,const char *const *values,unsigned count,int current,float y,int enabled,int next) {
    float x=gui.rail+48,width=gui.width-gui.rail-96,control=(float)count*88;
    const char *labels[8]; for (unsigned i=0;i<count;++i) labels[i]=S(values[i]);
    if (count==3) control=326;
    if (count==4) control=352;
    gui_ui_control_row(R(x,y,width,52));
    int result=gui_ui_segments(id,R(x+width-control,y+4,control,46),labels,(int)count,current,enabled);
    control_label(id,title,x+18,y+15,230,GUI_REGULAR,enabled ? GUI_TEXT : GUI_MUTED);
    if (next) label("tag.next_launch",x+240,y+19,142,GUI_SUB,GUI_MUTED);
    if (!strcmp(id,"video.pgxp") || !strcmp(id,"video.crt")) label("tag.vulkan",x+170,y+19,92,GUI_SUB,GUI_MUTED);
    return result;
}
static int toggle(const char *id,const char *title,int current,float y,int enabled,int next) {
    float x=gui.rail+48,width=gui.width-gui.rail-96;
    gui_ui_control_row(R(x,y,width,52));
    int result=gui_ui_toggle(id,R(x+width-64,y+13,46,26),current,enabled);
    control_label(id,title,x+18,y+15,330,GUI_REGULAR,enabled ? GUI_TEXT : GUI_MUTED);
    if (next) label("tag.next_launch",x+360,y+19,142,GUI_SUB,GUI_MUTED);
    if (!strcmp(id,"controls.rumble")) label("controls.rumble.sub",x+230,y+19,330,GUI_SUB,GUI_DIM);
    return result;
}
static void draw_video(void) {
    static const char *const renderers[]={"val.soft","val.vulkan"},*const pgxp[]={"val.off","val.vertex","val.perspective"},*const crts[]={"val.off","val.pvm","val.tv","val.arcade"},*const filters[]={"val.nearest","val.linear"},*const modes[]={"val.aspect","val.integer","val.stretch","val.crop"};
    char subtitle[512]; float x=gui.rail+48,width=gui.width-gui.rail-96; int value;
    int vulkan=gui.options->renderer;
    const char *device=gui.services.device ? gui.services.device(gui.services.context) : NULL;
    if (device && !strcmp(device,S("val.soft"))) snprintf(subtitle,sizeof(subtitle),"%s",S("val.soft"));
    else snprintf(subtitle,sizeof(subtitle),S("video.subtitle"),S(device || vulkan ? "val.vulkan" : "val.soft"),device ? device : S("ui.dash"));
    pane_header("nav.video",subtitle);
    value=segments("video.renderer","video.renderer",renderers,2,gui.options->renderer,116,1,overlay());
    if (value!=gui.options->renderer) { changed("renderer",value ? "vulkan" : "soft"); if (!overlay()) set_result(HOST_GUI_RECREATE); }
    if (gui.first_focus) gui_ui_focus("video.renderer");
    gui_ui_control_row(R(x,172,width,52));
    value=gui_ui_stepper("video.scale",R(x+width-196,172,196,46),(int)gui.options->internal_scale,1,8,vulkan);
    control_label("video.scale","video.scale",x+18,177,270,GUI_REGULAR,vulkan ? GUI_TEXT : GUI_MUTED);
    label("tag.vulkan",x+200,181,92,GUI_SUB,GUI_MUTED); label("tag.next_launch",x+300,181,142,GUI_SUB,GUI_MUTED);
    char scale_text[160]; unsigned scale=(unsigned)gui.options->internal_scale;
    snprintf(scale_text,sizeof(scale_text),S("video.scale.sub"),scale,320u*scale,240u*scale);
    text("video.scale.sub",scale_text,x+18,202,width-230,GUI_SUB,GUI_DIM);
    if (value!=(int)gui.options->internal_scale) changed_number("internal-scale",(unsigned)value);
    value=segments("video.pgxp","video.pgxp",pgxp,3,gui.options->pgxp,228,vulkan,1); if (value!=gui.options->pgxp) changed("pgxp",value==0 ? "off" : value==1 ? "vertex" : "perspective");
    value=segments("video.crt","video.crt",crts,4,gui.options->crt,284,vulkan,0); if (value!=gui.options->crt) changed("crt",value==0 ? "off" : value==1 ? "pvm" : value==2 ? "tv" : "arcade");
    value=segments("video.filter","video.filter",filters,2,gui.options->linear,340,1,0); if (value!=gui.options->linear) changed("filter",value ? "linear" : "nearest");
    value=segments("video.scale_mode","video.scale_mode",modes,4,gui.options->scale_mode,396,1,0); if (value!=gui.options->scale_mode) changed("scale-mode",value==0 ? "aspect" : value==1 ? "integer" : value==2 ? "stretch" : "aspect-crop");
    value=toggle("video.fullscreen","video.fullscreen",gui.options->fullscreen,452,1,0); if (value!=gui.options->fullscreen) changed_number("fullscreen",(unsigned)value);
    label("video.movies",x+18,523,230,GUI_REGULAR,vulkan ? GUI_TEXT : GUI_MUTED); label("tag.next_launch",x+250,527,142,GUI_SUB,GUI_MUTED);
    text("video.movies.path",*gui.options->movies ? base_name(gui.options->movies) : S("video.none"),x+18,554,width-260,GUI_MONO,GUI_DIM);
    if (gui_ui_button("video.movies.browse",S("btn.browse"),R(x+width-254,516,160,38),vulkan ? 0 : GUI_DISABLED)) pick(GUI_PICK_MOVIES,0);
    if (gui_ui_button("video.movies.clear",S("btn.clear"),R(x+width-84,516,76,38),vulkan && *gui.options->movies ? 0 : GUI_DISABLED)) changed("movies","");
    value=toggle("video.osd","video.osd",gui.options->osd,590,1,0); if (value!=gui.options->osd) changed_number("osd",(unsigned)value);
}
static const char *pad_name(unsigned bit) {
    int type=gui.services.controller_type ? gui.services.controller_type(gui.services.context) : SDL_CONTROLLER_TYPE_UNKNOWN;
    int ps=type==SDL_CONTROLLER_TYPE_PS3 || type==SDL_CONTROLLER_TYPE_PS4 || type==SDL_CONTROLLER_TYPE_PS5;
    int nintendo=type==SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO;
    switch (bit) {
    case PAD_CROSS:return S(ps ? "hint.pad.ps.select" : nintendo ? "hint.pad.nintendo.select" : "hint.pad.xbox.select");
    case PAD_R1:return S(ps ? "controls.r1" : nintendo ? "controls.nintendo.r" : "controls.xbox.rb");
    case PAD_L1:return S(ps ? "controls.l1" : nintendo ? "controls.nintendo.l" : "controls.xbox.lb");
    case PAD_R2:return S(ps ? "controls.r2" : nintendo ? "controls.nintendo.zr" : "controls.xbox.rt");
    case PAD_TRIANGLE:return S(ps ? "controls.triangle" : nintendo ? "hint.pad.nintendo.secondary" : "hint.pad.xbox.secondary");
    case PAD_SQUARE:return S(ps ? "controls.square" : nintendo ? "hint.pad.nintendo.backspace" : "hint.pad.xbox.backspace");
    case PAD_SELECT:return S(ps ? "controls.select" : nintendo ? "controls.nintendo.minus" : "controls.xbox.view");
    case PAD_START:return S(ps ? "controls.start" : nintendo ? "controls.nintendo.plus" : "controls.xbox.menu");
    case PAD_LEFT:case PAD_RIGHT:return S("controls.slots"); default:return S("ui.dash");
    }
}
static void draw_controls(void) {
    static const char *const types[]={"val.digital","val.dualshock"},*const hotkeys[]={"val.guide_chords","val.off"};
    float x=gui.rail+48,width=gui.width-gui.rail-96; char subtitle[512],id[64],pad[128]; int value;
    const char *controller=gui.services.controller ? gui.services.controller(gui.services.context) : NULL;
    if (controller && *controller) snprintf(subtitle,sizeof(subtitle),S("controls.sub"),controller);
    else snprintf(subtitle,sizeof(subtitle),"%s",S("controls.no_pad"));
    pane_header("nav.controls",subtitle);
    value=segments("controls.type","controls.type",types,2,gui.options->dualshock,116,1,1); if (value!=gui.options->dualshock) changed("pad",value ? "dualshock" : "digital");
    if (gui.first_focus) gui_ui_focus("controls.type");
    value=toggle("controls.rumble","controls.rumble",gui.options->rumble,172,1,0); if (value!=gui.options->rumble) changed_number("rumble",(unsigned)value);
    value=segments("controls.hotkeys","controls.hotkeys",hotkeys,2,gui.options->pad_hotkey,228,1,0); if (value!=gui.options->pad_hotkey) changed("pad-hotkey",value ? "off" : "guide");
    label("controls.shots",x+18,299,width-230,GUI_REGULAR,GUI_TEXT);
    text("controls.shots.path",*gui.options->screenshot_dir ? gui.options->screenshot_dir : S("video.none"),x+18,330,width-230,GUI_MONO,GUI_DIM);
    if (gui_ui_button("controls.shots.browse",S("btn.browse"),R(x+width-178,294,160,38),0)) pick(GUI_PICK_SCREENSHOTS,0);
    label("controls.table",x+18,378,width-36,GUI_SUB,GUI_MUTED);
    text("hk.menu.pad",S("hk.guide_tap"),x+18,412,230,GUI_SUB,GUI_DIM); text("hk.menu.key",S("controls.esc"),x+270,412,100,GUI_MONO,GUI_DIM); label("hk.menu",x+400,412,width-420,GUI_SUB,GUI_TEXT);
    unsigned count=0; const host_gui_chord_t *chords=host_display_chords(&count); unsigned row=0;
    for (unsigned i=0;i<count;++i) {
        if (i && chords[i].pad_button==PAD_RIGHT) continue;
        float y=440+(float)row++*25;
        snprintf(pad,sizeof(pad),S("controls.guide"),pad_name(chords[i].pad_button));
        snprintf(id,sizeof(id),"controls.chord.%u",i); text(id,pad,x+18,y,250,GUI_SUB,GUI_DIM);
        snprintf(id,sizeof(id),"controls.key.%u",i); text(id,chords[i].pad_button==PAD_LEFT ? S("controls.f6f8") : chords[i].pad_button==PAD_SELECT ? S("controls.f1f4") : SDL_GetScancodeName((SDL_Scancode)chords[i].scancode),x+270,y,110,GUI_MONO,GUI_DIM);
        text(chords[i].label_id,S(chords[i].label_id),x+400,y,width-420,GUI_SUB,GUI_TEXT);
    }
}
static void draw_ra(void) {
    float x=gui.rail+48,width=gui.width-gui.rail-96;
    const host_ra_status_t *status=host_ra_status();
    if (gui.ra_seen!=(int)status->state) {
        if (status->state==HOST_RA_REJECTED) close_form();
        request_focus();
    }
    gui.ra_seen=status->state;
    int online=status->state==HOST_RA_CONNECTED || (status->state==HOST_RA_OFFLINE && *status->username);
    if (online || (status->state==HOST_RA_REJECTED && !gui.ra_form)) {
        char value[1024]; close_form(); pane_header("ra.title",S("ra.site"));
        if (online) snprintf(value,sizeof(value),S("ra.signed_in"),status->username);
        else snprintf(value,sizeof(value),"%s",S("ra.rejected"));
        text(online ? "ra.signed_in" : "ra.rejected",value,x+18,138,width-36,GUI_TITLE,online ? GUI_TEXT : GUI_ERROR);
        label("tag.casual",x+18,184,width-36,GUI_SUB,GUI_CASUAL);
        const char *state=status->state==HOST_RA_OFFLINE ? "ra.offline" : online ? "ra.connected" : "ra.rejected.sub";
        label(state,x+18,234,width-36,GUI_REGULAR,status->state==HOST_RA_OFFLINE ? GUI_WARN : online ? GUI_OK : GUI_ERROR);
        if (status->state==HOST_RA_OFFLINE) label("ra.offline.sub",x+18,270,width-36,GUI_SUB,GUI_DIM);
        if (online) {
            if (status->loading) snprintf(value,sizeof(value),"%s",S("ach.loading"));
            else if (status->unknown_game) snprintf(value,sizeof(value),"%s",S("ach.unknown"));
            else snprintf(value,sizeof(value),S("ra.set.sub"),status->title,status->unlocked,status->total,status->earned,status->points);
            if (gui_ui_row("btn.view_ach",S("btn.view_ach"),value,R(x,316,width,76),0)) change_pane(HOST_GUI_ACHIEVEMENTS);
            gui_ui_control_row(R(x,410,width,52));
            int next=gui_ui_toggle("ra.notify",R(x+width-100,410,100,52),gui.options->ra_notify,1);
            control_label("ra.notify","ra.notify",x+18,422,width-140,GUI_REGULAR,GUI_TEXT);
            if (next!=gui.options->ra_notify) changed_number("ra-notify",(unsigned)next);
            if (gui.first_focus) gui_ui_focus("btn.view_ach");
        } else {
            if (button("btn.sign_in",x+18,316,160,GUI_PRIMARY)) { gui.ra_form=1; request_focus(); }
            if (gui.first_focus) gui_ui_focus("btn.sign_in");
        }
        if (button("btn.sign_out",x+18,494,160,0)) { host_ra_signout(); changed_number("ra",0); request_focus(); }
        label("ra.signout.sub",x+18,550,width-36,GUI_SUB,GUI_DIM);
        if (status->storage_failed) label("ra.err.storage",x+18,598,width-36,GUI_SUB,GUI_ERROR);
        return;
    }
    if (gui.options->ra && status->state!=HOST_RA_UNAVAILABLE) gui.ra_form=1;
    pane_header("ra.title",gui.ra_form ? S("ra.form.sub") : S("ra.site"));
    if (!gui.ra_form) {
        label("ra.off",x+18,148,width-36,GUI_TITLE,GUI_TEXT);
        label("ra.off.sub",x+18,194,width-36,GUI_SUB,GUI_DIM);
        if (button("btn.turn_on",x+18,254,170,GUI_PRIMARY)) { gui.modal=MODAL_RA_WARNING; request_focus(); }
        if (gui.first_focus) gui_ui_focus("btn.turn_on");
        return;
    }
    label("ra.signin",x+18,132,width-36,GUI_TITLE,GUI_TEXT); label("tag.casual",x+180,140,130,GUI_SUB,GUI_CASUAL);
    label("ra.user",x+18,198,380,GUI_SUB,GUI_DIM); gui_ui_edit("ra.user",R(x+18,226,380,48),gui.username,sizeof(gui.username),0);
    label("ra.pass",x+18,292,380,GUI_SUB,GUI_DIM); gui_ui_edit("ra.pass",R(x+18,320,380,48),gui.password,sizeof(gui.password),1);
    if (gui.first_focus) gui_ui_focus("ra.user");
    const char *error=status->error_id ? status->error_id : gui.ra_error ? "ra.err.unavailable" : NULL;
    if (error) label(error,x+18,386,width-36,GUI_SUB,GUI_ERROR);
    if (status->state==HOST_RA_SIGNING_IN) label("ra.signing_in",x+18,386,width-36,GUI_SUB,GUI_DIM);
    if (button("btn.sign_in",x+18,434,150,status->state==HOST_RA_SIGNING_IN ? GUI_DISABLED : GUI_PRIMARY)) {
        host_ra_login(gui.username,gui.password); gui.ra_error=status->state==HOST_RA_UNAVAILABLE;
        memset(gui.password,0,sizeof(gui.password)); gui.keyboard=0; gui.edit=NULL;
    }
    if (button("btn.cancel",x+180,434,128,0)) { host_ra_signout(); changed_number("ra",0); close_form(); request_focus(); }
    label("ra.token_note",x+18,500,width-36,GUI_SUB,GUI_DIM);
}
static const char *browser_tag(const char *path,char *out,size_t capacity) {
    for (unsigned i=0;i<gui.browser_result_count;++i) if (!strcmp(gui.browser_results[i].path,path)) {
        const gui_disc_result_t *result=gui.browser_results+i;
        if (result->state==GUI_DISC_VERIFIED) snprintf(out,capacity,S("disc.serial"),result->disc);
        else disc_status_data(result,result->disc ? result->disc-1 : 0,out,capacity);
        return out;
    }
    if (gui.browser.kind==GUI_PICK_DISC && gui.browser_result_count<GUI_BROWSER_MAX) {
        gui_disc_result_t *result=gui.browser_results+gui.browser_result_count++;
        memset(result,0,sizeof(*result)); snprintf(result->path,sizeof(result->path),"%s",path); result->state=GUI_DISC_CHECKING;
        if (!gui_media_identify(path,0,gui.generation,4)) { --gui.browser_result_count; return NULL; }
        return S("disc.checking");
    }
    return NULL;
}
static void browser_changed(void) { gui.browser_result_count=0; snprintf(gui.last_folder,sizeof(gui.last_folder),"%s",gui.browser.folder); persist_ui(); }
static void draw_browser(void) {
    char id[64],meta[256]; float list_x=300;
    gui_ui_modal_begin("browser",R(0,0,gui.width,gui.height-56));
    gui_ui_panel(R(0,0,gui.width,gui.height-56),0x0b0b10ff,0);
    label("browser.path",48,26,100,GUI_SUB,GUI_DIM);
    int submitted=gui_ui_edit("browser.path",R(142,20,gui.width-372,46),gui.browser.path,sizeof(gui.browser.path),0);
    if (submitted) {
        if (!gui_browser_open(&gui.browser,gui.browser.path,gui.browser.kind)) {
            if (gui_browser_accept(&gui.browser,gui.browser.path)) { selected(gui.browser.path,gui.pick_kind,(unsigned)gui.pick_slot); close_browser(); }
            else snprintf(gui.browser.error,sizeof(gui.browser.error),"%s",S("browser.error"));
        } else browser_changed();
    }
    if (gui_ui_button("browser.up",S("hint.up"),R(gui.width-216,20,168,46),0)) { gui_browser_up(&gui.browser); browser_changed(); }
    for (unsigned i=0;i<gui.browser_places;++i) {
        snprintf(id,sizeof(id),S("ui.browser.place"),i);
        if (gui_ui_row(id,gui.browser_names[i],NULL,R(48,94+(float)i*42,220,40),!strcmp(gui.browser.folder,gui.browser_paths[i]) ? GUI_SELECTED : 0)) {
            gui_browser_open(&gui.browser,gui.browser_paths[i],gui.browser.kind); browser_changed();
        }
    }
    label("browser.show_all",60,gui.height-154,170,GUI_SUB,GUI_DIM);
    int all=gui_ui_toggle("browser.show_all",R(220,gui.height-154,46,26),gui.browser.show_all,1);
    if (all!=gui.browser.show_all) { gui.browser.show_all=all; gui_browser_refresh(&gui.browser); browser_changed(); }
    gui_ui_scroll_begin("browser.list",R(list_x,94,gui.width-list_x-48,gui.height-254));
    if (!gui.browser.count) label("browser.empty",list_x+14,106,gui.width-list_x-76,GUI_SUB,GUI_MUTED);
    for (unsigned i=0;i<gui.browser.count;++i) {
        gui_browser_entry_t *entry=gui.browser.entries+i; float y=94+(float)i*46;
        snprintf(id,sizeof(id),S("ui.browser.entry"),i);
        const char *tag=entry->directory ? S("browser.folder") : gui_ui_visible(R(list_x,y,gui.width-list_x-48,44)) ? browser_tag(entry->path,meta,sizeof(meta)) : NULL;
        if (!tag) { snprintf(meta,sizeof(meta),S("browser.bytes"),(unsigned)((entry->bytes+1048575)/1048576)); tag=meta; }
        if (gui_ui_row(id,entry->name,tag,R(list_x,y,gui.width-list_x-48,44),0)) {
            if (entry->directory) { char path[512]; snprintf(path,sizeof(path),"%s",entry->path); gui_browser_open(&gui.browser,path,gui.browser.kind); browser_changed(); }
            else { char path[512]; snprintf(path,sizeof(path),"%s",entry->path); selected(path,gui.pick_kind,(unsigned)gui.pick_slot); close_browser(); }
            break;
        }
        if (gui.first_focus && !i) gui_ui_focus(id);
    }
    gui_ui_scroll_end();
    if (*gui.browser.error) text("browser.error",gui.browser.error,list_x,gui.height-140,gui.width-list_x-48,GUI_SUB,GUI_ERROR);
    if (gui.browser.kind==GUI_PICK_MOVIES || gui.browser.kind==GUI_PICK_SCREENSHOTS) {
        if (gui_ui_button("browser.choose_folder",S("btn.continue"),R(gui.width-230,gui.height-120,182,42),GUI_PRIMARY)) { selected(gui.browser.folder,gui.pick_kind,(unsigned)gui.pick_slot); close_browser(); }
    }
    if (gui_ui_button("browser.cancel",S("btn.cancel"),R(48,gui.height-120,132,42),0)) close_browser();
    gui_ui_modal_end();
}
static void draw_dialog(void) {
    float height=gui.modal==MODAL_RA_WARNING ? 346 : gui.modal==MODAL_NATIVE ? 470 : 236,width=gui.modal==MODAL_NATIVE ? 864 : 600;
    float x=(gui.width-width)/2,y=(gui.height-height)/2;
    gui_ui_modal_begin("dialog",R(x,y,width,height));
    if (gui.modal==MODAL_NATIVE) {
        if (gui.native.compiler!=HOST_GUI_COMPILER_READY) draw_native_install(x+32,y+28,width-64);
        else draw_native_details(x+32,y+28,width-64);
        if (button("btn.close",x+width-166,y+height-72,134,0)) { gui.modal=MODAL_NONE; request_focus(); }
    } else if (gui.modal==MODAL_RA_WARNING) {
        label("ra.warn.title",x+32,y+28,width-64,GUI_TITLE,GUI_TEXT);
        static const char *const lines[]={"ra.warn.1","ra.warn.2","ra.warn.3","ra.warn.4","ra.warn.5"};
        for (unsigned i=0;i<5;++i) label(lines[i],x+32,y+84+(float)i*34,width-64,GUI_SUB,GUI_TEXT);
        if (button("btn.not_now",x+width-340,y+height-72,140,0)) { gui.modal=MODAL_NONE; request_focus(); }
        if (gui.first_focus) gui_ui_focus("btn.not_now");
        if (button("btn.accept_risk",x+width-186,y+height-72,154,GUI_PRIMARY)) { gui.modal=MODAL_NONE; gui.ra_form=1; changed_number("ra",1); request_focus(); }
    } else if (gui.modal==MODAL_QUIT) {
        label("quit.title",x+32,y+28,width-64,GUI_TITLE,GUI_TEXT); label("quit.body",x+32,y+86,width-64,GUI_REGULAR,GUI_DIM);
        if (button("btn.cancel",x+width-312,y+height-72,130,0)) { gui.modal=MODAL_NONE; request_focus(); }
        if (gui.first_focus) gui_ui_focus("btn.cancel");
        if (button("btn.quit",x+width-166,y+height-72,134,GUI_DANGER)) set_result(HOST_GUI_QUIT);
    } else {
        label("ini.fail.title",x+32,y+28,width-64,GUI_TITLE,GUI_TEXT); label("ini.fail.body",x+32,y+86,width-64,GUI_REGULAR,GUI_DIM);
        if (*gui.error) text("ini.fail.detail",gui.error,x+32,y+124,width-64,GUI_SUB,GUI_ERROR);
        if (button("btn.ok",x+width-166,y+height-72,134,GUI_PRIMARY)) { gui.modal=MODAL_NONE; gui.error[0]=0; request_focus(); }
        if (gui.first_focus) gui_ui_focus("btn.ok");
    }
    gui_ui_modal_end();
}
static void append_edit(char c) {
    if (!gui.edit || !gui.edit_capacity) return;
    size_t n=strlen(gui.edit);
    if (n+1<gui.edit_capacity) { gui.edit[n]=c; gui.edit[n+1]=0; }
}
static void draw_keyboard(void) {
    if (!gui.keyboard || !gui.edit) return;
    float left=gui.modal==MODAL_BROWSER || gui.mode==HOST_GUI_SETUP ? 0 : gui.rail;
    float x=left+48,y=gui.height-242; char id[64],label_text[16];
    gui_ui_modal_begin("keyboard",R(left,gui.height-260,gui.width-left,204));
    if (gui.keyboard_first) { gui_ui_focus("keyboard.key.2.5"); gui.keyboard_first=0; }
    for (unsigned row=0;row<4;++row) {
        const char *keys=S(gui.symbols ? row==0 ? "keyboard.symbols0" : row==1 ? "keyboard.symbols1" : row==2 ? "keyboard.symbols2" : "keyboard.symbols3" : row==0 ? "keyboard.row0" : row==1 ? "keyboard.row1" : row==2 ? "keyboard.row2" : "keyboard.row3");
        for (unsigned col=0;keys[col];++col) {
            char c=gui.shift && !gui.symbols ? (char)toupper((unsigned char)keys[col]) : keys[col];
            snprintf(label_text,sizeof(label_text),S("keyboard.key"),c);
            snprintf(id,sizeof(id),S("ui.keyboard.key"),row,col);
            if (gui_ui_button(id,c==' ' ? S("keyboard.space") : label_text,R(x+(float)col*58,y+(float)row*46,52,40),0)) append_edit(c);
        }
    }
    if (gui_ui_button("keyboard.backspace",S("keyboard.backspace"),R(x+580,y,142,40),0) && *gui.edit) {
        size_t n=strlen(gui.edit)-1; while (n && ((unsigned char)gui.edit[n]&0xc0)==0x80) --n; gui.edit[n]=0;
    }
    if (gui_ui_button("keyboard.shift",S("keyboard.shift"),R(x+580,y+46,142,40),gui.shift ? GUI_SELECTED : 0)) gui.shift=!gui.shift;
    if (gui_ui_button("keyboard.symbols",S("keyboard.symbols"),R(x+580,y+92,142,40),gui.symbols ? GUI_SELECTED : 0)) gui.symbols=!gui.symbols;
    if (gui_ui_button("keyboard.done",S("keyboard.done"),R(x+580,y+138,142,40),GUI_PRIMARY)) { gui.keyboard=0; gui.edit=NULL; }
    gui_ui_modal_end();
}

void host_gui_frame(unsigned width,unsigned height) {
    if (!host_gui_visible()) return;
    host_ra_idle();
    gui.width=(float)width/gui_scale(height); gui.height=(float)height/gui_scale(height); gui.rail=gui_scale(height)<0.9f ? 240 : 300;
    apply_results();
    if (gui.services.native) {
        gui.services.native(gui.services.context,&gui.native);
        if ((int)gui.native.compiler!=gui.native_seen) {
            gui.native_seen=(int)gui.native.compiler;
            if ((gui.mode==HOST_GUI_SETUP && gui.setup_step==2) || gui.modal==MODAL_NATIVE) request_focus();
        }
    }
    unsigned focus_epoch=gui.focus_epoch;
    gui_ui_begin(width,height,overlay());
    gui_ui_panel(R(0,0,gui.width,gui.height),overlay() ? 0x00000000 : 0x0b0b10ff,0);
    if (gui.mode==HOST_GUI_SETUP) draw_setup();
    else {
        gui_ui_panel(R(gui.rail,0,gui.width-gui.rail,gui.height-56),overlay() ? 0x101014c7 : 0x0b0b10ff,0);
        gui_ui_region_begin("rail",R(0,0,gui.rail,gui.height-56));
        rail();
        gui_ui_region_end();
        gui_ui_region_begin("pane",R(gui.rail,0,gui.width-gui.rail,gui.height-56));
        switch (gui.pane) {
        case HOST_GUI_GAME:draw_game();break; case HOST_GUI_DISCS:draw_discs();break;
        case HOST_GUI_CARD:draw_cards();break; case HOST_GUI_VIDEO:draw_video();break;
        case HOST_GUI_CONTROLS:draw_controls();break; case HOST_GUI_RA:draw_ra();break;
        case HOST_GUI_ACHIEVEMENTS:gui_achievements_draw(gui.rail+48,gui.width-gui.rail-96,gui.height,gui.first_focus);break;
        }
        if (*gui.startup_error && gui.pane!=HOST_GUI_GAME) text("startup.error",gui.startup_error,gui.rail+66,gui.height-85,gui.width-gui.rail-114,GUI_SUB,GUI_ERROR);
        gui_ui_region_end();
    }
    hints();
    if (gui.modal==MODAL_BROWSER) draw_browser();
    else if (gui.modal!=MODAL_NONE) draw_dialog();
    draw_keyboard();
    gui_achievement_toast(width,height,(host_rect_t){0,0,(int)width,(int)height});
    gui_ui_end(); if (focus_epoch==gui.focus_epoch) gui.first_focus=0;
}
int host_gui_toast_visible(void) {
    host_ra_achievement_t achievement; float opacity;
    return gui.configured && host_ra_toast(&achievement,host_osd_now(),&opacity);
}
void host_gui_toast_frame(unsigned width,unsigned height,host_rect_t area) {
    if (!host_gui_toast_visible() || host_gui_visible()) return;
    gui_ui_begin(width,height,0);
    gui_achievement_toast(width,height,area);
    gui_ui_end();
}
