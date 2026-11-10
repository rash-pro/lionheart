#define FF8_CPU_IMPLEMENTATION
#include "cpu.h"
#include "scratchpad.h"
#include "poll.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overlay_profile.h"
#if defined(FF8_SAVESTATES)
#include "savestate.h"
#endif

__attribute__((weak)) recompiled_fn native_overlay_acquire(cpu_t *c, uint32_t pc) {
    (void)c; (void)pc; return NULL;
}

__attribute__((weak)) void native_overlay_release(recompiled_fn fn) { (void)fn; }

extern const unsigned generated_host_precise __attribute__((weak));
static int precise_host(void) {
#ifdef FF8_COMPACT
    return 0;
#else
    return &generated_host_precise && generated_host_precise;
#endif
}

typedef struct {
    dispatch_cache_t *slot;
    unsigned first, skip, complete;
    int recordable;
} dispatch_builder_t;
static int dispatch_check(cpu_t *c, dispatch_builder_t *index, const entry_t *entry, const code_region_t *region);

static recompiled_fn precise_source(cpu_t *c, const entry_t *entries, size_t count,
                                   const code_region_t *const *regions, const source_bounds_t *bounds,
                                   uint32_t pc, int *registered, dispatch_builder_t *index) {
    size_t lo = 0, hi = count;
    const code_region_t *previous = NULL;
    if (!regions || (bounds && (pc < bounds->first || pc >= bounds->end))) return NULL;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (entries[mid].pc <= pc) lo = mid + 1; else hi = mid;
    }
    while (lo) {
        const code_region_t *r = regions[--lo];
        size_t i;
        if (!r || r == previous) continue;
        previous = r;
        for (i = 0; i < r->count; ++i) if (pc >= r->spans[i].address && pc - r->spans[i].address < r->spans[i].size) {
            if (registered) *registered = 1;
            if (dispatch_check(c, index, &entries[lo], r)) return entries[lo].fn;
            break;
        }
    }
    return NULL;
}

static source_bounds_t source_bounds(const code_region_t *const *regions, size_t count) {
    source_bounds_t bounds = {UINT32_MAX, 0};
    const code_region_t *previous = NULL;
    size_t i, j;
    if (!regions) return bounds;
    for (i = 0; i < count; ++i) {
        const code_region_t *r = regions[i];
        if (!r || r == previous) continue;
        previous = r;
        for (j = 0; j < r->count; ++j) {
            uint64_t end = (uint64_t)r->spans[j].address + r->spans[j].size;
            if (!r->spans[j].size) continue;
            if (r->spans[j].address < bounds.first) bounds.first = r->spans[j].address;
            if (end > bounds.end) bounds.end = end;
        }
    }
    return bounds;
}

void c_release_overlays(cpu_t *c, unsigned keep) {
    while (c->native_pin_count > keep) {
        native_overlay_release(c->native_pins[--c->native_pin_count]);
        ++c->native_released;
    }
}

void cpu_init(cpu_t *c) {
    memset(c, 0, sizeof(*c));
    c->ram_top = 0x800000u;
    c->r[29] = 0x801ffff0u;
    c->r[31] = 0xffffffffu;
    c_set_sr(c, 0x10900000u);
    c->cop0[15] = 2;
    c->seed = 1;
    c->code_epoch = 1;
    c->stall_limit = 1000000;
    c->instruction_limit = 100000000;
    c->event_next = 1;
    c->thread_next = 1;
    c->ram[0x60] = 2;
    if (getenv("FF8_INTERP_PROFILE")) c->interp_profile = calloc(RAM_SIZE / 4, sizeof(uint64_t));
    overlay_profile_init(c);
#if defined(FF8_STATS)
    if (getenv("FF8_PC_PROFILE")) {
        c->pc_profile = calloc(RAM_SIZE / 4, sizeof(uint64_t));
        c->pc_profile_first = getenv("FF8_PC_FIRST") ? strtoull(getenv("FF8_PC_FIRST"), NULL, 0) : 0;
        c->pc_profile_last = getenv("FF8_PC_LAST") ? strtoull(getenv("FF8_PC_LAST"), NULL, 0) : UINT64_MAX;
        c->pc_profile_active = c->pc_profile && !c->pc_profile_first;
    }
#endif
}

void c_candidate(cpu_t *c, uint32_t pc, unsigned kind) {
#ifdef FF8_STATS
    unsigned i, slot = ((pc >> 2) ^ kind) % C_CANDIDATE_SLOTS;
    for (i = 0; i < C_CANDIDATE_SLOTS; ++i) {
        candidate_stat_t *s = &c->perf.candidates[(slot + i) % C_CANDIDATE_SLOTS];
        if (!s->hits || (s->pc == pc && s->kind == kind)) {
            s->pc = pc; s->kind = kind; ++s->hits; return;
        }
    }
    ++c->perf.candidate_lost;
#else
    (void)c; (void)pc; (void)kind;
#endif
}

void c_perf_summary(const cpu_t *c, uint64_t vblank) {
#ifdef FF8_STATS
    unsigned i;
    printf("CPU MEMORY vb=%llu ram=%llu scratch=%llu mmio=%llu other=%llu gap=%llu fence=%llu watched=%llu\n",
        (unsigned long long)vblank,
        (unsigned long long)c->perf.memory[C_MEM_RAM], (unsigned long long)c->perf.memory[C_MEM_SCRATCH],
        (unsigned long long)c->perf.memory[C_MEM_MMIO], (unsigned long long)c->perf.memory[C_MEM_OTHER],
        (unsigned long long)c->perf.memory[C_MEM_GAP], (unsigned long long)c->perf.memory[C_MEM_FENCE],
        (unsigned long long)c->perf.memory[C_MEM_WATCHED]);
    printf("CPU DISPATCH vb=%llu call=%llu return=%llu table=%llu cross_page=%llu candidate_lost=%llu native_cross_page=%llu\n",
        (unsigned long long)vblank, (unsigned long long)c->perf.dispatch[C_DISPATCH_CALL],
        (unsigned long long)c->perf.dispatch[C_DISPATCH_RETURN], (unsigned long long)c->perf.dispatch[C_DISPATCH_TABLE],
        (unsigned long long)c->perf.dispatch[C_DISPATCH_CROSS_PAGE], (unsigned long long)c->perf.candidate_lost,
        (unsigned long long)c->perf.native_cross_page);
    for (i = 0; i < C_CANDIDATE_SLOTS; ++i) {
        const candidate_stat_t *s = &c->perf.candidates[i];
        if (s->hits) printf("CPU CANDIDATE vb=%llu pc=%08x kind=%u hits=%llu\n",
            (unsigned long long)vblank, (unsigned)s->pc, (unsigned)s->kind, (unsigned long long)s->hits);
    }
#else
    (void)c; (void)vblank;
#endif
}

void c_perf_checkpoint(cpu_t *c, uint64_t vblank) {
#if defined(FF8_STATS)
    if (c->pc_profile && vblank == c->pc_profile_first) c->pc_profile_active = 1;
    if (c->pc_profile && vblank >= c->pc_profile_last) {
        FILE *file = fopen(getenv("FF8_PC_PROFILE"), "w");
        uint32_t i;
        if (file) {
            fputs("pc,instructions\n", file);
            for (i = 0; i < RAM_SIZE / 4; ++i) if (c->pc_profile[i])
                fprintf(file, "%08x,%llu\n", 0x80000000u + i * 4, (unsigned long long)c->pc_profile[i]);
            fclose(file);
        }
        free(c->pc_profile); c->pc_profile = NULL; c->pc_profile_active = 0;
    }
#else
    (void)c; (void)vblank;
#endif
}

void cpu_log(cpu_t *c, const char *format, ...) {
    char buffer[512];
    va_list args;
    if (!c->log) return;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    c->log(c->log_context, buffer);
}

void cpu_stop(cpu_t *c, const char *format, ...) {
    va_list args;
    if (c->halted) return;
    c->halted = 1;
    if (!c->fault_counted) c_count_recompiled(c, c->pc, c->fault_instructions);
    if (c->fault_counted != 1) c_recomp_budget(c, c->fault_instructions);
    c->cycles += (uint64_t)c->fault_instructions * CYCLES_PER_INSN;
    c->fault_instructions = 0;
    va_start(args, format);
    vsnprintf(c->stop_reason, sizeof(c->stop_reason), format, args);
    va_end(args);
    cpu_log(c, "STOP pc=%08x %s", c->pc, c->stop_reason);
    c_release_overlays(c, 0);
    overlay_profile_save(c);
    c_perf_checkpoint(c, UINT64_MAX);
    if (c->interp_profile) {
        FILE *profile = fopen(getenv("FF8_INTERP_PROFILE"), "w");
        uint32_t i;
        if (profile) {
            fputs("pc,instructions\n", profile);
            for (i = 0; i < RAM_SIZE / 4; ++i) if (c->interp_profile[i])
                fprintf(profile, "%08x,%llu\n", 0x80000000u + i * 4,
                        (unsigned long long)c->interp_profile[i]);
            fclose(profile);
        }
        free(c->interp_profile); c->interp_profile = NULL;
    }
    if (c->stop_armed) longjmp(c->stop, 1);
}

void cpu_exception(cpu_t *c, unsigned code, uint32_t bad_address) {
    /* A faulting instruction does not cancel the preceding delayed load. */
    if (c->load_reg) c->r[c->load_reg] = c->load_value;
    c->load_reg = c->load_value = 0;
    c->cop0[8] = bad_address;
    c->cop0[13] = (code << 2) | (c->in_delay_slot ? 0x80000000u : 0);
    c->cop0[14] = c->in_delay_slot ? c->branch_pc : c->pc;
    c->branch_pending = 0;
    c_set_sr(c, (c->cop0[12] & ~63u) | ((c->cop0[12] << 2) & 63));
    cpu_stop(c, "CPU exception %u EPC=%08x BadVAddr=%08x%s", code,
             c->cop0[14], bad_address, c->in_delay_slot ? " (delay slot)" : "");
}

void c_poll(cpu_t *c) {
    poll_t *p = &c->polls[(c->pc >> 2) % POLL_SLOTS];
    if (c->halted) return;
    if (c->instruction_limit && c_instructions(c) >= c->instruction_limit) {
        cpu_stop(c, "instruction budget exhausted");
        return;
    }
    if (p->pc != c->pc || p->epoch != c->mmio_epoch) {
        p->pc = c->pc; p->epoch = c->mmio_epoch; p->hits = 0;
    }
    if (c->stall_limit && ++p->hits >= c->stall_limit)
        cpu_stop(c, "stall: loop safe point repeated %llu times without MMIO change",
                 (unsigned long long)p->hits);
}

/* hw supplies the strong implementation when that stream is linked. */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void hw_event(cpu_t *c) { c->next_event = UINT64_MAX / 2; }
#if defined(__GNUC__)
__attribute__((weak))
#endif
void hw_sr_changed(cpu_t *c) { (void)c; }
#if defined(__GNUC__)
__attribute__((weak))
#endif
int hw_interrupt_window(cpu_t *c, uint32_t window) { (void)c; (void)window; return 1; }
#if defined(__GNUC__)
__attribute__((weak))
#endif
void hw_ram_fence(cpu_t *c, uint32_t physical, uint32_t length) { (void)c; (void)physical; (void)length; }

void c_tick_slow(cpu_t *c) {
    /* A prefix flush precedes execution of the current delay-slot instruction.
     * hw_event initially sees its fault context; deliver an IRQ already due
     * before that instruction, matching the interpreter's branch retirement. */
    if (c->in_delay_slot && !c->fault_instructions &&
        (c->cop0[12] & 0x401) == 0x401 && (c->cop0[13] & 0x400)) {
        c->in_delay_slot = 0;
        hw_event(c);
        c->in_delay_slot = 1;
    }
    /* A caller flush and the callee's entry tick describe the same boundary. */
    if (!c->tick_seen || c->last_tick_pc != c->pc || c->last_tick_cycles != c->cycles) {
        c->tick_seen = 1;
        c->last_tick_pc = c->pc;
        c->last_tick_cycles = c->cycles;
        c_poll(c);
    }
    /* Even a distant hardware event must not bypass the diagnostic budget. */
    if (c->instruction_limit) {
        uint64_t remaining = c_instructions(c) < c->instruction_limit ? c->instruction_limit - c_instructions(c) : 0;
        uint64_t deadline = c->cycles + remaining * CYCLES_PER_INSN;
        if ((int64_t)(c->next_event - deadline) > 0) c->next_event = deadline;
    }
}

/* Each cache records the newest write to any page containing its source spans.
 * Data writes elsewhere do not trigger hashing, even when the global epoch moves. */
void code_invalidate(cpu_t *c, uint32_t address, size_t length) {
    uint32_t physical = address & 0x1fffffffu;
    if (physical >= 0x800000u || !length) return;
    if (length > RAM_SIZE) length = RAM_SIZE;
    while (length) {
        uint32_t offset = physical & (RAM_SIZE - 1);
        size_t chunk = 256 - (offset & 255);
        unsigned first = (offset & 255) / 4, last;
        uint64_t mask;
        if (chunk > length) chunk = length;
        last = ((offset & 255) + (unsigned)chunk - 1) / 4;
        mask = (UINT64_MAX << first) & (UINT64_MAX >> (63 - last));
        if (c->code_words[offset / 256] & mask)
            c->code_pages[offset >> CODE_PAGE_SHIFT] = ++c->code_epoch;
        physical += (uint32_t)chunk;
        length -= chunk;
    }
}

static uint64_t code_version(cpu_t *c, uint32_t address, uint32_t length) {
    uint32_t first = (address & (RAM_SIZE - 1)) >> CODE_PAGE_SHIFT;
    uint32_t last = ((address & (RAM_SIZE - 1)) + length - 1) >> CODE_PAGE_SHIFT, page;
    uint64_t version = 1;
    uint32_t offset = address & (RAM_SIZE - 1), left = length;
    /* Watching or hashing code first completes deferred device writes to it. */
    if (c->ram_top < 0x800000u) hw_ram_fence(c, address & (RAM_SIZE - 1), length);
    while (left) {
        unsigned first = (offset & 255) / 4, chunk = 256 - (offset & 255), last;
        if (chunk > left) chunk = left;
        last = ((offset & 255) + chunk - 1) / 4;
        c->code_words[offset / 256] |= (UINT64_MAX << first) & (UINT64_MAX >> (63 - last));
        offset += chunk; left -= chunk;
    }
    for (page = first; page <= last; ++page) {
        c->code_watched[page] = 1;
        if (c->code_pages[page] > version) version = c->code_pages[page];
    }
    return version;
}

void code_watch(cpu_t *c, uint32_t address, uint32_t length) {
    if (length) (void)code_version(c, address, length);
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static int code_valid_slow(cpu_t *c, const code_region_t *region, code_cache_t *cache) {
    uint64_t version = 1, hash = UINT64_C(14695981039346656037);
    size_t i, j;
    for (i = 0; i < region->count; ++i) {
        uint64_t v = code_version(c, region->spans[i].address, region->spans[i].size);
        if (v > version) version = v;
    }
    if (cache->region != region || cache->version != version) {
        for (i = 0; i < region->count; ++i) {
            const code_span_t *span = &region->spans[i];
            const uint8_t *source = c->ram + (span->address & (RAM_SIZE - 1));
            for (j = 0; j < span->size; ++j) hash = (hash ^ source[j]) * UINT64_C(1099511628211);
        }
        cache->valid = hash == region->hash;
    }
    cache->region = region; cache->epoch = c->code_epoch; cache->version = version;
    return cache->valid;
}

/* The epoch hit is the common case; keep it free of the slow path's frame. */
static int code_valid_full(cpu_t *c, const code_region_t *region) {
    code_cache_t *cache;
    if (!region) return 0;
    cache = &c->code_cache[((uintptr_t)region >> 4) % CODE_CACHE_SIZE];
    if (cache->region == region && cache->epoch == c->code_epoch) return cache->valid;
    return code_valid_slow(c, region, cache);
}

int code_valid(cpu_t *c, const code_region_t *region) {
    /* Loop checkpoints and timed memory accesses normally ask about the same
     * region. Keep that answer beside the CPU state instead of touching the
     * large, hashed source cache at every checkpoint. */
    if (!region) return 0;
    if (c->recent_region == region && c->recent_epoch == c->code_epoch)
        return c->recent_valid;
    c->recent_valid = code_valid_full(c, region);
    c->recent_region = region; c->recent_epoch = c->code_epoch;
    return c->recent_valid;
}

static uint32_t rol(uint32_t n, unsigned shift) { return (n << shift) | (n >> (32 - shift)); }

void code_sha1(const uint8_t *data, size_t length, uint8_t digest[20]) {
    uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    uint8_t padding[128] = {0};
    size_t full = length / 64, tail = length % 64, padded = tail < 56 ? 64 : 128;
    size_t blocks = full + padded / 64, block;
    uint64_t bits = (uint64_t)length * 8;
    unsigned i;
    /* Whole image/payload blocks need no per-byte padding or length checks. */
    if (tail) memcpy(padding, data + full * 64, tail);
    padding[tail] = 0x80;
    for (i = 0; i < 8; ++i) padding[padded - 1 - i] = (uint8_t)(bits >> (i * 8));
    for (block = 0; block < blocks; ++block) {
        uint32_t w[80], a, b, c, d, e;
        const uint8_t *input = block < full ? data + block * 64 : padding + (block - full) * 64;
        for (i = 0; i < 16; ++i) {
            const uint8_t *p = input + i * 4;
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (i = 16; i < 80; ++i) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4];
        /* Five rounds restore the register naming, avoiding state shuffles
         * and a phase-selection branch on every round. */
#define SHA_CH(B,C,D) ((D) ^ ((B) & ((C) ^ (D))))
#define SHA_XOR(B,C,D) ((B) ^ (C) ^ (D))
#define SHA_MAJ(B,C,D) (((B) & (C)) | ((D) & ((B) | (C))))
#define SHA_ROUND(A,B,C,D,E,F,K,W) do { (E) += rol((A),5) + (F) + (K) + (W); (B) = rol((B),30); } while (0)
#define SHA_FIVE(F,K) do { \
    SHA_ROUND(a,b,c,d,e,F(b,c,d),K,w[i]); \
    SHA_ROUND(e,a,b,c,d,F(a,b,c),K,w[i+1]); \
    SHA_ROUND(d,e,a,b,c,F(e,a,b),K,w[i+2]); \
    SHA_ROUND(c,d,e,a,b,F(d,e,a),K,w[i+3]); \
    SHA_ROUND(b,c,d,e,a,F(c,d,e),K,w[i+4]); \
} while (0)
        for (i = 0; i < 20; i += 5) SHA_FIVE(SHA_CH,0x5a827999u);
        for (; i < 40; i += 5) SHA_FIVE(SHA_XOR,0x6ed9eba1u);
        for (; i < 60; i += 5) SHA_FIVE(SHA_MAJ,0x8f1bbcdcu);
        for (; i < 80; i += 5) SHA_FIVE(SHA_XOR,0xca62c1d6u);
#undef SHA_FIVE
#undef SHA_ROUND
#undef SHA_MAJ
#undef SHA_XOR
#undef SHA_CH
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    for (i = 0; i < 20; ++i) digest[i] = (uint8_t)(h[i/4] >> (24 - (i%4)*8));
}

static const entry_t *find_entry(const entry_t *entries, size_t count, uint32_t pc) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (entries[mid].pc < pc) lo = mid + 1;
        else hi = mid;
    }
    return lo < count && entries[lo].pc == pc ? &entries[lo] : NULL;
}

static const source_bounds_t *catalog_bounds(cpu_t *c, unsigned source) {
    if (!c->source_bounds_ready[source]) {
        const overlay_t *o = source ? &c->overlays[source - 1] : NULL;
        c->source_bounds[source] = source_bounds(o ? o->regions : c->entry_regions,
                                                 o ? o->count : c->entry_count);
        c->source_bounds_ready[source] = 1;
    }
    return &c->source_bounds[source];
}

static recompiled_fn lookup_source(cpu_t *c, uint32_t pc, int *registered, dispatch_builder_t *index) {
    const entry_t *entry;
    size_t i;
    entry = find_entry(c->entries, c->entry_count, pc);
    if (entry && registered) *registered = 1;
    if (entry && (c->entry_regions ? dispatch_check(c, index, entry, c->entry_regions[entry - c->entries]) : !c->load_reg)) return entry->fn;
    for (i = 0; i < c->overlay_count && i < CODE_OVERLAYS; ++i) {
        const overlay_t *o = &c->overlays[i];
        if (pc < o->base || pc - o->base >= o->size) continue;
        entry = find_entry(o->entries, o->count, pc);
        if (entry && registered) *registered = 1;
        if (entry && dispatch_check(c, index, entry, o->regions[entry - o->entries])) return entry->fn;
        if (precise_host()) {
            recompiled_fn fn = precise_source(c, o->entries, o->count, o->regions,
                                             c->entry_regions ? catalog_bounds(c, (unsigned)i + 1) : NULL, pc, registered, index);
            if (fn) return fn;
        }
    }
    if (precise_host()) {
        recompiled_fn fn = precise_source(c, c->entries, c->entry_count, c->entry_regions,
                                         c->entry_regions ? catalog_bounds(c, 0) : NULL, pc, registered, index);
        if (fn) return fn;
    }
    if (index) index->complete = 1;
    return NULL;
}

static void dispatch_reset(cpu_t *c) {
    memset(c->dispatch_cache, 0, sizeof(c->dispatch_cache));
    memset(c->source_bounds_ready, 0, sizeof(c->source_bounds_ready));
    c->dispatch_candidate_count = 0; c->dispatch_generation = 1;
    c->dispatch_entries = c->entries; c->dispatch_overlays = c->overlays;
    c->dispatch_entry_regions = c->entry_regions;
    c->dispatch_entry_count = c->entry_count; c->dispatch_overlay_count = c->overlay_count;
}

/* Registry owners call this before replacing an in-place catalog. Pointer and
 * count changes are also recognized by lookup, including removal of all sets. */
void dispatch_invalidate(cpu_t *c) {
    dispatch_reset(c);
    /* An unloaded catalog's storage may be reused for different fingerprints. */
    memset(c->code_cache, 0, sizeof(c->code_cache));
    c->recent_region = NULL;
}

#define DISPATCH_COMPLETE (1u << 31)

static void dispatch_recycle(cpu_t *c) {
    c->dispatch_candidate_count = 0;
    if (!++c->dispatch_generation) {
        unsigned i;
        for (i = 0; i < CODE_CACHE_SIZE; ++i) c->dispatch_cache[i].generation = 0;
        c->dispatch_generation = 1;
    }
}

static unsigned dispatch_dependencies(dispatch_cache_t *slot, const code_region_t *r) {
    size_t i;
    unsigned source = 0, single = 1;
    if (!r) return 0;
    for (i = 0; i < r->count; ++i) {
        const code_span_t *s = &r->spans[i];
        uint32_t first = (s->address & (RAM_SIZE - 1)) >> CODE_PAGE_SHIFT;
        uint32_t last = ((s->address & (RAM_SIZE - 1)) + s->size - 1) >> CODE_PAGE_SHIFT;
        if (!s->size) continue;
        if (first != last || (source && source != first + 1)) single = 0;
        if (!source) source = first + 1;
        for (; first <= last; ++first) slot->pages[first / 64] |= UINT64_C(1) << (first % 64);
    }
    return single ? source : 0;
}

static uint64_t dispatch_version(const cpu_t *c, const dispatch_cache_t *slot) {
    uint64_t version = 1;
    unsigned i;
    for (i = 0; i < CODE_PAGES / 64; ++i) {
        uint64_t bits = slot->pages[i];
        while (bits) {
            unsigned page = i * 64 + (unsigned)__builtin_ctzll(bits);
            if (c->code_pages[page] > version) version = c->code_pages[page];
            bits &= bits - 1;
        }
    }
    return version;
}

/* Record only the walk's consulted prefix. Its unvisited tail can be huge
 * even when the very first exact entry supplies the answer. */
static int dispatch_check(cpu_t *c, dispatch_builder_t *index, const entry_t *entry, const code_region_t *region) {
    if (index) {
        if (index->skip) { --index->skip; return 0; }
        if (index->recordable) {
            if (c->dispatch_candidate_count == DISPATCH_CANDIDATES) {
                unsigned count = c->dispatch_candidate_count - index->first;
                if (index->first) memmove(c->dispatch_candidates, c->dispatch_candidates + index->first,
                                          count * sizeof(*c->dispatch_candidates));
                dispatch_recycle(c); index->first = 0;
                if (count == DISPATCH_CANDIDATES) index->recordable = 0;
                else c->dispatch_candidate_count = count;
            }
            if (index->recordable) {
                dispatch_candidate_t *candidate = &c->dispatch_candidates[c->dispatch_candidate_count++];
                candidate->entry = entry; candidate->region = region;
                /* A NULL interior function ends only its own set's search.
                 * Such a walk need not be a prefix of all possible owners. */
                if (!entry->fn) index->recordable = 0;
            }
        }
    }
    {
        int valid = code_valid(c, region);
        if (index) {
            unsigned source = dispatch_dependencies(index->slot, region);
            /* Most owners occupy one page. Retain that immutable shortcut
             * beside the candidate instead of revisiting its spans. */
            if (index->recordable) c->dispatch_candidates[c->dispatch_candidate_count - 1].source = source;
        }
        return valid;
    }
}

static recompiled_fn dispatch_resolve(cpu_t *c, dispatch_cache_t *slot, int *found) {
    unsigned i, count = slot->count & ~DISPATCH_COMPLETE;
    for (i = 0; i < count; ++i) {
        const entry_t *entry = c->dispatch_candidates[slot->first + i].entry;
        const code_region_t *region = c->dispatch_candidates[slot->first + i].region;
        unsigned source = c->dispatch_candidates[slot->first + i].source;
        int valid = code_valid(c, region);
        /* Watching later candidates could fence deferred writes too early. */
        if (source) slot->pages[(source - 1) / 64] |= UINT64_C(1) << ((source - 1) % 64);
        else (void)dispatch_dependencies(slot, region);
        if (valid) { *found = 1; return entry->fn; }
    }
    *found = (slot->count & DISPATCH_COMPLETE) != 0;
    return NULL;
}

recompiled_fn lookup(cpu_t *c, uint32_t pc) {
    dispatch_cache_t *slot;
    dispatch_builder_t index;
    recompiled_fn fn;
    uint64_t epoch;
    int registered = 0, found = 0;
    if (c->interpreter_only) return NULL;
    /* Native HLE entries still depend on the incoming delayed-load tag. */
    if (!c->entry_regions) return lookup_source(c, pc, NULL, NULL);
    if (c->dispatch_entries != c->entries || c->dispatch_overlays != c->overlays ||
        c->dispatch_entry_regions != c->entry_regions ||
        c->dispatch_entry_count != c->entry_count || c->dispatch_overlay_count != c->overlay_count) {
        dispatch_reset(c);
    }
    slot = &c->dispatch_cache[(pc >> 2) % CODE_CACHE_SIZE];
    if (slot->pc != pc) {
        dispatch_cache_t *other = &c->dispatch_cache[((pc >> 2) ^ (pc >> 14)) % CODE_CACHE_SIZE];
        uint64_t age = slot->epoch == UINT64_MAX ? slot->version : slot->epoch;
        uint64_t other_age = other->epoch == UINT64_MAX ? other->version : other->epoch;
        /* Keep the common direct hit cheap, but let colliding hot PCs retain
         * their catalog prefixes. The write epoch supplies a coarse age. */
        if (other->pc == pc || other_age < age) slot = other;
    }
    if ((slot->epoch == c->code_epoch || slot->epoch == UINT64_MAX) && slot->pc == pc) {
        if (slot->epoch == UINT64_MAX) slot->version = c->code_epoch;
        return slot->fn;
    }
    /* An epoch miss must still complete deferred writes before trusting RAM. */
    if (slot->pc == pc && slot->epoch && c->ram_top == 0x800000u &&
        slot->epoch < c->code_epoch && slot->version <= slot->epoch &&
        slot->version == dispatch_version(c, slot)) {
        slot->epoch = c->code_epoch;
        return slot->fn;
    }
    epoch = c->code_epoch;
    memset(slot->pages, 0, sizeof(slot->pages));
    index = (dispatch_builder_t){slot, c->dispatch_candidate_count, 0, 0, 1};
    if (slot->pc == pc && slot->generation == c->dispatch_generation) {
        fn = dispatch_resolve(c, slot, &found);
        registered = (slot->count & ~DISPATCH_COMPLETE) != 0;
        if (found) goto resolved;
        index.skip = slot->count & ~DISPATCH_COMPLETE;
        /* Resume after the already rejected prefix without validating it twice.
         * Move its records if another PC has appended since it was built. */
        if (DISPATCH_CANDIDATES - c->dispatch_candidate_count < index.skip) {
            memmove(c->dispatch_candidates, c->dispatch_candidates + slot->first,
                    index.skip * sizeof(*c->dispatch_candidates));
            dispatch_recycle(c); index.first = 0;
        } else {
            memmove(c->dispatch_candidates + index.first, c->dispatch_candidates + slot->first,
                    index.skip * sizeof(*c->dispatch_candidates));
        }
        c->dispatch_candidate_count = index.first + index.skip;
    }
    fn = lookup_source(c, pc, &registered, &index);
    if (index.recordable) {
        slot->first = index.first;
        slot->count = c->dispatch_candidate_count - index.first;
        if (index.complete) slot->count |= DISPATCH_COMPLETE;
        slot->generation = c->dispatch_generation;
    } else {
        c->dispatch_candidate_count = index.first;
        slot->generation = 0;
    }
resolved:
    /* A later source's device fence can change an earlier rejected source.
     * Preserve this walk's answer for its epoch, but reconsider it next time
     * the epoch moves even if no further dependency page has changed. */
    slot->version = !registered ? c->code_epoch :
        epoch == c->code_epoch ? dispatch_version(c, slot) : UINT64_MAX;
    slot->pc = pc; slot->epoch = registered ? c->code_epoch : UINT64_MAX; slot->fn = fn;
    return fn;
}

static trampoline_t *current_trampoline(cpu_t *c) {
    unsigned i = c->dispatch_depth;
    while (i && !c->trampolines[i].active) --i;
    return i ? &c->trampolines[i] : NULL;
}

void c_unwind(cpu_t *c) {
    trampoline_t *t = current_trampoline(c);
    if (!t) return;
    c_release_overlays(c, t->native_pins);
    {
        unsigned i;
        for (i = (unsigned)(t - c->trampolines) + 1; i <= c->dispatch_depth; ++i)
            c->trampolines[i].active = 0;
    }
    ++c->nonlocal_unwinds;
    longjmp(t->jump, 1);
}

int c_enter(cpu_t *c, uint32_t target, uint32_t ret) {
    trampoline_t *t = current_trampoline(c);
    /* A callback first installs its own dispatch frame. Never bounce across
     * the BIOS frame that is about to call dispatch. */
    if (ret != C_CALLBACK_RETURN && t && c->depth - t->depth >= C_STACK_SLICE)
        c_unwind(c);
    if (c->depth >= STACK_DEPTH) {
        cpu_stop(c, "guest callback nesting limit"); return 0;
    }
    c->stack[c->depth].pc = target;
    c->stack[c->depth++].ret = ret;
    return 1;
}

void c_return(cpu_t *c) {
    if (c->depth && c->pc != c->stack[c->depth - 1].ret) c_unwind(c);
}

void c_entry(cpu_t *c, uint32_t owner) {
    trampoline_t *t = current_trampoline(c);
    unsigned i;
    if (!t || !c->depth || c->stack[c->depth - 1].pc == owner) return;
    for (i = t->depth ? t->depth - 1 : 0; i + 1 < c->depth; ++i)
        if (c->stack[i].pc == owner) c_unwind(c);
}

static void dispatch_loop(cpu_t *c, uint32_t ret) {
    int missing = 0;
    while (!c->halted && c->pc != ret) {
#if defined(FF8_SAVESTATES)
        if (savestate_pending == c && savestate_ready(c)) {
            if (c->dispatch_depth != 1) c_unwind(c);
            if (savestate_boundary(c)) return;
        }
#endif
        uint32_t p = c->pc & 0x1fffffffu;
        recompiled_fn fn;
        if (!c->branch_pending && (p == 0xa0 || p == 0xb0 || p == 0xc0)) {
            unsigned nonlocal = p == 0xa0 && c->r[9] == 0x14;
            bios_call(c, (p - 0xa0) >> 4);
            if (!c->halted) c->pc = c->r[31];
            if (nonlocal) c_unwind(c);
            continue;
        }
        /* Acquire even when lookup has cached a permanent miss. Dynamic
         * module pointers never enter the ordinary dispatch cache. */
        fn = !c->branch_pending && !c->interpreter_only ? native_overlay_acquire(c, c->pc) : NULL;
        if (fn) {
            unsigned keep = c->native_pin_count;
            uint64_t before = c->recomp_instructions;
            if (keep == STACK_DEPTH + 1) {
                native_overlay_release(fn);
                cpu_stop(c, "native overlay nesting limit"); break;
            }
            c->native_pins[c->native_pin_count++] = fn;
            ++c->native_acquired; ++c->native_executed;
            missing = 0; fn(c);
            c->native_instructions += c->recomp_instructions - before;
            c_release_overlays(c, keep);
            continue;
        }
        fn = c->branch_pending ? NULL : lookup(c, c->pc);
        if (c->branch_pending && precise_host() && c->entry_regions) fn = lookup(c, c->pc);
        if (fn) {
            uint64_t before = c->cycles;
            missing = 0; fn(c);
            if (precise_host() && c->cycles != before) goto retired;
            continue;
        }
        if (!missing) {
            unsigned region = p >= 0x1e2800 && p < RAM_SIZE ? 2 :
                              p >= 0x98000 && p < 0x1e2800 ? 1 :
                              p >= 0x10000 && p < 0x98000 ? 0 : 3;
            ++c->dispatch_misses; ++c->dispatch_regions[region];
            missing = 1; cpu_log(c, "MISS pc=%08x", c->pc);
        }
        interp_step(c);
retired:
        if (c->flow && c->flow_kind == 2 && !c->halted) c_return(c);
        if (c->flow && c->flow_kind == 1 && !c->halted) {
            uint32_t back = c->flow_return;
            if (!c_enter(c, c->pc, back)) break;
            dispatch(c, c->pc);
            c_leave(c);
            if (c->pc != back) break;
        }
    }
}

static void dispatch_run(cpu_t *c, uint32_t target) {
    unsigned level = ++c->dispatch_depth;
    uint32_t ret = c->depth ? c->stack[c->depth - 1].ret : 0xffffffffu;
    trampoline_t *t;
    if (level > STACK_DEPTH) {
        --c->dispatch_depth; c_unwind(c); return;
    }
    t = &c->trampolines[level];
    t->active = level == 1 || ret == C_CALLBACK_RETURN;
    c->pc = target;
    if (t->active) {
        t->depth = c->depth; t->ret = ret; t->native_pins = c->native_pin_count;
        if (setjmp(t->jump)) {
            /* Only guest state survives an unwind; discard abandoned C-call
             * bookkeeping, preserving the callback's sentinel boundary. */
            c->depth = t->depth;
            c->dispatch_depth = level;
        }
        dispatch_loop(c, t->ret);
    } else {
        if (level >= C_STACK_SLICE) c_unwind(c);
        dispatch_loop(c, ret);
    }
    t->active = 0;
    --c->dispatch_depth;
}

void dispatch(cpu_t *c, uint32_t target) {
    if (c->stop_armed) { dispatch_run(c, target); return; }
    /* Compatibility with callers that have not installed a host stop frame. */
    c->stop_armed = 1;
    if (!setjmp(c->stop)) dispatch_run(c, target);
    c->stop_armed = 0;
}

static void compiled_memory_time(cpu_t *c, uint32_t offset) {
    c_count_recompiled(c, c->pc, offset);
    c_recomp_budget(c, offset);
    c->cycles += (uint64_t)offset * CYCLES_PER_INSN;
}

static void compiled_memory_event(cpu_t *c) {
    if ((int64_t)(c->cycles - c->next_event) >= 0) {
        uint32_t fault = c->fault_instructions;
        c->fault_instructions = 0;
        hw_event(c); c_tick_slow(c);
        c->fault_instructions = fault;
    }
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static int compiled_event_near(cpu_t *c, uint32_t pc, const code_region_t *region, unsigned gap) {
    if (region->event_windows) {
        size_t i, offset = 0;
        unsigned remaining = 0;
        for (i = 0; i < region->count; ++i) {
            const code_span_t *s = &region->spans[i];
            if (pc >= s->address && pc - s->address < s->size) {
                remaining = region->event_windows[offset + (pc - s->address) / 4];
                break;
            }
            offset += s->size / 4;
        }
        /* External transfers are about to dispatch to their own region. A
         * saturated byte retains the conservative full-function window. */
        if (remaining != 255) gap = remaining;
    }
    return gap && (int64_t)(c->cycles + (uint64_t)gap * CYCLES_PER_INSN - c->next_event) >= 0 &&
        hw_interrupt_window(c, gap);
}

/* The coarse region check, inline at every block: interrupts enabled and the
 * next event within the function's whole window. */
static inline int compiled_event_due(cpu_t *c, uint32_t pc, const code_region_t *region) {
    unsigned gap = region->event_gap;
    if ((c->cop0[12] & 0x401) != 0x401 || !gap ||
        (int64_t)(c->cycles + (uint64_t)gap * CYCLES_PER_INSN - c->next_event) < 0) return 0;
    return compiled_event_near(c, pc, region, gap);
}

int c_block(cpu_t *c, uint32_t pc, uint32_t n, const code_region_t *region) {
    int valid;
    c->pc = pc;
    c_count_recompiled(c, region->spans[0].address, n);
    c_recomp_budget(c, n);
    c_tick(c, n);
    /* code_valid's own first check, without the call. */
    valid = c->recent_region == region && c->recent_epoch == c->code_epoch ? c->recent_valid : code_valid(c, region);
    if (compiled_event_due(c, pc, region)) {
        /* The interpreter owns instructions crossing an IRQ deadline. Loads
         * have already been exported by the caller; branch state is created
         * by interp_step and survives the return to dispatch. */
        if (valid) c_event_step(c, pc, 0);
        return 0;
    }
    return valid;
}

/* A recognized countdown/wait loop is pure between device deadlines. Batch
 * complete iterations only; leave the iteration crossing the next event to
 * ordinary compiled code, retaining exact IRQ state and guest cycle timing. */
void c_idle(cpu_t *c, const idle_site_t *s) {
    uint32_t address = c->r[s->base] + s->counter_offset, p = address & 0x1fffffffu;
    uint32_t clock = s->clock & 0x1fffffffu, counter, n, delta;
    uint64_t remaining;
    C_STATS(c_candidate(c, c->pc, C_CANDIDATE_IDLE));
    if ((address & 3) || p >= c->ram_limit || clock >= c->ram_limit ||
        (p & (RAM_SIZE - 1)) == (clock & (RAM_SIZE - 1)) ||
        c->code_watched[(p & (RAM_SIZE - 1)) >> CODE_PAGE_SHIFT] ||
        c->r[s->sentinel] != UINT32_MAX || c->load_reg ||
        (int32_t)ram_read(c, clock, 4) >= (int32_t)c->r[s->limit] ||
        (int64_t)(c->next_event - c->cycles) <= 0) return;
    remaining = c->next_event - c->cycles - 1;
    delta = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    n = delta / (14 * CYCLES_PER_INSN);
    counter = ram_read(c, p, 4);
    if (n > counter) n = counter;
    if (c->instruction_limit) {
        uint64_t count = c_instructions(c);
        uint64_t budget = count < c->instruction_limit ? c->instruction_limit - count - 1 : 0;
        if (budget < (uint64_t)n * 14) n = (uint32_t)budget / 14;
    }
    if (!n) return;
    ram_write(c, p, 4, counter - n);
    c->r[s->value] = 1;
    C_STATS(c->recomp_instructions += n * 14);
    C_STATS(c->idle_instructions += n * 14);
    {
        unsigned i;
        for (i = 0; i < 8; ++i) c_pc_count(c, c->pc + i * 4, n);
        for (i = 0; i < 6; ++i) c_pc_count(c, s->clock_pc + i * 4, n);
    }
    c_recomp_budget(c, n * 14);
    c->cycles += (uint64_t)n * (14 * CYCLES_PER_INSN);
}

void c_poll_skip(cpu_t *c, const poll_site_t *s) {
    uint32_t p = s->address & 0x1fffffffu, value, n, delta;
    uint64_t remaining;
    unsigned i;
    C_STATS(c_candidate(c, s->head, C_CANDIDATE_POLL));
    if (!c->ram_limit || c->load_reg || c->branch_pending ||
        c->r[31] != s->head + 8 || (int64_t)(c->next_event - c->cycles) <= 0) return;
    if (c_scratch_fast(c, p, 4)) value = scratch_read(c, p, 4);
    else if (p < c->ram_limit && c->ram_limit - p >= 4) value = ram_read(c, p, 4);
    else return;
    if (!value || c->r[s->result] != value) return;
    /* Both code ranges are RAM, and neither can intersect deferred DMA. */
    for (i = 0; i < 8; ++i) {
        uint32_t pc = (i < 4 ? s->head + i * 4 : s->getter + (i - 4) * 4) & 0x1fffffffu;
        if (pc >= c->ram_limit || c->ram_limit - pc < 4 || ram_read(c, pc, 4) != s->words[i]) return;
    }
    remaining = c->next_event - c->cycles - 1;
    delta = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    n = delta / (8 * CYCLES_PER_INSN);
    if (c->instruction_limit) {
        uint64_t count = c_instructions(c);
        uint64_t budget = count < c->instruction_limit ? c->instruction_limit - count - 1 : 0;
        if (budget < (uint64_t)n * 8) n = (uint32_t)budget / 8;
    }
    if (!n) return;
    c_count_recompiled(c, s->head, n * 4);
    c_count_recompiled(c, s->getter, n * 4);
    C_STATS(c->idle_instructions += n * 8);
    c_recomp_budget(c, n * 8);
    for (i = 0; i < 4; ++i) {
        c_pc_count(c, s->head + i * 4, n);
        c_pc_count(c, s->getter + i * 4, n);
    }
    c->cycles += (uint64_t)n * (8 * CYCLES_PER_INSN);
}

void c_event_step(cpu_t *c, uint32_t pc, uint32_t prefix) {
    uint64_t deadline;
    c->pc = pc;
    compiled_memory_time(c, prefix);
    deadline = c->next_event;
    c_tick(c, 0);
    /* External transfers still belong to dispatch: BIOS vectors are HLE
     * services, and callback sentinels are not guest instructions. */
    do {
        uint32_t p = c->pc & 0x1fffffffu;
        if ((c->pc & 3) || p >= 0x800000u ||
            (!c->branch_pending && (p == 0xa0 || p == 0xb0 || p == 0xc0))) return;
        interp_step(c);
        /* Leave the delay slot and call/return bookkeeping to dispatch. */
    } while (!c->halted && !c->branch_pending && !c->flow &&
             c->next_event == deadline && (int64_t)(c->cycles - deadline) < 0);
}

#if defined(__GNUC__)
__attribute__((cold, noinline))
#endif
int c_tick_window_slow(cpu_t *c, uint32_t window) {
    c_tick(c, 0);
    if (window && (c->cop0[12] & 0x401) == 0x401 &&
        (int64_t)(c->cycles + (uint64_t)window * CYCLES_PER_INSN - c->next_event) >= 0) {
        c_event_step(c, c->pc, 0);
        return 0;
    }
    return 1;
}

void c_retire_shared(cpu_t *c, unsigned dst, uint32_t value, unsigned load, uint32_t loaded) {
    c_retire(c, dst, value, load, loaded);
}

/* Compact native translations share memory machinery, never guest instruction
 * decoding. The generator has already selected the operation and registers. */
static inline __attribute__((always_inline)) uint32_t memory_access(cpu_t *c, const memory_site_t *s, uint32_t delayed, uint32_t ticked, unsigned op) {
    uint32_t address = c->r[s->rs] + s->imm, value = c->r[s->rt], loaded = 0;
    uint32_t aligned = op == 34 || op == 38 || op == 42 || op == 46 ? address & ~3u : address;
    unsigned width = op == 32 || op == 36 || op == 40 ? 1 :
                     op == 33 || op == 37 || op == 41 ? 2 : 4;
    uint32_t physical = aligned & 0x1fffffffu;
    unsigned scratch = physical >= c->ram_limit && c_scratch_fast(c, physical, width);
    unsigned slow = (physical >= c->ram_limit && !scratch) || (aligned & (width - 1));
    unsigned load = op < 40 ? s->rt : 0;
    unsigned gap = (c->cop0[12] & 0x401) == 0x401 ? s->region->event_gap : 0;
    unsigned timed = slow || (int64_t)(c->cycles +
        ((uint64_t)(s->offset + 1 - ticked) + gap) * CYCLES_PER_INSN - c->next_event) >= 0;
#ifdef FF8_STATS
    {
        uint32_t p = aligned & 0x1fffffffu;
        ++c->perf.memory[p < c->ram_limit ? C_MEM_RAM :
            p - 0x1f800000u < 1024 ? C_MEM_SCRATCH :
            p >= 0x1f800400u && p < 0x1f810000u ? C_MEM_MMIO : C_MEM_OTHER];
        if (!slow && timed) ++c->perf.memory[C_MEM_GAP];
        if (p < 0x800000u && p >= c->ram_top) ++c->perf.memory[C_MEM_FENCE];
        if (op >= 40 && op != 50 && p < 0x800000u &&
            c->code_watched[(p & (RAM_SIZE - 1)) >> CODE_PAGE_SHIFT]) ++c->perf.memory[C_MEM_WATCHED];
    }
#endif
    uint64_t epoch = c->code_epoch;
    /* The common aligned RAM word needs neither fault context nor a dynamic
     * load-delay tag. Capture a store's operand before retiring its predecessor.
     * Watched writes retain the invalidation/unwind path below. */
    if (!timed && s->pending >= 0 && (op == 35 ||
        (op == 43 && (scratch || !c->code_watched[(address & (RAM_SIZE - 1)) >> CODE_PAGE_SHIFT])))) {
        if (op == 35) loaded = scratch ? scratch_read(c, physical, 4) : ram_read(c, address, 4);
        else if (scratch) scratch_write(c, physical, 4, value);
        else ram_write(c, address, 4, value);
        if (s->pending && (unsigned)s->pending != load) c->r[s->pending] = delayed;
        if (!s->defer && load) c->r[load] = loaded;
        c->load_reg = c->load_value = 0;
        c->memory_ticked = ticked; c->memory_exit = 0;
        return loaded;
    }
    if (s->pending >= 0) { c->load_reg = (unsigned)s->pending; c->load_value = delayed; }
    if (timed) {
        c->pc = s->pc; c->branch_pc = s->branch; c->in_delay_slot = !!s->branch;
        c->fault_instructions = 1; c->fault_counted = 0;
        compiled_memory_time(c, s->offset - ticked); ticked = s->offset;
        compiled_memory_event(c);
    }
    if (op == 58) value = gte_read(c, s->rt, 0);
    switch (op) {
    case 32: loaded = (uint32_t)(int32_t)(int8_t)mem_read8(c, address); break;
    case 33: loaded = (uint32_t)(int32_t)(int16_t)mem_read16(c, address); break;
    case 34: loaded = c_lwl(c, address, c_merge_value(c, s->rt)); break;
    case 35: loaded = mem_read32(c, address); break;
    case 36: loaded = mem_read8(c, address); break;
    case 37: loaded = mem_read16(c, address); break;
    case 38: loaded = c_lwr(c, address, c_merge_value(c, s->rt)); break;
    case 40: mem_write8(c, address, value); break;
    case 41: mem_write16(c, address, value); break;
    case 42: c_swl(c, address, value); break;
    case 43: mem_write32(c, address, value); break;
    case 46: c_swr(c, address, value); break;
    case 50: gte_write(c, s->rt, mem_read32(c, address), 0); break;
    case 58: mem_write32(c, address, value); break;
    }
    c_retire(c, 0, 0, load, loaded);
    c->fault_instructions = c->in_delay_slot = 0;
    if (!s->defer && load) { c->r[load] = loaded; c->load_reg = c->load_value = 0; }
    if (timed || epoch != c->code_epoch) {
        c->pc = s->branch ? c->branch_target : s->pc + 4;
        c_count_recompiled(c, s->region->spans[0].address, s->offset + 1 - ticked);
        c_recomp_budget(c, s->offset + 1 - ticked);
        c_tick(c, s->offset + 1 - ticked);
        ticked = s->offset + 1;
        if (epoch != c->code_epoch && !code_valid(c, s->region)) c_unwind(c);
    }
    c->memory_ticked = ticked;
    c->memory_exit = compiled_event_due(c, s->branch ? c->branch_target : s->pc + 4, s->region);
    /* A timed access can schedule an earlier event. Leave its retired load
     * in architectural state when returning to dispatch at the next PC. */
    if (!c->memory_exit) c->load_reg = c->load_value = 0;
    return loaded;
}

uint32_t c_memory(cpu_t *c, const memory_site_t *s, uint32_t delayed, uint32_t ticked) {
    return memory_access(c, s, delayed, ticked, s->op);
}
uint32_t c_memory_lw(cpu_t *c, const memory_site_t *s, uint32_t delayed, uint32_t ticked) {
    return memory_access(c, s, delayed, ticked, 35);
}
uint32_t c_memory_sw(cpu_t *c, const memory_site_t *s, uint32_t delayed, uint32_t ticked) {
    return memory_access(c, s, delayed, ticked, 43);
}

uint32_t mem_compiled_read(cpu_t *c, uint32_t address, unsigned width, uint32_t offset) {
    compiled_memory_time(c, offset);
    compiled_memory_event(c);
    return mem_slow_read(c, address, width);
}

void mem_compiled_write(cpu_t *c, uint32_t address, unsigned width, uint32_t value, uint32_t offset) {
    compiled_memory_time(c, offset);
    compiled_memory_event(c);
    mem_slow_write(c, address, width, value);
}

uint32_t mem_slow_read(cpu_t *c, uint32_t address, unsigned width) {
    uint32_t p = address & 0x1fffffffu;
    if (address & (width - 1)) { cpu_exception(c, 4, address); return 0; }
    if (p < 0x800000u) {
        if (p >= c->ram_top) hw_ram_fence(c, p, width);
        return ram_read(c, p, width);
    }
    if (p >= 0x1f800000 && p + width <= 0x1f800400) {
        return scratch_read(c, p, width);
    }
    if (p >= 0x1f800400 && p < 0x1f810000) return hw_read(c, p, width);
    cpu_log(c, "UNMAPPED read address=%08x width=%u value=00000000 pc=%08x", address, width, c->pc);
    return 0;
}

void mem_slow_write(cpu_t *c, uint32_t address, unsigned width, uint32_t value) {
    uint32_t p = address & 0x1fffffffu;
    if (address & (width - 1)) { cpu_exception(c, 5, address); return; }
    if (p < 0x800000u) {
        if (p >= c->ram_top) hw_ram_fence(c, p, width);
        if (!(c->cop0[12] & 0x10000u)) ram_write(c, p, width, value);
        return;
    }
    if (p >= 0x1f800000 && p + width <= 0x1f800400) {
        scratch_write(c, p, width, value);
        return;
    }
    if (p >= 0x1f800400 && p < 0x1f810000) { hw_write(c, p, width, value); return; }
    cpu_log(c, "UNMAPPED write address=%08x width=%u value=%08x pc=%08x", address, width, value, c->pc);
}
