#include "hw.h"
#include "sio.h"
#include <string.h>

#define BLOCK_SIZE 8192u
#define SECTOR_SIZE 128u

static uint32_t le32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v) {
    unsigned i; for (i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static uint32_t fail(cpu_t *c, unsigned error) { c->hw->file_errno = error; return ~0u; }
static int path(cpu_t *c, uint32_t address, unsigned *slot, char name[21]) {
    char text[27]; unsigned i;
    for (i = 0; i < sizeof(text); ++i) {
        text[i] = (char)mem_read8(c, address + i);
        if (!text[i]) break;
    }
    if (i == sizeof(text) || i < 5 || text[0] != 'b' || text[1] != 'u' ||
        (text[2] != '0' && text[2] != '1') || text[3] != '0' || text[4] != ':') return 0;
    *slot = (unsigned)(text[2] - '0'); memset(name, 0, 21);
    if (i - 5 > 20) return 0;
    memcpy(name, text + 5, i - 5 + 1); return 1;
}
static int matches(const char *pattern, const uint8_t *name) {
    unsigned i;
    for (i = 0; i < 20; ++i) {
        if (pattern[i] == '*') return 1;
        if (!pattern[i]) return !name[i];
        if (!name[i] || (pattern[i] != '?' && (uint8_t)pattern[i] != name[i])) return 0;
    }
    return !pattern[20];
}
static int directory(memcard_t *card, unsigned block, uint8_t data[128]) {
    unsigned i, checksum = 0;
    if (block >= 15 || !memcard_read_sector(card, block + 1, data)) return 0;
    for (i = 0; i < 128; ++i) checksum ^= data[i];
    return !checksum;
}
static int write_directory(memcard_t *card, unsigned block, uint8_t data[128]) {
    unsigned i; data[127] = 0;
    for (i = 0; i < 127; ++i) data[127] ^= data[i];
    return memcard_write_sector(card, block + 1, data);
}
static int chain(memcard_t *card, unsigned first, unsigned ids[15], uint32_t *size) {
    uint8_t data[128]; unsigned count = 0, seen = 0, block = first;
    do {
        unsigned next;
        if (block >= 15 || (seen & (1u << block)) || !directory(card, block, data) ||
            data[0] != (count ? (data[8] == 255 && data[9] == 255 ? 0x53 : 0x52) : 0x51)) return 0;
        seen |= 1u << block; ids[count++] = block;
        if (count == 1) *size = le32(data + 4);
        next = data[8] | ((unsigned)data[9] << 8);
        if (next == 0xffff)
            return *size && !(*size % SECTOR_SIZE) && *size <= count * BLOCK_SIZE &&
                   *size > (count - 1) * BLOCK_SIZE ? (int)count : 0;
        block = next;
    } while (count < 15);
    return 0;
}
static int ready(cpu_t *c, unsigned slot) {
    memcard_t *card = sio_card(c, slot);
    uint8_t header[128];
    if (!card || memcard_failed(card)) { fail(c, 5); return 0; }
    bios_card_wait(c, slot);
    if (memcard_flags(card) & 8) {
        if (!bios_card_begin(c, slot * 16, BIOS_CARD_LOAD, 0, 0, 0) || bios_card_wait(c, slot) != 1) {
            fail(c, 5); return 0;
        }
    }
    memcard_read_sector(card, 0, header);
    if (header[0] != 'M' || header[1] != 'C') { fail(c, 5); return 0; }
    return 1;
}
static int find(memcard_t *card, const char *name, unsigned start, uint8_t data[128]) {
    unsigned i;
    for (i = start; i < 15; ++i)
        if (directory(card, i, data) && data[0] == 0x51 && matches(name, data + 10)) return (int)i;
    return -1;
}
static uint32_t open_file(cpu_t *c, unsigned slot, const char *name, unsigned mode) {
    struct hw *h = c->hw;
    memcard_t *card = sio_card(c, slot);
    uint8_t data[128]; unsigned fd, ids[15], i, count = 0;
    uint32_t size = 0; int block;
    if (!*name || !mode) return fail(c, 0x16);
    if (!ready(c, slot)) return ~0u;
    for (fd = 2; fd < 16 && h->files[fd].used; ++fd) {}
    if (fd == 16) return fail(c, 0x18);
    block = find(card, name, 0, data);
    if (mode & 0x200) {
        unsigned blocks = mode >> 16;
        if (block >= 0) return fail(c, 0x11);
        if (!blocks || blocks > 15 || strchr(name, '*') || strchr(name, '?')) return fail(c, 0x16);
        for (i = 0; i < 15; ++i) {
            if (!directory(card, i, data)) return fail(c, 5);
            if ((data[0] & 0xf0) == 0xa0) ids[count++] = i;
        }
        if (count < blocks) return fail(c, 0x1c);
        size = blocks * BLOCK_SIZE; block = (int)ids[0];
        for (i = blocks; i-- > 0;) {
            memset(data, 0, sizeof(data));
            data[0] = i == 0 ? 0x51 : i + 1 == blocks ? 0x53 : 0x52;
            if (!i) { put32(data + 4, size); memcpy(data + 10, name, strlen(name)); }
            data[8] = i + 1 < blocks ? (uint8_t)ids[i + 1] : 255;
            data[9] = i + 1 < blocks ? 0 : 255;
            if (!write_directory(card, ids[i], data)) return fail(c, 5);
        }
    } else if (block < 0) return fail(c, 2);
    else if (!chain(card, (unsigned)block, ids, &size)) return fail(c, 5);
    memset(&h->files[fd], 0, sizeof(h->files[fd]));
    h->files[fd].used = 1; h->files[fd].slot = slot; h->files[fd].block = (unsigned)block;
    h->files[fd].size = size; h->files[fd].mode = mode;
    return fd;
}
static bios_file_t *file(cpu_t *c, unsigned fd) {
    if (fd < 2 || fd >= 16 || !c->hw->files[fd].used) { fail(c, 9); return NULL; }
    return &c->hw->files[fd];
}
unsigned bios_file_complete(cpu_t *c, unsigned slot) {
    bios_card_t *op = &c->hw->cards[slot];
    bios_file_t *f = &c->hw->files[op->file];
    memcard_t *card = sio_card(c, slot);
    unsigned ids[15], i, j; uint32_t size = 0; uint8_t data[128];
    if (!chain(card, f->block, ids, &size) || size != f->size) return 0x8000;
    for (i = 0; i < op->length; i += SECTOR_SIZE) {
        uint32_t pos = f->offset + i;
        unsigned sector = (ids[pos / BLOCK_SIZE] + 1) * 64 + (pos % BLOCK_SIZE) / SECTOR_SIZE;
        if (op->operation == BIOS_CARD_FILE_WRITE) {
            for (j = 0; j < 128; ++j) data[j] = mem_read8(c, op->buffer + i + j);
            if (!memcard_write_sector(card, sector, data)) return 0x8000;
        } else {
            if (!memcard_read_sector(card, sector, data)) return 0x8000;
            for (j = 0; j < 128; ++j) mem_write8(c, op->buffer + i + j, data[j]);
        }
    }
    f->offset += op->length; return 4;
}
static uint32_t transfer(cpu_t *c, unsigned fd, uint32_t buffer, uint32_t length, int write) {
    bios_file_t *f = file(c, fd);
    bios_card_t *op;
    if (!f) return ~0u;
    if (f->busy || length % 128 || f->offset % 128 || !length ||
        length > RAM_SIZE || (buffer & 0x1fffffffu) > RAM_SIZE - length) return fail(c, 0x16);
    if (f->offset >= f->size) return 0;
    if (length > f->size - f->offset) length = f->size - f->offset;
    if (!bios_card_begin(c, f->slot * 16, write ? BIOS_CARD_FILE_WRITE : BIOS_CARD_FILE_READ, 0, buffer, 1)) return fail(c, 0x10);
    op = &c->hw->cards[f->slot]; op->file = fd; op->length = length;
    op->due = c->cycles + (uint64_t)(HW_CPU_HZ / 30) * (length / 128);
    f->busy = 1; hw_reschedule(c);
    if (f->mode & 0x8000) return 0;
    return bios_card_wait(c, f->slot) == 1 ? length : fail(c, 5);
}
static uint32_t next_file(cpu_t *c, uint32_t destination) {
    struct hw *h = c->hw;
    uint8_t data[128]; unsigned i;
    int block = find(sio_card(c, h->find_slot), h->find_pattern, h->find_index, data);
    if (block < 0) return 0;
    h->find_index = (unsigned)block + 1;
    for (i = 0; i < 40; ++i) mem_write8(c, destination + i, 0);
    for (i = 0; i < 20; ++i) mem_write8(c, destination + i, data[10 + i]);
    mem_write32(c, destination + 20, 0x50); mem_write32(c, destination + 24, le32(data + 4));
    mem_write32(c, destination + 32, ((unsigned)block + 1) * 64);
    return destination;
}
int bios_file_call(cpu_t *c, unsigned table, unsigned fn) {
    struct hw *h = c->hw;
    uint32_t a = c->r[4], b = c->r[5], n = c->r[6], result = ~0u;
    bios_file_t *f; unsigned slot = 0, i, ids[15];
    char name[21]; uint8_t data[128]; uint32_t size = 0; int block, count;
    int device_firstfile = table == 0 && fn == 0x69;
    if (device_firstfile) { a = b; b = n; fn = 0x42; }
    else if (table == 0 && fn <= 4) fn += 0x32;
    else if (table != 1) return 0;
    switch (fn) {
    case 0x32:
        result = path(c, a, &slot, name) ? open_file(c, slot, name, b) : fail(c, 0x13); break;
    case 0x33:
        f = file(c, a);
        if (f) {
            int64_t pos = (int32_t)b;
            if (n == 1) pos += f->offset;
            else if (n == 2) pos += f->size;
            if (f->busy || n > 2 || pos < 0 || pos > f->size) result = fail(c, 0x16);
            else result = f->offset = (uint32_t)pos;
        }
        break;
    case 0x34: case 0x35: result = transfer(c, a, b, n, fn == 0x35); break;
    case 0x36:
        f = file(c, a);
        if (f) { if (f->busy) bios_card_wait(c, f->slot); memset(f, 0, sizeof(*f)); result = a; }
        break;
    case 0x41:
        result = 0;
        if (!path(c, a, &slot, name) || *name || !sio_card(c, slot)) { fail(c, 0x13); break; }
        bios_card_wait(c, slot);
        if (memcard_format(sio_card(c, slot))) {
            for (i = 2; i < 16; ++i) if (h->files[i].used && h->files[i].slot == slot) h->files[i].used = 0;
            if (bios_card_begin(c, slot * 16, BIOS_CARD_LOAD, 0, 0, 1)) result = bios_card_wait(c, slot) == 1;
        } else fail(c, 5);
        break;
    case 0x42:
        result = 0;
        if (!path(c, a, &slot, name) || !ready(c, slot)) break;
        if (!device_firstfile) {
            uint32_t base = mem_read32(c, 0x150), entries = mem_read32(c, 0x154) / 0x50;
            uint32_t args[3] = {c->r[4], c->r[5], c->r[6]};
            for (i = 0; i < entries && i < 10; ++i) {
                uint32_t device = base + i * 0x50, label = mem_read32(c, device);
                if (mem_read8(c, label) == 'b' && mem_read8(c, label + 1) == 'u' && !mem_read8(c, label + 2)) {
                    uint32_t handler = mem_read32(c, device + 0x34);
                    mem_write32(c, BIOS_FCB_BASE, 0);
                    c->r[4] = BIOS_FCB_BASE; c->r[5] = a; c->r[6] = b;
                    result = bios_guest_call(c, handler);
                    break;
                }
            }
            c->r[4] = args[0]; c->r[5] = args[1]; c->r[6] = args[2];
            break;
        }
        h->find_slot = slot; h->find_index = 0; memcpy(h->find_pattern, name, sizeof(name));
        result = next_file(c, b); break;
    case 0x43: result = next_file(c, a); break;
    case 0x45:
        result = 0;
        if (!path(c, a, &slot, name) || !ready(c, slot)) break;
        block = find(sio_card(c, slot), name, 0, data);
        if (block < 0) { fail(c, 2); break; }
        count = chain(sio_card(c, slot), (unsigned)block, ids, &size);
        if (!count) { fail(c, 5); break; }
        for (i = 2; i < 16; ++i) if (h->files[i].used && h->files[i].slot == slot && h->files[i].block == (unsigned)block) break;
        if (i < 16) { fail(c, 0x10); break; }
        for (i = 0; i < (unsigned)count; ++i) {
            directory(sio_card(c, slot), ids[i], data); data[0] = (data[0] & 15) | 0xa0;
            if (!write_directory(sio_card(c, slot), ids[i], data)) { fail(c, 5); break; }
        }
        result = i == (unsigned)count; break;
    case 0x54: result = h->file_errno; break;
    case 0x55: f = file(c, a); result = f ? f->error : ~0u; break;
    default: return 0;
    }
    c->r[2] = result; return 1;
}
