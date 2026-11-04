#include "hw.h"
#include <stdlib.h>

static unsigned mode, acquired, released, pins, calls, outer_resumed, error;
static void inner(cpu_t *c) {
    ++calls;
    if (pins != 2) error = 1;
    if (mode == 3) { cpu_stop(c, "test stop"); return; }
    if (mode == 4) { c->r[9] = 0x17; bios_call(c, 1); error = 2; return; }
    c->pc = mode == 5 ? 0xffffffffu : C_CALLBACK_RETURN;
    if (mode == 2 || mode == 5) c_unwind(c);
}
static void outer(cpu_t *c) {
    ++calls;
    if (pins != 1) error = 3;
    if (mode == 4) {
        struct hw *h = hw_get(c);
        h->i_stat = h->i_mask = 1;
        h->events[0].state = 2; h->events[0].class_id = 0xf2000003;
        h->events[0].spec = 2; h->events[0].mode = 0x1000;
        h->events[0].handler = 0x801a0000;
        bios_exception(c, 0x400);
    } else if (mode >= 1 && mode <= 5) {
        if (c_enter(c, 0x801a0000, mode == 5 ? 0x80098004 : C_CALLBACK_RETURN)) {
            dispatch(c, 0x801a0000); c_leave(c);
        }
    }
    if (pins != 1) error = 4;
    ++outer_resumed;
    c->pc = 0xffffffffu;
}
recompiled_fn native_overlay_acquire(cpu_t *c, uint32_t pc) {
    (void)c;
    if (pc != 0x80098000 && pc != 0x801a0000) return NULL;
    ++acquired; ++pins;
    return pc == 0x80098000 ? outer : inner;
}
void native_overlay_release(recompiled_fn fn) {
    (void)fn;
    if (!pins) error = 5;
    else --pins;
    ++released;
}
int main(int argc, char **argv) {
    cpu_t *c = malloc(sizeof(*c));
    const code_region_t *regions[1] = {NULL};
    unsigned expected;
    if (!c || argc != 2) return 99;
    mode = (unsigned)atoi(argv[1]); cpu_init(c); hw_get(c);
    c->entry_regions = regions;
    /* Populate a permanent dispatch miss before the platform supplies a module. */
    if (lookup(c, 0x80098000)) return 98;
    c->instruction_limit = c->stall_limit = 0;
    if (mode == 6) {
        c->interpreter_only = 1;
        mem_write32(c, 0x80098000, 0x03e00008); mem_write32(c, 0x80098004, 0);
    }
    dispatch(c, 0x80098000);
    expected = mode == 6 ? 0 : mode ? 2 : 1;
    if (mode == 3) outer_resumed = 1; /* cpu_stop abandons the outer C frame. */
    if (error || pins || c->native_pin_count || acquired != expected || released != expected ||
        calls != expected || c->native_acquired != expected || c->native_executed != expected ||
        c->native_released != expected || outer_resumed != (mode == 5 || mode == 6 ? 0 : 1)) return 10 + error;
    hw_destroy(c); free(c); return 0;
}
