/* Unit harness for the pre-game native compilation loading screen
 * (HOST_GUI_LOADING): verifies logo, progress bar, copy, auto-start on DONE,
 * Play anyway, Quit and Escape handling without needing real game discs. */
#define _POSIX_C_SOURCE 200809L
#include "gui_internal.h"
#include "display.h"
#include "viewport.h"
#include "ra.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static host_gui_native_t mock_native;
static const char *click_button;
static int send_escape_frame;
static int frame_no;
static int emblem_count, panel_count;
static char last_focus[64];
static char texts[32][256], text_ids[32][64];
static unsigned text_count;
static int scenario;

static void record_text(const char *id, const char *value) {
    if (!id || !value || text_count >= 32) return;
    snprintf(text_ids[text_count], sizeof(text_ids[0]), "%s", id);
    snprintf(texts[text_count], sizeof(texts[0]), "%s", value);
    ++text_count;
}
static const char *find_text(const char *id) {
    for (unsigned i = 0; i < text_count; ++i)
        if (!strcmp(text_ids[i], id)) return texts[i];
    return NULL;
}

/* Minimal UI stubs capturing draw calls from gui.c. */
void gui_ui_event(const SDL_Event *e) { (void)e; }
void gui_ui_begin(unsigned w, unsigned h, int ov) { (void)w; (void)h; (void)ov; emblem_count = panel_count = 0; text_count = 0; }
void gui_ui_end(void) {}
void gui_ui_control_row(gui_rect_t r) { (void)r; }
int gui_ui_visible(gui_rect_t r) { (void)r; return 1; }
int gui_ui_wants_text(void) { return 0; }
void gui_ui_text(const char *id, const char *t, gui_rect_t r, gui_font_t f, gui_colour_t c) { (void)r; (void)f; (void)c; record_text(id, t); }
void gui_ui_text_centred(const char *id, const char *t, gui_rect_t r, gui_font_t f, gui_colour_t c) { (void)r; (void)f; (void)c; record_text(id, t); }
void gui_ui_keycap(const char *id, const char *k, const char *l, gui_rect_t r) { (void)k; (void)r; record_text(id, l); }
void gui_ui_panel(gui_rect_t r, unsigned rgba, float radius) { (void)r; (void)rgba; (void)radius; ++panel_count; }
void gui_ui_line(float x1, float y1, float x2, float y2, unsigned rgba, float w) { (void)x1; (void)y1; (void)x2; (void)y2; (void)rgba; (void)w; }
int gui_ui_button(const char *id, const char *label, gui_rect_t r, unsigned flags) {
    (void)r; (void)flags; record_text(id, label);
    return click_button && !strcmp(click_button, id) && frame_no >= 2;
}
int gui_ui_row(const char *id, const char *l, const char *s, gui_rect_t r, unsigned f) { (void)id; (void)l; (void)s; (void)r; (void)f; return 0; }
int gui_ui_segments(const char *id, gui_rect_t r, const char *const *l, int n, int c, int e) { (void)id; (void)r; (void)l; (void)n; (void)e; return c; }
int gui_ui_toggle(const char *id, gui_rect_t r, int c, int e) { (void)id; (void)r; (void)e; return c; }
int gui_ui_stepper(const char *id, gui_rect_t r, int c, int a, int b, int e) { (void)id; (void)r; (void)a; (void)b; (void)e; return c; }
int gui_ui_edit(const char *id, gui_rect_t r, char *u, size_t n, int p) { (void)id; (void)r; (void)u; (void)n; (void)p; return 0; }
void gui_ui_modal_begin(const char *id, gui_rect_t r) { (void)id; (void)r; }
void gui_ui_modal_end(void) {}
void gui_ui_focus(const char *id) { snprintf(last_focus, sizeof(last_focus), "%s", id ? id : ""); }
const char *gui_ui_focused(void) { return last_focus; }
int gui_ui_native_pick(gui_pick_t k, char *p, size_t n) { (void)k; (void)p; (void)n; return -1; }
void gui_ui_emblem(gui_rect_t r) { (void)r; ++emblem_count; }
void gui_ui_scroll_reset(void) {}
void gui_ui_scroll_begin(const char *id, gui_rect_t r) { (void)id; (void)r; }
void gui_ui_scroll_end(void) {}
void gui_ui_region_begin(const char *id, gui_rect_t r) { (void)id; (void)r; }
void gui_ui_region_end(void) {}
void gui_achievements_draw(float x, float w, float h, int f) { (void)x; (void)w; (void)h; (void)f; }
void gui_achievements_event(const SDL_Event *e) { (void)e; }
void gui_achievements_reset(void) {}
int gui_achievements_back(void) { return 0; }
void gui_achievement_toast(unsigned w, unsigned h, host_rect_t r) { (void)w; (void)h; (void)r; }
int host_gui_test_load(const char *p) { (void)p; return 1; }
void host_gui_test_shutdown(void) {}
int host_gui_test_active(void) { return 1; }
uint64_t host_osd_now(void) { return 0; }

static void service_native(void *ctx, host_gui_native_t *out) {
    (void)ctx;
    *out = mock_native;
}

static host_gui_result_t service_loop(void *ctx, int shell) {
    (void)ctx;
    assert(shell == 1);
    frame_no = 0;
    while (host_gui_visible() && host_gui_result() == HOST_GUI_RUNNING && frame_no < 10) {
        ++frame_no;
        if (send_escape_frame && frame_no == send_escape_frame) {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_KEYDOWN;
            ev.key.keysym.sym = SDLK_ESCAPE;
            host_gui_event(&ev);
        }
        if (scenario == 1 && frame_no == 2) {
            mock_native.build = HOST_GUI_BUILD_DONE;
            mock_native.ready = mock_native.sets;
        }
        if (scenario == 4 && frame_no == 2) {
            mock_native.loading = 0;
            mock_native.waiting = 1;
            mock_native.sets = 428;
            mock_native.ready = 50;
        }
        if (scenario == 4 && frame_no == 3) {
            mock_native.build = HOST_GUI_BUILD_STOPPED;
        }
        host_gui_frame(1280, 720);
        if (scenario == 1 && frame_no == 1) {
            assert(host_gui_mode() == HOST_GUI_LOADING);
            assert(emblem_count == 1);
            assert(panel_count >= 4);
            assert(find_text("app.name") && !strcmp(find_text("app.name"), "Lionheart"));
            assert(find_text("loading.title") && !strcmp(find_text("loading.title"), "Preparing native code"));
            assert(find_text("loading.status") && strstr(find_text("loading.status"), "128 of 428 ready · 29%"));
            assert(find_text("loading.status") && strstr(find_text("loading.status"), "about 10 min left"));
            assert(find_text("loading.compiling") && !strcmp(find_text("loading.compiling"), "Compiling ovl_field"));
            assert(find_text("btn.play_anyway") && !strcmp(find_text("btn.play_anyway"), "Play anyway"));
            assert(find_text("btn.quit") && !strcmp(find_text("btn.quit"), "Quit"));
            assert(!strcmp(last_focus, "btn.play_anyway"));
        }
        if (scenario == 4 && frame_no == 1) {
            assert(find_text("loading.title") && !strcmp(find_text("loading.title"), "Checking native code cache…"));
        }
        if (scenario == 4 && frame_no == 2) {
            assert(find_text("loading.title") && !strcmp(find_text("loading.title"), "Waiting for another Lionheart build…"));
        }
    }
    return host_gui_result();
}

int main(void) {
    host_options_t options;
    host_gui_services_t services;
    host_options_defaults(&options);
    memset(&services, 0, sizeof(services));
    services.loop = service_loop;
    services.native = service_native;
    assert(host_gui_configure(&options, "", &services, NULL));

    /* 1: Progress rendering + auto-start when DONE. */
    scenario = 1; click_button = NULL; send_escape_frame = 0;
    memset(&mock_native, 0, sizeof(mock_native));
    mock_native.compiler = HOST_GUI_COMPILER_READY;
    mock_native.build = HOST_GUI_BUILD_RUNNING;
    mock_native.sets = 428; mock_native.ready = 128; mock_native.seconds_left = 600;
    snprintf(mock_native.compiling, sizeof(mock_native.compiling), "ovl_field");
    assert(host_gui_loading() == HOST_GUI_PLAY);
    assert(host_gui_mode() == HOST_GUI_HIDDEN);

    /* 2: Clicking Play anyway starts immediately while still building. */
    scenario = 2; click_button = "btn.play_anyway"; send_escape_frame = 0;
    mock_native.build = HOST_GUI_BUILD_RUNNING; mock_native.ready = 12;
    assert(host_gui_loading() == HOST_GUI_PLAY);

    /* 3: Clicking Quit exits cleanly. */
    scenario = 3; click_button = "btn.quit"; send_escape_frame = 0;
    assert(host_gui_loading() == HOST_GUI_QUIT);

    /* 4: Escape exits cleanly without a modal dialog. */
    scenario = 3; click_button = NULL; send_escape_frame = 2;
    assert(host_gui_loading() == HOST_GUI_QUIT);

    /* 5: Checking -> Waiting -> Stopped exits cleanly to play. */
    scenario = 4; click_button = NULL; send_escape_frame = 0;
    memset(&mock_native, 0, sizeof(mock_native));
    mock_native.compiler = HOST_GUI_COMPILER_READY;
    mock_native.build = HOST_GUI_BUILD_RUNNING;
    mock_native.loading = 1;
    assert(host_gui_loading() == HOST_GUI_PLAY);

    host_gui_shutdown();
    puts("loading-ok");
    return 0;
}
