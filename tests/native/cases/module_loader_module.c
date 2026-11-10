/* Synthetic modules for the loader's refusal cases. One source, one variant per
 * build: each define spoils exactly one thing a real generated module carries.
 * The image is the 64-byte executable module_loader_host.c boots, never game code. */
#include "native_module.h"

lh_module_api lh_api;

#define EXE_BASE 0x80010000u
#define EXE_SIZE 64u
/* FNV-1a 32 of the host's synthetic executable. */
#define EXE_FNV 0x35e25b05u
/* FNV-1a 64 of its first sixteen bytes, the main function's span. */
#define MAIN_HASH UINT64_C(0xabe96772d4e34c95)

#ifdef MODULE_MAIN
void f_80010000(cpu_t *c);
static const code_span_t main_spans[] = {{EXE_BASE, 16}};
#ifdef MODULE_MAIN_CODE_MISMATCH
static const code_region_t code_80010000 = {main_spans, 1, MAIN_HASH ^ 1, 0, NULL};
#else
static const code_region_t code_80010000 = {main_spans, 1, MAIN_HASH, 0, NULL};
#endif
void f_80010000(cpu_t *c) { c->r[2] = 42; }
static const entry_t module_entries[] = {{EXE_BASE, f_80010000}};
static const code_region_t *const module_regions[] = {&code_80010000};
#define SET_INDEX LH_MODULE_MAIN_SET
#define SET_BASE EXE_BASE
#define SET_SIZE EXE_SIZE
#define IMPORTS NULL, 0
#else
/* An overlay function that reaches the runtime through the table and main
 * through an import, the two ways a module leaves itself. */
#ifdef MODULE_OVERLAY_MISSING_IMPORT
#define IMPORTED_PC 0x80010020u
#else
#define IMPORTED_PC EXE_BASE
#endif
static recompiled_fn main_function;
static const lh_module_import module_imports[] = {{IMPORTED_PC, &main_function}};
static const code_span_t overlay_spans[] = {{0x80100000u, 8}};
static const code_region_t code_80100000 = {overlay_spans, 1, 0, 0, NULL};
static void f_80100000(cpu_t *c) {
    main_function(c);
    cpu_stop(c, "overlay %d", 7);
}
static const entry_t module_entries[] = {{0x80100000u, f_80100000}};
static const code_region_t *const module_regions[] = {&code_80100000};
#define SET_INDEX 5
#define SET_BASE 0x80100000u
#define SET_SIZE 0x100u
#define IMPORTS module_imports, 1
#endif

#ifdef MODULE_OVERLAY_OTHER_EXE
#define DESCRIPTOR_EXE_FNV (EXE_FNV ^ 0x100u)
#else
#define DESCRIPTOR_EXE_FNV EXE_FNV
#endif

#ifndef MODULE_OVERLAY_NO_DESCRIPTOR
LH_MODULE_EXPORT const lh_module_descriptor_t lh_module_descriptor = {
    LH_MODULE_MAGIC, LH_MODULE_ABI_VERSION, (uint32_t)sizeof(lh_module_descriptor_t), LH_MODULE_ABI_ID,
    LH_MODULE_TRAITS_INIT, SET_INDEX, SET_BASE, SET_SIZE, {0},
    DESCRIPTOR_EXE_FNV, EXE_BASE, EXE_SIZE,
    module_entries, 1, module_regions,
    IMPORTS, &lh_api
};
#else
/* Everything a module has, exported under any name but the descriptor's. */
LH_MODULE_EXPORT const void *const lh_module_unrelated[] = {module_entries, module_regions, module_imports};
#endif
