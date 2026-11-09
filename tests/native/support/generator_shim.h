/* The generator library's setjmp error handling kept on C frames, so a failure
 * unwinds through plain C only and reaches the C++ caller as a return code. */
#ifndef LH_GENERATOR_SHIM_H
#define LH_GENERATOR_SHIM_H

#include <stddef.h>
#include <stdint.h>

#include "base.h"
#include "catalog.h"
#include "decode.h"
#include "disc.h"
#include "discover.h"
#include "emit.h"
#include "json.h"
#include "locals.h"
#include "sha1.h"

typedef struct {
    lhg_ctx ctx;
    lhg_arena arena;
} lhgt_session;

lhgt_session *lhgt_open(void);
void lhgt_close(lhgt_session *session);
const char *lhgt_error(const lhgt_session *session);

/* Each returns 0, or nonzero with the reason in lhgt_error. */
int lhgt_discover(lhgt_session *session, const lhg_image *image, const uint32_t *seeds, size_t seed_count,
                  int scan_calls, lhg_functions *functions);
int lhgt_emit(lhgt_session *session, const lhg_image *image, const lhg_functions *functions, const char *output,
              const lhg_overlay_set *overlays, size_t overlay_count, const lhg_emit_options *options);
int lhgt_effects(lhgt_session *session, const lhg_insn *insn, lhg_effect *effect);
int lhgt_json_parse(lhgt_session *session, const char *data, size_t size, const char *name, const lhg_json **value);
int lhgt_infer_effect_base(lhgt_session *session, const uint8_t *data, size_t size, const char *name,
                           uint32_t *base, const lhg_json **evidence);
int lhgt_load_legacy_disc(lhgt_session *session, const char *catalog_dir, const char *cue, const char *assets_dir,
                          lhg_program *program);
int lhgt_cache_read(lhgt_session *session, const char *path, const lhg_register_cache **selection);

#endif
