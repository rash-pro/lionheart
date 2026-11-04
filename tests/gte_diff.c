/* Run both GTE paths against the scalar implementation. Optional arguments are FF8GTE1 traces
 * produced by tools/gte_trace.c. No disc-derived data belongs in the repo. */
#include "cpu.h"
#include "gte_specialized.h"
#include <stdio.h>
#include <stdlib.h>

void gte_reference_cmd(cpu_t *c, uint32_t opcode);
void gte_reference_write(cpu_t *c, unsigned reg, uint32_t value, int control);
uint32_t gte_reference_read(cpu_t *c, unsigned reg, int control);

static uint32_t seed = 0x89348721u;
static uint32_t random_word(void) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

static int compare(cpu_t *a, cpu_t *b, uint32_t opcode, unsigned index) {
    unsigned r;
    uint32_t data[32], control[32], expected_data[32], expected_control[32];
    memcpy(data, a->gte_data, sizeof(data));
    memcpy(control, a->gte_ctrl, sizeof(control));
    gte_cmd(a, opcode);
    memcpy(expected_data, a->gte_data, sizeof(data));
    memcpy(expected_control, a->gte_ctrl, sizeof(control));
    memcpy(a->gte_data, data, sizeof(data));
    memcpy(a->gte_ctrl, control, sizeof(control));
    --a->gte_commands;
    specialized_command(a, opcode);
    if (memcmp(a->gte_data, expected_data, sizeof(data)) ||
        memcmp(a->gte_ctrl, expected_control, sizeof(control))) {
        printf("GTE SPECIALIZED FAIL record=%u opcode=%08x\n", index, (unsigned)opcode);
        return 0;
    }
    gte_reference_cmd(b, opcode);
    for (r = 0; r < 64; ++r) {
        uint32_t x = r < 32 ? a->gte_data[r] : a->gte_ctrl[r - 32];
        uint32_t y = r < 32 ? b->gte_data[r] : b->gte_ctrl[r - 32];
        if (x != y || gte_read(a, r & 31, r >= 32) != gte_reference_read(b, r & 31, r >= 32)) {
            printf("GTE DIFF FAIL record=%u opcode=%08x register=%u actual=%08x expected=%08x\n",
                   index, (unsigned)opcode, r, (unsigned)x, (unsigned)y);
            return 0;
        }
    }
    return a->gte_commands == b->gte_commands;
}

static int replay(cpu_t *a, cpu_t *b, const char *path) {
    char magic[8];
    uint32_t record[131];
    unsigned count = 0, opcodes[64] = {0}, r;
    size_t bytes;
    FILE *file = fopen(path, "rb");
    if (!file || fread(magic, 1, 8, file) != 8 || memcmp(magic, "FF8GTE1", 8)) {
        printf("GTE TRACE FAIL header path=%s\n", path);
        if (file) fclose(file);
        return 0;
    }
    while ((bytes = fread(record, 1, sizeof(record), file)) == sizeof(record)) {
        memcpy(a->gte_data, record + 3, 128); memcpy(b->gte_data, record + 3, 128);
        memcpy(a->gte_ctrl, record + 35, 128); memcpy(b->gte_ctrl, record + 35, 128);
        if (!compare(a, b, record[2], count)) { fclose(file); return 0; }
        if (memcmp(a->gte_data, record + 67, 128) || memcmp(a->gte_ctrl, record + 99, 128)) {
            printf("GTE TRACE FAIL recorded output record=%u vb=%u pc=%08x\n",
                   count, (unsigned)record[0], (unsigned)record[1]);
            fclose(file); return 0;
        }
        ++opcodes[record[2] & 63]; ++count;
    }
    if (bytes || ferror(file) || !count) {
        printf("GTE TRACE FAIL truncated, unreadable or empty path=%s\n", path);
        fclose(file); return 0;
    }
    fclose(file);
    printf("GTE TRACE PASS path=%s records=%u opcodes=", path, count);
    for (r = 0; r < 64; ++r) if (opcodes[r]) printf(" %02x:%u", r, opcodes[r]);
    putchar('\n');
    return 1;
}

int main(int argc, char **argv) {
    static const unsigned commands[] = {1, 6, 12, 16, 17, 18, 19, 20, 22, 27, 28,
                                       30, 32, 40, 41, 42, 45, 46, 48, 61, 62, 63};
    static const uint32_t extremes[] = {0, 1, 0x7fff, 0x8000, 0xffff, 0x7fffffff,
                                      0x80000000, 0xffffffff, 0xffff8000};
    cpu_t *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
    unsigned i, j;
    int ok = a && b;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (i = 0; ok && i < 100000; ++i) {
        uint32_t options;
        for (j = 0; j < 8; ++j) {
            unsigned reg = random_word() & 63;
            uint32_t value = (i & 1) ? random_word() : extremes[random_word() % 9];
            gte_write(a, reg & 31, value, reg >= 32);
            gte_reference_write(b, reg & 31, value, reg >= 32);
        }
        options = random_word() & ((1u << 19) | (63u << 13) | (1u << 10));
        ok = compare(a, b, commands[i % 22] | options, i);
    }
    if (ok) puts("GTE SYNTHETIC PASS commands=100000 raw_registers=64 transfers=64");
    if (ok) {
        static const int32_t edge[] = {-32768, 0, 32767};
        /* Every corner includes products near signed32's limits and areas
         * outside signed32. Only the products may be narrowed. */
        for (i = 0; ok && i < 729; ++i) {
            unsigned digits = i;
            for (j = 0; j < 3; ++j) {
                int32_t x = edge[digits % 3], y;
                uint32_t xy;
                digits /= 3; y = edge[digits % 3]; digits /= 3;
                xy = ((uint32_t)x & 65535u) | ((uint32_t)y << 16);
                gte_write(a, 12 + j, xy, 0);
                gte_reference_write(b, 12 + j, xy, 0);
            }
            ok = compare(a, b, 6, i);
        }
        if (ok) puts("GTE NCLIP PASS corners=729");
    }
    if (ok) {
        unsigned z, h;
        memset(a->gte_data, 0, 128); memset(a->gte_ctrl, 0, 128);
        memset(b->gte_data, 0, 128); memset(b->gte_ctrl, 0, 128);
        gte_write(a, 27, 1, 1); gte_reference_write(b, 27, 1, 1);
        /* Zero rotation and TRZ=z project exactly z. DQA=1 exposes the UNR
         * quotient in MAC0. Sweep all depths and both sides of H=2*SZ3. */
        for (z = 0; ok && z < 65536; ++z) {
            unsigned values[6] = {0, 1, z / 2, z, z ? z * 2 - 1 : 0, z * 2};
            gte_write(a, 7, z, 1); gte_reference_write(b, 7, z, 1);
            for (h = 0; ok && h < 6; ++h) {
                unsigned value = values[h] > 65535 ? 65535 : values[h];
                gte_write(a, 26, value, 1); gte_reference_write(b, 26, value, 1);
                ok = compare(a, b, 1 | (1u << 19), z * 6 + h);
            }
        }
        if (ok) puts("GTE DIVIDE PASS depths=65536 commands=393216");
    }
    for (i = 1; ok && i < (unsigned)argc; ++i) ok = replay(a, b, argv[i]);
    free(a); free(b);
    if (ok) puts("GTE DIFF PASS");
    return ok ? 0 : 2;
}
