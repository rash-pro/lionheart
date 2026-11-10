#include "native_modules.h"
#include "native_module.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

struct native_module {
    native_module_t *next;
    void *handle;
    const lh_module_descriptor_t *descriptor;
    int registered;
    /* Host copies of the tables, made only when some entry was redirected to
     * an identical body that an earlier module already registered. */
    entry_t *entries;
    const code_region_t **regions;
};

/* A function body registered first, by its region's content. */
typedef struct {
    const code_region_t *region;
    recompiled_fn fn;
} twin_t;

static const lh_module_api host_api = {
#define LHM_VOID_ENTRY(name, params, args) .name = name,
#define LHM_VALUE_ENTRY(type, name, params, args) .name = name,
    LH_MODULE_VOID_CALLS(LHM_VOID_ENTRY)
    LH_MODULE_VALUE_CALLS(LHM_VALUE_ENTRY)
#undef LHM_VOID_ENTRY
#undef LHM_VALUE_ENTRY
    .cpu_stop = cpu_stop,
};

static const lh_module_traits host_traits = LH_MODULE_TRAITS_INIT;
static const char host_magic[8] = LH_MODULE_MAGIC;

static struct {
    int configured;
    uint32_t fnv, base, size;
    uint8_t *text;
    native_module_t *main_module;
    /* Sorted by set index, so that a late arrival leaves the walk order the
     * linked build uses once every module has been registered. */
    overlay_t overlays[CODE_OVERLAYS];
    native_module_t *modules[CODE_OVERLAYS];
    size_t count;
    native_module_t *pending;
    twin_t *twins;
    size_t twin_capacity, twin_count;
    char **scan;
    size_t scan_count, scan_next;
} state;

static native_module_status_t refuse(native_module_status_t status, char *error, size_t size, const char *format, ...) {
    va_list arguments;
    if (error && size) {
        va_start(arguments, format);
        vsnprintf(error, size, format, arguments);
        va_end(arguments);
    }
    return status;
}

int native_modules_configure(uint32_t fnv, uint32_t base, uint32_t size, const uint8_t *text) {
    uint32_t hash = 2166136261u, i;
    uint8_t *copy;
    /* Main modules are checked against this copy, so it must be the EXE the
     * generator saw, not whatever happens to be in RAM at the call. */
    for (i = 0; i < size; ++i) hash = (hash ^ text[i]) * 16777619u;
    if (hash != fnv || !(copy = malloc(size ? size : 1))) return 0;
    memcpy(copy, text, size);
    free(state.text);
    state.fnv = fnv; state.base = base; state.size = size; state.text = copy;
    state.configured = 1;
    return 1;
}

static uint32_t read_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t read_u32(const uint8_t *p) { return read_u16(p) | read_u16(p + 2) << 16; }
#ifndef _WIN32
static uint64_t read_u64(const uint8_t *p) { return (uint64_t)read_u32(p) | (uint64_t)read_u32(p + 4) << 32; }
#endif

/* Mapping a file whose contents end early succeeds and faults later, so its
 * declared extent is compared with the real length before anything maps it. */
static native_module_status_t check_complete(const char *path, char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    uint8_t header[64];
    uint64_t length, needed = 0;
    int bad = 0;
    if (!file) return refuse(NATIVE_MODULE_UNREADABLE, error, error_size, "cannot open file");
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) { fclose(file); return refuse(NATIVE_MODULE_UNREADABLE, error, error_size, "cannot measure file"); }
    length = (uint64_t)ftell(file);
    rewind(file);
    if (length < sizeof(header) || fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file); return refuse(NATIVE_MODULE_TRUNCATED, error, error_size, "shorter than a header");
    }
#ifdef _WIN32
    if (header[0] == 'M' && header[1] == 'Z') {
        uint32_t pe = read_u32(header + 60), sections, optional, i;
        uint8_t coff[24], row[40];
        if (fseek(file, (long)pe, SEEK_SET) || fread(coff, 1, sizeof(coff), file) != sizeof(coff) || memcmp(coff, "PE\0\0", 4)) bad = 1;
        else {
            sections = read_u16(coff + 6); optional = read_u16(coff + 20);
            for (i = 0; i < sections && !bad; ++i) {
                uint64_t offset = (uint64_t)pe + 24 + optional + (uint64_t)i * 40;
                if (offset + 40 > length || fseek(file, (long)offset, SEEK_SET) || fread(row, 1, sizeof(row), file) != sizeof(row)) bad = 1;
                else {
                    uint64_t end = (uint64_t)read_u32(row + 20) + read_u32(row + 16);
                    if (read_u32(row + 16) && end > needed) needed = end;
                }
            }
        }
    } else bad = 1;
#else
    if (!memcmp(header, "\177ELF", 4) && header[4] == 2 && header[5] == 1) {
        uint64_t phoff = read_u64(header + 32), shoff = read_u64(header + 40);
        uint32_t phentsize = read_u16(header + 54), phnum = read_u16(header + 56);
        uint32_t shentsize = read_u16(header + 58), shnum = read_u16(header + 60), i;
        uint8_t row[56];
        if (phentsize < sizeof(row) || phoff > length || phnum > (length - phoff) / phentsize) bad = 1;
        for (i = 0; i < phnum && !bad; ++i) {
            if (fseek(file, (long)(phoff + (uint64_t)i * phentsize), SEEK_SET) || fread(row, 1, sizeof(row), file) != sizeof(row)) bad = 1;
            else if (read_u32(row) == 1) { /* PT_LOAD */
                uint64_t offset = read_u64(row + 8), filesz = read_u64(row + 32);
                if (offset > UINT64_MAX - filesz) bad = 1;
                else if (offset + filesz > needed) needed = offset + filesz;
            }
        }
        if (!bad && shnum) {
            uint64_t end = shoff + (uint64_t)shnum * shentsize;
            if (end < shoff) bad = 1;
            else if (end > needed) needed = end;
        }
    } else bad = 1;
#endif
    fclose(file);
    if (bad) return refuse(NATIVE_MODULE_TRUNCATED, error, error_size, "malformed header");
    if (needed > length) return refuse(NATIVE_MODULE_TRUNCATED, error, error_size, "file ends before its contents do");
    return NATIVE_MODULE_OK;
}

#ifdef _WIN32
static void *library_open(const char *path, char *error, size_t size) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide = count > 0 ? malloc((size_t)count * sizeof(wchar_t)) : NULL;
    HMODULE handle = NULL;
    if (wide && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) handle = LoadLibraryW(wide);
    free(wide);
    if (!handle) snprintf(error, size, "system refused it (error %lu)", GetLastError());
    return handle;
}
static void *library_symbol(void *handle, const char *name) { return (void *)GetProcAddress((HMODULE)handle, name); }
static void library_close(void *handle) { FreeLibrary((HMODULE)handle); }
#else
static void *library_open(const char *path, char *error, size_t size) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) snprintf(error, size, "%s", dlerror());
    return handle;
}
static void *library_symbol(void *handle, const char *name) { return dlsym(handle, name); }
static void library_close(void *handle) { dlclose(handle); }
#endif

static uint64_t region_fingerprint(const code_region_t *region) {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i, j;
    for (i = 0; i < region->count; ++i) {
        const uint8_t *source = state.text + (region->spans[i].address - state.base);
        for (j = 0; j < region->spans[i].size; ++j) hash = (hash ^ source[j]) * UINT64_C(1099511628211);
    }
    return hash;
}

static int pc_in_ram(uint32_t pc, uint32_t size) {
    uint32_t physical = pc & 0x1fffffffu;
    return physical < RAM_SIZE && size <= RAM_SIZE - physical;
}

/* A loaded descriptor is only as good as its file, so its tables are checked
 * as data before the runtime is allowed to walk them. */
static native_module_status_t check_tables(const lh_module_descriptor_t *d, char *error, size_t size) {
    uint32_t i;
    if (d->entry_count && (!d->entries || !d->regions))
        return refuse(NATIVE_MODULE_BAD_TABLES, error, size, "entry count without tables");
    for (i = 0; i < d->entry_count; ++i) {
        const code_region_t *region = d->regions[i];
        size_t s;
        if (i && d->entries[i - 1].pc >= d->entries[i].pc)
            return refuse(NATIVE_MODULE_BAD_TABLES, error, size, "entries out of order at %08" PRIx32, d->entries[i].pc);
        if (!d->entries[i].fn || !region || !region->count || !region->spans)
            return refuse(NATIVE_MODULE_BAD_TABLES, error, size, "incomplete entry at %08" PRIx32, d->entries[i].pc);
        if (!pc_in_ram(d->entries[i].pc, 4))
            return refuse(NATIVE_MODULE_BAD_TABLES, error, size, "entry outside guest RAM: %08" PRIx32, d->entries[i].pc);
        for (s = 0; s < region->count; ++s)
            if (!region->spans[s].size || !pc_in_ram(region->spans[s].address, region->spans[s].size))
                return refuse(NATIVE_MODULE_BAD_TABLES, error, size, "region outside guest RAM at %08" PRIx32, d->entries[i].pc);
    }
    return NATIVE_MODULE_OK;
}

/* A module for the main executable can be checked against what actually booted.
 * Overlay images are in RAM only while the game has them loaded, so those rely
 * on the fingerprint checks every generated function makes when it is entered. */
static native_module_status_t check_main_code(const lh_module_descriptor_t *d, char *error, size_t size) {
    uint32_t i;
    for (i = 0; i < d->entry_count; ++i) {
        const code_region_t *region = d->regions[i];
        size_t s;
        if (i && d->regions[i - 1] == region) continue;
        for (s = 0; s < region->count; ++s) {
            uint32_t address = region->spans[s].address, length = region->spans[s].size;
            if (address < state.base || length > state.size || address - state.base > state.size - length)
                return refuse(NATIVE_MODULE_CODE_MISMATCH, error, size, "code at %08" PRIx32 " lies outside the executable", address);
        }
        if (region_fingerprint(region) != region->hash)
            return refuse(NATIVE_MODULE_CODE_MISMATCH, error, size, "code at %08" PRIx32 " does not match guest memory", d->entries[i].pc);
    }
    return NATIVE_MODULE_OK;
}

static const entry_t *main_entry(uint32_t pc) {
    /* Opening may run on a builder thread while the guest thread registers main. */
    native_module_t *main_module = __atomic_load_n(&state.main_module, __ATOMIC_ACQUIRE);
    const lh_module_descriptor_t *d = main_module ? main_module->descriptor : NULL;
    uint32_t lo = 0, hi = d ? d->entry_count : 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (d->entries[mid].pc < pc) lo = mid + 1;
        else hi = mid;
    }
    return d && lo < d->entry_count && d->entries[lo].pc == pc ? &d->entries[lo] : NULL;
}

native_module_status_t native_module_open(const char *path, native_module_t **module, char *error, size_t error_size) {
    native_module_status_t status;
    native_module_t *result;
    const lh_module_descriptor_t *d;
    void *handle;
    uint32_t i;
    char reason[256] = "";
    if (module) *module = NULL;
    if (!state.configured) return refuse(NATIVE_MODULE_NOT_CONFIGURED, error, error_size, "loader has no booted executable");
    if ((status = check_complete(path, reason, sizeof(reason))) != NATIVE_MODULE_OK)
        return refuse(status, error, error_size, "%s", reason);
    if (!(handle = library_open(path, reason, sizeof(reason))))
        return refuse(NATIVE_MODULE_LOAD_FAILED, error, error_size, "%s", reason);
    d = library_symbol(handle, LH_MODULE_SYMBOL);
    if (!d) { library_close(handle); return refuse(NATIVE_MODULE_NO_DESCRIPTOR, error, error_size, "no module descriptor"); }
    /* The version and size fields come first and never move, so they can be
     * read before knowing whether the rest of the descriptor has this layout. */
    if (memcmp(d->magic, host_magic, sizeof(host_magic)) || d->abi_version != LH_MODULE_ABI_VERSION ||
        d->descriptor_size != sizeof(lh_module_descriptor_t)) {
        library_close(handle); return refuse(NATIVE_MODULE_BAD_ABI, error, error_size, "descriptor from another ABI version");
    }
    if (!strcmp(d->abi_id, "unconfigured") || memcmp(d->abi_id, LH_MODULE_ABI_ID, sizeof(LH_MODULE_ABI_ID)) ||
        memcmp(&d->traits, &host_traits, sizeof(host_traits))) {
        library_close(handle); return refuse(NATIVE_MODULE_BAD_ABI, error, error_size, "built against different headers or build settings");
    }
    if (d->exe_fnv != state.fnv || d->exe_base != state.base || d->exe_size != state.size) {
        library_close(handle); return refuse(NATIVE_MODULE_BAD_IMAGE, error, error_size, "generated from another executable");
    }
    if (d->set_index < LH_MODULE_MAIN_SET || (d->set_index == LH_MODULE_MAIN_SET && (d->base != state.base || d->size != state.size))) {
        library_close(handle); return refuse(NATIVE_MODULE_BAD_IMAGE, error, error_size, "bad set identity");
    }
    if ((status = check_tables(d, reason, sizeof(reason))) != NATIVE_MODULE_OK ||
        (d->set_index == LH_MODULE_MAIN_SET && (status = check_main_code(d, reason, sizeof(reason))) != NATIVE_MODULE_OK)) {
        library_close(handle); return refuse(status, error, error_size, "%s", reason);
    }
    if (d->import_count && !d->imports) {
        library_close(handle); return refuse(NATIVE_MODULE_BAD_TABLES, error, error_size, "import count without a table");
    }
    for (i = 0; i < d->import_count; ++i) {
        const entry_t *target = main_entry(d->imports[i].pc);
        if (!target || !d->imports[i].slot) {
            uint32_t pc = d->imports[i].pc;
            library_close(handle);
            return refuse(NATIVE_MODULE_MISSING_IMPORT, error, error_size, "main-owned function %08" PRIx32 " is not available", pc);
        }
        *d->imports[i].slot = target->fn;
    }
    if (!d->api) { library_close(handle); return refuse(NATIVE_MODULE_BAD_TABLES, error, error_size, "no call table"); }
    *d->api = host_api;
    if (!(result = calloc(1, sizeof(*result)))) {
        library_close(handle); return refuse(NATIVE_MODULE_LOAD_FAILED, error, error_size, "out of memory");
    }
    result->handle = handle; result->descriptor = d;
    if (module) *module = result;
    else native_module_close(result);
    return NATIVE_MODULE_OK;
}

void native_module_close(native_module_t *module) {
    if (!module || module->registered) return;
    library_close(module->handle);
    free(module->entries);
    free(module->regions);
    free(module);
}

static uint64_t region_key(const code_region_t *region) {
    uint64_t key = region->hash ^ (uint64_t)region->count * UINT64_C(0x9e3779b97f4a7c15);
    size_t i;
    for (i = 0; i < region->count; ++i)
        key = (key ^ ((uint64_t)region->spans[i].address << 32 | region->spans[i].size)) * UINT64_C(0x100000001b3);
    return key ^ key >> 31;
}

static int same_region(const code_region_t *a, const code_region_t *b) {
    size_t i;
    if (a->hash != b->hash || a->count != b->count) return 0;
    for (i = 0; i < a->count; ++i)
        if (a->spans[i].address != b->spans[i].address || a->spans[i].size != b->spans[i].size) return 0;
    return 1;
}

static int grow_twins(void) {
    size_t capacity = state.twin_capacity ? state.twin_capacity * 2 : 4096, i;
    twin_t *grown = calloc(capacity, sizeof(*grown)), *old = state.twins;
    if (!grown) return 0;
    for (i = 0; i < state.twin_capacity; ++i) if (old[i].region) {
        size_t at = (size_t)region_key(old[i].region) & (capacity - 1);
        while (grown[at].region) at = (at + 1) & (capacity - 1);
        grown[at] = old[i];
    }
    free(old);
    state.twins = grown; state.twin_capacity = capacity;
    return 1;
}

/* The body to run for a region: the one registered first with the same spans
 * and fingerprint. Sets that share a function then share one body and one
 * validity cache entry, as the linked shape's single owner does. A precise
 * body is a function of its instructions alone, so the twin behaves the same.
 * Without memory for the table, every module simply keeps its own bodies. */
static const twin_t *canonical(const code_region_t *region, recompiled_fn fn) {
    size_t at;
    if (state.twin_count * 2 >= state.twin_capacity && !grow_twins()) return NULL;
    at = (size_t)region_key(region) & (state.twin_capacity - 1);
    while (state.twins[at].region) {
        if (state.twins[at].region == region || same_region(state.twins[at].region, region)) return &state.twins[at];
        at = (at + 1) & (state.twin_capacity - 1);
    }
    state.twins[at].region = region; state.twins[at].fn = fn;
    ++state.twin_count;
    return &state.twins[at];
}

/* Tables as the dispatcher will walk them: the module's own unless some
 * entry was redirected, then a host copy. */
static void share_bodies(native_module_t *module, const entry_t **entries, const code_region_t *const **regions) {
    const lh_module_descriptor_t *d = module->descriptor;
    uint32_t i;
    *entries = d->entries; *regions = d->regions;
    for (i = 0; i < d->entry_count; ++i) {
        const twin_t *twin = canonical(d->regions[i], d->entries[i].fn);
        if (!twin || twin->region == d->regions[i]) continue;
        if (!module->entries) {
            module->entries = malloc(d->entry_count * sizeof(*module->entries));
            module->regions = malloc(d->entry_count * sizeof(*module->regions));
            if (!module->entries || !module->regions) {
                free(module->entries); free(module->regions); module->entries = NULL; module->regions = NULL;
                return;
            }
            memcpy(module->entries, d->entries, d->entry_count * sizeof(*module->entries));
            memcpy(module->regions, d->regions, d->entry_count * sizeof(*module->regions));
        }
        module->entries[i].fn = twin->fn;
        module->regions[i] = twin->region;
    }
    if (module->entries) { *entries = module->entries; *regions = module->regions; }
}

static void commit_at_boundary(cpu_t *c) { native_modules_commit(c); }

void native_module_publish(native_module_t *module) {
    native_module_t *head = __atomic_load_n(&state.pending, __ATOMIC_RELAXED);
    do module->next = head;
    while (!__atomic_compare_exchange_n(&state.pending, &head, module, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    __atomic_store_n(&dispatch_safe_point, commit_at_boundary, __ATOMIC_RELEASE);
}

/* First registration wins. An index can only have one live module; a second
 * is the arrival race a later builder must resolve by not opening it twice. */
static void register_module(cpu_t *c, native_module_t *module) {
    const lh_module_descriptor_t *d = module->descriptor;
    const entry_t *entries;
    const code_region_t *const *regions;
    int already = 0;
    size_t i;
    if (d->set_index == LH_MODULE_MAIN_SET) {
        if (state.main_module) { native_module_close(module); return; }
        module->registered = 1;
        __atomic_store_n(&state.main_module, module, __ATOMIC_RELEASE);
        share_bodies(module, &entries, &regions);
        c->entries = entries; c->entry_count = d->entry_count; c->entry_regions = regions;
        return;
    }
    if (state.count == CODE_OVERLAYS) { native_module_close(module); return; }
    for (i = 0; i < state.count; ++i) {
        if (state.overlays[i].index == (uint32_t)d->set_index) { already = 1; break; }
        if (state.overlays[i].index > (uint32_t)d->set_index) break;
    }
    if (already) { native_module_close(module); return; }
    share_bodies(module, &entries, &regions);
    memmove(&state.overlays[i + 1], &state.overlays[i], (state.count - i) * sizeof(*state.overlays));
    memmove(&state.modules[i + 1], &state.modules[i], (state.count - i) * sizeof(*state.modules));
    state.overlays[i] = (overlay_t){(uint32_t)d->set_index, d->base, d->size, {0}, entries, d->entry_count, regions};
    memcpy(state.overlays[i].sha1, d->image_sha1, sizeof(d->image_sha1));
    state.modules[i] = module;
    state.count++;
    module->registered = 1;
}

void native_modules_commit(cpu_t *c) {
    native_module_t *module, *ordered = NULL;
    __atomic_store_n(&dispatch_safe_point, NULL, __ATOMIC_RELEASE);
    module = __atomic_exchange_n(&state.pending, NULL, __ATOMIC_ACQUIRE);
    if (!module) return;
    /* Publication order, so that among identical bodies the first published
     * one is shared, as the lowest set owns a shared body in the linked shape. */
    while (module) { native_module_t *next = module->next; module->next = ordered; ordered = module; module = next; }
    module = ordered;
    while (module) {
        native_module_t *next = module->next;
        register_module(c, module);
        module = next;
    }
    c->overlays = state.overlays;
    c->overlay_count = state.count;
    dispatch_invalidate(c);
}

size_t native_modules_registered(void) { return state.count + (state.main_module != NULL); }

void native_modules_shutdown(cpu_t *c) {
    size_t i;
    native_module_t *module = __atomic_exchange_n(&state.pending, NULL, __ATOMIC_ACQUIRE);
    if (c) {
        c->entries = NULL; c->entry_count = 0; c->entry_regions = NULL;
        c->overlays = NULL; c->overlay_count = 0;
        dispatch_invalidate(c);
    }
    __atomic_store_n(&dispatch_safe_point, NULL, __ATOMIC_RELEASE);
    while (module) { native_module_t *next = module->next; native_module_close(module); module = next; }
    for (i = 0; i < state.count; ++i) { state.modules[i]->registered = 0; native_module_close(state.modules[i]); }
    if (state.main_module) { state.main_module->registered = 0; native_module_close(state.main_module); }
    for (i = 0; i < state.scan_count; ++i) free(state.scan[i]);
    free(state.scan);
    free(state.twins);
    free(state.text);
    memset(&state, 0, sizeof(state));
}

static long module_order(const char *name) {
    const char *base = strrchr(name, '/');
#ifdef _WIN32
    const char *back = strrchr(name, '\\');
    if (back && (!base || back > base)) base = back;
#endif
    base = base ? base + 1 : name;
    return !strncmp(base, "set-", 4) ? strtol(base + 4, NULL, 10) : -1;
}

static int compare_module_names(const void *a, const void *b) {
    long x = module_order(*(char *const *)a), y = module_order(*(char *const *)b);
    return x < y ? -1 : x > y;
}

static int add_scan_name(const char *directory, const char *name) {
    size_t length = strlen(directory) + strlen(name) + 2;
    char **grown = realloc(state.scan, (state.scan_count + 1) * sizeof(*state.scan));
    char *path = malloc(length);
    if (!grown || !path) { free(path); if (grown) state.scan = grown; return 0; }
    state.scan = grown;
    snprintf(path, length, "%s/%s", directory, name);
    state.scan[state.scan_count++] = path;
    return 1;
}

#ifdef _WIN32
static const char module_suffix[] = ".dll";
#else
static const char module_suffix[] = ".so";
#endif

static int is_module_name(const char *name) {
    size_t length = strlen(name), suffix = sizeof(module_suffix) - 1;
    return length > suffix && !strcmp(name + length - suffix, module_suffix);
}

int native_modules_scan(const char *directory) {
    int ok = 1;
#ifdef _WIN32
    char pattern[4096];
    WIN32_FIND_DATAA found;
    HANDLE find;
    snprintf(pattern, sizeof(pattern), "%s/*%s", directory, module_suffix);
    find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do ok = is_module_name(found.cFileName) ? add_scan_name(directory, found.cFileName) && ok : ok;
    while (FindNextFileA(find, &found));
    FindClose(find);
#else
    DIR *listing = opendir(directory);
    struct dirent *row;
    if (!listing) return 0;
    while ((row = readdir(listing))) if (is_module_name(row->d_name)) ok = add_scan_name(directory, row->d_name) && ok;
    closedir(listing);
#endif
    if (state.scan_count) qsort(state.scan, state.scan_count, sizeof(*state.scan), compare_module_names);
    return ok;
}

size_t native_modules_feed(size_t count) {
    size_t opened = 0;
    while (count-- && state.scan_next < state.scan_count) {
        const char *path = state.scan[state.scan_next++];
        char reason[256];
        native_module_t *module;
        native_module_status_t status = native_module_open(path, &module, reason, sizeof(reason));
        if (status != NATIVE_MODULE_OK) { fprintf(stderr, "Native module %s refused: %s\n", path, reason); continue; }
        native_module_publish(module);
        ++opened;
    }
    return opened;
}

size_t native_modules_remaining(void) { return state.scan_count - state.scan_next; }
