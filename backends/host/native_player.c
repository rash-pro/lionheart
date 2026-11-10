#include "native_player.h"
#include "native_modules.h"
#include "native_module.h"
#include "player_catalog.h"
#include "native_setup.h"
#include "lionheart_generator.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define NATIVE_TARGET "x86_64-windows-gnu"
#else
/* The oldest glibc the release player supports; modules use none of it. */
#define NATIVE_TARGET "x86_64-linux-gnu.2.17"
#endif

#ifdef _WIN32
/* A DLL needs an entry point and modules link no C runtime to supply one. */
static const char windows_entry[] =
    "int __stdcall DllMainCRTStartup(void *module, unsigned long reason, void *reserved);\n"
    "int __stdcall DllMainCRTStartup(void *module, unsigned long reason, void *reserved) {\n"
    "    (void)module; (void)reason; (void)reserved;\n    return 1;\n}\n";
#endif

static struct {
    int booted, running, service, verbose;
    unsigned workers;
    char root[4096], compiler[4096], cache_id[33], exe_id[33];
    char discs[4][4096];
    unsigned disc_mask;
    lhg_catalog_file catalog[8];
    native_build_file_t files[8];
    size_t catalog_count, file_count;
    lhg_session *session;
} np;

static const char *const driver[] = {"cc"};

static lhg_options generator_options(void) {
    lhg_options options;
    memset(&options, 0, sizeof(options));
    options.catalog_files = np.catalog;
    options.catalog_file_count = np.catalog_count;
    options.shape = LHG_SHAPE_MODULES;
    options.mode = LHG_MODE_FAST;
    options.host_mode = LHG_HOST_PRECISE;
    options.timing = LHG_TIMING_EXACT;
    return options;
}

static int copy_rows(const lhg_set_info *sets, size_t count, native_build_set_t **rows) {
    size_t i;
    if (!(*rows = calloc(count ? count : 1, sizeof(**rows)))) return 0;
    for (i = 0; i < count; ++i) {
        native_build_set_t *row = &(*rows)[i];
        row->index = sets[i].index;
        row->discs = sets[i].discs;
        row->effect = sets[i].effect;
        row->base = (uint32_t)sets[i].base;
        row->size = (uint32_t)sets[i].size;
        row->work = (uint32_t)sets[i].instructions;
        memcpy(row->name, sets[i].name, sizeof(row->name));
        memcpy(row->identity, sets[i].identity, sizeof(row->identity));
    }
    return 1;
}

static int source_plan(void *user, unsigned discs, native_build_set_t **rows, size_t *count, char *error, size_t size) {
    lhg_options options = generator_options();
    lhg_set_info *sets;
    int ok;
    (void)user;
    if (np.session) {
        const lhg_set_info *held;
        *count = lhg_session_sets(np.session, &held);
        return copy_rows(held, *count, rows);
    }
    if (lhg_plan_sets(&options, discs, &sets, count, error, size) != LHG_OK) return 0;
    ok = copy_rows(sets, *count, rows);
    free(sets);
    return ok;
}

static int source_open(void *user, char *error, size_t size) {
    lhg_options options = generator_options();
    unsigned d;
    (void)user;
    for (d = 0; d < 4; ++d) if (*np.discs[d]) options.disc_paths[options.disc_count++] = np.discs[d];
    return lhg_session_open(&options, &np.session, error, size) == LHG_OK;
}

static int source_emit(void *user, size_t set, const char *directory, char key[65], char *error, size_t size) {
    (void)user;
    return lhg_session_emit(np.session, set, directory, key, error, size) == LHG_OK;
}

static int source_contains(void *user, size_t set, uint32_t pc, const uint8_t *bytes, size_t size) {
    (void)user;
    return lhg_session_contains(np.session, set, pc, bytes, size);
}

static void source_close(void *user) {
    (void)user;
    lhg_session_close(np.session);
    np.session = NULL;
}

static int deliver(void *user, const char *path, int32_t index, char *error, size_t size) {
    native_module_t *module;
    (void)user;
    if (native_module_open(path, &module, error, size) != NATIVE_MODULE_OK) return 0;
    native_module_publish_as(module, index);
    return 1;
}

/* Enough of the code at pc to tell sets that share a load address apart. */
static void ran(cpu_t *c, uint32_t pc) {
    uint32_t physical = pc & 0x1fffffffu;
    if (physical >= 0x800000u) return;
    physical &= RAM_SIZE - 1;
    native_build_ran(0x80000000u | physical, c->ram + physical, RAM_SIZE - physical < 64 ? RAM_SIZE - physical : 64);
}

static void hex_digest(lhg_sha256 *hash, char out[33]) {
    char full[65];
    lhg_sha256_finish(hash, full);
    memcpy(out, full, 32);
    out[32] = 0;
}

static void add_line(lhg_sha256 *hash, const char *text) {
    lhg_sha256_add(hash, text, strlen(text));
    lhg_sha256_add(hash, "\n", 1);
}

int host_native_boot(cpu_t *c) {
    lhg_sha256 hash;
    if (np.booted) return 1;
    lhg_sha256_init(&hash);
    lhg_sha256_add(&hash, c->ram + (generated_base & 0x1fffffffu), generated_size);
    hex_digest(&hash, np.exe_id);
    /* The text is intact only now; the loader keeps its own copy, so building
     * can start whenever the compiler arrives. */
    if (!native_modules_configure(generated_hash, generated_base, generated_size, c->ram + (generated_base & 0x1fffffffu))) return 0;
    np.booted = 1;
    return 1;
}

static int set_discs(const char *const discs[4]) {
    unsigned d;
    np.disc_mask = 0;
    for (d = 0; d < 4; ++d) {
        np.discs[d][0] = 0;
        if (!discs[d]) continue;
        if (strlen(discs[d]) >= sizeof(np.discs[d])) return 0;
        strcpy(np.discs[d], discs[d]);
        np.disc_mask |= 1u << (d + 1);
    }
    return np.disc_mask != 0;
}

static int start_service(unsigned inserted) {
    static const native_build_source_t source = {NULL, source_plan, source_open, source_emit, source_contains, source_close};
    native_build_config_t config;
    char error[256];
    memset(&config, 0, sizeof(config));
    config.root = np.root; config.cache_id = np.cache_id; config.exe_id = np.exe_id;
    config.compiler = np.compiler; config.driver = driver; config.driver_count = 1;
    config.target = NATIVE_TARGET;
#ifdef _WIN32
    config.windows = 1;
#endif
    config.abi_id = LH_MODULE_ABI_ID;
    config.files = np.files; config.file_count = np.file_count;
    config.discs = np.disc_mask; config.disc = inserted;
    config.workers = np.workers;
    config.source = &source;
    config.deliver = deliver;
    config.verbose = np.verbose;
    if (!native_build_start(&config, error, sizeof(error))) {
        fprintf(stderr, "native: %s\n", error);
        return 0;
    }
    np.service = 1;
    return 1;
}

int host_native_start(const char *data, const char *const discs[4], unsigned inserted, const host_native_options_t *options) {
    const host_tc_manifest *manifest = native_setup_manifest();
    lhg_sha256 hash;
    size_t i;
    if (np.running || !np.booted || !data || !*data || !native_setup_compiler(np.compiler, sizeof(np.compiler)))
        return 0;
    if (options && options->compiler) {
        if (strlen(options->compiler) >= sizeof(np.compiler)) return 0;
        strcpy(np.compiler, options->compiler);
    }
    if (snprintf(np.root, sizeof(np.root), "%s/native", data) >= (int)sizeof(np.root)) return 0;
    if (!set_discs(discs)) return 0;
    np.catalog_count = host_native_catalog_count;
    for (i = 0; i < np.catalog_count; ++i) {
        np.catalog[i].name = host_native_catalog[i].name;
        np.catalog[i].data = host_native_catalog[i].data;
        np.catalog[i].size = host_native_catalog[i].size;
    }
    np.file_count = 0;
    for (i = 0; i < host_native_headers_count; ++i) {
        np.files[np.file_count].name = host_native_headers[i].name;
        np.files[np.file_count].data = host_native_headers[i].data;
        np.files[np.file_count++].size = host_native_headers[i].size;
    }
#ifdef _WIN32
    np.files[np.file_count].name = "entry.c";
    np.files[np.file_count].data = windows_entry;
    np.files[np.file_count++].size = sizeof(windows_entry) - 1;
#endif
    /* Everything that shapes module code names the cache folder: a change to
     * any of it starts a new folder, and the old one goes once unused. */
    lhg_sha256_init(&hash);
    add_line(&hash, "lionheart-native-cache 1");
    add_line(&hash, LH_MODULE_ABI_ID);
    add_line(&hash, lhg_generator_fingerprint());
    add_line(&hash, host_native_catalog_digest);
    add_line(&hash, manifest->version);
    add_line(&hash, manifest->sha256);
    add_line(&hash, NATIVE_TARGET);
    add_line(&hash, NATIVE_BUILD_RECIPE);
    hex_digest(&hash, np.cache_id);

    np.workers = options ? options->workers : 0;
    np.verbose = options && options->verbose;
    native_modules_take_over(1);
    if (!start_service(inserted)) {
        native_modules_take_over(0);
        return 0;
    }
    dispatch_miss = ran;
    np.running = 1;
    return 1;
}

void host_native_disc(unsigned disc) {
    if (np.service) native_build_disc(disc);
}

void host_native_discs(const char *const discs[4], unsigned inserted) {
    unsigned d, same = 1;
    if (!np.running) return;
    for (d = 0; d < 4; ++d) same = same && !strcmp(np.discs[d], discs[d] ? discs[d] : "");
    if (same) return;
    /* The plan covers the discs it started with. Building again lists the
     * new ones; what is loaded stays loaded, and the cache hands over the
     * rest at once. */
    if (np.service) native_build_stop();
    np.service = 0;
    if (set_discs(discs)) start_service(inserted);
}

int host_native_running(void) { return np.running; }

void host_native_stop(cpu_t *c, int wait) {
    native_build_status_t status;
    if (np.service) {
        if (wait) native_build_wait(-1);
        native_build_status(&status);
        native_build_stop();
        np.service = 0;
        if (np.verbose)
            fprintf(stderr, "native: %u/%u sets native, %u failed, %u compiled, %u shared, %u from cache; cache %.3f s, "
                    "first tier %.1f s (%.1f cpu s), done %.1f s (%.1f cpu s); %zu modules registered\n",
                    status.ready, status.sets, status.failed, status.compiled, status.shared, status.cached, status.cached_seconds,
                    status.first_tier_seconds, status.first_tier_cpu, status.done_seconds, status.done_cpu, native_modules_registered());
    }
    dispatch_miss = NULL;
    if (np.booted) native_modules_shutdown(c);
    native_modules_take_over(0);
    np.running = np.booted = 0;
}
