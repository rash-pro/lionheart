#include "frame_oracle.h"
#if defined(FF8_RECORD_FRAME_ORACLE)
#include "cpu.h"
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Independent diagnostic observer. It never reads frame_tags, its identities,
 * intern tables, transform-context policy, or projection cache. The binary
 * records contain actual hardware inputs plus a separate symbolic copy ledger
 * for the matrix's load slots. Arithmetic-produced slots remain unproven until
 * stored and loaded from memory. Unknown slots cannot pass the oracle. */
typedef struct { uint32_t address; uint64_t generation; } oracle_slot_t;
typedef struct {
    uint64_t observation;
    uint32_t frame, pc, vxy, vz, matrix[8], slots[8];
    uint64_t generations[8];
} oracle_record_t;
typedef char oracle_record_size[(sizeof(oracle_record_t) == 152) ? 1 : -1];
struct frame_oracle {
    oracle_slot_t registers[32], pending, controls[8], *memory;
    oracle_slot_t saved[33];
    uint64_t *generations, generation, observations;
    unsigned first;
    FILE *file;
};

static unsigned cell(uint32_t address) {
    address &= 0x1fffffffu;
    if (address < 0x800000) return (address & 0x1fffff) / 4;
    if (address >= 0x1f800000 && address < 0x1f800400) return RAM_SIZE / 4 + (address & 1023) / 4;
    return UINT32_MAX;
}

int frame_oracle_enable(cpu_t *c) {
    struct frame_oracle *o;
    const char *path = getenv("FF8_FRAME_ORACLE"), *first;
    if (c->frame_oracle || !path || !*path) return 1;
    o = calloc(1, sizeof(*o));
    if (!o) return 0;
    c->frame_oracle = o;
    o->memory = calloc((RAM_SIZE + 1024) / 4, sizeof(*o->memory));
    o->generations = calloc((RAM_SIZE + 1024) / 4, sizeof(*o->generations));
    o->file = fopen(path, "wb");
    if (!o->memory || !o->generations || !o->file) { frame_oracle_destroy(c); return 0; }
    first = getenv("FF8_ORACLE_FIRST");
    o->first = first ? (unsigned)strtoul(first, NULL, 0) : 0;
    o->generation = 1;
    o->registers[0].address = UINT32_MAX;
    {
        unsigned i;
        for (i = 0; i < 8; ++i) o->controls[i].address = UINT32_MAX - i;
    }
    setvbuf(o->file, NULL, _IOFBF, 1024 * 1024);
    fwrite("FRAMEORACLE00001", 1, 16, o->file);
    return 1;
}

void frame_oracle_destroy(cpu_t *c) {
    struct frame_oracle *o = c->frame_oracle;
    if (!o) return;
    if (o->file) fclose(o->file);
    free(o->memory); free(o->generations); free(o); c->frame_oracle = NULL;
}

static oracle_slot_t read_slot(struct frame_oracle *o, uint32_t address) {
    unsigned index = cell(address);
    oracle_slot_t slot = {0, 0};
    if (index == UINT32_MAX) return slot;
    slot = o->memory[index];
    if (!slot.address) {
        slot.address = index * 4 + 1;
        slot.generation = o->generations[index] ? o->generations[index] : 1;
    }
    return slot;
}

void frame_oracle_write(cpu_t *c, uint32_t address, unsigned width) {
    struct frame_oracle *o = c->frame_oracle;
    unsigned index = cell(address), last = cell(address + width - 1);
    if (!o || index == UINT32_MAX || last == UINT32_MAX) return;
    for (; index <= last; ++index) memset(&o->memory[index], 0, sizeof(o->memory[index]));
}

void frame_oracle_reload(cpu_t *c, uint32_t address, unsigned width) {
    struct frame_oracle *o = c->frame_oracle;
    unsigned index = cell(address), last = cell(address + width - 1);
    if (!o || index == UINT32_MAX || last == UINT32_MAX) return;
    ++o->generation;
    for (; index <= last; ++index) { o->generations[index] = o->generation; o->memory[index].address = 0; }
}

void frame_oracle_instruction(cpu_t *c, uint32_t word, uint32_t a, uint32_t b) {
    struct frame_oracle *o = c->frame_oracle;
    unsigned op = word >> 26, rs = (word >> 21) & 31, rt = (word >> 16) & 31;
    unsigned rd = (word >> 11) & 31, fn = word & 63, dst = 0, load = 0;
    uint32_t address = a + (uint32_t)(int32_t)(int16_t)word;
    oracle_slot_t value = {0, 0}, loaded = {0, 0};
    if (!o || c->halted) return;
    if (!op) {
        if (fn == 0 || fn == 2 || fn == 3 || fn == 4 || fn == 6 || fn == 7 ||
            fn == 9 || fn == 16 || fn == 18 || fn >= 32) dst = rd;
        if ((fn == 0 || fn == 2 || fn == 3) && !((word >> 6) & 31)) value = o->registers[rt];
        if (fn == 33 || fn == 37) {
            if (!a) value = o->registers[rt]; else if (!b) value = o->registers[rs];
        }
        if (fn == 35 && !b) value = o->registers[rs];
    } else if (op == 1 && (rt == 16 || rt == 17)) dst = 31;
    else if (op == 3) dst = 31;
    else if (op >= 8 && op <= 15) {
        dst = rt;
        if ((op == 8 || op == 9 || op == 13 || op == 14) && !(word & 65535)) value = o->registers[rs];
        else if (op == 15 || (o->registers[rs].address & 0x80000000u)) {
            value.address = 0x80000000u | (c->pc & 0x1fffffu); value.generation = word;
        }
    } else if ((op == 16 || op == 18) && (rs == 0 || rs == 2)) {
        load = rt;
        if (op == 18 && rs == 2 && rd < 8) loaded = o->controls[rd];
    } else if (op == 18 && rs == 6 && rd < 8) o->controls[rd] = o->registers[rt];
    else if (op >= 32 && op <= 38 && op != 34 && op != 38) { load = rt; loaded = read_slot(o, address); }
    else if (op == 34 || op == 38) load = rt; /* Unaligned matrix sources are explicitly unproven. */
    else if (op == 43) {
        unsigned index = cell(address);
        if (index != UINT32_MAX && !(address & 3)) o->memory[index] = o->registers[rt];
    }
    if (c->load_reg && c->load_reg != dst && c->load_reg != load) o->registers[c->load_reg] = o->pending;
    if (dst) o->registers[dst] = value;
    o->pending = loaded;
    o->registers[0].address = UINT32_MAX; o->registers[0].generation = 0;
}

void frame_oracle_projection(cpu_t *c, unsigned v) {
    struct frame_oracle *o = c->frame_oracle;
    oracle_record_t record;
    unsigned i;
    if (!o) return;
    memset(&record, 0, sizeof(record));
    record.observation = ++o->observations;
    if (c->hw->vblanks < o->first) return;
    record.frame = (uint32_t)c->hw->vblanks; record.pc = c->pc;
    record.vxy = c->gte_data[v * 2]; record.vz = c->gte_data[v * 2 + 1];
    for (i = 0; i < 8; ++i) {
        record.matrix[i] = c->gte_ctrl[i]; record.slots[i] = o->controls[i].address;
        record.generations[i] = o->controls[i].generation;
    }
    fwrite(&record, sizeof(record), 1, o->file);
}

void frame_oracle_irq(cpu_t *c, int enter) {
    struct frame_oracle *o = c->frame_oracle;
    if (!o) return;
    if (enter) { memcpy(o->saved, o->registers, sizeof(o->registers)); o->saved[32] = o->pending; }
    else { memcpy(o->registers, o->saved, sizeof(o->registers)); o->pending = o->saved[32]; }
}
#endif
