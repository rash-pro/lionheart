/* Reads PlayStation disc images (raw MODE2/2352 .bin described by a .cue) and FF8 IMG containers. */
#ifndef LHG_DISC_H
#define LHG_DISC_H

#include "base.h"

typedef struct {
    const char *name;
    uint32_t lba, size;
    int directory;
    int link;
} lhg_iso_record;

typedef struct {
    const char *path;
    lhg_iso_record record;
} lhg_disc_file;

typedef struct {
    lhg_ctx *ctx;
    lhg_arena *arena;
    FILE *bin;
    int64_t bin_size;
    lhg_iso_record root;
    lhg_disc_file *files;
    size_t file_count, file_capacity;
} lhg_disc;

/* The file list keeps ISO traversal order, which later duplicate-name resolution relies on. */
void lhg_disc_open(lhg_disc *disc, lhg_arena *arena, const char *cue_path);
void lhg_disc_close(lhg_disc *disc);
uint8_t *lhg_disc_read(lhg_disc *disc, uint32_t lba, int64_t size);
/* Path comparison is case-insensitive, like ISO9660 itself. */
const lhg_disc_file *lhg_disc_find(lhg_disc *disc, const char *path);

/* A bounded view of FF8DISCn.IMG on a verified disc. */
typedef struct {
    lhg_disc disc;
    lhg_iso_record record;
    int number;
} lhg_ff8_image;

void lhg_ff8_image_open(lhg_ff8_image *image, lhg_arena *arena, const char *cue_path);
void lhg_ff8_image_close(lhg_ff8_image *image);
uint8_t *lhg_ff8_image_read(lhg_ff8_image *image, int64_t sector, int64_t size);

/* Decompresses FF8 LZS. Returns a tracked buffer; size receives its length. */
uint8_t *lhg_lzs_decompress(lhg_ctx *ctx, const uint8_t *input, size_t input_size, size_t *size);

#endif
