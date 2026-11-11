/* Smooth motion's host side binds nothing until it starts and leaves nothing
 * bound when it stops: the interpreter's GTE commands, the projection entries
 * modules call (native_modules_bind_projection, recorded here in its place)
 * and the frame identity service. */
#include "frame_present_host.h"
#include "frame_identity.h"
#include "gte.h"
#include "hw.h"
#include "interp.h"
#include <stdio.h>
#include <stdlib.h>

static void (*bound_rtps)(cpu_t *, unsigned, int), (*bound_rtpt)(cpu_t *, unsigned, int);
static unsigned binds;
void native_modules_bind_projection(void (*rtps)(cpu_t *, unsigned, int), void (*rtpt)(cpu_t *, unsigned, int));
void native_modules_bind_projection(void (*rtps)(cpu_t *, unsigned, int), void (*rtpt)(cpu_t *, unsigned, int)) {
    bound_rtps = rtps; bound_rtpt = rtpt; ++binds;
}

static int plain(const char *when) {
    int ok = interp_gte_command == gte_cmd && !bound_rtps && !bound_rtpt && !frame_identity_running() &&
             !host_frame_present_running() && !host_frame_present_gpu();
    printf("%s: %s binds=%u\n", when, ok ? "plain" : "bound", binds);
    return ok;
}

int main(void) {
    cpu_t *c = calloc(1, sizeof(*c));
    struct hw *h = calloc(1, sizeof(*h));
    int ok = 1;
    if (!c || !h || !(h->gpu = gpu_create())) return 2;
    c->hw = h;
    ok &= plain("before");
    if (!host_frame_present_start(c, 0, 0)) return 2;
    ok &= interp_gte_command == frame_identity_gte_command && bound_rtps == frame_identity_rtps &&
          bound_rtpt == frame_identity_rtpt && frame_identity_running() && host_frame_present_gpu() != NULL &&
          host_frame_present_gpu() != h->gpu;
    printf("running: %s\n", ok ? "recording entries, own GPU" : "wrong");
    host_frame_present_stop(0);
    ok &= plain("after");
    free(h->gpu); free(h); free(c);
    return ok ? 0 : 1;
}
