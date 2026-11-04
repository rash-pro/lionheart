#include <stddef.h>
#ifdef FF8_SS_PRIVATE
#include "savestate_internal.h"
memcard_t *ss_memcard(ss_io *io, memcard_t *m) {
    uint32_t present = m != NULL;
    S32(present);
    if (present > 1) { io->failed = 1; return NULL; }
    if (!present) return NULL;
    if (io->reading) {
        m = calloc(1, sizeof(*m));
        if (!m) { io->failed = 1; return NULL; }
        m->file = tmpfile();
        if (!m->file) { free(m); io->failed = 1; return NULL; }
    }
    S8(m->data); S8(m->pending); S8(m->flag); S8(m->command); S8(m->previous); S8(m->checksum); S8(m->result);
    S32(m->step); S32(m->sector); S32(m->failed);
    if ((m->step > 140 && m->step != UINT_MAX) || m->sector > 65535 || m->failed > 1) io->failed = 1;
    if (io->reading && !io->failed &&
        (fwrite(m->data, 1, CARD_SIZE, m->file) != CARD_SIZE || fflush(m->file))) io->failed = 1;
    return m;
}
int ss_card_publish(memcard_t *m, FILE *f) {
    if (fwrite(m->data, 1, CARD_SIZE, f) != CARD_SIZE || fflush(f)) return 0;
    fclose(m->file); m->file = f; return 1;
}
void ss_card_hash(const memcard_t *m, uint8_t hash[20]) {
    if (m) code_sha1(m->data,CARD_SIZE,hash);
    else memset(hash,0,20);
}
void ss_card_live_reset(memcard_t *m, int changed) {
    if (!m) return;
    memcard_reset(m);
    memset(m->pending,0,sizeof(m->pending)); m->sector = m->result = 0;
    if (changed) m->flag |= 8;
}
#endif
