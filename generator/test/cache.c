/* Synthetic cache receipts exercise integrity without any owned game data. */
#include "cache.h"
#include "base.h"
#include "platform.h"
#include "sha256.h"
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <sys/stat.h>
#endif

#define CHECK(expression) do { \
    if (!(expression)) lhg_fail(&ctx, "line %d: %s", __LINE__, #expression); \
} while (0)

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    lhg_ctx ctx;
    lhg_ctx_init(&ctx);
    if (setjmp(ctx.bail)) {
        fprintf(stderr, "%s\n", ctx.error);
        lhg_ctx_release(&ctx);
        return 1;
    }
    lhg_arena arena;
    lhg_arena_init(&arena, &ctx);
    char hex[65];
    lhg_sha256 hash;
    lhg_sha256_init(&hash);
    lhg_sha256_finish(&hash, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    lhg_sha256_init(&hash);
    lhg_sha256_add(&hash, "a", 1);
    lhg_sha256_add(&hash, "bc", 2);
    lhg_sha256_finish(&hash, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    lhg_sha256_init(&hash);
    const char *long_vector = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    lhg_sha256_add(&hash, long_vector, strlen(long_vector));
    lhg_sha256_finish(&hash, hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    lhg_make_directories(&ctx, argv[1]);
    const char *catalog_dir = lhg_join_path(&arena, argv[1], "catalog");
    lhg_make_directories(&ctx, catalog_dir);
    const char *catalog = lhg_join_path(&arena, catalog_dir, "metadata.json");
    lhg_write_file(&ctx, catalog, "{}", 2);
    lhg_options options;
    memset(&options, 0, sizeof options);
    options.catalog_dir = catalog_dir;
    options.legacy_overlays = 1;
    lhg_image image;
    memset(&image, 0, sizeof image);
    image.data = (const uint8_t *)"synthetic";
    image.size = 9;
    lhg_program program;
    memset(&program, 0, sizeof program);
    program.main = &image;
    program.disc_mask = 2;
    char original[65], changed[65];
    lhg_cache_key(&arena, &options, &program, original);
    options.host_mode = LHG_HOST_FAST;
    lhg_cache_key(&arena, &options, &program, changed);
    CHECK(strcmp(original, changed));
    options.host_mode = LHG_HOST_PRECISE;
    program.disc_mask = 6;
    lhg_cache_key(&arena, &options, &program, changed);
    CHECK(strcmp(original, changed));
    program.disc_mask = 2;
    lhg_write_file(&ctx, catalog, "{}\n", 3);
    lhg_cache_key(&arena, &options, &program, changed);
    CHECK(strcmp(original, changed));
    lhg_write_file(&ctx, catalog, "{}", 2);
    const char *required[] = {"generated.h", "dispatch_table.c", "manifest.json", "profile-catalog.txt", "sources.cmake", "functions_0.c"};
    for (size_t i = 0; i < sizeof required / sizeof *required; i++)
        lhg_write_file(&ctx, lhg_join_path(&arena, argv[1], required[i]), "synthetic\n", 10);
    lhg_output_cache_invalidate(&arena, argv[1]);
    CHECK(!lhg_output_cache_hit(&arena, argv[1], original));
    lhg_write_file(&ctx, lhg_join_path(&arena, argv[1], "setup-cache.json"), "{}\n", 3);
    CHECK(!lhg_output_cache_hit(&arena, argv[1], original));
    lhg_output_cache_save(&arena, argv[1], original);
    CHECK(lhg_output_cache_hit(&arena, argv[1], original));
    CHECK(!lhg_output_cache_hit(&arena, argv[1], changed));
    lhg_write_file(&ctx, lhg_join_path(&arena, argv[1], "functions_0.c"), "corrupt\n", 8);
    CHECK(!lhg_output_cache_hit(&arena, argv[1], original));
    lhg_write_file(&ctx, lhg_join_path(&arena, argv[1], "functions_0.c"), "synthetic\n", 10);
    CHECK(lhg_output_cache_hit(&arena, argv[1], original));
    for (size_t i = 0; i < sizeof required / sizeof *required; i++)
        lhg_remove_file(&ctx, lhg_join_path(&arena, argv[1], required[i]));
    CHECK(!lhg_output_cache_hit(&arena, argv[1], original));
    for (size_t i = 0; i < sizeof required / sizeof *required; i++)
        lhg_write_file(&ctx, lhg_join_path(&arena, argv[1], required[i]), "synthetic\n", 10);
    CHECK(lhg_output_cache_hit(&arena, argv[1], original));
    lhg_remove_file(&ctx, lhg_join_path(&arena, argv[1], "generated.h"));
    CHECK(!lhg_output_cache_hit(&arena, argv[1], original));
    lhg_write_file(&ctx, lhg_join_path(&arena, argv[1], "generated.h"), "synthetic\n", 10);
    lhg_write_file(&ctx, lhg_join_path(&arena, argv[1], "setup-cache.native"), "truncated", 9);
    CHECK(!lhg_output_cache_hit(&arena, argv[1], original));
    lhg_output_cache_save(&arena, argv[1], original);
#ifndef _WIN32
    const char *marker = lhg_join_path(&arena, argv[1], "setup-cache.native");
    CHECK(chmod(marker, 0) == 0);
    lhg_output_cache_invalidate(&arena, argv[1]);
    CHECK(chmod(marker, 0600) != 0 && errno == ENOENT);
#else
    lhg_output_cache_invalidate(&arena, argv[1]);
#endif
    lhg_write_file(&ctx, lhg_join_path(&arena, argv[1], "setup-cache.native.tmp"), "partial", 7);
    CHECK(!lhg_output_cache_hit(&arena, argv[1], original));
    lhg_arena_release(&arena);
    lhg_ctx_release(&ctx);
    puts("SHA-256 vectors and cache identity/integrity/interruption checks passed");
    return 0;
}
