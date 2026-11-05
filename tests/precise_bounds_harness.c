#include "cpu.h"
#include <assert.h>
#include <stdlib.h>

const unsigned generated_host_precise = 1;
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
static void first(cpu_t *c) { (void)c; }
static void second(cpu_t *c) { (void)c; }

static uint64_t zero_hash(unsigned n) {
    uint64_t hash=UINT64_C(14695981039346656037);
    while (n--) hash *= UINT64_C(1099511628211);
    return hash;
}

int main(void) {
    cpu_t *c=malloc(sizeof(*c)); assert(c); cpu_init(c);
    code_span_t spans[]={{0x80010000,8},{0x801a0000,8}}, later={0x80020000,8};
    code_region_t one={spans,2,zero_hash(16),0,NULL}, two={&later,1,zero_hash(8),0,NULL};
    const code_region_t *regions[]={&one,&two};
    entry_t entries[]={{0x80010000,first},{0x80020000,second}};
    c->entries=entries; c->entry_count=2; c->entry_regions=regions;
    assert(lookup(c,0x801a0004)==first); /* Earlier owner has the furthest span. */
    assert(lookup(c,0x801a0008)==NULL);
    assert(lookup(c,0x8000fffc)==NULL);
    assert(lookup(c,0x80020004)==second);
    mem_write32(c,0x801a0004,1); assert(lookup(c,0x801a0004)==NULL);
    mem_write32(c,0x801a0004,0); assert(lookup(c,0x801a0004)==first);
    code_span_t elsewhere={0x801c0000,8};
    code_region_t three={&elsewhere,1,zero_hash(8),0,NULL};
    const code_region_t *replacement[]={&three};
    entry_t other[]={{0x801c0000,second}};
    c->entries=other; c->entry_count=1; c->entry_regions=replacement;
    assert(lookup(c,0x801c0004)==second);
    assert(lookup(c,0x801a0004)==NULL);
    overlay_t overlay={0,0x80010000,0x1b0008,{0},entries,2,regions};
    c->overlays=&overlay; c->overlay_count=1;
    assert(lookup(c,0x801a0004)==first);
    assert(lookup(c,0x801a0008)==NULL);
    c->overlay_count=0; assert(lookup(c,0x801a0004)==NULL);
    /* HLE registries deliberately bypass the immutable-catalog cache. */
    c->entries=NULL; c->entry_count=0; c->entry_regions=NULL;
    c->overlay_count=1;
    assert(lookup(c,0x801a0004)==first);
    overlay.entries=other; overlay.count=1; overlay.regions=replacement;
    overlay.size=0x1c0008;
    assert(lookup(c,0x801c0004)==second);
    free(c); return 0;
}
