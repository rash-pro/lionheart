/* The runtime services that recompiled code reaches through the module boundary.
 * Everything else a body needs is inlined from cpu.h and never crosses it, so
 * the list stays short; the order here is part of the module ABI. Every GTE
 * command the generator can name is listed, not only those today's discs use. */
#ifndef FF8_NATIVE_MODULE_API_H
#define FF8_NATIVE_MODULE_API_H

#define LH_MODULE_VOID_CALLS(X) \
    X(bios_syscall, (cpu_t *c), (c)) \
    X(c_candidate, (cpu_t *c, uint32_t pc, unsigned kind), (c, pc, kind)) \
    X(c_profile_count, (cpu_t *c, uint32_t pc, uint32_t n, int compiled), (c, pc, n, compiled)) \
    X(code_invalidate, (cpu_t *c, uint32_t address, size_t length), (c, address, length)) \
    X(cpu_exception, (cpu_t *c, unsigned code, uint32_t bad_address), (c, code, bad_address)) \
    X(c_tick_slow, (cpu_t *c), (c)) \
    X(gte_avsz3, (cpu_t *c), (c)) \
    X(gte_avsz4, (cpu_t *c), (c)) \
    X(gte_cc, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_cdp, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_cmd, (cpu_t *c, uint32_t opcode), (c, opcode)) \
    X(gte_dcpl, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_dpcs, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_dpct, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_gpf, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_gpl, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_intpl, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_mvmva, (cpu_t *c, unsigned shift, int lm, unsigned mx, unsigned vs, unsigned cv), (c, shift, lm, mx, vs, cv)) \
    X(gte_nccs, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_ncct, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_ncds, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_ncdt, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_nclip, (cpu_t *c), (c)) \
    X(gte_ncs, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_nct, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_op, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_rtps, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_rtpt, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_sqr, (cpu_t *c, unsigned shift, int lm), (c, shift, lm)) \
    X(gte_write, (cpu_t *c, unsigned reg, uint32_t value, int control), (c, reg, value, control)) \
    X(hw_event, (cpu_t *c), (c)) \
    X(hw_sr_changed, (cpu_t *c), (c)) \
    X(interp_step, (cpu_t *c), (c)) \
    X(mem_slow_write, (cpu_t *c, uint32_t address, unsigned width, uint32_t value), (c, address, width, value))

#define LH_MODULE_VALUE_CALLS(X) \
    X(int, code_valid, (cpu_t *c, const code_region_t *region), (c, region)) \
    X(uint32_t, gte_read, (cpu_t *c, unsigned reg, int control), (c, reg, control)) \
    X(uint32_t, mem_slow_read, (cpu_t *c, uint32_t address, unsigned width), (c, address, width))

typedef struct lh_module_api {
#define LHM_VOID_FIELD(name, params, args) void (*name) params;
#define LHM_VALUE_FIELD(type, name, params, args) type (*name) params;
    LH_MODULE_VOID_CALLS(LHM_VOID_FIELD)
    LH_MODULE_VALUE_CALLS(LHM_VALUE_FIELD)
#undef LHM_VOID_FIELD
#undef LHM_VALUE_FIELD
    /* Variadic, so no forwarder can stand in for it; see the rename below. */
    void (*cpu_stop)(cpu_t *c, const char *format, ...);
} lh_module_api;

#ifdef FF8_NATIVE_MODULE
/* The table lives inside each module, one load away from every call site,
 * and the host fills it in once while binding. */
#if defined(__GNUC__) && !defined(_WIN32)
__attribute__((visibility("hidden")))
#endif
extern lh_module_api lh_api;

/* Forwarders rather than macros: the later cpu.h and gte.h prototypes then
 * redeclare these internal functions and need no changes of their own. */
#define LHM_VOID_FORWARD(name, params, args) \
    static inline __attribute__((always_inline)) void lhm_##name params { lh_api.name args; }
#define LHM_VALUE_FORWARD(type, name, params, args) \
    static inline __attribute__((always_inline)) type lhm_##name params { return lh_api.name args; }
LH_MODULE_VOID_CALLS(LHM_VOID_FORWARD)
LH_MODULE_VALUE_CALLS(LHM_VALUE_FORWARD)
#undef LHM_VOID_FORWARD
#undef LHM_VALUE_FORWARD

#define bios_syscall lhm_bios_syscall
#define c_candidate lhm_c_candidate
#define c_profile_count lhm_c_profile_count
#define code_invalidate lhm_code_invalidate
#define cpu_exception lhm_cpu_exception
#define c_tick_slow lhm_c_tick_slow
#define gte_avsz3 lhm_gte_avsz3
#define gte_avsz4 lhm_gte_avsz4
#define gte_cc lhm_gte_cc
#define gte_cdp lhm_gte_cdp
#define gte_cmd lhm_gte_cmd
#define gte_dcpl lhm_gte_dcpl
#define gte_dpcs lhm_gte_dpcs
#define gte_dpct lhm_gte_dpct
#define gte_gpf lhm_gte_gpf
#define gte_gpl lhm_gte_gpl
#define gte_intpl lhm_gte_intpl
#define gte_mvmva lhm_gte_mvmva
#define gte_nccs lhm_gte_nccs
#define gte_ncct lhm_gte_ncct
#define gte_ncds lhm_gte_ncds
#define gte_ncdt lhm_gte_ncdt
#define gte_nclip lhm_gte_nclip
#define gte_ncs lhm_gte_ncs
#define gte_nct lhm_gte_nct
#define gte_op lhm_gte_op
#define gte_rtps lhm_gte_rtps
#define gte_rtpt lhm_gte_rtpt
#define gte_sqr lhm_gte_sqr
#define gte_write lhm_gte_write
#define hw_event lhm_hw_event
#define hw_sr_changed lhm_hw_sr_changed
#define interp_step lhm_interp_step
#define mem_slow_write lhm_mem_slow_write
#define code_valid lhm_code_valid
#define gte_read lhm_gte_read
#define mem_slow_read lhm_mem_slow_read
/* cpu.h withholds this one's prototype from module builds. */
#define cpu_stop lh_api.cpu_stop
#endif

#endif
