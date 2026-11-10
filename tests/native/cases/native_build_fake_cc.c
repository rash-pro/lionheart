/* Stands in for the compiler driver the build service runs. A compile copies
 * the source into the object; a link concatenates objects into the module.
 * Markers in a source fail the compile, fail the link, or slow the compile
 * down, so cases can place failures and kills where they want them. Every
 * call is appended to $FAKE_CC_LOG, so a case can see whether any ran. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static char *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    char *data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    data = malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    if (data) { data[n] = 0; *size = (size_t)n; }
    return data;
}

static void pause_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
#endif
}

static void note(const char *what, const char *name) {
    const char *log = getenv("FAKE_CC_LOG");
    FILE *f = log ? fopen(log, "ab") : NULL;
    const char *base = strrchr(name, '/');
    if (!f) return;
    fprintf(f, "%s %s\n", what, base ? base + 1 : name);
    fclose(f);
}

int main(int argc, char **argv) {
    const char *source = NULL, *output = NULL;
    int link = 0, i;
    FILE *out;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) source = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "-shared")) link = 1;
    }
    if (!output || (!link && !source)) { fprintf(stderr, "fake-cc: usage\n"); return 2; }
    if (!getenv("ZIG_LOCAL_CACHE_DIR") || !getenv("ZIG_GLOBAL_CACHE_DIR")) { fprintf(stderr, "fake-cc: no cache folders\n"); return 2; }
    if (!link) {
        size_t size;
        char *text = slurp(source, &size);
        if (!text) { fprintf(stderr, "%s: error: cannot read\n", source); return 1; }
        note("compile", source);
        if (strstr(text, "LH_FAKE_SLOW")) pause_ms(400);
        if (strstr(text, "LH_FAKE_FAIL")) {
            fprintf(stderr, "%s:1:1: error: forced failure\n1 error generated.\n", source);
            free(text);
            return 1;
        }
        out = fopen(output, "wb");
        if (!out || fwrite(text, 1, size, out) != size) return 1;
        free(text);
        return fclose(out) ? 1 : 0;
    }
    note("link", output);
    out = fopen(output, "wb");
    if (!out) return 1;
    for (i = 1; i < argc; ++i) {
        size_t n = strlen(argv[i]), size;
        char *text;
        if (n < 3 || strcmp(argv[i] + n - 2, ".o") || !(text = slurp(argv[i], &size))) continue;
        if (strstr(text, "LH_FAKE_LINK_FAIL")) { fprintf(stderr, "ld.lld: error: forced link failure\n"); return 1; }
        fwrite(text, 1, size, out);
        free(text);
    }
    return fclose(out) ? 1 : 0;
}
