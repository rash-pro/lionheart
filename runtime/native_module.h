/* The binary contract between the player and code recompiled on the player's
 * own machine. Module sources and the host loader both include this header.
 *
 * Identity has two halves, because each catches what the other cannot: the
 * ABI id is a hash of these headers and the build settings that shape code,
 * computed by cmake/native_module_abi.cmake; the traits below are what the
 * compiler actually laid out on each side. */
#ifndef FF8_NATIVE_MODULE_H
#define FF8_NATIVE_MODULE_H

#include "cpu.h"
#include "gte.h"
#include "native_module_api.h"

#ifndef LH_MODULE_ABI_ID
#define LH_MODULE_ABI_ID "unconfigured"
#endif

#ifdef FF8_NATIVE_MODULE
#if defined(FF8_COMPACT) || defined(FF8_PLAY)
#error "Native modules contain exact-timing precise code only"
#endif
#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility push(hidden)
#endif
#endif

/* Spelled out: the field holds exactly these eight bytes and no terminator. */
#define LH_MODULE_MAGIC {'L', 'H', 'M', 'O', 'D', 'U', 'L', 'E'}
#define LH_MODULE_ABI_VERSION 1u
#define LH_MODULE_ABI_ID_SIZE 65
#define LH_MODULE_SYMBOL "lh_module_descriptor"
/* The main executable's module; overlay sets use their catalog index. */
#define LH_MODULE_MAIN_SET (-1)

#define LH_MODULE_FLAG_STATS 1u
#define LH_MODULE_FLAG_WINDOWS 2u
#define LH_MODULE_FLAG_BIG_ENDIAN 4u
#ifdef FF8_STATS
#define LHM_FLAG_STATS LH_MODULE_FLAG_STATS
#else
#define LHM_FLAG_STATS 0u
#endif
#ifdef _WIN32
#define LHM_FLAG_WINDOWS LH_MODULE_FLAG_WINDOWS
#else
#define LHM_FLAG_WINDOWS 0u
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define LHM_FLAG_ENDIAN LH_MODULE_FLAG_BIG_ENDIAN
#else
#define LHM_FLAG_ENDIAN 0u
#endif

typedef struct {
    uint32_t flags, api_slots;
    uint32_t cpu_size, cpu_r, cpu_cycles, cpu_ram, cpu_code_epoch, cpu_recomp_instructions;
    uint32_t cpu_ram_limit, cpu_code_watched, cpu_load_reg, cpu_perf;
    uint32_t cpu_recent_region, cpu_recent_epoch, cpu_recent_valid;
    uint32_t entry_size, region_size, span_size, overlay_size;
} lh_module_traits;

#define LH_MODULE_TRAITS_INIT { \
    LHM_FLAG_STATS | LHM_FLAG_WINDOWS | LHM_FLAG_ENDIAN, \
    (uint32_t)(sizeof(lh_module_api) / sizeof(void (*)(void))), \
    (uint32_t)sizeof(cpu_t), (uint32_t)offsetof(cpu_t, r), (uint32_t)offsetof(cpu_t, cycles), \
    (uint32_t)offsetof(cpu_t, ram), (uint32_t)offsetof(cpu_t, code_epoch), \
    (uint32_t)offsetof(cpu_t, recomp_instructions), (uint32_t)offsetof(cpu_t, ram_limit), \
    (uint32_t)offsetof(cpu_t, code_watched), (uint32_t)offsetof(cpu_t, load_reg), (uint32_t)offsetof(cpu_t, perf), \
    (uint32_t)offsetof(cpu_t, recent_region), (uint32_t)offsetof(cpu_t, recent_epoch), (uint32_t)offsetof(cpu_t, recent_valid), \
    (uint32_t)sizeof(entry_t), (uint32_t)sizeof(code_region_t), (uint32_t)sizeof(code_span_t), (uint32_t)sizeof(overlay_t) }

/* A main-owned function a module calls directly; the host writes the slot
 * from the main module's entry table before any of the module's code runs. */
typedef struct {
    uint32_t pc;
    recompiled_fn *slot;
} lh_module_import;

typedef struct {
    char magic[8];
    uint32_t abi_version;
    uint32_t descriptor_size;
    char abi_id[LH_MODULE_ABI_ID_SIZE];
    lh_module_traits traits;
    int32_t set_index;
    uint32_t base, size;
    uint8_t image_sha1[20];
    /* Which EXE this code was generated against, on every module, so a set
     * from another edition of the game cannot be mistaken for this one. */
    uint32_t exe_fnv, exe_base, exe_size;
    const entry_t *entries;
    uint32_t entry_count;
    const code_region_t *const *regions;
    const lh_module_import *imports;
    uint32_t import_count;
    lh_module_api *api;
} lh_module_descriptor_t;

#ifdef FF8_NATIVE_MODULE
#ifdef _WIN32
#define LH_MODULE_EXPORT __declspec(dllexport)
#else
#define LH_MODULE_EXPORT __attribute__((visibility("default")))
#endif
#endif

#endif
