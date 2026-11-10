#include "metadata_io.h"
#include "disc.h"
#include "sha1.h"
#include <stdlib.h>
#include <string.h>
struct metadata_image {
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
} void *metadata_read(metadata_image *m, int64_t sector, int64_t size,
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
  lhg_ctx ctx;
  lhg_ctx_init(&ctx);
  if (setjmp(ctx.bail)) {
    strcpy(error, ctx.error);
    lhg_ctx_release(&ctx);
    return NULL;
  }
  void *p = lhg_lzs_decompress(&ctx, input, size, output_size);
  void *out = copy_release(&ctx, p, *output_size);
  lhg_ctx_release(&ctx);
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
