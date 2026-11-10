#include "generator_shim.h"

#include <stdlib.h>
#include <string.h>

lhgt_session *lhgt_open(void)
{
    lhgt_session *session = calloc(1, sizeof *session);
    if (!session)
        return NULL;
    lhg_ctx_init(&session->ctx);
    if (setjmp(session->ctx.bail)) {
        free(session);
        return NULL;
    }
    lhg_arena_init(&session->arena, &session->ctx);
    return session;
}

void lhgt_close(lhgt_session *session)
{
    if (!session)
        return;
    lhg_arena_release(&session->arena);
    lhg_ctx_release(&session->ctx);
    free(session);
}

const char *lhgt_error(const lhgt_session *session)
{
    return session->ctx.error;
}

int lhgt_discover(lhgt_session *session, const lhg_image *image, const uint32_t *seeds, size_t seed_count,
                  int scan_calls, lhg_functions *functions)
{
    if (setjmp(session->ctx.bail))
        return 1;
    lhg_discover(&session->arena, image, seeds, seed_count, scan_calls, functions);
    return 0;
}

int lhgt_emit(lhgt_session *session, const lhg_image *image, const lhg_functions *functions, const char *output,
              const lhg_overlay_set *overlays, size_t overlay_count, const lhg_emit_options *options)
{
    lhg_emit_result result;
    if (setjmp(session->ctx.bail))
        return 1;
    if (options->register_cache)
        lhg_cache_validate(&session->ctx, options->register_cache);
    lhg_emit(&session->arena, image, functions, output, overlays, overlay_count, options, &result);
    return 0;
}

int lhgt_effects(lhgt_session *session, const lhg_insn *insn, lhg_effect *effect)
{
    if (setjmp(session->ctx.bail))
        return 1;
    lhg_effects(&session->arena, insn, effect);
    return 0;
}

int lhgt_json_parse(lhgt_session *session, const char *data, size_t size, const char *name, const lhg_json **value)
{
    if (setjmp(session->ctx.bail))
        return 1;
    *value = lhg_json_parse(&session->arena, data, size, name);
    return 0;
}

int lhgt_infer_effect_base(lhgt_session *session, const uint8_t *data, size_t size, const char *name,
                           uint32_t *base, const lhg_json **evidence)
{
    if (setjmp(session->ctx.bail))
        return 1;
    lhg_json *proof = lhg_json_new_object(&session->arena);
    *base = lhg_infer_effect_base(&session->arena, data, size, name, proof);
    *evidence = proof;
    return 0;
}

int lhgt_load_legacy_disc(lhgt_session *session, const char *catalog_dir, const char *cue, const char *assets_dir,
                          lhg_program *program)
{
    if (setjmp(session->ctx.bail))
        return 1;
    const char *cues[1] = {cue};
    lhg_load_legacy_discs(&session->arena, catalog_dir, cues, 1, assets_dir, program, 0);
    lhg_discover_legacy(&session->arena, program);
    return 0;
}

int lhgt_load_exe(lhgt_session *session, const char *catalog_dir, const char *path)
{
    lhg_program program;
    if (setjmp(session->ctx.bail))
        return 1;
    lhg_load_exe(&session->arena, catalog_dir, path, &program);
    return 0;
}

int lhgt_cache_read(lhgt_session *session, const char *path, const lhg_register_cache **selection)
{
    if (setjmp(session->ctx.bail))
        return 1;
    *selection = lhg_cache_read(&session->arena, path);
    return 0;
}
