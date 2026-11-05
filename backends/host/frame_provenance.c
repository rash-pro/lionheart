/* FIFO -> SWC2 -> DMA observation audit. No guest writes and no temporal matcher. */
#include "frame_provenance.h"
#include "cpu.h"
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t serial; uint32_t key, source, revision; } sample_t;
typedef struct { uint32_t xy, z, revision; } staging_t;
static sample_t fifo[3], *packets;
static staging_t *staging;
static FILE *output;
static uint64_t serial;
static int configured;

static void close_output(void) {
    if (output && fclose(output)) abort();
    free(packets); free(staging);
}
static int active(void) {
    if (!configured) {
        const char *path = getenv("FF8_FRAME_PROVENANCE");
        configured = 1;
        if (path && *path) {
            output = fopen(path, "w");
            packets = calloc(RAM_SIZE / 4, sizeof(*packets));
            staging = calloc(RAM_SIZE / 2, sizeof(*staging));
            if (!output || !packets || !staging) abort();
            if (setvbuf(output, NULL, _IOFBF, 1024 * 1024)) abort();
            fputs("kind,frame,pc,address,projection,source,key,event,revision\n", output);
            atexit(close_output);
        }
    }
    return output != NULL;
}
static void row(char kind, unsigned frame, uint32_t pc, uint32_t address,
                sample_t s, uint64_t event) {
    if (fprintf(output, "%c,%u,%u,%u,%llu,%u,%u,%llu,%u\n", kind, frame, pc,
                address, (unsigned long long)s.serial, s.source, s.key,
                (unsigned long long)event, s.revision) < 0) abort();
}
void frame_provenance_projection(const cpu_t *c, unsigned frame, uint32_t key,
                                 uint32_t source, unsigned vertex, uint64_t event) {
    sample_t s = {0};
    if (!c || !active()) return;
    s.serial = ++serial; s.key = key;
    if (source && source < RAM_SIZE - 7 && !(source & 1) && vertex < 3) {
        staging_t *p = &staging[source / 2];
        uint32_t xy = c->gte_data[vertex * 2], z = c->gte_data[vertex * 2 + 1];
        /* Changed values prove reuse. Equal values do not prove no writes:
         * this is an observed revision, not a certified staging generation. */
        if (!p->revision || p->xy != xy || p->z != z) ++p->revision;
        p->xy = xy; p->z = z;
        s.source = source; s.revision = p->revision;
    }
    fifo[0] = fifo[1]; fifo[1] = fifo[2]; fifo[2] = s;
    row('P', frame, c->pc, 0, s, event);
}
void frame_provenance_write(unsigned reg, int control) {
    if (control || reg < 12 || reg > 15 || !active()) return;
    if (reg == 15) {
        fifo[0] = fifo[1]; fifo[1] = fifo[2];
        memset(&fifo[2], 0, sizeof(fifo[2]));
    } else memset(&fifo[reg - 12], 0, sizeof(fifo[0]));
}
uint32_t __real_gte_read(cpu_t *, unsigned, int);
uint32_t __wrap_gte_read(cpu_t *c, unsigned reg, int control) {
    uint32_t value = __real_gte_read(c, reg, control);
    if (!control && reg >= 12 && reg <= 15 && active()) {
        uint32_t pc = c->pc & 0x1fffffff, instruction = 0, address = 0;
        sample_t s = fifo[reg == 15 ? 2 : reg - 12];
        if (pc < RAM_SIZE - 3 && !(pc & 3)) instruction = ram_read(c, pc, 4);
        if (s.key != value) memset(&s, 0, sizeof(s));
        s.key = value;
        if ((instruction >> 26) == 58 && ((instruction >> 16) & 31) == reg) {
            address = (c->r[(instruction >> 21) & 31] +
                       (uint32_t)(int32_t)(int16_t)instruction) & 0x1fffffff;
            if (address < RAM_SIZE && !(address & 3)) {
                packets[address / 4] = s;
                row('S', c->hw ? (unsigned)c->hw->vblanks : 0, c->pc, address, s, 0);
            }
        } else row('R', c->hw ? (unsigned)c->hw->vblanks : 0, c->pc, 0, s, 0);
    }
    return value;
}
void frame_provenance_dma(const cpu_t *c, unsigned frame, const uint8_t *data,
                          unsigned count, uint64_t event) {
    uintptr_t address;
    if (!c || !active() || (uintptr_t)data < (uintptr_t)c->ram) return;
    address = (uintptr_t)data - (uintptr_t)c->ram;
    if ((address & 3) || address >= RAM_SIZE || count > (RAM_SIZE - address) / 4) return;
    for (unsigned i = 0; i < count; ++i) {
        sample_t s = packets[address / 4 + i];
        uint32_t value = ram_read(c, (uint32_t)address + i * 4, 4);
        if (!s.serial) continue;
        if (s.key != value) {
            row('X', frame, c->pc, (uint32_t)address + i * 4, s, event + i);
            memset(&packets[address / 4 + i], 0, sizeof(s));
        } else {
            /* Equal bits can have a different writer. D means candidate only;
             * it never certifies packet, model, pass or vertex identity. */
            row('D', frame, c->pc, (uint32_t)address + i * 4, s, event + i);
        }
    }
}
