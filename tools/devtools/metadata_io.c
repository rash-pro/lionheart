#include "metadata_io.h"
#include "catalog.h"
#include "disc.h"
#include "sha1.h"
#include <stdlib.h>
#include <string.h>
struct metadata_image {
  lhg_ctx ctx;
  lhg_arena arena;
  lhg_ff8_image image;
};
metadata_image *metadata_open(const char *cue, char *error) {
  metadata_image *m = calloc(1, sizeof *m);
  if (!m) {
    strcpy(error, "out of memory");
    return NULL;
  }
  lhg_ctx_init(&m->ctx);
  lhg_arena_init(&m->arena, &m->ctx);
  if (setjmp(m->ctx.bail)) {
    strcpy(error, m->ctx.error);
    metadata_close(m);
    return NULL;
  }
  lhg_ff8_image_open(&m->image, &m->arena, cue);
  return m;
}
void metadata_close(metadata_image *m) {
  lhg_ctx_release(&m->ctx);
  free(m);
}
int metadata_number(metadata_image *m) { return m->image.number; }
uint32_t metadata_lba(metadata_image *m) { return m->image.record.lba; }
uint32_t metadata_size(metadata_image *m) { return m->image.record.size; }
static void *copy_release(lhg_ctx *ctx, void *p, size_t size) {
  void *out = malloc(size ? size : 1);
  if (out)
    memcpy(out, p, size);
  lhg_free(ctx, p);
  return out;
}
void *metadata_read(metadata_image *m, int64_t sector, int64_t size,
                    char *error) {
  if (setjmp(m->ctx.bail)) {
    strcpy(error, m->ctx.error);
    return NULL;
  }
  return copy_release(&m->ctx, lhg_ff8_image_read(&m->image, sector, size),
                      (size_t)size);
}
void *metadata_decode(const void *input, size_t size, size_t *output_size,
                      char *error) {
  // Failure cleanup must outlive setjmp's automatic-variable rules.
  lhg_ctx *ctx = calloc(1, sizeof *ctx);
  if (!ctx) {
    strcpy(error, "out of memory");
    return NULL;
  }
  lhg_ctx_init(ctx);
  if (setjmp(ctx->bail)) {
    strcpy(error, ctx->error);
    lhg_ctx_release(ctx);
    free(ctx);
    return NULL;
  }
  void *p = lhg_lzs_decompress(ctx, input, size, output_size);
  void *out = copy_release(ctx, p, *output_size);
  lhg_ctx_release(ctx);
  free(ctx);
  return out;
}
void *metadata_exe(metadata_image *m, const char *name, size_t *size,
                   char *error) {
  if (setjmp(m->ctx.bail)) {
    strcpy(error, m->ctx.error);
    return NULL;
  }
  const lhg_disc_file *f = lhg_disc_find(&m->image.disc, name);
  *size = f->record.size;
  return copy_release(
      &m->ctx, lhg_disc_read(&m->image.disc, f->record.lba, *size), *size);
}
void metadata_hash(const void *data, size_t size, char hex[41]) {
  lhg_sha1_hex(data, size, hex);
}
int metadata_entry_valid(const void *data, size_t size, uint32_t base,
                         uint32_t pc) {
  lhg_image image;
  memset(&image, 0, sizeof image);
  image.data = data;
  image.size = size;
  image.base = base;
  image.entry = base;
  return lhg_entry_hint_valid(&image, (int64_t)pc);
}
uint32_t metadata_effect_base(const void *data, size_t size, const char *name,
                              char *error) {
  // Failure cleanup must outlive setjmp's automatic-variable rules.
  lhg_ctx *ctx = calloc(1, sizeof *ctx);
  if (!ctx) {
    strcpy(error, "out of memory");
    return 0;
  }
  lhg_ctx_init(ctx);
  lhg_arena *arena = calloc(1, sizeof *arena);
  if (!arena || setjmp(ctx->bail)) {
    strcpy(error, arena ? ctx->error : "out of memory");
    lhg_ctx_release(ctx);
    free(ctx);
    free(arena);
    return 0;
  }
  lhg_arena_init(arena, ctx);
  uint32_t base = lhg_infer_effect_base(arena, data, size, name,
                                        lhg_json_new_object(arena));
  lhg_arena_release(arena);
  lhg_ctx_release(ctx);
  free(ctx);
  free(arena);
  return base;
}
int64_t metadata_code_prefix(const void *data, size_t size, uint32_t base,
                             size_t limit, char *error) {
  // Failure cleanup must outlive setjmp's automatic-variable rules.
  lhg_ctx *ctx = calloc(1, sizeof *ctx);
  lhg_arena *arena = calloc(1, sizeof *arena);
  if (!ctx || !arena) {
    strcpy(error, "out of memory");
    free(ctx);
    free(arena);
    return -1;
  }
  lhg_ctx_init(ctx);
  if (setjmp(ctx->bail)) {
    strcpy(error, ctx->error);
    lhg_ctx_release(ctx);
    free(ctx);
    free(arena);
    return -1;
  }
  lhg_arena_init(arena, ctx);
  lhg_image image;
  memset(&image, 0, sizeof image);
  image.data = data;
  image.size = size;
  image.base = base;
  image.entry = base;
  lhg_functions functions;
  int64_t length = (int64_t)lhg_code_prefix(arena, &image, limit, &functions);
  lhg_arena_release(arena);
  lhg_ctx_release(ctx);
  free(ctx);
  free(arena);
  return length;
}
