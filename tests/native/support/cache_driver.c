/* Cancellation uses the embedding API, which the command line does not expose. */
#include "lionheart_generator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cancel_emit;
static int progress(void *user, const char *stage, unsigned long done, unsigned long total)
{
    (void)user;
    printf("%s %lu/%lu\n", stage, done, total);
    return cancel_emit && !strcmp(stage, "emit");
}
int main(int argc, char **argv)
{
    if (argc < 5 || argc > 8) return 2;
    lhg_options options = {0};
    options.catalog_dir = argv[1];
    options.output_dir = argv[2];
    options.legacy_overlays = 1;
    options.use_cache = strcmp(argv[3], "uncached") != 0;
    cancel_emit = strcmp(argv[3], "good") != 0;
    options.progress = progress;
    options.disc_count = argc - 4;
    for (int i = 0; i < options.disc_count; i++) options.disc_paths[i] = argv[i + 4];
    lhg_summary summary;
    char error[1024];
    lhg_status status = lhg_generate(&options, &summary, error, sizeof error);
    printf("status=%d hit=%d\n", status, summary.cache_hit);
    if (status != LHG_OK) fprintf(stderr, "%s\n", error);
    return (int)status;
}
