#include "cpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef TEST_PRECISE
#define TEST_PRECISE 1
#endif
const unsigned generated_host_precise = TEST_PRECISE;
void bios_call(cpu_t *c, unsigned t) { (void)c; (void)t; }
void bios_syscall(cpu_t *c) { (void)c; }
uint32_t hw_read(cpu_t *c, uint32_t a, unsigned w) { (void)c; (void)a; (void)w; return 0; }
void hw_write(cpu_t *c, uint32_t a, unsigned w, uint32_t v) { (void)c; (void)a; (void)w; (void)v; }
void gte_cmd(cpu_t *c, uint32_t v) { (void)c; (void)v; }
uint32_t gte_read(cpu_t *c, unsigned r, int t) { (void)c; (void)r; (void)t; return 0; }
void gte_write(cpu_t *c, unsigned r, uint32_t v, int t) { (void)c; (void)r; (void)v; (void)t; }
static void first(cpu_t *c) { (void)c; }
static void second(cpu_t *c) { (void)c; }
static void third(cpu_t *c) { (void)c; }
static cpu_t *fence_cpu[2];
static unsigned fence_pending[2];
static uint32_t fence_target = 0x20000, fence_address = 0x80010000, fence_value;
void hw_ram_fence(cpu_t *c, uint32_t physical, uint32_t length) {
    unsigned i;
    (void)length;
    for (i = 0; i < 2; ++i) if (c == fence_cpu[i] && fence_pending[i] && physical == fence_target) {
        fence_pending[i] = 0;
        mem_write32(c, fence_address, fence_value);
        if (fence_target == 0x20000) c->ram_top = 0x800000u;
    }
}

/* Deliberately walk the catalog rather than sharing the dispatch index. */
static recompiled_fn precise(cpu_t *c, const entry_t *entries, size_t count,
                             const code_region_t *const *regions, uint32_t pc) {
    const code_region_t *previous = NULL;
    if (!regions) return NULL;
    while (count && entries[count - 1].pc > pc) --count;
    while (count) {
        const code_region_t *r = regions[--count];
        size_t j;
        if (!r || r == previous) continue;
        previous = r;
        for (j = 0; j < r->count; ++j)
            if (pc >= r->spans[j].address && pc - r->spans[j].address < r->spans[j].size) {
                if (code_valid(c, r)) return entries[count].fn;
                break;
            }
    }
    return NULL;
}
static recompiled_fn linear(cpu_t *c, uint32_t pc) {
    size_t i, j;
    if (c->interpreter_only) return NULL;
    for (j = 0; j < c->entry_count; ++j) if (c->entries[j].pc == pc) {
        if (c->entry_regions ? code_valid(c, c->entry_regions[j]) : !c->load_reg) return c->entries[j].fn;
        break;
    }
    for (i = 0; i < c->overlay_count && i < CODE_OVERLAYS; ++i) {
        const overlay_t *o = &c->overlays[i];
        recompiled_fn fn;
        if (pc < o->base || pc - o->base >= o->size) continue;
        for (j = 0; j < o->count; ++j) if (o->entries[j].pc == pc) {
            if (code_valid(c, o->regions[j])) return o->entries[j].fn;
            break;
        }
        fn = generated_host_precise ? precise(c, o->entries, o->count, o->regions, pc) : NULL;
        if (fn) return fn;
    }
    return generated_host_precise ? precise(c, c->entries, c->entry_count, c->entry_regions, pc) : NULL;
}
#define SETS 427
#define ENTRIES 6
static overlay_t overlays[SETS + 1], other[SETS + 1];
static entry_t entries[SETS][ENTRIES];
static code_region_t regions[SETS][4];
static code_span_t spans[SETS][4][2];
static const code_region_t *catalog[SETS][ENTRIES];
static uint32_t rng = 0x9e3779b9;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint64_t hash(cpu_t *c, const code_region_t *r) {
    uint64_t h = UINT64_C(14695981039346656037);
    size_t i, j;
    for (i = 0; i < r->count; ++i) for (j = 0; j < r->spans[i].size; ++j)
        h = (h ^ c->ram[(r->spans[i].address & (RAM_SIZE - 1)) + j]) * UINT64_C(1099511628211);
    return h;
}
static void check(cpu_t *c, cpu_t *reference, uint32_t pc) {
    recompiled_fn expected = linear(reference, pc), actual = lookup(c, pc);
    if (actual != expected) {
        fprintf(stderr, "dispatch mismatch seed=%08x pc=%08x sets=%zu\n", rng, pc, c->overlay_count);
        abort();
    }
}

static void deferred_write(void) {
    cpu_t *c = malloc(sizeof(*c)), *reference = malloc(sizeof(*reference));
    code_span_t spans2[] = {{0x80010000, 8}, {0x80020000, 8}};
    code_region_t regions2[2];
    const code_region_t *catalog2[2];
    entry_t entries2[] = {{0x80010000, first}, {0x80010000, second}};
    overlay_t sets2[2];
    unsigned i;
    assert(c && reference); cpu_init(c); cpu_init(reference);
    c->ram_limit = reference->ram_limit = RAM_SIZE;
    c->ram_top = reference->ram_top = 0;
    for (i = 0; i < 2; ++i) {
        regions2[i] = (code_region_t){&spans2[i], 1, 0, 0, NULL};
        regions2[i].hash = hash(c, &regions2[i]); catalog2[i] = &regions2[i];
        sets2[i] = (overlay_t){i, 0x80010000, 8, {0}, &entries2[i], 1, &catalog2[i]};
    }
    c->entry_regions = reference->entry_regions = catalog2;
    c->overlays = reference->overlays = sets2; c->overlay_count = reference->overlay_count = 2;
    mem_write32(c, 0x80010000, 1); mem_write32(reference, 0x80010000, 1);
    fence_cpu[0] = c; fence_cpu[1] = reference; fence_pending[0] = fence_pending[1] = 1;
    assert(linear(reference, 0x80010000) == second);
    assert(lookup(c, 0x80010000) == second);
    ++c->code_epoch; ++reference->code_epoch;
    check(c, reference, 0x80010000);
    /* A write armed after a cached hit still fences at the next epoch miss. */
    fence_target = 0x10000; fence_value = 1;
    c->ram_top = reference->ram_top = 0;
    fence_pending[0] = fence_pending[1] = 1;
    ++c->code_epoch; ++reference->code_epoch;
    check(c, reference, 0x80010000);
    fence_target = 0x20000; fence_value = 0;
    fence_cpu[0] = fence_cpu[1] = NULL; free(reference); free(c);
}

static void hle_roundtrip(void) {
    cpu_t *c = malloc(sizeof(*c)), *reference = malloc(sizeof(*reference));
    code_span_t span = {0x80010000, 8};
    code_region_t r = {&span, 1, 0, 0, NULL};
    const code_region_t *p = &r;
    entry_t e = {span.address, first};
    assert(c && reference); cpu_init(c); cpu_init(reference); r.hash = hash(c, &r);
    c->entries = reference->entries = &e; c->entry_count = reference->entry_count = 1;
    c->entry_regions = reference->entry_regions = &p;
    check(c, reference, span.address);
    c->entry_regions = reference->entry_regions = NULL;
    check(c, reference, span.address + 16);
    c->entry_regions = reference->entry_regions = &p;
    check(c, reference, span.address + 4);
    free(reference); free(c);
}

static void crowded_catalog(void) {
    cpu_t *c = malloc(sizeof(*c)), *reference = malloc(sizeof(*reference));
    code_span_t span = {0x80010000, 0x10000};
    code_region_t r[CODE_OVERLAYS + 1];
    const code_region_t *p[CODE_OVERLAYS + 1];
    entry_t e[CODE_OVERLAYS + 1];
    overlay_t sets[CODE_OVERLAYS + 1];
    unsigned i;
    assert(c && reference); cpu_init(c); cpu_init(reference);
    c->ram_limit = reference->ram_limit = RAM_SIZE;
    for (i = 0; i <= CODE_OVERLAYS; ++i) {
        r[i] = (code_region_t){&span, 1, 0, 0, NULL};
        r[i].hash = hash(c, &r[i]) + (i != CODE_OVERLAYS - 1);
        p[i] = &r[i]; e[i] = (entry_t){span.address, i % 2 ? first : second};
        sets[i] = (overlay_t){i, span.address, span.size, {0}, &e[i], 1, &p[i]};
    }
    c->entry_regions = reference->entry_regions = p;
    c->overlays = reference->overlays = sets;
    c->overlay_count = reference->overlay_count = CODE_OVERLAYS + 1;
    /* Distinct interior PCs exercise sustained overlap and index recycling. */
    for (i = 0; i < 2000; ++i) check(c, reference, span.address + i * 4 + 1);
    /* The sole valid owner just beyond the registry limit must stay invisible. */
    sets[CODE_OVERLAYS] = sets[CODE_OVERLAYS - 1];
    sets[CODE_OVERLAYS - 1] = sets[0];
    dispatch_invalidate(c); check(c, reference, span.address + 4);
    sets[CODE_OVERLAYS - 1] = sets[CODE_OVERLAYS];
    dispatch_invalidate(c); check(c, reference, span.address + 4);
    code_watch(c, 0x80030000, 4); code_watch(reference, 0x80030000, 4);
    mem_write32(c, 0x80030000, 1); mem_write32(reference, 0x80030000, 1);
    check(c, reference, span.address + 4);
    mem_write32(c, span.address + 4, 1); mem_write32(reference, span.address + 4, 1);
    check(c, reference, span.address + 4);
    mem_write32(c, span.address + 4, 0); mem_write32(reference, span.address + 4, 0);
    check(c, reference, span.address + 4);
    /* A previously permanent miss must become visible after registration. */
    c->overlay_count = reference->overlay_count = 0;
    check(c, reference, span.address + 4);
    c->overlay_count = reference->overlay_count = 1;
    sets[0] = sets[CODE_OVERLAYS - 1];
    check(c, reference, span.address + 4);
    sets[0] = (overlay_t){0, span.address, span.size, {0}, &e[0], 1, &p[0]};
    dispatch_invalidate(c);
    check(c, reference, span.address + 4);
    /* Reuse the descriptor storage without changing its pointer or code epoch. */
    r[0].hash = hash(c, &r[0]);
    dispatch_invalidate(c); reference->recent_region = NULL;
    memset(reference->code_cache, 0, sizeof(reference->code_cache));
    check(c, reference, span.address);
    free(reference); free(c);
}

static void oversized_catalog(void) {
    const size_t count = DISPATCH_CANDIDATES + 1;
    cpu_t *c = malloc(sizeof(*c)), *reference = malloc(sizeof(*reference));
    code_region_t *r = calloc(count, sizeof(*r));
    const code_region_t **p = malloc(count * sizeof(*p));
    entry_t *e = malloc(count * sizeof(*e));
    code_span_t span = {0x80010000, 8};
    size_t i;
    assert(c && reference && r && p && e); cpu_init(c); cpu_init(reference);
    c->ram_limit = reference->ram_limit = RAM_SIZE;
    for (i = 0; i < count; ++i) {
        r[i] = (code_region_t){&span, 1, 0, 0, NULL};
        r[i].hash = hash(c, &r[i]) + (i != 0);
        p[i] = &r[i]; e[i] = (entry_t){span.address, first};
    }
    c->entries = reference->entries = e;
    c->entry_count = reference->entry_count = count;
    c->entry_regions = reference->entry_regions = p;
    check(c, reference, span.address + 4);
    mem_write32(c, span.address + 4, 1); mem_write32(reference, span.address + 4, 1);
    check(c, reference, span.address + 4);
    free(e); free(p); free(r); free(reference); free(c);
}
int main(int argc, char **argv) {
    cpu_t *c = malloc(sizeof(*c)), *reference = malloc(sizeof(*reference));
    static const unsigned owners[ENTRIES] = {0, 0, 1, 0, 2, 3};
    static recompiled_fn fns[] = {first, second, third, NULL};
    unsigned i, j, n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 10000;
    if (argc > 2) rng = (uint32_t)strtoul(argv[2], NULL, 0);
    assert(c && reference); cpu_init(c); cpu_init(reference);
    c->ram_limit = reference->ram_limit = RAM_SIZE;
    for (i = 0; i < SETS; ++i) {
        for (j = 0; j < 4; ++j) {
            uint32_t base = 0x80010000u + (i % 7) * 0x1000u;
            spans[i][j][0] = (code_span_t){base + j * 16, 12};
            spans[i][j][1] = (code_span_t){0x80020000u + (i % 3) * 0x1000u + j * 16, 12};
            regions[i][j] = (code_region_t){spans[i][j], 2, 0, 0, NULL};
            regions[i][j].hash = hash(c, &regions[i][j]) + (i % 11 != 0);
        }
        for (j = 0; j < ENTRIES; ++j) {
            entries[i][j] = (entry_t){spans[i][0][0].address + j * 4, fns[(i + j) % 4]};
            catalog[i][j] = j == 2 && i % 5 == 0 ? NULL : &regions[i][owners[j]];
        }
        overlays[i] = (overlay_t){i, 0x80010000u, 0x13000u, {0}, entries[i], ENTRIES, catalog[i]};
        other[SETS - 1 - i] = overlays[i];
    }
    /* The sentinel must remain invisible beyond the registry limit. */
    overlays[SETS] = other[SETS] = overlays[0];
    c->entries = reference->entries = entries[3];
    c->entry_count = reference->entry_count = ENTRIES;
    c->entry_regions = reference->entry_regions = catalog[3];
    c->overlays = reference->overlays = overlays;
    c->overlay_count = reference->overlay_count = SETS;
    for (i = 0; i < n; ++i) {
        uint32_t value = random32(), pc;
        switch (value % 12) {
        case 0: case 1: case 2: {
            uint32_t address = 0x80010000u + (random32() % 7) * 0x1000u + (random32() % 16) * 4;
            unsigned data = random32() % 4 == 0 ? random32() : 0;
            if (value & 0x100) address += 0x10000u;
            mem_write32(c, address, data); mem_write32(reference, address, data); break;
        }
        case 3: {
            uint32_t address = 0x80010000u + random32() % 0x14000u;
            size_t size = random32() % 8192;
            code_invalidate(c, address, size); code_invalidate(reference, address, size); break;
        }
        case 4: ++c->code_epoch; ++reference->code_epoch; break;
        case 5:
            c->overlay_count = reference->overlay_count = random32() % (SETS + 2); break;
        case 6:
            c->overlays = reference->overlays = value & 0x100 ? other : overlays; break;
        case 7: {
            unsigned set = random32() % SETS;
            c->entries = reference->entries = entries[set];
            c->entry_regions = reference->entry_regions = value & 0x100 ? catalog[set] : NULL;
            c->entry_count = reference->entry_count = random32() % (ENTRIES + 1); break;
        }
        case 8:
            c->interpreter_only = reference->interpreter_only = (value >> 8) & 1;
            c->load_reg = reference->load_reg = (value >> 9) & 1; break;
        default: break;
        }
        pc = 0x80010000u + (random32() % 20) * 0x1000u + random32() % 80;
        check(c, reference, pc); check(c, reference, pc);
        /* Hot addresses collide in the ordinary direct-mapped cache. */
        check(c, reference, 0x80010004u + (random32() % 7) * 0x1000u);
        check(c, reference, 0x80020004u + (random32() % 3) * 0x1000u);
    }
    free(reference); free(c); deferred_write(); hle_roundtrip(); crowded_catalog(); oversized_catalog();
    puts("PASS overlapping dispatch matches the linear catalog"); return 0;
}
