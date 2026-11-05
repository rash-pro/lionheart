#include "gui_internal.h"
#include "ra.h"
#include "osd.h"
#include "viewport.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define S(id) host_gui_string(id)
static struct { char search[256]; int filter,trigger[2],error_dismissed; } view;
static gui_rect_t R(float x,float y,float w,float h) { gui_rect_t r={x,y,w,h}; return r; }
static void label(const char *id,const char *value,float x,float y,float w,gui_font_t font,gui_colour_t colour) {
    gui_ui_text(id,value,R(x,y,w,32),font,colour);
}
static void date(time_t when,char *out,size_t cap) {
    struct tm *local=localtime(&when);
    if (!local || !strftime(out,cap,"%Y-%m-%d %H:%M",local)) *out=0;
}
static int contains(const char *value,const char *needle) {
    if (!*needle) return 1;
    for (;*value;++value) {
        size_t n=0;
        while (needle[n] && value[n] && tolower((unsigned char)value[n])==tolower((unsigned char)needle[n])) ++n;
        if (!needle[n]) return 1;
    }
    return 0;
}
void gui_achievements_reset(void) { memset(&view,0,sizeof(view)); }
int gui_achievements_back(void) {
    if (!host_ra_status()->load_failed || view.error_dismissed) return 0;
    view.error_dismissed=1; return 1;
}
void gui_achievements_event(const SDL_Event *event) {
    if (gui_ui_wants_text()) return;
    int delta=0;
    if (event->type==SDL_CONTROLLERAXISMOTION &&
        (event->caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERLEFT || event->caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
        unsigned side=event->caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        int pressed=event->caxis.value>16000;
        if (pressed && !view.trigger[side]) delta=side ? 1 : -1;
        view.trigger[side]=pressed;
    }
    if (event->type==SDL_KEYDOWN && !event->key.repeat && strcmp(gui_ui_focused(),"ach.filters")) {
        if (event->key.keysym.scancode==SDL_SCANCODE_LEFT) delta=-1;
        if (event->key.keysym.scancode==SDL_SCANCODE_RIGHT) delta=1;
    }
    if (event->type==SDL_CONTROLLERBUTTONDOWN && strcmp(gui_ui_focused(),"ach.filters")) {
        if (event->cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT) delta=-1;
        if (event->cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT) delta=1;
    }
    if (delta) view.filter=(view.filter+delta+3)%3;
}
void gui_achievements_draw(float x,float width,float height,int first_focus) {
    const host_ra_status_t *status=host_ra_status();
    char buffer[1024],all[48],unlocked[48],locked[48]; float y=32;
    if (status->state==HOST_RA_OFFLINE && status->downloaded) {
        char when[64]; date(status->downloaded,when,sizeof(when));
        snprintf(buffer,sizeof(buffer),S("ach.offline"),when);
        gui_ui_panel(R(x,24,width,64),0xe6b85c18,7);
        gui_ui_text("ach.offline",buffer,R(x+16,32,width-32,48),GUI_SUB,GUI_WARN); y=108;
    }
    label("ach.title",*status->title ? status->title : S("nav.achievements"),x,y,width-116,GUI_TITLE,GUI_TEXT);
    label("tag.casual",S("tag.casual"),x+width-100,y+7,100,GUI_SUB,GUI_CASUAL);
    snprintf(buffer,sizeof(buffer),S("ach.stats"),status->unlocked,status->total,status->earned,status->points);
    label("ach.stats",buffer,x,y+38,width,GUI_MONO,GUI_DIM);
    y+=82;
    snprintf(all,sizeof(all),S("ach.all"),status->total);
    snprintf(unlocked,sizeof(unlocked),S("ach.unlocked"),status->unlocked);
    snprintf(locked,sizeof(locked),S("ach.locked"),status->total-status->unlocked);
    const char *labels[]={all,unlocked,locked};
    view.filter=gui_ui_segments("ach.filters",R(x,y,450,40),labels,3,view.filter,1);
    if (first_focus) { gui_ui_focus("ach.filters"); view.error_dismissed=0; }
    label("ach.search.label",S("ach.search"),x+474,y-19,width-474,GUI_SUB,GUI_DIM);
    gui_ui_edit("ach.search",R(x+474,y,width-474,40),view.search,sizeof(view.search),0);
    y+=56;
    if (!status->total) {
        const char *id=status->loading ? "ach.loading" : status->unknown_game ? "ach.unknown" :
            status->state==HOST_RA_OFF || status->state==HOST_RA_SIGNED_OUT || status->state==HOST_RA_REJECTED ? "ach.signin" : "ach.empty";
        label(id,S(id),x+14,y+12,width-28,GUI_REGULAR,GUI_DIM);
    }
    gui_ui_scroll_begin("ach.list",R(x,y,width,height-y-70));
    unsigned shown=0;
    for (unsigned i=0;i<status->total;++i) {
        host_ra_achievement_t a;
        if (!host_ra_achievement(i,&a) || (view.filter==1 && !a.unlocked) || (view.filter==2 && a.unlocked) ||
            (!contains(a.title,view.search) && !contains(a.description,view.search))) continue;
        float row_y=y+shown++*90; char id[64];
        snprintf(id,sizeof(id),"ach.%u",a.id);
        gui_ui_row(id,S("ui.blank"),NULL,R(x,row_y,width-12,86),0);
        if (!gui_ui_visible(R(x,row_y,width,86))) continue;
        gui_ui_badge(a.id,R(x+14,row_y+11,64,64),!a.unlocked,1);
        snprintf(id,sizeof(id),"ach.%u.title",a.id);
        label(id,a.title,x+94,row_y+10,width-386,GUI_REGULAR,a.unlocked ? GUI_TEXT : GUI_DIM);
        snprintf(id,sizeof(id),"ach.%u.description",a.id);
        gui_ui_text(id,a.description,R(x+94,row_y+39,width-386,40),GUI_SUB,GUI_DIM);
        snprintf(id,sizeof(id),"ach.%u.points",a.id); snprintf(buffer,sizeof(buffer),S("ui.number"),a.points);
        label(id,buffer,x+width-280,row_y+29,48,GUI_MONO,GUI_DIM);
        snprintf(id,sizeof(id),"ach.%u.status",a.id);
        if (a.unlocked) {
            char when[64]; date(a.unlocked_at,when,sizeof(when));
            snprintf(buffer,sizeof(buffer),S("ach.unlocked_at"),when);
            label(id,buffer,x+width-230,row_y+23,218,GUI_SUB,GUI_OK);
            if (a.pending) label("ach.pending",S("ach.pending.marker"),x+width-230,row_y+48,218,GUI_SUB,GUI_WARN);
        } else if (a.progress && *a.progress) {
            label(id,a.progress,x+width-214,row_y+16,196,GUI_MONO,GUI_DIM);
            gui_ui_panel(R(x+width-214,row_y+53,184,6),0x2a2a33ff,3);
            float progress=a.percent<0 ? 0 : a.percent>100 ? 1 : a.percent/100;
            gui_ui_panel(R(x+width-214,row_y+53,184*progress,6),0x9fb4d8ff,3);
        } else label(id,S("ach.locked.state"),x+width-214,row_y+29,196,GUI_SUB,GUI_DIM);
    }
    gui_ui_scroll_end();
    if (!shown && status->total) label("ach.no_matches",S("ach.no_matches"),x+14,y+12,width-28,GUI_REGULAR,GUI_DIM);
    if (status->load_failed && !view.error_dismissed) {
        gui_rect_t dialog=R(x+(width-600)/2,(height-256)/2,600,256);
        gui_ui_modal_begin("ach.load",dialog);
        label("ach.load.title",S("ach.load.title"),dialog.x+32,dialog.y+28,536,GUI_TITLE,GUI_TEXT);
        snprintf(buffer,sizeof(buffer),S("ach.load.body"),S(status->error_id ? status->error_id : "ra.err.network"));
        gui_ui_text("ach.load.body",buffer,R(dialog.x+32,dialog.y+78,536,84),GUI_REGULAR,GUI_TEXT);
        if (gui_ui_button("ach.close",S("btn.close"),R(dialog.x+288,dialog.y+184,130,42),0)) view.error_dismissed=1;
        if (first_focus) gui_ui_focus("ach.close");
        if (gui_ui_button("ach.retry",S("btn.retry"),R(dialog.x+434,dialog.y+184,134,42),GUI_PRIMARY)) host_ra_retry();
        gui_ui_modal_end();
    }
}
void gui_achievement_toast(unsigned width,unsigned height,host_rect_t area) {
    host_ra_achievement_t a; float opacity;
    if (!host_ra_toast(&a,host_osd_now(),&opacity)) return;
    float scale=gui_scale(height),right=(float)(area.x+area.w)/scale,top=(float)area.y/scale;
    float inset=(int)(area.h*0.03+0.5)/scale;
    float w=410,x=right-w-inset,y=top+inset;
    if (x<0) x=8;
    if (x+w>(float)width/scale) w=(float)width/scale-x-8;
    gui_ui_opacity(opacity);
    gui_ui_panel(R(x,y,w,68),0x101014c7,host_osd_corner_radius((int)height)/scale);
    gui_ui_badge(a.id,R(x+10,y+10,48,48),0,opacity);
    label("toast.unlocked",S("toast.unlocked.upper"),x+72,y+10,w-126,GUI_SUB,GUI_DIM);
    label("toast.title",a.title,x+72,y+32,w-136,GUI_REGULAR,GUI_TEXT);
    char points[32]; snprintf(points,sizeof(points),S("toast.points"),a.points);
    label("toast.points",points,x+w-54,y+24,48,GUI_MONO,GUI_CASUAL);
    gui_ui_opacity(1);
}
