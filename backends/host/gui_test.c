#define _POSIX_C_SOURCE 200809L
#include "gui_internal.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Diagnostics drive the SDL queue; none of these commands calls GUI actions. */
static struct {
    FILE *file,*measure,*measure_gpu;
    Uint32 text_event;
    char source[4096], capture[4096], id[512], expected[4096];
    unsigned line, wait, close_attempts, measure_remaining, measure_frame, measure_gpu_frame, patience;
    uint64_t at;
    int at_pending, close_pending, assertion, failed, finished;
    SDL_Scancode release;
    int pad_release, held_select, held_mouse;
} script;
static SDL_Window *attached_window;
void host_gui_test_window(SDL_Window *window) { attached_window=window; }


static void failure(const char *reason) {
    fprintf(stderr,"GUI test %s:%u: %s\n",script.source,script.line,reason);
    script.failed=1;
}
static int queue(SDL_Event *event) {
    if (SDL_PushEvent(event)==1) return 1;
    failure("cannot queue SDL event"); return 0;
}
static void key(SDL_Scancode scan,int down) {
    SDL_Event event; SDL_zero(event);
    event.type=down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.state=down ? SDL_PRESSED : SDL_RELEASED;
    event.key.windowID=attached_window ? SDL_GetWindowID(attached_window) : 0;
    event.key.keysym.scancode=scan;
    event.key.keysym.sym=SDL_GetKeyFromScancode(scan);
    /* Offscreen/dummy SDL may not supply a keyboard layout. */
    if (!event.key.keysym.sym) {
        switch (scan) {
        case SDL_SCANCODE_ESCAPE: event.key.keysym.sym=SDLK_ESCAPE; break;
        case SDL_SCANCODE_RETURN: event.key.keysym.sym=SDLK_RETURN; break;
        case SDL_SCANCODE_UP: event.key.keysym.sym=SDLK_UP; break;
        case SDL_SCANCODE_DOWN: event.key.keysym.sym=SDLK_DOWN; break;
        case SDL_SCANCODE_LEFT: event.key.keysym.sym=SDLK_LEFT; break;
        case SDL_SCANCODE_RIGHT: event.key.keysym.sym=SDLK_RIGHT; break;
        case SDL_SCANCODE_Q: event.key.keysym.sym=SDLK_q; break;
        case SDL_SCANCODE_E: event.key.keysym.sym=SDLK_e; break;
        default: break;
        }
    }
    queue(&event);
    if (down) script.release=scan;
}
static void pad(unsigned button,int down) {
    SDL_Event event; SDL_zero(event);
    event.type=down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    event.cbutton.which=-1;
    event.cbutton.button=(Uint8)button;
    event.cbutton.state=down ? SDL_PRESSED : SDL_RELEASED;
    queue(&event); if (down) script.pad_release=(int)button+1;
}
static char *trim(char *text) {
    char *end;
    while (isspace((unsigned char)*text)) ++text;
    end=text+strlen(text);
    while (end>text && isspace((unsigned char)end[-1])) --end;
    *end=0; return text;
}
static int argument(const char *text,char *out,size_t capacity) {
    size_t count=0; int quoted=*text=='"';
    if (quoted) ++text;
    while (*text && (!quoted || *text!='"')) {
        if (quoted && *text=='\\' && (text[1]=='"' || text[1]=='\\')) ++text;
        if (count+1>=capacity) return 0;
        out[count++]=*text++;
    }
    if (quoted && *text++!='"') return 0;
    if (*text) return 0;
    out[count]=0; return 1;
}
static int number(const char *text,uint64_t *value) {
    char *end; unsigned long long result;
    if (!isdigit((unsigned char)*text)) return 0;
    errno=0; result=strtoull(text,&end,10);
    if (errno || *end) return 0;
    *value=(uint64_t)result; return 1;
}
int host_gui_test_load(const char *path) {
    host_gui_test_shutdown();
    if (!path || !*path) return 1;
    if (strlen(path)>=sizeof(script.source)) { failure("script path too long"); return 0; }
    strcpy(script.source,path);
    script.file=fopen(path,"rb");
    if (!script.file) { failure("cannot open script"); return 0; }
    return 1;
}
void host_gui_test_tick(uint64_t guest_frame) {
    char buffer[8192], *command, *args, *space;
    uint64_t count;
    if (!script.file || script.failed || script.finished) return;
    if (script.release) {
        SDL_Scancode release=script.release;
        script.release=SDL_SCANCODE_UNKNOWN; key(release,0); return;
    }
    if (script.pad_release) {
        unsigned release=(unsigned)(script.pad_release-1);
        script.pad_release=0; pad(release,0); return;
    }
    if (script.close_pending) {
        if (!host_gui_visible()) { script.close_pending=0; return; }
        if (++script.close_attempts>16) { failure("close did not dismiss the GUI after 16 back events"); return; }
        key(SDL_SCANCODE_ESCAPE,1); return;
    }
    if (script.wait) { --script.wait; return; }
    if (script.at_pending) {
        if (guest_frame<script.at) return;
        if (guest_frame>script.at) { failure("scheduled frame was missed"); return; }
        script.at_pending=0;
    }
    if (*script.capture || script.assertion || script.measure_remaining) return;
    do {
        if (!fgets(buffer,sizeof(buffer),script.file)) {
            if (ferror(script.file)) failure("cannot read script");
            else if (script.held_select || script.held_mouse) failure("script ended with held input");
            else { script.finished=1; fprintf(stderr,"GUI test complete: %s (%u lines)\n",script.source,script.line); }
            return;
        }
        ++script.line;
        if (!strchr(buffer,'\n') && !feof(script.file)) { failure("script line too long"); return; }
        command=trim(buffer);
    } while (!*command || *command=='#');
    space=strpbrk(command," \t");
    if (space) { *space++=0; args=trim(space); }
    else args=command+strlen(command);
    if (!host_gui_visible() && (!strcmp(command,"nav") || !strcmp(command,"select") ||
        !strcmp(command,"select-down") || !strcmp(command,"back") || !strcmp(command,"section") ||
        !strcmp(command,"text") || !strcmp(command,"drop") || !strcmp(command,"mouse") ||
        !strcmp(command,"mouse-down") || !strcmp(command,"pad") || !strcmp(command,"assert-focus") ||
        !strcmp(command,"assert-text") || !strcmp(command,"wait-text"))) {
        failure("interaction or assertion requires a visible GUI"); return;
    }
    if (!strcmp(command,"quit") && !*args) {
        SDL_Event event;
        while (fgets(buffer,sizeof(buffer),script.file)) {
            ++script.line;
            char *remaining=trim(buffer);
            if (*remaining && *remaining!='#') { failure("quit must be the final command"); return; }
        }
        if (ferror(script.file)) { failure("cannot read script"); return; }
        if (script.held_select || script.held_mouse) { failure("quit with held input"); return; }
        SDL_zero(event); event.type=SDL_QUIT; queue(&event);
        script.finished=1; fprintf(stderr,"GUI test complete: %s (%u lines)\n",script.source,script.line);
    } else if (!strcmp(command,"open") && !*args) {
        if (!host_gui_visible()) key(SDL_SCANCODE_ESCAPE,1);
    } else if (!strcmp(command,"open") && !strcmp(args,"achievements")) {
        if (!host_gui_visible()) key(SDL_SCANCODE_F10,1);
    } else if (!strcmp(command,"close") && !*args) {
        if (script.measure) { if (fclose(script.measure)) failure("cannot finish timing CSV"); script.measure=NULL; }
        if (script.measure_gpu) { if (fclose(script.measure_gpu)) failure("cannot finish GPU timing CSV"); script.measure_gpu=NULL; }
        script.close_pending=host_gui_visible(); script.close_attempts=0;
        if (script.close_pending) { ++script.close_attempts; key(SDL_SCANCODE_ESCAPE,1); }
    } else if (!strcmp(command,"select-down") && !*args) {
        if (script.held_select) { failure("select is already held"); return; }
        key(SDL_SCANCODE_RETURN,1); script.release=SDL_SCANCODE_UNKNOWN; script.held_select=1;
    } else if (!strcmp(command,"select-up") && !*args) {
        if (!script.held_select) { failure("select is not held"); return; }
        key(SDL_SCANCODE_RETURN,0); script.held_select=0;
    } else if (!strcmp(command,"mouse")) {
        SDL_Event event; char *end; long x,y;
        errno=0; x=strtol(args,&end,10);
        if (end==args || !isspace((unsigned char)*end)) { failure("expected mouse X Y"); return; }
        args=trim(end); y=strtol(args,&end,10);
        if (end==args || *end || errno || x<-32768 || x>32767 || y<-32768 || y>32767) { failure("invalid mouse position"); return; }
        SDL_zero(event); event.type=SDL_MOUSEMOTION;
        event.motion.windowID=attached_window ? SDL_GetWindowID(attached_window) : 0;
        event.motion.x=(Sint32)x; event.motion.y=(Sint32)y; queue(&event);
    } else if ((!strcmp(command,"mouse-down") || !strcmp(command,"mouse-up")) && !*args) {
        int down=!strcmp(command,"mouse-down"); SDL_Event event;
        if (down==script.held_mouse) { failure("unbalanced mouse button"); return; }
        SDL_zero(event); event.type=down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        event.button.windowID=attached_window ? SDL_GetWindowID(attached_window) : 0;
        event.button.button=SDL_BUTTON_LEFT; event.button.state=down ? SDL_PRESSED : SDL_RELEASED;
        queue(&event); script.held_mouse=down;
    } else if (!strcmp(command,"nav")) {
        SDL_Scancode scan=!strcmp(args,"down") ? SDL_SCANCODE_DOWN : !strcmp(args,"up") ? SDL_SCANCODE_UP :
            !strcmp(args,"left") ? SDL_SCANCODE_LEFT : !strcmp(args,"right") ? SDL_SCANCODE_RIGHT : SDL_SCANCODE_UNKNOWN;
        if (!scan) failure("expected nav down|up|left|right"); else key(scan,1);
    } else if (!strcmp(command,"select") && !*args) key(SDL_SCANCODE_RETURN,1);
    else if (!strcmp(command,"back") && !*args) key(SDL_SCANCODE_ESCAPE,1);
    else if (!strcmp(command,"section")) {
        if (!strcmp(args,"next")) key(SDL_SCANCODE_E,1);
        else if (!strcmp(args,"prev")) key(SDL_SCANCODE_Q,1);
        else failure("expected section next|prev");
    } else if (!strcmp(command,"pad")) {
        static const struct { const char *name; unsigned button; } buttons[]={
            {"a",SDL_CONTROLLER_BUTTON_A},{"b",SDL_CONTROLLER_BUTTON_B},
            {"x",SDL_CONTROLLER_BUTTON_X},{"y",SDL_CONTROLLER_BUTTON_Y},
            {"up",SDL_CONTROLLER_BUTTON_DPAD_UP},{"down",SDL_CONTROLLER_BUTTON_DPAD_DOWN},
            {"left",SDL_CONTROLLER_BUTTON_DPAD_LEFT},{"right",SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
            {"l1",SDL_CONTROLLER_BUTTON_LEFTSHOULDER},{"r1",SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
            {"start",SDL_CONTROLLER_BUTTON_START},{"guide",SDL_CONTROLLER_BUTTON_GUIDE}
        };
        unsigned i;
        for (i=0;i<sizeof(buttons)/sizeof(buttons[0]);++i) if (!strcmp(args,buttons[i].name)) break;
        if (i==sizeof(buttons)/sizeof(buttons[0])) failure("unknown pad button"); else pad(buttons[i].button,1);
    } else if (!strcmp(command,"text")) {
        char text[4096]; size_t at=0, length;
        if (!argument(args,text,sizeof(text))) { failure("invalid text argument"); return; }
        length=strlen(text);
        while (at<length) {
            SDL_Event event; size_t n=length-at;
            SDL_zero(event);
            if (!script.text_event) script.text_event=SDL_RegisterEvents(1);
            if (script.text_event==(Uint32)-1) { failure("cannot register diagnostic text event"); break; }
            event.type=script.text_event; event.user.code=SDL_TEXTINPUT;
            event.user.windowID=attached_window ? SDL_GetWindowID(attached_window) : 0;
            if (n>=SDL_TEXTINPUTEVENT_TEXT_SIZE) n=SDL_TEXTINPUTEVENT_TEXT_SIZE-1;
            while (n && at+n<length && ((unsigned char)text[at+n]&0xc0)==0x80) --n;
            if (!n) { failure("invalid UTF-8 input chunk"); break; }
            char chunk[SDL_TEXTINPUTEVENT_TEXT_SIZE];
            memcpy(chunk,text+at,n); chunk[n]=0;
            event.user.data1=SDL_strdup(chunk);
            if (!event.user.data1) { failure("out of memory"); break; }
            if (!queue(&event)) { SDL_free(event.user.data1); break; }
            at+=n;
        }
        memset(text,0,sizeof(text));
    } else if (!strcmp(command,"drop")) {
        char path[4096]; SDL_Event event;
        if (!argument(args,path,sizeof(path)) || !*path) { failure("invalid drop path"); return; }
        SDL_zero(event);
        if (!script.text_event) script.text_event=SDL_RegisterEvents(1);
        if (script.text_event==(Uint32)-1) { failure("cannot register diagnostic drop event"); return; }
        event.type=script.text_event; event.user.code=SDL_DROPFILE;
        event.user.windowID=attached_window ? SDL_GetWindowID(attached_window) : 0;
        event.user.data1=SDL_strdup(path);
        if (!event.user.data1) failure("out of memory");
        else if (!queue(&event)) SDL_free(event.user.data1);
    } else if (!strcmp(command,"wait")) {
        if (!number(args,&count) || count>1000000) failure("invalid wait count");
        else script.wait=(unsigned)count;
    } else if (!strcmp(command,"at")) {
        if (!number(args,&script.at) || host_gui_visible() || guest_frame>script.at) failure("invalid absolute frame schedule");
        else script.at_pending=1;
    } else if (!strcmp(command,"measure")) {
        char path[4096];
        space=strrchr(args,' ');
        if (!space) { failure("expected measure PATH N"); return; }
        *space++=0;
        if (!host_gui_visible() || !argument(trim(args),path,sizeof(path)) || !*path ||
            !number(trim(space),&count) || !count || count>1000000) { failure("invalid measurement request"); return; }
        if (script.measure) fclose(script.measure);
        if (script.measure_gpu) fclose(script.measure_gpu);
        script.measure=NULL; script.measure_gpu=NULL;
        script.measure=fopen(path,"w");
        if (!script.measure || fprintf(script.measure,"frame,build_ms,present_ms,frame_ms\n")<0) { failure("cannot create timing CSV"); return; }
        char gpu_path[4352];
        if (snprintf(gpu_path,sizeof(gpu_path),"%s.gpu.csv",path)>=(int)sizeof(gpu_path) ||
            !(script.measure_gpu=fopen(gpu_path,"w")) || fprintf(script.measure_gpu,"sample,gui_gpu_ms\n")<0) { failure("cannot create GPU timing CSV"); return; }
        script.measure_remaining=(unsigned)count; script.measure_frame=script.measure_gpu_frame=0;
    } else if (!strcmp(command,"capture")) {
        if ((!host_gui_visible() && !host_gui_toast_visible()) || !argument(args,script.capture,sizeof(script.capture)) || !*script.capture)
            failure("capture requires a visible GUI and a path");
    } else if (!strcmp(command,"assert-focus")) {
        if (!*args || strlen(args)>=sizeof(script.id)) failure("invalid focus id");
        else { strcpy(script.id,args); script.assertion=1; }
    } else if (!strcmp(command,"wait-text")) {
        /* For states a worker reaches in its own time: up to N frames. */
        char *limit=strrchr(args,' ');
        if (!limit) { failure("expected wait-text ID text N"); return; }
        *limit++=0; space=strpbrk(args," \t");
        if (!space || !number(trim(limit),&count) || !count || count>1000000) { failure("expected wait-text ID text N"); return; }
        *space++=0; space=trim(space);
        if (!*args || strlen(args)>=sizeof(script.id) || !argument(space,script.expected,sizeof(script.expected)))
            failure("invalid text wait");
        else { strcpy(script.id,args); script.assertion=3; script.patience=(unsigned)count; }
    } else if (!strcmp(command,"assert-text")) {
        space=strpbrk(args," \t");
        if (!space) { failure("expected assert-text ID text"); return; }
        *space++=0; space=trim(space);
        if (!*args || strlen(args)>=sizeof(script.id) || !argument(space,script.expected,sizeof(script.expected)))
            failure("invalid text assertion");
        else { strcpy(script.id,args); script.assertion=2; }
    } else failure("unknown or malformed command");
}
/* sdl2-compat crashes translating injected pointer-based text/drop events.
 * Queue a registered user event, then restore the standard event before input
 * dispatch. Physical and scripted input share the actual GUI event path. */
void host_gui_test_event(SDL_Event *event) {
    if (script.text_event && event->type==script.text_event) {
        char *text=event->user.data1;
        Uint32 window=event->user.windowID,kind=(Uint32)event->user.code;
        SDL_zero(*event); event->type=kind;
        if (kind==SDL_DROPFILE) {
            event->drop.windowID=window; event->drop.file=text ? SDL_strdup(text) : NULL;
            if (!event->drop.file) { event->type=SDL_FIRSTEVENT; failure("out of memory"); }
        } else {
            event->text.windowID=window;
            if (text) snprintf(event->text.text,sizeof(event->text.text),"%s",text);
        }
        SDL_free(text);
    }
}
void host_gui_test_observe(void) {
    const char *actual;
    if (!script.assertion || script.failed) return;
    actual=script.assertion==1 ? gui_ui_focused() : gui_ui_text_for(script.id);
    if (script.assertion==3 && (!actual || strcmp(actual,script.expected)) && --script.patience) return;
    if (!actual || strcmp(actual,script.assertion==1 ? script.id : script.expected)) {
        fprintf(stderr,"GUI test %s:%u: %s assertion failed for %s (focus=%s, actual=%.200s)\n",script.source,script.line,
                script.assertion==1 ? "focus" : "text",script.id,
                gui_ui_focused() ? gui_ui_focused() : "<none>",
                !strcmp(script.id,"ra.pass") ? "<masked>" : actual ? actual : "<absent>");
        script.failed=1;
    }
    script.assertion=0; memset(script.expected,0,sizeof(script.expected));
}
void host_gui_test_timing(double build_ms,double present_ms,double frame_ms) {
    if (!script.measure_remaining || script.failed) return;
    if (!isfinite(build_ms) || !isfinite(present_ms) || !isfinite(frame_ms) ||
        build_ms<0 || present_ms<0 || frame_ms<0 || !host_gui_visible()) {
        failure("invalid or hidden GUI timing sample"); return;
    }
    if (fprintf(script.measure,"%u,%.9f,%.9f,%.9f\n",script.measure_frame++,build_ms,present_ms,frame_ms)<0) {
        failure("cannot write timing CSV"); return;
    }
    if (!--script.measure_remaining && fflush(script.measure)) failure("cannot flush timing CSV");
}
void host_gui_test_gpu_timing(double pass_ms) {
    if (!script.measure_gpu || script.failed || !isfinite(pass_ms) || pass_ms<0) return;
    if (fprintf(script.measure_gpu,"%u,%.9f\n",script.measure_gpu_frame++,pass_ms)<0) failure("cannot write GPU timing CSV");
}
const char *host_gui_test_capture(void) { return *script.capture && !script.failed ? script.capture : NULL; }
void host_gui_test_captured(int success) {
    if (!success) failure("composed capture failed");
    else fprintf(stderr,"GUI capture: %s\n",script.capture);
    script.capture[0]=0;
}
int host_gui_test_failed(void) { return script.failed; }
int host_gui_test_finished(void) { return script.finished; }
void host_gui_test_shutdown(void) {
    if (script.file) fclose(script.file);
    if (script.measure) fclose(script.measure);
    if (script.measure_gpu) fclose(script.measure_gpu);
    if (script.text_event && script.text_event!=(Uint32)-1 && (SDL_WasInit(SDL_INIT_EVENTS)&SDL_INIT_EVENTS)) {
        SDL_Event event;
        while (SDL_PeepEvents(&event,1,SDL_GETEVENT,script.text_event,script.text_event)==1) SDL_free(event.user.data1);
    }
    memset(&script,0,sizeof(script));
}

int host_gui_test_active(void) { return script.file!=NULL; }
