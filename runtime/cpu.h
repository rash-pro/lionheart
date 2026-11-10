#ifndef FF8_CPU_H
#define FF8_CPU_H

#include <setjmp.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "perf_stats.h"

#define CYCLES_PER_INSN 2u
#define RAM_SIZE 0x200000u
#define STACK_DEPTH 256
#define C_STACK_SLICE 48
#define C_CALLBACK_RETURN 0xfffffff0u
#define MMIO_STATS 512
#define POLL_SLOTS 4096
#define CODE_PAGE_SHIFT 12
#define CODE_PAGES (RAM_SIZE >> CODE_PAGE_SHIFT)
#define CODE_CACHE_SIZE 4096
#define CODE_OVERLAYS 512
#define DISPATCH_CANDIDATES 65536
#ifdef FF8_STATS
#define C_STATS(expression) (expression)
#else
#define C_STATS(expression) ((void)0)
#endif

typedef struct cpu cpu_t;
typedef void (*recompiled_fn)(cpu_t *);
typedef struct { uint32_t address, size; } code_span_t;
typedef struct {
    const code_span_t *spans;
    size_t count;
    uint64_t hash;
    unsigned event_gap;
    const uint8_t *event_windows; /* Span order; 255 uses the full event_gap. */
} code_region_t;
typedef struct { uint32_t pc; recompiled_fn fn; } entry_t;
typedef struct {
    uint32_t index, base, size;
    uint8_t sha1[20];
    const entry_t *entries;
    size_t count;
    const code_region_t *const *regions;
} overlay_t;
typedef struct {
    const code_region_t *region;
    uint64_t epoch, version;
    int valid;
} code_cache_t;
typedef struct {
    uint32_t pc;
    uint64_t epoch;
    recompiled_fn fn;
    uint64_t version, pages[CODE_PAGES / 64];
    unsigned first, count, generation;
} dispatch_cache_t;
typedef struct {
    const entry_t *entry;
    const code_region_t *region;
    unsigned source;
} dispatch_candidate_t;
typedef struct { uint32_t first; uint64_t end; } source_bounds_t;
typedef struct { uint32_t pc, ret; } frame_t;
typedef struct {
    jmp_buf jump;
    unsigned active, depth, native_pins;
    uint32_t ret;
} trampoline_t;
typedef struct {
    uint32_t address, last, last_read, last_write;
    uint64_t reads, writes;
    unsigned width;
} mmio_stat_t;
typedef struct { uint32_t pc; uint64_t epoch, hits; } poll_t;

struct cpu {
    struct hw *hw;
    uint32_t r[32], hi, lo, pc, cop0[32], gte_data[32], gte_ctrl[32];
    uint32_t load_reg, load_value;
    uint32_t branch_target, branch_pc, branch_return;
    unsigned branch_pending, branch_kind, in_delay_slot;
    unsigned flow, flow_kind;
    uint32_t flow_return;
    uint64_t cycles, next_event, last_tick_cycles;
    uint32_t last_tick_pc;
    unsigned tick_seen;
    uint32_t ram_limit, fault_instructions;
    unsigned fault_counted, stop_armed;
    jmp_buf stop;
    uint64_t code_epoch, code_pages[CODE_PAGES];
    const code_region_t *recent_region;
    uint64_t recent_epoch;
    int recent_valid;
    uint8_t code_watched[CODE_PAGES];
    uint64_t interp_instructions, recomp_instructions, recomp_functions, idle_instructions;
    uint64_t stall_limit, instruction_limit;
    uint64_t budget_recompiled;
    unsigned mmio_count, depth, dispatch_depth, halted, interpreter_only;
    uint32_t memory_ticked;
    unsigned memory_exit;
    uint8_t ram[RAM_SIZE] __attribute__((aligned(16)));
    uint8_t scratch[1024], mmio[65536];
    const entry_t *entries;
    size_t entry_count;
    /* NULL is a native HLE table; generated tables supply source fingerprints. */
    const code_region_t *const *entry_regions;
    const overlay_t *overlays;
    size_t overlay_count;
    uint64_t code_words[RAM_SIZE / 256];
    code_cache_t code_cache[CODE_CACHE_SIZE];
    uint64_t overlay_epoch[CODE_OVERLAYS], overlay_version[CODE_OVERLAYS];
    uint8_t overlay_valid[CODE_OVERLAYS];
    uint64_t dispatch_regions[4]; /* main, shared overlay, menu overlay, other */
    uint64_t dispatch_misses, mmio_epoch, bios_counts[4][256], gte_commands;
    uint64_t *interp_profile;
    struct overlay_profile *overlay_profile;
    poll_t polls[POLL_SLOTS];
    mmio_stat_t mmio_stats[MMIO_STATS];
    frame_t stack[STACK_DEPTH];
    /* Indexed by dispatch_depth: BIOS exception restoration also discards any
     * callback trampoline bypassed by ReturnFromException's own longjmp. */
    trampoline_t trampolines[STACK_DEPTH + 1];
    uint64_t nonlocal_unwinds;
    recompiled_fn native_pins[STACK_DEPTH + 1];
    unsigned native_pin_count;
    uint64_t native_acquired, native_released, native_executed, native_instructions;
    uint32_t seed, heap_next, heap_end, event_next, thread_next, pad_clear;
    char stop_reason[192];
    void (*log)(void *, const char *);
    void *log_context;
    /* Code writes change code_epoch; registry changes discard both hits and misses. */
    const entry_t *dispatch_entries;
    const overlay_t *dispatch_overlays;
    const code_region_t *const *dispatch_entry_regions;
    size_t dispatch_entry_count, dispatch_overlay_count;
    dispatch_cache_t dispatch_cache[CODE_CACHE_SIZE];
    dispatch_candidate_t dispatch_candidates[DISPATCH_CANDIDATES];
    unsigned dispatch_candidate_count, dispatch_generation;
    source_bounds_t source_bounds[CODE_OVERLAYS + 1];
    uint8_t source_bounds_ready[CODE_OVERLAYS + 1];
    /* RAM accesses at or above ram_top take the timed slow path, which keeps
     * them ordered with deferred device RAM traffic (see hw_reschedule). */
    uint32_t ram_top;
    perf_stats_t perf;
    uint64_t *pc_profile;
    uint64_t pc_profile_first, pc_profile_last;
    unsigned pc_profile_active;
};

/* code_valid's first answer: the region asked about last, unchanged since.
 * Module code takes it without crossing into the runtime. */
static inline int code_valid_recent(const cpu_t *c, const code_region_t *region) {
    return c->recent_region == region && c->recent_epoch == c->code_epoch;
}

/* Module builds reach the runtime through a table, not through symbols. This
 * has to precede every prototype and inline below so that they all see it. */
#ifdef FF8_NATIVE_MODULE
#include "native_module_api.h"
#endif

void cpu_init(cpu_t *c);
void dispatch_invalidate(cpu_t *c);
/* Runs once at the next dispatch boundary, the only point where the registry
 * may change. Publishers store it; the callback clears it. */
extern void (*dispatch_safe_point)(cpu_t *c);
/* Runs when the guest enters code that has no native body, once per entry
 * into interpreted code. A host building modules learns from it which pending
 * code the game needs; most hosts leave it NULL. */
extern void (*dispatch_miss)(cpu_t *c, uint32_t pc);
void cpu_log(cpu_t *c, const char *format, ...);
#ifndef FF8_NATIVE_MODULE
void cpu_stop(cpu_t *c, const char *format, ...);
#endif
void cpu_exception(cpu_t *c, unsigned code, uint32_t bad_address);
void c_poll(cpu_t *c);
void hw_event(cpu_t *c);
void hw_sr_changed(cpu_t *c);
void hw_ram_fence(cpu_t *c, uint32_t physical, uint32_t length);
void c_tick_slow(cpu_t *c);
void c_profile_count(cpu_t *c, uint32_t pc, uint32_t n, int compiled);
void c_profile_checkpoint(cpu_t *c, uint64_t vblank);
void c_perf_checkpoint(cpu_t *c, uint64_t vblank);
void c_perf_summary(const cpu_t *c, uint64_t vblank);
void c_candidate(cpu_t *c, uint32_t pc, unsigned kind);
static inline void c_pc_count(cpu_t *c, uint32_t pc, uint32_t n) {
#if defined(FF8_STATS)
    if (c->pc_profile_active) c->pc_profile[(pc & (RAM_SIZE - 1)) / 4] += n;
#else
    (void)c; (void)pc; (void)n;
#endif
}
static inline void c_transfer_stats(cpu_t *c, uint32_t pc, uint32_t target, unsigned kind) {
#ifdef FF8_STATS
    if (kind < C_DISPATCH_COUNT) ++c->perf.dispatch[kind];
    if (((pc ^ target) & 0x1ffff000u) != 0) ++c->perf.dispatch[C_DISPATCH_CROSS_PAGE];
    if (target <= pc) c_candidate(c, target, C_CANDIDATE_BACKEDGE);
#else
    (void)c; (void)pc; (void)target; (void)kind;
#endif
}
static inline void c_count_recompiled(cpu_t *c, uint32_t pc, uint32_t n) {
#ifdef FF8_STATS
    c->recomp_instructions += n;
    if (c->overlay_profile) c_profile_count(c, pc, n, 1);
#else
    (void)c; (void)pc; (void)n;
#endif
}

static inline void c_recomp_budget(cpu_t *c, uint32_t n) {
#ifndef FF8_STATS
    /* Only an explicitly active instruction budget needs accounting in the runtime. */
    if (c->instruction_limit) c->budget_recompiled += n;
#else
    (void)c; (void)n;
#endif
}

static inline uint64_t c_instructions(const cpu_t *c) {
#ifdef FF8_STATS
    return c->interp_instructions + c->recomp_instructions;
#else
    return c->interp_instructions + c->budget_recompiled;
#endif
}

static inline void c_tick(cpu_t *c, uint32_t n) {
    c->cycles += (uint64_t)n * CYCLES_PER_INSN;
    if ((int64_t)(c->cycles - c->next_event) >= 0) {
        hw_event(c);
        c_tick_slow(c);
    }
}

static inline void c_precise_begin(cpu_t *c, uint32_t pc) {
    c->in_delay_slot = c->branch_pending; c->flow = 0;
    c_count_recompiled(c, pc, 1); c_recomp_budget(c, 1);
    c_pc_count(c, pc, 1);
    c->fault_instructions = 1; c->fault_counted = 1;
}

static inline void c_set_sr(cpu_t *c, uint32_t value) {
    uint32_t changed = c->cop0[12] ^ value;
    c->cop0[12] = value;
    /* Fold cache isolation into the existing RAM range comparison. */
    c->ram_limit = value & 0x10000u ? 0 : c->ram_top;
    if (changed & 0x401) hw_sr_changed(c);
}
int c_enter(cpu_t *c, uint32_t target, uint32_t ret);
void c_unwind(cpu_t *c);
void c_return(cpu_t *c);
void c_entry(cpu_t *c, uint32_t owner);
/* Optional platform loader. Returned functions are callable only while pinned. */
recompiled_fn native_overlay_acquire(cpu_t *c, uint32_t pc);
void native_overlay_release(recompiled_fn fn);
void c_release_overlays(cpu_t *c, unsigned keep);

static inline void c_leave(cpu_t *c) { if (!c->halted && c->depth) --c->depth; }
recompiled_fn lookup(cpu_t *c, uint32_t pc);
void code_invalidate(cpu_t *c, uint32_t address, size_t length);
int code_valid(cpu_t *c, const code_region_t *region);
void code_watch(cpu_t *c, uint32_t address, uint32_t length);
void code_sha1(const uint8_t *data, size_t length, uint8_t digest[20]);
void dispatch(cpu_t *c, uint32_t target);
void interp_step(cpu_t *c);
void bios_call(cpu_t *c, unsigned table);
void bios_syscall(cpu_t *c);
void gte_cmd(cpu_t *c, uint32_t opcode);
uint32_t gte_read(cpu_t *c, unsigned reg, int control);
void gte_write(cpu_t *c, unsigned reg, uint32_t value, int control);
uint32_t hw_read(cpu_t *c, uint32_t address, unsigned width);
void hw_write(cpu_t *c, uint32_t address, unsigned width, uint32_t value);
uint32_t mem_slow_read(cpu_t *c, uint32_t address, unsigned width);
void mem_slow_write(cpu_t *c, uint32_t address, unsigned width, uint32_t value);
uint32_t mem_compiled_read(cpu_t *c, uint32_t address, unsigned width, uint32_t offset);
void mem_compiled_write(cpu_t *c, uint32_t address, unsigned width, uint32_t value, uint32_t offset);
typedef struct {
    uint32_t pc, branch, offset, imm;
    int pending;
    unsigned rs, rt, op, defer;
    const code_region_t *region;
} memory_site_t;
uint32_t c_memory(cpu_t *, const memory_site_t *, uint32_t, uint32_t);
uint32_t c_memory_lw(cpu_t *, const memory_site_t *, uint32_t, uint32_t);
uint32_t c_memory_sw(cpu_t *, const memory_site_t *, uint32_t, uint32_t);
int c_block(cpu_t *, uint32_t, uint32_t, const code_region_t *);
typedef struct { uint32_t counter_offset, clock; unsigned base, value, limit, sentinel; uint32_t clock_pc; } idle_site_t;
void c_idle(cpu_t *, const idle_site_t *);
void c_event_step(cpu_t *, uint32_t, uint32_t);
int hw_interrupt_window(cpu_t *, uint32_t);
int c_tick_window_slow(cpu_t *, uint32_t);
void c_retire_shared(cpu_t *, unsigned, uint32_t, unsigned, uint32_t);

/* Combine the usual polling comparison with the next block's IRQ window. */
static inline int c_tick_window(cpu_t *c, uint32_t n, uint32_t window) {
    c->cycles += (uint64_t)n * CYCLES_PER_INSN;
    if ((int64_t)(c->cycles + (uint64_t)window * CYCLES_PER_INSN - c->next_event) >= 0)
        return c_tick_window_slow(c, window);
    return 1;
}

typedef uint32_t ram_word_t __attribute__((may_alias));
typedef uint16_t ram_half_t __attribute__((may_alias));
/* Callers check guest alignment; RAM itself is aligned for direct access. */
static inline uint32_t ram_read(const cpu_t *c, uint32_t p, unsigned width) {
    uint32_t value = 0;
    p &= RAM_SIZE - 1;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (width == 4) value = ((const ram_word_t *)c->ram)[p >> 2];
    else if (width == 2) value = ((const ram_half_t *)c->ram)[p >> 1];
    else value = c->ram[p];
#else
    unsigned i;
    for (i = 0; i < width; ++i) value |= (uint32_t)c->ram[p + i] << (8 * i);
#endif
    return value;
}

static inline void ram_write(cpu_t *c, uint32_t p, unsigned width, uint32_t value) {
    p &= RAM_SIZE - 1;
    if (c->code_watched[p >> CODE_PAGE_SHIFT]) code_invalidate(c, p, width);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (width == 4) ((ram_word_t *)c->ram)[p >> 2] = value;
    else if (width == 2) ((ram_half_t *)c->ram)[p >> 1] = (uint16_t)value;
    else c->ram[p] = (uint8_t)value;
#else
    unsigned i;
    for (i = 0; i < width; ++i) c->ram[p + i] = (uint8_t)(value >> (8 * i));
#endif
}

#define MEMORY_ACCESSORS(bits, width) \
static inline uint32_t mem_read##bits(cpu_t *c, uint32_t address) { \
    uint32_t p = address & 0x1fffffffu; \
    if (p < c->ram_limit && !(address & (width - 1))) return ram_read(c, p, width); \
    return mem_slow_read(c, address, width); \
} \
static inline void mem_write##bits(cpu_t *c, uint32_t address, uint32_t value) { \
    uint32_t p = address & 0x1fffffffu; \
    if (p < c->ram_limit && !(address & (width - 1))) ram_write(c, p, width, value); \
    else mem_slow_write(c, address, width, value); \
}
MEMORY_ACCESSORS(8, 1)
MEMORY_ACCESSORS(16, 2)
MEMORY_ACCESSORS(32, 4)
#undef MEMORY_ACCESSORS

/* Operands are captured before retirement. A following write (including a
 * second load) cancels a pending load to the same register. LWL/LWR alone
 * forward that pending value when merging the two halves. */
static inline void c_retire(cpu_t *c, unsigned dst, uint32_t value,
                            unsigned load, uint32_t loaded) {
    if (c->load_reg && (c->halted || (c->load_reg != dst && c->load_reg != load)))
        c->r[c->load_reg] = c->load_value;
    if (!c->halted && dst) c->r[dst] = value;
    c->load_reg = c->halted ? 0 : load;
    c->load_value = load && !c->halted ? loaded : 0;
    c->r[0] = 0;
}

static inline uint32_t c_merge_value(cpu_t *c, unsigned reg) {
    return reg && c->load_reg == reg ? c->load_value : c->r[reg];
}

static inline uint32_t c_sra(uint32_t value, unsigned shift) {
    shift &= 31;
    return shift ? (value >> shift) | ((value & 0x80000000u) ? (~0u << (32 - shift)) : 0) : value;
}

static inline uint32_t c_add(cpu_t *c, uint32_t a, uint32_t b) {
    uint32_t v = a + b;
    if ((~(a ^ b) & (a ^ v)) >> 31) cpu_exception(c, 12, 0);
    return v;
}

static inline uint32_t c_sub(cpu_t *c, uint32_t a, uint32_t b) {
    uint32_t v = a - b;
    if (((a ^ b) & (a ^ v)) >> 31) cpu_exception(c, 12, 0);
    return v;
}

static inline void c_div(cpu_t *c, uint32_t a, uint32_t b, int sign) {
    if (!b) { c->lo = sign && (a >> 31) ? 1 : 0xffffffffu; c->hi = a; }
    else if (sign && a == 0x80000000u && b == 0xffffffffu) { c->lo = a; c->hi = 0; }
    else if (sign) { c->lo = (uint32_t)((int32_t)a / (int32_t)b); c->hi = (uint32_t)((int32_t)a % (int32_t)b); }
    else { c->lo = a / b; c->hi = a % b; }
}

static inline uint32_t c_lwl(cpu_t *c, uint32_t address, uint32_t old) {
    unsigned shift = (3 - (address & 3)) * 8;
    uint32_t mask = 0xffffffffu << shift;
    return (old & ~mask) | (mem_read32(c, address & ~3u) << shift);
}
static inline uint32_t c_lwr(cpu_t *c, uint32_t address, uint32_t old) {
    unsigned shift = (address & 3) * 8;
    uint32_t mask = 0xffffffffu >> shift;
    return (old & ~mask) | (mem_read32(c, address & ~3u) >> shift);
}
static inline void c_swl(cpu_t *c, uint32_t address, uint32_t value) {
    unsigned n = address & 3, i;
    for (i = 0; i <= n; ++i) mem_write8(c, (address & ~3u) + i, value >> ((3 - n + i) * 8));
}
static inline void c_swr(cpu_t *c, uint32_t address, uint32_t value) {
    unsigned n = address & 3, i;
    for (i = n; i < 4; ++i) mem_write8(c, (address & ~3u) + i, value >> ((i - n) * 8));
}

#if (defined(FF8_COMPACT) && !defined(FF8_CPU_IMPLEMENTATION))
#define c_retire c_retire_shared
#endif

#endif
