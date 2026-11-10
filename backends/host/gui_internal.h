#ifndef FF8_HOST_GUI_INTERNAL_H
#define FF8_HOST_GUI_INTERNAL_H
#include "gui.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float x,y,w,h; } gui_rect_t;
float gui_scale(unsigned height);
typedef enum { GUI_REGULAR, GUI_TITLE, GUI_SUB, GUI_MONO, GUI_HINT } gui_font_t;
typedef enum { GUI_TEXT, GUI_DIM, GUI_MUTED, GUI_ACCENT, GUI_OK, GUI_WARN, GUI_ERROR, GUI_CASUAL } gui_colour_t;
/* GUI_ALERT colours only a row's second line, as a status in error. */
enum { GUI_DISABLED=1, GUI_PRIMARY=2, GUI_DANGER=4, GUI_SELECTED=8, GUI_GHOST=16, GUI_ALERT=32 };
typedef enum { GUI_PICK_DISC, GUI_PICK_CARD, GUI_PICK_MOVIES, GUI_PICK_SCREENSHOTS, GUI_PICK_ARCHIVE } gui_pick_t;

/* Every rectangle is in logical 1280x720 coordinates. begin computes scale once. */
int gui_ui_attach(SDL_Window *, SDL_Renderer *);
void gui_ui_detach(void);
void gui_ui_event(const SDL_Event *);
void gui_ui_reset_input(void);
void gui_ui_begin(unsigned width, unsigned height, int overlay);
void gui_ui_end(void);
void gui_ui_soft_render(SDL_Renderer *);
void gui_ui_control_row(gui_rect_t);
int gui_ui_visible(gui_rect_t);
int gui_ui_wants_text(void);
void gui_ui_text(const char *id, const char *text, gui_rect_t, gui_font_t, gui_colour_t);
void gui_ui_text_centred(const char *id, const char *text, gui_rect_t, gui_font_t, gui_colour_t);
float gui_ui_text_width(const char *text, gui_font_t);
void gui_ui_keycap(const char *id, const char *key, const char *label, gui_rect_t);
void gui_ui_panel(gui_rect_t, unsigned rgba, float radius);
void gui_ui_line(float x1,float y1,float x2,float y2,unsigned rgba,float width);
int gui_ui_button(const char *id, const char *label, gui_rect_t, unsigned flags);
int gui_ui_row(const char *id, const char *label, const char *sub, gui_rect_t, unsigned flags);
int gui_ui_segments(const char *id, gui_rect_t, const char *const *labels, int count, int current, int enabled);
int gui_ui_toggle(const char *id, gui_rect_t, int current, int enabled);
int gui_ui_stepper(const char *id, gui_rect_t, int current, int minimum, int maximum, int enabled);
int gui_ui_edit(const char *id, gui_rect_t, char *utf8, size_t capacity, int password);
void gui_ui_modal_begin(const char *id, gui_rect_t);
void gui_ui_modal_end(void);
void gui_ui_focus(const char *id);
const char *gui_ui_focused(void);
const char *gui_ui_text_for(const char *id);
/* 1 selected, 0 cancelled, -1 unavailable (caller opens in-app browser). */
int gui_ui_native_pick(gui_pick_t, char *path, size_t capacity);
void gui_ui_emblem(gui_rect_t);
void gui_ui_badge(unsigned id,gui_rect_t,int locked,float opacity);
void gui_ui_opacity(float);
void gui_achievements_draw(float x,float width,float height,int first_focus);
void gui_achievements_event(const SDL_Event *);
void gui_achievements_reset(void);
int gui_achievements_back(void);
struct host_rect;
void gui_achievement_toast(unsigned width,unsigned height,struct host_rect);
int host_gui_toast_visible(void);
void host_gui_toast_frame(unsigned width,unsigned height,struct host_rect);
void gui_ui_scroll_begin(const char *id, gui_rect_t);
void gui_ui_scroll_end(void);
void gui_ui_region_begin(const char *id, gui_rect_t);
void gui_ui_region_end(void);

/* Pure helpers and one worker. No GUI/toolkit or guest state access. */
typedef struct { unsigned blocks_used, ff8_saves; } gui_card_summary_t;
int gui_card_summary(const char *path, gui_card_summary_t *);
int gui_card_import(const char *source,const char *data_dir,char *destination,size_t,char *error,size_t);
int gui_card_new(const char *data_dir,unsigned slot,char *destination,size_t,char *error,size_t);
typedef enum { GUI_DISC_EMPTY, GUI_DISC_CHECKING, GUI_DISC_VERIFIED, GUI_DISC_MISSING, GUI_DISC_ERROR } gui_disc_state_t;
typedef struct { char path[512],detail[512]; unsigned disc,generation; gui_disc_state_t state; double elapsed; int full,scan; } gui_disc_result_t;
int gui_media_start(void);
void gui_media_stop(void);
int gui_media_identify(const char *path,int full,unsigned generation,int scan);
int gui_media_scan(const char *folder,unsigned generation);
int gui_media_result(gui_disc_result_t *);
const char *gui_disc_error_id(const char *error);

#define GUI_BROWSER_MAX 1024
typedef struct { char name[512],path[512]; uint64_t bytes; int directory; } gui_browser_entry_t;
typedef struct { char folder[512],last_folder[512],path[512]; gui_browser_entry_t entries[GUI_BROWSER_MAX]; unsigned count; int show_all; gui_pick_t kind; char error[512]; } gui_browser_t;
int gui_browser_open(gui_browser_t *,const char *folder,gui_pick_t);
int gui_browser_refresh(gui_browser_t *);
int gui_browser_up(gui_browser_t *);
int gui_browser_accept(const gui_browser_t *,const char *path);
unsigned gui_browser_places(char names[][512],char paths[][512],unsigned capacity,const char *last);
int gui_state_load(const char *data_dir,char *folder,size_t,int *input,int *native_offered);
int gui_state_save(const char *data_dir,const char *folder,int input,int native_offered);
/* Native code in words: the hub row's second line (alert when it reports a
 * failure), and the copy for an installer's reason. */
void gui_native_summary(const host_gui_native_t *,char *,size_t,int *alert);
const char *gui_native_reason_id(const char *reason);
/* "zip" or "tar.xz": the compiler archive this player installs. */
const char *gui_archive_format(void);
#ifdef __cplusplus
}
#endif
#endif
