/* Stands in for zig in the compiler archive the first-run cases serve. The
 * installer's self-check asks it for a version, an object and a library; each
 * output it is asked for gets a few bytes. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int i;
    for (i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "-o")) {
            FILE *out = fopen(argv[i + 1], "wb");
            if (!out || fputs("probe", out) == EOF || fclose(out)) return 1;
        }
    }
    if (argc > 2 && !strcmp(argv[2], "--version")) puts("0.0.0-fixture");
    return 0;
}
