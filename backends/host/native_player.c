/* For nanosecond file times in struct stat, which strict C99 hides. */
#define _POSIX_C_SOURCE 200809L
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
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    char root[4096], cache_id[33], exe_id[33];
    native_setup_compiler_t compiler;
    char discs[4][4096];
    unsigned disc_mask;
    lhg_catalog_file catalog[8];
    native_build_file_t files[8];
    size_t catalog_count, file_count;
    lhg_session *session;
} np;

/* zig is a driver for several tools; cc is the C compiler. */
static const char *const zig_driver[] = {"cc"};

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

/* The generator reads discs through cue sheets, and the player also takes a
 * raw image. Such an image gets a sheet beside the cache while the discs are
 * read; an open session holds all it needs. 1 made, 0 none needed, -1 failed. */
static int raw_sheet(unsigned d, char *sheet, size_t size) {
    char absolute[4096];
    const char *path = np.discs[d];
    size_t n = strlen(path);
    FILE *f;
    int ok;
    if (n >= 4 && path[n - 4] == '.' && (path[n - 3] | 32) == 'c' && (path[n - 2] | 32) == 'u' && (path[n - 1] | 32) == 'e') return 0;
#ifdef _WIN32
    if (!_fullpath(absolute, path, sizeof(absolute))) return -1;
#else
    if (!realpath(path, absolute)) return -1;
#endif
    if (strchr(absolute, '"') || snprintf(sheet, size, "%s/disc%u.cue", np.root, d + 1) >= (int)size || !(f = fopen(sheet, "wb"))) return -1;
    ok = fprintf(f, "FILE \"%s\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n", absolute) > 0;
    if (fclose(f) || !ok) { remove(sheet); return -1; }
    return 1;
}

static int source_open(void *user, char *error, size_t size) {
    lhg_options options = generator_options();
    char sheets[4][4200];
    int made[4] = {0}, ok = 1;
    unsigned d;
    (void)user;
    for (d = 0; d < 4 && ok; ++d) {
        if (!*np.discs[d]) continue;
        made[d] = raw_sheet(d, sheets[d], sizeof(sheets[d]));
        if (made[d] < 0) { snprintf(error, size, "cannot read %s", np.discs[d]); ok = 0; }
        else options.disc_paths[options.disc_count++] = made[d] ? sheets[d] : np.discs[d];
    }
    ok = ok && lhg_session_open(&options, &np.session, error, size) == LHG_OK;
    for (d = 0; d < 4; ++d) if (made[d] > 0) remove(sheets[d]);
    return ok;
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

/* The headers modules compile against and, on Windows, the entry point. */
static void support_files(void) {
    size_t i;
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
}

#ifndef _WIN32
/* ---- a compiler the system has ---- */

/* The compiler's own predefined macros say which compiler it is: a name on
 * the PATH may be anything, and the recipe depends on the answer. */
static const char identify_source[] =
    "#if defined(__clang__)\nLIONHEART_COMPILER clang __clang_version__\n"
    "#elif defined(__GNUC__)\nLIONHEART_COMPILER gcc __VERSION__\n#endif\n";

/* A module as the generator writes one, small: guest registers and memory,
 * locals on the stack, an entry table and the call table. */
static const char check_source[] =
    "#include \"native_module.h\"\n"
    "lh_module_api lh_api;\n"
    "void f_check(cpu_t *c);\n"
    "void f_check(cpu_t *c) {\n"
    "    uint32_t local[16];\n"
    "    unsigned k;\n"
    "    for (k = 0; k < 16; ++k) local[k] = c->r[4] * k + c->ram[k];\n"
    "    c->r[2] = local[c->r[5] & 15] ^ 0x5a5a5a5au;\n"
    "}\n"
    "static const code_span_t check_spans[] = {{0x80010000u, 4}};\n"
    "static const code_region_t check_region = {check_spans, 1, 0, 0, NULL};\n"
    "static const entry_t check_entries[] = {{0x80010000u, f_check}};\n"
    "static const code_region_t *const check_regions[] = {&check_region};\n"
    "LH_MODULE_EXPORT const lh_module_descriptor_t lh_module_descriptor = {\n"
    "LH_MODULE_MAGIC, LH_MODULE_ABI_VERSION, (uint32_t)sizeof(lh_module_descriptor_t), LH_MODULE_ABI_ID,\n"
    "LH_MODULE_TRAITS_INIT, 0, 0x80010000u, 4, {0}, 0, 0, 0, check_entries, 1, check_regions, NULL, 0, &lh_api};\n";

/* Accepted only as the loader accepts a module, and only if its code, run
 * on a blank processor, computes what it says. */
static int check_module(void *user, const char *module, char *error, size_t size) {
    void *handle;
    recompiled_fn first;
    cpu_t *c;
    int ok;
    (void)user;
    if (native_module_try(module, &handle, &first, error, size) != NATIVE_MODULE_OK) return 0;
    ok = (c = calloc(1, sizeof(*c))) != NULL;
    if (ok) {
        c->r[4] = 7; c->r[5] = 3; c->ram[3] = 9;
        first(c);
        ok = c->r[2] == ((7u * 3u + 9u) ^ 0x5a5a5a5au);
        if (!ok) snprintf(error, size, "its code computes a wrong result");
    }
    free(c);
    native_module_untry(handle);
    return ok;
}

/* Absolute PATH folders only: a compiler is never taken from wherever the
 * player happens to start. */
static int find_program(const char *name, char *out, size_t size) {
    const char *path = getenv("PATH"), *at, *end;
    struct stat s;
    for (at = path; at && *at; at = *end ? end + 1 : end) {
        size_t n;
        end = strchr(at, ':');
        if (!end) end = at + strlen(at);
        n = (size_t)(end - at);
        if (!n || *at != '/' || snprintf(out, size, "%.*s/%s", (int)n, at, name) >= (int)size) continue;
        if (!stat(out, &s) && S_ISREG(s.st_mode) && !access(out, X_OK)) return 1;
    }
    return 0;
}

/* The verdict on one file, kept so that later launches start no compiler
 * until that file changes, or the headers, or the recipe. */
static int verdict_key(const char *path, native_build_family_t family, char *out, size_t size) {
    struct stat s;
    if (stat(path, &s) || strchr(path, '\n')) return 0;
    return snprintf(out, size, "%s %llu %llu %llu %lld.%09ld %s %s", path, (unsigned long long)s.st_dev, (unsigned long long)s.st_ino,
                    (unsigned long long)s.st_size, (long long)s.st_mtim.tv_sec, (long)s.st_mtim.tv_nsec, LH_MODULE_ABI_ID,
                    native_build_recipe(family)) < (int)size;
}

static int read_verdict(const char *record, const char *path, native_setup_compiler_t *out) {
    char text[2048], key[1024], expected[1024], *line, *next, *family = NULL, *identity = NULL, *version = NULL, *recorded = NULL;
    FILE *f = fopen(record, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    for (line = text; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "family ", 7)) family = line + 7;
        else if (!strncmp(line, "identity ", 9)) identity = line + 9;
        else if (!strncmp(line, "version ", 8)) version = line + 8;
        else if (!strncmp(line, "key ", 4)) recorded = line + 4;
    }
    if (!family || !identity || !version || !recorded || (strcmp(family, "gcc") && strcmp(family, "clang"))) return 0;
    memset(out, 0, sizeof(*out));
    out->family = strcmp(family, "gcc") ? NATIVE_BUILD_CLANG : NATIVE_BUILD_GCC;
    snprintf(key, sizeof(key), "%s", recorded);
    if (!verdict_key(path, out->family, expected, sizeof(expected)) || strcmp(key, expected) ||
        snprintf(out->path, sizeof(out->path), "%s", path) >= (int)sizeof(out->path)) return 0;
    snprintf(out->identity, sizeof(out->identity), "%s", identity);
    snprintf(out->version, sizeof(out->version), "%s", version);
    out->system = 1;
    return 1;
}

static void write_verdict(const char *record, const native_setup_compiler_t *c) {
    char key[1024], temporary[4200];
    FILE *f;
    int ok;
    if (!verdict_key(c->path, c->family, key, sizeof(key)) || snprintf(temporary, sizeof(temporary), "%s.tmp", record) >= (int)sizeof(temporary) ||
        !(f = fopen(temporary, "wb"))) return;
    ok = fprintf(f, "family %s\nidentity %s\nversion %s\nkey %s\n", c->family == NATIVE_BUILD_GCC ? "gcc" : "clang", c->identity, c->version, key) > 0;
    if (fclose(f) || !ok || rename(temporary, record)) remove(temporary);
}

static int try_compiler(const char *root, const char *path, native_setup_compiler_t *out, char *error, size_t size) {
    native_build_config_t config;
    char said[160], *version, *end;
    memset(out, 0, sizeof(*out));
    snprintf(error, size, "it does not say which compiler it is");
    if (!native_build_identify(root, path, identify_source, "LIONHEART_COMPILER", said, sizeof(said))) return 0;
    /* `gcc "16.2.1 20260810"` or `clang "21.1.8 "`: the family, then a string. */
    if (!strncmp(said, "gcc ", 4)) out->family = NATIVE_BUILD_GCC;
    else if (!strncmp(said, "clang ", 6)) out->family = NATIVE_BUILD_CLANG;
    else return 0;
    if (!(version = strchr(said, '"')) || !(end = strchr(++version, '"'))) return 0;
    while (end > version && end[-1] == ' ') --end;
    if (end == version) return 0;
    snprintf(out->identity, sizeof(out->identity), "%.*s %.*s", (int)(strchr(said, ' ') - said), said, (int)(end - version), version);
    snprintf(out->version, sizeof(out->version), "%.*s", (int)strcspn(version, " \""), version);
    snprintf(out->path, sizeof(out->path), "%s", path);
    out->system = 1;
    memset(&config, 0, sizeof(config));
    config.root = root; config.compiler = path; config.family = out->family;
    config.abi_id = LH_MODULE_ABI_ID; config.files = np.files; config.file_count = np.file_count;
    return native_build_try(&config, check_source, check_module, NULL, error, size);
}

int host_native_system_compiler(const char *data, native_setup_compiler_t *compiler) {
    /* Clang first: it builds the game's code faster than GCC, and it is the
     * compiler inside Zig. */
    static const char *const names[] = {"clang", "gcc"};
    char root[4096], record[4200], path[4096], error[256];
    size_t i;
    if (snprintf(root, sizeof(root), "%s/native", data) >= (int)sizeof(root) || snprintf(record, sizeof(record), "%s/compiler", root) >= (int)sizeof(record))
        return 0;
    support_files();
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!find_program(names[i], path, sizeof(path))) continue;
        if (read_verdict(record, path, compiler)) return 1;
        if (try_compiler(root, path, compiler, error, sizeof(error))) {
            write_verdict(record, compiler);
            fprintf(stderr, "Compiler found: %s (%s)\n", path, compiler->identity);
            return 1;
        }
        fprintf(stderr, "Compiler not used: %s: %s\n", path, error);
    }
    return 0;
}
#else
int host_native_system_compiler(const char *data, native_setup_compiler_t *compiler) {
    (void)data; (void)compiler;
    return 0;
}
#endif

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
    config.compiler = np.compiler.path; config.family = np.compiler.family;
    if (np.compiler.family == NATIVE_BUILD_ZIG) { config.driver = zig_driver; config.driver_count = 1; config.target = NATIVE_TARGET; }
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
    lhg_sha256 hash;
    size_t i;
    if (np.running || !np.booted || !data || !*data || !native_setup_compiler(&np.compiler))
        return 0;
    /* The developer's driver takes the place of the chosen one's, options and all. */
    if (options && options->compiler) {
        if (strlen(options->compiler) >= sizeof(np.compiler.path)) return 0;
        strcpy(np.compiler.path, options->compiler);
    }
    if (snprintf(np.root, sizeof(np.root), "%s/native", data) >= (int)sizeof(np.root)) return 0;
    if (!set_discs(discs)) return 0;
    np.catalog_count = host_native_catalog_count;
    for (i = 0; i < np.catalog_count; ++i) {
        np.catalog[i].name = host_native_catalog[i].name;
        np.catalog[i].data = host_native_catalog[i].data;
        np.catalog[i].size = host_native_catalog[i].size;
    }
    support_files();
    /* Everything that shapes module code names the cache folder: a change to
     * any of it starts a new folder, and the old one goes once unused. */
    lhg_sha256_init(&hash);
    add_line(&hash, "lionheart-native-cache 1");
    add_line(&hash, LH_MODULE_ABI_ID);
    add_line(&hash, lhg_generator_fingerprint());
    add_line(&hash, host_native_catalog_digest);
    add_line(&hash, np.compiler.identity);
    add_line(&hash, NATIVE_TARGET);
    add_line(&hash, native_build_recipe(np.compiler.family));
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
