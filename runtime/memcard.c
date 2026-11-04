#include "sio.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARD_SIZE (128u * 1024u)
#define SECTOR_SIZE 128u

struct memcard {
    FILE *file;
    uint8_t data[CARD_SIZE], pending[SECTOR_SIZE];
    uint8_t flag, command, previous, checksum, result;
    unsigned step, sector;
    int failed;
};

static void format_card(uint8_t *data) {
    unsigned frame, i;
    memset(data, 0xff, CARD_SIZE);
    memset(data, 0, 36 * SECTOR_SIZE);
    data[0] = 'M'; data[1] = 'C';
    for (frame = 1; frame < 16; ++frame) {
        uint8_t *p = data + frame * SECTOR_SIZE;
        p[0] = 0xa0; p[8] = p[9] = 0xff;
    }
    for (frame = 16; frame < 36; ++frame) memset(data + frame * SECTOR_SIZE, 0xff, 4);
    for (frame = 0; frame < 36; ++frame)
        for (i = 0; i < 127; ++i) data[frame * SECTOR_SIZE + 127] ^= data[frame * SECTOR_SIZE + i];
    memcpy(data + 63 * SECTOR_SIZE, data, SECTOR_SIZE);
}

memcard_t *memcard_open(const char *path, char *error, size_t error_size) {
    memcard_t *m = calloc(1, sizeof(*m));
    int created = 0;
    if (!m) { snprintf(error, error_size, "cannot allocate memory card"); return NULL; }
    m->file = fopen(path, "r+b");
    if (!m->file && errno == ENOENT) { m->file = fopen(path, "w+b"); created = 1; }
    if (!m->file) { snprintf(error, error_size, "%s: %s", path, strerror(errno)); free(m); return NULL; }
    if (created) {
        format_card(m->data);
        if (fwrite(m->data, 1, CARD_SIZE, m->file) != CARD_SIZE || fflush(m->file)) {
            snprintf(error, error_size, "%s: cannot write formatted card", path); memcard_close(m); return NULL;
        }
    } else if (fread(m->data, 1, CARD_SIZE, m->file) != CARD_SIZE || fgetc(m->file) != EOF || ferror(m->file)) {
        snprintf(error, error_size, "%s: expected a raw 131072-byte PS1 card", path); memcard_close(m); return NULL;
    }
    m->flag = 8;
    return m;
}

void memcard_close(memcard_t *m) { if (m) { if (m->file) fclose(m->file); free(m); } }
void memcard_reset(memcard_t *m) { if (m) { m->step = 0; m->command = m->checksum = m->previous = 0; } }
int memcard_failed(const memcard_t *m) { return m && m->failed; }
unsigned memcard_flags(const memcard_t *m) { return m ? m->flag : 0; }

int memcard_read_sector(memcard_t *m, unsigned sector, uint8_t data[128]) {
    if (!m || sector >= 1024) return 0;
    memcpy(data, m->data + sector * SECTOR_SIZE, SECTOR_SIZE);
    return 1;
}

int memcard_write_sector(memcard_t *m, unsigned sector, const uint8_t data[128]) {
    if (!m || sector >= 1024) return 0;
    if (m->failed || fseek(m->file, (long)sector * SECTOR_SIZE, SEEK_SET) ||
        fwrite(data, 1, SECTOR_SIZE, m->file) != SECTOR_SIZE || fflush(m->file)) {
        m->failed = 1; m->flag |= 4; return 0;
    }
    memcpy(m->data + sector * SECTOR_SIZE, data, SECTOR_SIZE);
    m->flag &= (uint8_t)~8u;
    return 1;
}

int memcard_format(memcard_t *m) {
    uint8_t *data;
    unsigned i;
    int ok = 1;
    if (!m) return 0;
    data = malloc(CARD_SIZE);
    if (!data) return 0;
    format_card(data);
    for (i = 0; i < 36 && ok; ++i) ok = memcard_write_sector(m, i, data + i * SECTOR_SIZE);
    if (ok) ok = memcard_write_sector(m, 63, data);
    free(data); return ok;
}

static uint8_t commit_sector(memcard_t *m) {
    if (m->result != 0x47) return m->result;
    return memcard_write_sector(m, m->sector, m->pending) ? 0x47 : 0x4e;
}

uint8_t memcard_exchange(memcard_t *m, uint8_t tx, unsigned *ack_delay) {
    unsigned step;
    uint8_t rx = 0xff;
    *ack_delay = 0;
    if (!m || m->step == UINT_MAX) return rx;
    step = m->step++;
    *ack_delay = 412;
    if (step == 0) {
        if (tx != 0x81) *ack_delay = 0;
    } else if (step == 1) {
        rx = m->flag; m->command = tx;
        if (tx != 0x52 && tx != 0x57 && tx != 0x53) *ack_delay = 0;
    } else if (step == 2) rx = 0x5a;
    else if (step == 3) rx = 0x5d;
    else if (m->command == 0x53) {
        static const uint8_t id[] = {0x5c, 0x5d, 4, 0, 0, 0x80};
        rx = id[step - 4];
        if (step == 9) *ack_delay = 0;
    } else if (step == 4) {
        m->sector = (unsigned)tx << 8; m->checksum = tx; rx = 0;
    } else if (step == 5) {
        m->sector |= tx; m->checksum ^= tx; rx = m->previous;
    } else if (m->command == 0x52) {
        if (step == 6) { rx = 0x5c; *ack_delay += 31000; }
        else if (step == 7) rx = 0x5d;
        else if (step == 8) rx = m->sector < 1024 ? (uint8_t)(m->sector >> 8) : 0xff;
        else if (step == 9) {
            rx = m->sector < 1024 ? (uint8_t)m->sector : 0xff;
            if (m->sector >= 1024) *ack_delay = 0;
        } else if (step < 138) {
            rx = m->data[m->sector * SECTOR_SIZE + step - 10]; m->checksum ^= rx;
        } else if (step == 138) rx = m->checksum;
        else { rx = 0x47; *ack_delay = 0; }
    } else {
        if (step < 134) {
            m->pending[step - 6] = tx; m->checksum ^= tx; rx = m->previous;
        } else if (step == 134) {
            m->result = m->sector >= 1024 ? 0xff : tx == m->checksum ? 0x47 : 0x4e;
            rx = m->previous;
        } else if (step == 135) rx = 0x5c;
        else if (step == 136) rx = 0x5d;
        else { rx = commit_sector(m); *ack_delay = 0; }
    }
    m->previous = tx;
    if (!*ack_delay) m->step = UINT_MAX;
    return rx;
}

#if defined(FF8_SAVESTATES)
#define FF8_SS_PRIVATE
#include "savestate_memcard.c"
#endif
