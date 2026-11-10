/* The generator's command line. Flags mirror the original recompiler driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lionheart_generator.h"

#ifndef _WIN32
#include <errno.h>
#include <sys/stat.h>
#endif

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>

/* The console hands main() its codepage; paths must reach the library as UTF-8. */
static char **utf8_arguments(int *count)
{
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), count);
    if (!wide)
        return NULL;
    char **converted = malloc(sizeof *converted * (size_t)(*count + 1));
    if (!converted)
        return NULL;
    for (int index = 0; index < *count; index++) {
        int size = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, NULL, 0, NULL, NULL);
        converted[index] = malloc(size > 0 ? (size_t)size : 1);
        if (!converted[index])
            return NULL;
        WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, converted[index], size, NULL, NULL);
    }
    converted[*count] = NULL;
    LocalFree(wide);
    return converted;
}
#endif

static void usage(FILE *to)
{
    fprintf(to,
            "usage: lionheart-gen [EXE] [--disc CUE ...] [--catalog DIR] [--assets-dir DIR]\n"
            "                     [-o DIR] [--mode trace|fast] [--timing exact|block|loose]\n"
            "                     [--host-mode precise|fast] [--legacy-overlays] [--shape linked|modules]\n"
            "                     [--cache] [--fingerprint]\n"
            "       lionheart-gen --disc CUE ... --shape modules --plan | --emit-set INDEX|all [-o DIR]\n");
}

static const char *value_of(int argc, char **argv, int *index, const char *arg, const char *flag)
{
    size_t size = strlen(flag);
    if (arg[size] == '=')
        return arg + size + 1;
    if (*index + 1 >= argc) {
        fprintf(stderr, "lionheart-gen: %s needs a value\n", flag);
        return NULL;
    }
    return argv[++*index];
}

static int matches_flag(const char *arg, const char *flag)
{
    size_t size = strlen(flag);
    return !strncmp(arg, flag, size) && (arg[size] == 0 || arg[size] == '=');
}

static int make_directory(const char *path)
{
#ifdef _WIN32
    int count = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wide = count > 0 ? malloc((size_t)count * sizeof *wide) : NULL;
    int ok = wide && MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, count) && (CreateDirectoryW(wide, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    free(wide);
    return ok;
#else
    return !mkdir(path, 0777) || errno == EEXIST;
#endif
}

static int progress(void *user, const char *stage, unsigned long done, unsigned long total)
{
    (void)user;
    (void)done;
    (void)total;
    if (!strcmp(stage, "generation")) {
        puts("Catalog cache miss: generating native functions...");
        fflush(stdout);
    }
    return 0;
}

/* Developer view of what a player builds: the plan from metadata, or modules
 * emitted one set at a time through a session, each printed with its key. */
static int single_sets(const lhg_options *options, int plan, const char *which)
{
    char error[1024];
    if (plan) {
        unsigned mask = 0;
        lhg_set_info *sets;
        size_t count;
        for (int disc = 0; disc < options->disc_count; disc++) {
            const char *path = options->disc_paths[disc];
            const char *digit = strstr(path, "Disc ");
            /* Plans need disc numbers only; the session verifies the images themselves. */
            if (!digit || digit[5] < '1' || digit[5] > '4') {
                fprintf(stderr, "lionheart-gen: --plan needs disc numbers in the image names\n");
                return 2;
            }
            mask |= 1u << (digit[5] - '0');
        }
        if (lhg_plan_sets(options, mask, &sets, &count, error, sizeof error) != LHG_OK) {
            fprintf(stderr, "lionheart-gen: %s\n", error);
            return 1;
        }
        for (size_t set = 0; set < count; set++)
            printf("%d %s discs=%x effect=%d base=%08lx size=%lu %s\n", sets[set].index, sets[set].identity, sets[set].discs, sets[set].effect,
                   sets[set].base, sets[set].size, sets[set].name);
        free(sets);
        return 0;
    }
    lhg_session *session;
    if (lhg_session_open(options, &session, error, sizeof error) != LHG_OK) {
        fprintf(stderr, "lionheart-gen: %s\n", error);
        return 1;
    }
    const lhg_set_info *sets;
    size_t count = lhg_session_sets(session, &sets);
    int all = !strcmp(which, "all"), found = 0;
    for (size_t set = 0; set < count; set++) {
        if (!all && atoi(which) != sets[set].index)
            continue;
        char directory[4096], key[65];
        found = 1;
        if (sets[set].index == LHG_MAIN_SET)
            snprintf(directory, sizeof directory, "%s/main", options->output_dir);
        else
            snprintf(directory, sizeof directory, "%s/set-%d", options->output_dir, sets[set].index);
        /* A key-only pass first: the key must not depend on whether files are written. */
        char quiet[65];
        if (lhg_session_emit(session, set, NULL, quiet, error, sizeof error) != LHG_OK ||
            !make_directory(options->output_dir) || !make_directory(directory) ||
            lhg_session_emit(session, set, directory, key, error, sizeof error) != LHG_OK) {
            fprintf(stderr, "lionheart-gen: %s\n", *error ? error : "cannot create the output directory");
            lhg_session_close(session);
            return 1;
        }
        if (strcmp(quiet, key)) {
            fprintf(stderr, "lionheart-gen: set %d key changed when writing\n", sets[set].index);
            lhg_session_close(session);
            return 1;
        }
        printf("%d %s %s\n", sets[set].index, sets[set].identity, key);
    }
    lhg_session_close(session);
    if (!found)
        fprintf(stderr, "lionheart-gen: no set %s for these discs\n", which);
    return found ? 0 : 1;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    char **utf8 = utf8_arguments(&argc);
    if (!utf8) {
        fprintf(stderr, "lionheart-gen: cannot read the command line\n");
        return 2;
    }
    argv = utf8;
#endif
    lhg_options options;
    memset(&options, 0, sizeof options);
    options.catalog_dir = "games/ff8-usa";
    options.output_dir = "build/gen";
    int have_exe = 0, plan = 0;
    const char *emit_set = NULL;
    for (int index = 1; index < argc; index++) {
        const char *arg = argv[index];
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(arg, "--fingerprint")) {
            puts(lhg_generator_fingerprint());
            return 0;
        } else if (!strcmp(arg, "--cache")) {
            options.use_cache = 1;
        } else if (matches_flag(arg, "--disc")) {
            const char *value = value_of(argc, argv, &index, arg, "--disc");
            if (!value)
                return 2;
            if (options.disc_count >= 4) {
                fprintf(stderr, "lionheart-gen: --disc may be supplied at most four times\n");
                return 2;
            }
            options.disc_paths[options.disc_count++] = value;
        } else if (matches_flag(arg, "--catalog")) {
            if (!(options.catalog_dir = value_of(argc, argv, &index, arg, "--catalog")))
                return 2;
        } else if (matches_flag(arg, "--assets-dir")) {
            if (!(options.assets_dir = value_of(argc, argv, &index, arg, "--assets-dir")))
                return 2;
        } else if (matches_flag(arg, "--output")) {
            if (!(options.output_dir = value_of(argc, argv, &index, arg, "--output")))
                return 2;
        } else if (!strncmp(arg, "-o", 2) && arg[1] == 'o') {
            options.output_dir = arg[2] ? arg + 2 : (index + 1 < argc ? argv[++index] : NULL);
            if (!options.output_dir) {
                fprintf(stderr, "lionheart-gen: -o needs a value\n");
                return 2;
            }
        } else if (matches_flag(arg, "--mode")) {
            const char *value = value_of(argc, argv, &index, arg, "--mode");
            if (!value)
                return 2;
            if (!strcmp(value, "fast"))
                options.mode = LHG_MODE_FAST;
            else if (!strcmp(value, "trace"))
                options.mode = LHG_MODE_TRACE;
            else {
                fprintf(stderr, "lionheart-gen: unknown --mode %s\n", value);
                return 2;
            }
        } else if (matches_flag(arg, "--timing")) {
            const char *value = value_of(argc, argv, &index, arg, "--timing");
            if (!value)
                return 2;
            if (!strcmp(value, "exact"))
                options.timing = LHG_TIMING_EXACT;
            else if (!strcmp(value, "block"))
                options.timing = LHG_TIMING_BLOCK;
            else if (!strcmp(value, "loose"))
                options.timing = LHG_TIMING_LOOSE;
            else {
                fprintf(stderr, "lionheart-gen: unknown --timing %s\n", value);
                return 2;
            }
        } else if (matches_flag(arg, "--host-mode")) {
            const char *value = value_of(argc, argv, &index, arg, "--host-mode");
            if (!value)
                return 2;
            if (!strcmp(value, "precise"))
                options.host_mode = LHG_HOST_PRECISE;
            else if (!strcmp(value, "fast"))
                options.host_mode = LHG_HOST_FAST;
            else {
                fprintf(stderr, "lionheart-gen: unknown --host-mode %s\n", value);
                return 2;
            }
        } else if (matches_flag(arg, "--shape")) {
            const char *value = value_of(argc, argv, &index, arg, "--shape");
            if (!value)
                return 2;
            if (!strcmp(value, "linked"))
                options.shape = LHG_SHAPE_LINKED;
            else if (!strcmp(value, "modules"))
                options.shape = LHG_SHAPE_MODULES;
            else {
                fprintf(stderr, "lionheart-gen: unknown --shape %s\n", value);
                return 2;
            }
        } else if (!strcmp(arg, "--plan")) {
            plan = 1;
        } else if (matches_flag(arg, "--emit-set")) {
            if (!(emit_set = value_of(argc, argv, &index, arg, "--emit-set")))
                return 2;
        } else if (!strcmp(arg, "--legacy-overlays")) {
            options.legacy_overlays = 1;
        } else if (arg[0] == '-' && arg[1]) {
            fprintf(stderr, "lionheart-gen: unknown option %s\n", arg);
            usage(stderr);
            return 2;
        } else if (!have_exe) {
            options.exe_path = arg;
            have_exe = 1;
        } else {
            fprintf(stderr, "lionheart-gen: unexpected argument %s\n", arg);
            return 2;
        }
    }
    if (options.exe_path && options.disc_count) {
        fprintf(stderr, "lionheart-gen: use either EXE or --disc\n");
        return 2;
    }
    if (!options.exe_path && !options.disc_count) {
        fprintf(stderr, "lionheart-gen: EXE or --disc is required\n");
        usage(stderr);
        return 2;
    }
    if (plan || emit_set)
        return single_sets(&options, plan, emit_set);
    lhg_summary summary;
    if (options.use_cache)
        options.progress = progress;
    char error[1024];
    if (lhg_generate(&options, &summary, error, sizeof error) != LHG_OK) {
        fprintf(stderr, "lionheart-gen: %s\n", error);
        return 1;
    }
    if (options.use_cache)
        puts(summary.cache_hit ? "Catalog cache hit: verified inputs, generator, catalogs, options and output SHA-256."
                               : "Catalog cache miss: generated and verified native functions.");
    if (summary.cache_hit)
        printf("Reused %lu overlay sets in %s\n", summary.overlay_sets, options.output_dir);
    else if (summary.full_catalog)
        printf("Generated %lu main functions and %lu catalog sets in %s\n", summary.functions, summary.overlay_sets, options.output_dir);
    else
        printf("Generated %lu functions, %lu instructions, %lu jump tables in %s\n", summary.functions, summary.instructions, summary.jump_tables,
               options.output_dir);
    return 0;
}
