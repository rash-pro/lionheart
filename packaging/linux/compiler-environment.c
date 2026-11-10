/* A shell wrapper would add its own environment. Record the compiler's actual
 * input before executing it, including compiler discovery and link commands. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern char **environ;
int main(int argc, char **argv) {
    char path[4096];
    FILE *f;
    char **e;
    (void)argc;
    snprintf(path, sizeof(path), "%s/%ld.env", getenv("COMPILER_ENV_LOG"), (long)getpid());
    if (!(f = fopen(path, "w"))) return 126;
    for (e = environ; *e; ++e) fprintf(f, "%s\n", *e);
    if (fclose(f)) return 126;
    argv[0] = getenv("REAL_COMPILER");
    execv(argv[0], argv);
    return 127;
}
