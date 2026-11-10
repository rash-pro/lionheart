/* Drives the build service with synthetic sets instead of the generator, and
 * a fake compiler, so the cache rules, the order, promotion, failures and the
 * locks can be checked without game data.
 *
 * Spec lines: INDEX DISCS EFFECT TOKEN [fail|slow|child|linkfail|units=N|pc=HEX|name=NAME]...
 * The first row is the executable (index -1). TOKEN names the code: rows with
 * the same token emit identical sources, as one overlay on several discs does.
 * Output: "deliver TOKEN INDEX" per module handed over, then a status line. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "native_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif

typedef struct {
    native_build_set_t info;
    char token[64], key[65];
    int fail, slow, child, link_fail, units;
    uint32_t pc;
} row_t;

static row_t rows[600];
static size_t row_count;
static int opened;
#ifdef _WIN32
static CRITICAL_SECTION output_guard;
#else
static pthread_mutex_t output_guard = PTHREAD_MUTEX_INITIALIZER;
#endif

static void pause_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
#endif
}

static void expand(const char *text, char out[65]) {
    unsigned part;
    for (part = 0; part < 4; ++part) {
        uint64_t hash = 14695981039346656037ull ^ part;
        const char *p;
        for (p = text; *p; ++p) hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
        snprintf(out + part * 16, 17, "%016llx", (unsigned long long)hash);
    }
}

static int load_spec(const char *path) {
    FILE *f = fopen(path, "r");
    char line[512];
    if (!f) return 0;
    while (fgets(line, sizeof(line), f) && row_count < sizeof(rows) / sizeof(rows[0])) {
        row_t *r = &rows[row_count];
        char *word, *save = NULL, identity[96];
        int index, field = 0;
        if (line[0] == '#' || line[0] == '\n') continue;
        memset(r, 0, sizeof(*r));
        r->units = 1;
        for (word = strtok_r(line, " \t\r\n", &save); word; word = strtok_r(NULL, " \t\r\n", &save), ++field) {
            if (field == 0) index = atoi(word), r->info.index = index;
            else if (field == 1) r->info.discs = (unsigned)strtoul(word, NULL, 0);
            else if (field == 2) r->info.effect = atoi(word);
            else if (field == 3) snprintf(r->token, sizeof(r->token), "%s", word);
            else if (!strcmp(word, "fail")) r->fail = 1;
            else if (!strcmp(word, "slow")) r->slow = 1;
            else if (!strcmp(word, "child")) r->child = 1;
            else if (!strcmp(word, "linkfail")) r->link_fail = 1;
            else if (!strncmp(word, "units=", 6)) r->units = atoi(word + 6);
            else if (!strncmp(word, "pc=", 3)) r->pc = (uint32_t)strtoul(word + 3, NULL, 16);
            else if (!strncmp(word, "name=", 5)) snprintf(r->info.name, sizeof(r->info.name), "%s", word + 5);
        }
        if (field < 4) continue;
        if (!*r->info.name) {
            size_t n = strlen(r->token) < sizeof(r->info.name) - 1 ? strlen(r->token) : sizeof(r->info.name) - 1;
            memcpy(r->info.name, r->token, n);
            r->info.name[n] = 0;
        }
        r->info.size = 1000;
        expand(r->token, r->key);
        snprintf(identity, sizeof(identity), "set:%d:%s", r->info.index, r->token);
        expand(identity, r->info.identity);
        row_count++;
    }
    fclose(f);
    return row_count > 0;
}

static int plan(void *user, unsigned discs, native_build_set_t **out, size_t *count, char *error, size_t size) {
    size_t i, n = 0;
    (void)user; (void)error; (void)size;
    *out = calloc(row_count, sizeof(**out));
    if (!*out) return 0;
    /* The generator lists only the discs' sets; a spec simply lists those. */
    (void)discs;
    for (i = 0; i < row_count; ++i) (*out)[n++] = rows[i].info;
    *count = n;
    return 1;
}

static const row_t *row_of(const native_build_set_t *plan_rows, size_t set) {
    (void)plan_rows;
    return &rows[set];
}

static int open_source(void *user, char *error, size_t size) {
    (void)user;
    if (getenv("HARNESS_OPEN_FAILS")) { snprintf(error, size, "cannot read the discs"); return 0; }
    opened = 1;
    return 1;
}

static int emit(void *user, size_t set, const char *directory, char key[65], char *error, size_t size) {
    const row_t *r = row_of(NULL, set);
    int unit;
    (void)user;
    for (unit = 0; unit < r->units; ++unit) {
        char path[4096];
        FILE *f;
        snprintf(path, sizeof(path), "%s/unit_%03d.c", directory, unit);
        if (!(f = fopen(path, "wb"))) { snprintf(error, size, "cannot write %s", path); return 0; }
        fprintf(f, "/* %s unit %d */\nSOURCE-TEXT-MUST-NOT-LEAK\n%s%s%s%s", r->token, unit, r->fail ? "LH_FAKE_FAIL\n" : "", r->child ? "LH_FAKE_CHILD\n" : "",
                r->slow ? "LH_FAKE_SLOW\n" : "", r->link_fail ? "LH_FAKE_LINK_FAIL\n" : "");
        fclose(f);
    }
    {
        char path[4096];
        FILE *f;
        snprintf(path, sizeof(path), "%s/tables.c", directory);
        if (!(f = fopen(path, "wb"))) { snprintf(error, size, "cannot write"); return 0; }
        fprintf(f, "/* %s tables */\n", r->token);
        fclose(f);
    }
    memcpy(key, r->key, 65);
    return 1;
}

static int contains(void *user, size_t set, uint32_t pc, const uint8_t *bytes, size_t size) {
    (void)user; (void)bytes; (void)size;
    return rows[set].pc && rows[set].pc == pc;
}

static void close_source(void *user) { (void)user; opened = 0; }

static int deliver(void *user, const char *path, int32_t index, char *error, size_t size) {
    const char *name = strrchr(path, '/'), *token = "?";
    size_t i;
    FILE *f = fopen(path, "rb");
    (void)user;
    if (!f) { snprintf(error, size, "missing"); return 0; }
    fclose(f);
    name = name ? name + 1 : path;
    for (i = 0; i < row_count; ++i) if (!strncmp(name, rows[i].key, 64)) token = rows[i].token;
    if (getenv("HARNESS_REFUSE") && !strcmp(getenv("HARNESS_REFUSE"), token)) { snprintf(error, size, "refused by the loader"); return 0; }
#ifdef _WIN32
    EnterCriticalSection(&output_guard);
#else
    pthread_mutex_lock(&output_guard);
#endif
    printf("deliver %s %d\n", token, (int)index);
    fflush(stdout);
#ifdef _WIN32
    LeaveCriticalSection(&output_guard);
#else
    pthread_mutex_unlock(&output_guard);
#endif
    return 1;
}

static const char *state_name(native_build_state_t s) {
    static const char *names[] = {"off", "loading", "building", "waiting", "done", "stopped"};
    return names[s];
}

static int twin(int argc, char **argv, const char *out) {
#ifdef _WIN32
    char line[8192] = "", self[4096];
    int i;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    HANDLE file = CreateFileA(out, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, 0, NULL);
    GetModuleFileNameA(NULL, self, sizeof(self));
    snprintf(line, sizeof(line), "\"%s\"", self);
    for (i = 1; i < argc; ++i) if (strcmp(argv[i], "--twin")) { strcat(line, " \""); strcat(line, argv[i]); strcat(line, "\""); }
    strcat(line, " --as-twin");
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = file; si.hStdError = file;
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return 0;
    CloseHandle(file); CloseHandle(pi.hThread);
    return (int)(intptr_t)pi.hProcess;
#else
    char **args = calloc((size_t)argc + 2, sizeof(*args));
    int i, n = 0;
    pid_t pid;
    posix_spawn_file_actions_t actions;
    args[n++] = "/proc/self/exe";
    for (i = 1; i < argc; ++i) if (strcmp(argv[i], "--twin")) args[n++] = argv[i];
    args[n++] = "--as-twin";
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    if (posix_spawn(&pid, "/proc/self/exe", &actions, NULL, args, environ)) pid = 0;
    posix_spawn_file_actions_destroy(&actions);
    free(args);
    return (int)pid;
#endif
}

static void twin_wait(int child) {
#ifdef _WIN32
    WaitForSingleObject((HANDLE)(intptr_t)child, INFINITE);
#else
    int status;
    waitpid((pid_t)child, &status, 0);
#endif
}

int main(int argc, char **argv) {
    native_build_config_t config;
    native_build_source_t source = {NULL, plan, open_source, emit, contains, close_source};
    native_build_status_t status;
    native_build_file_t files[2] = {{"module.h", "/* header */\n", 13}, {"extra.c", "/* extra unit */\n", 17}};
    const char *driver[] = {"cc"};
    const char *spec = NULL, *ran[16];
    char error[256], twin_out[4096] = "";
    unsigned ran_count = 0, kill_after = 0, hold = 0, stop_after = 0, delay = 0, twin_hold = 0;
    const char *twin_cache = NULL;
    int as_twin = 0;
    double timeout = 120;
    int i, child = 0, with_extra = 0;
#ifdef _WIN32
    InitializeCriticalSection(&output_guard);
    /* Cases compare lines byte for byte. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    memset(&config, 0, sizeof(config));
    config.cache_id = "cache-a"; config.exe_id = "exe";
    config.driver = driver; config.driver_count = 1;
    config.target = "fake-target"; config.abi_id = "test-abi";
    config.discs = 0x1e; config.disc = 1;
#ifdef _WIN32
    config.windows = 1;
#endif
    for (i = 1; i < argc; ++i) {
        const char *next = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(argv[i], "--root")) config.root = next, ++i;
        else if (!strcmp(argv[i], "--compiler")) config.compiler = next, ++i;
        else if (!strcmp(argv[i], "--spec")) spec = next, ++i;
        else if (!strcmp(argv[i], "--discs")) config.discs = (unsigned)strtoul(next, NULL, 0), ++i;
        else if (!strcmp(argv[i], "--disc")) config.disc = (unsigned)atoi(next), ++i;
        else if (!strcmp(argv[i], "--workers")) config.workers = (unsigned)atoi(next), ++i;
        else if (!strcmp(argv[i], "--cache-id")) config.cache_id = next, ++i;
        else if (!strcmp(argv[i], "--timeout")) timeout = atof(next), ++i;
        else if (!strcmp(argv[i], "--ran") && ran_count < 16) ran[ran_count++] = next, ++i;
        else if (!strcmp(argv[i], "--kill-after-ms")) kill_after = (unsigned)atoi(next), ++i;
        else if (!strcmp(argv[i], "--stop-after-ms")) stop_after = (unsigned)atoi(next), ++i;
        else if (!strcmp(argv[i], "--hold-ms")) hold = (unsigned)atoi(next), ++i;
        else if (!strcmp(argv[i], "--twin-out")) snprintf(twin_out, sizeof(twin_out), "%s", next), ++i;
        else if (!strcmp(argv[i], "--extra-unit")) with_extra = 1;
        else if (!strcmp(argv[i], "--twin")) child = -1;
        else if (!strcmp(argv[i], "--as-twin")) as_twin = 1;
        else if (!strcmp(argv[i], "--twin-cache-id")) twin_cache = next, ++i;
        else if (!strcmp(argv[i], "--twin-hold-ms")) twin_hold = (unsigned)atoi(next), ++i;
        else if (!strcmp(argv[i], "--delay-ms")) delay = (unsigned)atoi(next), ++i;
        else if (!strcmp(argv[i], "--verbose")) config.verbose = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!spec || !load_spec(spec) || !config.root || !config.compiler) { fprintf(stderr, "usage\n"); return 2; }
    config.files = files; config.file_count = with_extra ? 2 : 1;
    config.source = &source;
    config.deliver = deliver;
    if (as_twin) {
        /* The second player of a pair: its own folder and hold, no delay. */
        if (twin_cache) config.cache_id = twin_cache;
        if (twin_hold) hold = twin_hold;
        delay = 0;
        child = 0;
    }
    if (child < 0 && !(child = twin(argc, argv, twin_out))) { fprintf(stderr, "cannot start the twin\n"); return 2; }
    if (delay) pause_ms(delay);
    if (!native_build_start(&config, error, sizeof(error))) { fprintf(stderr, "start: %s\n", error); return 2; }
    if (ran_count) {
        /* Samples count only once the sets can be told apart. */
        for (i = 0; i < 2000; ++i) {
            native_build_status(&status);
            if (status.state == NATIVE_BUILD_BUILDING || status.state >= NATIVE_BUILD_DONE) break;
            pause_ms(5);
        }
        pause_ms(50);
        for (i = 0; i < (int)ran_count; ++i) {
            uint8_t bytes[4] = {0};
            native_build_ran((uint32_t)strtoul(ran[i], NULL, 16), bytes, sizeof(bytes));
        }
    }
    if (kill_after) {
        pause_ms(kill_after);
        /* As abrupt as a crash: no stop, no cleanup, compilers die with us. */
        fflush(stdout);
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 9);
#else
        kill(getpid(), SIGKILL);
#endif
    }
    if (stop_after) pause_ms(stop_after);
    else native_build_wait(timeout);
    if (hold) pause_ms(hold);
    native_build_status(&status);
    {
        native_build_failure_t failures[8];
        size_t total = native_build_failures(failures, 8), k;
        for (k = 0; k < total && k < 8; ++k) {
            char *line = strrchr(failures[k].words, '\n');
            printf("failure %s: %s\n", failures[k].name, line ? line + 1 : failures[k].words);
        }
    }
    native_build_stop();
    printf("state %s sets %u ready %u failed %u compiled %u shared %u cached %u\n", state_name(status.state), status.sets,
           status.ready, status.failed, status.compiled, status.shared, status.cached);
    if (*status.error) printf("error %s\n", status.error);
    if (opened) printf("source still open\n");
    fflush(stdout);
    if (child > 0) twin_wait(child);
    return 0;
}
