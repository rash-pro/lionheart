/* Stands in for zig in the compiler archive the first-run cases serve, and
 * for a system compiler that is not one. The installer's self-check asks it
 * for a version, an object and a library; each output it is asked for gets a
 * few bytes. Asked to preprocess, it says it is GCC, so the player goes on to
 * build with it and finds no module. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int i, preprocess = 0;
    for (i = 1; i < argc; ++i) preprocess |= !strcmp(argv[i], "-E");
    for (i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "-o")) {
            FILE *out = fopen(argv[i + 1], "wb");
            if (!out || fputs(preprocess ? "LIONHEART_COMPILER gcc \"99.1.0 fixture\"\n" : "probe", out) == EOF || fclose(out)) return 1;
        }
    }
    if (argc > 2 && !strcmp(argv[2], "--version")) puts("0.0.0-fixture");
    return 0;
}
