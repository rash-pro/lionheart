#include "native_setup.h"
#include "native_player.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    SDL_mutex *guard;
    SDL_Thread *thread;
    SDL_atomic_t cancel;
    char data[4096], archive[4096];
    native_setup_compiler_t compiler;
    int has_archive;
    native_setup_status_t status;
} setup;

/* ---- the manifest ---- */

#define FIXTURE_URLS 4
#define FIXTURE_FILES 64
static struct {
    int loaded;
    host_tc_manifest manifest;
    char text[16384];
    const char *urls[FIXTURE_URLS], *files[FIXTURE_FILES];
} fixture;

/* Tests serve their archive themselves; nothing else may stand in for the
 * pinned addresses. */
static int loopback(const char *url) {
    static const char *const hosts[] = {"http://127.0.0.1:", "http://localhost:", "http://[::1]:"};
    size_t i;
    for (i = 0; i < sizeof(hosts) / sizeof(hosts[0]); ++i)
        if (!strncmp(url, hosts[i], strlen(hosts[i]))) return 1;
    return 0;
}

static int load_fixture(const char *path) {
    host_tc_manifest *m = &fixture.manifest;
    FILE *f = fopen(path, "rb");
    size_t n;
    char *line, *next;
    if (!f) return 0;
    n = fread(fixture.text, 1, sizeof(fixture.text) - 1, f);
    fclose(f);
    fixture.text[n] = 0;
    memset(m, 0, sizeof(*m));
    /* The archive kind this system downloads, unless the fixture names one. */
    m->format = host_tc_pinned()->format;
    m->urls = fixture.urls;
    m->files = fixture.files;
    for (line = fixture.text; line && *line; line = next) {
        char *value;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        if (!(value = strchr(line, '='))) continue;
        *value++ = 0;
        if (!strcmp(line, "version")) m->version = value;
        else if (!strcmp(line, "format")) m->format = value;
        else if (!strcmp(line, "prefix")) m->prefix = value;
        else if (!strcmp(line, "executable")) m->executable = value;
        else if (!strcmp(line, "sha256")) m->sha256 = value;
        else if (!strcmp(line, "size")) m->size = strtoull(value, NULL, 10);
        else if (!strcmp(line, "url")) {
            if (!loopback(value) || m->url_count == FIXTURE_URLS) return 0;
            fixture.urls[m->url_count++] = value;
        } else if (!strcmp(line, "file")) {
            if (m->file_count == FIXTURE_FILES) return 0;
            fixture.files[m->file_count++] = value;
        }
    }
    return m->version && m->prefix && m->executable && m->sha256 && strlen(m->sha256) == 64 && m->size && m->file_count;
}

const host_tc_manifest *native_setup_manifest(void) {
    const char *path;
    if (fixture.loaded) return &fixture.manifest;
    path = getenv("LIONHEART_TOOLCHAIN_TEST_MANIFEST");
    if (!path) return host_tc_pinned();
    fixture.loaded = 1;
    if (!load_fixture(path)) {
        fprintf(stderr, "Toolchain fixture %s is not usable; no download address is used\n", path);
        fixture.manifest = *host_tc_pinned();
        fixture.manifest.urls = NULL;
        fixture.manifest.url_count = 0;
    }
    return &fixture.manifest;
}

/* ---- the installation ---- */

static int progress(void *user, host_tc_phase phase, uint64_t done, uint64_t total) {
    (void)user;
    SDL_LockMutex(setup.guard);
    setup.status.phase = phase;
    setup.status.done = done;
    setup.status.total = total;
    SDL_UnlockMutex(setup.guard);
    return SDL_AtomicGet(&setup.cancel);
}

/* The installed Zig, named by the archive it came from. */
static int installed_zig(const host_tc_manifest *m, native_setup_compiler_t *out) {
    memset(out, 0, sizeof(*out));
    if (!host_tc_executable(setup.data, m, out->path, sizeof(out->path))) return 0;
    out->family = NATIVE_BUILD_ZIG;
    snprintf(out->identity, sizeof(out->identity), "zig %s %s", m->version, m->sha256);
    snprintf(out->version, sizeof(out->version), "%s", m->version);
    return 1;
}

static int install_main(void *unused) {
    const host_tc_manifest *m = native_setup_manifest();
    host_tc_error error;
    native_setup_compiler_t compiler;
    int ok;
    (void)unused;
    memset(&error, 0, sizeof(error));
    ok = host_tc_install(setup.data, m, setup.has_archive ? setup.archive : NULL, progress, NULL, &error) && installed_zig(m, &compiler);
    if (!ok && !*error.reason) snprintf(error.reason, sizeof(error.reason), "The installed compiler cannot be found");
    SDL_LockMutex(setup.guard);
    if (ok) {
        setup.compiler = compiler;
        setup.status.state = NATIVE_SETUP_READY;
        setup.status.reason[0] = 0;
    } else {
        setup.status.state = NATIVE_SETUP_FAILED;
        snprintf(setup.status.reason, sizeof(setup.status.reason), "%s", error.reason);
    }
    SDL_UnlockMutex(setup.guard);
    fprintf(stderr, ok ? "Compiler installed: %s\n" : "Compiler not installed: %s\n", ok ? compiler.path : error.reason);
    return ok;
}

void native_setup_configure(const char *data) {
    native_setup_shutdown();
    if (!setup.guard && !(setup.guard = SDL_CreateMutex())) return;
    snprintf(setup.data, sizeof(setup.data), "%s", data ? data : "");
    memset(&setup.status, 0, sizeof(setup.status));
    memset(&setup.compiler, 0, sizeof(setup.compiler));
    if (!*setup.data) return;
    if (installed_zig(native_setup_manifest(), &setup.compiler)) setup.status.state = NATIVE_SETUP_READY;
    else if (host_native_system_compiler(setup.data, &setup.compiler)) {
        setup.status.state = NATIVE_SETUP_READY;
        setup.status.system = 1;
    }
}

int native_setup_install(const char *archive) {
    if (!setup.guard || !*setup.data) return 0;
    SDL_LockMutex(setup.guard);
    if (setup.status.state == NATIVE_SETUP_RUNNING || setup.status.state == NATIVE_SETUP_READY) {
        SDL_UnlockMutex(setup.guard);
        return 0;
    }
    SDL_UnlockMutex(setup.guard);
    /* A finished attempt's thread has ended; collect it before the next. */
    if (setup.thread) { SDL_WaitThread(setup.thread, NULL); setup.thread = NULL; }
    setup.has_archive = archive && *archive;
    snprintf(setup.archive, sizeof(setup.archive), "%s", setup.has_archive ? archive : "");
    SDL_AtomicSet(&setup.cancel, 0);
    SDL_LockMutex(setup.guard);
    memset(&setup.status, 0, sizeof(setup.status));
    setup.status.state = NATIVE_SETUP_RUNNING;
    setup.status.local = setup.has_archive;
    SDL_UnlockMutex(setup.guard);
    if (!(setup.thread = SDL_CreateThread(install_main, "compiler-install", NULL))) {
        SDL_LockMutex(setup.guard);
        setup.status.state = NATIVE_SETUP_FAILED;
        snprintf(setup.status.reason, sizeof(setup.status.reason), "Cannot start the installer");
        SDL_UnlockMutex(setup.guard);
        return 0;
    }
    return 1;
}

void native_setup_cancel(void) { SDL_AtomicSet(&setup.cancel, 1); }

void native_setup_status(native_setup_status_t *status) {
    if (!setup.guard) { memset(status, 0, sizeof(*status)); return; }
    SDL_LockMutex(setup.guard);
    *status = setup.status;
    SDL_UnlockMutex(setup.guard);
}

int native_setup_compiler(native_setup_compiler_t *compiler) {
    int ok;
    if (!setup.guard) return 0;
    SDL_LockMutex(setup.guard);
    ok = setup.status.state == NATIVE_SETUP_READY;
    if (ok) *compiler = setup.compiler;
    SDL_UnlockMutex(setup.guard);
    return ok;
}

void native_setup_shutdown(void) {
    if (!setup.thread) return;
    native_setup_cancel();
    SDL_WaitThread(setup.thread, NULL);
    setup.thread = NULL;
}
