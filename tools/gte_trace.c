/* Host diagnostic, linked with --wrap=gte_cmd. Traces stay under build/.
 * FF8_GTE_TRACE names the output; FIRST/LAST select inclusive VBlanks.
 * Each little-endian record holds vb, pc, opcode, 64 input and 64 output
 * raw registers. Raw copies retain FIFO aliases and write-only registers. */
#include "cpu.h"
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef FF8_GTE_CAPTURE_FRAMES
#include "display.h"

/* host_present's first call is VBlank 1; the regular host PNG filenames start
 * at zero. Capture just three matching guest VBlanks without dumping a movie. */
void __real_host_present(gpu_t *g);
void __wrap_host_present(gpu_t *g) {
    static unsigned vb;
    const char *directory = getenv("FF8_GTE_FRAMES");
    __real_host_present(g);
    ++vb;
    if (directory && vb >= 1800 && vb <= 3000 && vb % 600 == 0) {
        char path[1024];
        int length = snprintf(path, sizeof(path), "%s/frame-%06u.png", directory, vb);
        if (length < 0 || (size_t)length >= sizeof(path) || !host_display_dump(g, path)) {
            fputs("GTE frame capture failed\n", stderr); exit(2);
        }
    }
}
#endif

static FILE *trace;
static unsigned initialized, first, last;
static unsigned long count, limit;

static void finish(void) {
    if (trace) {
        if (fclose(trace)) { perror("GTE trace close"); exit(2); }
        printf("GTE TRACE records=%lu\n", count);
    }
}

static unsigned depth;

static int begin_record(cpu_t *c, uint32_t opcode, uint32_t record[131]) {
    unsigned vb = c->hw ? (unsigned)c->hw->vblanks : 0;
    int capture;
    if (depth++) return 0;
    if (!initialized) {
        const char *path = getenv("FF8_GTE_TRACE");
        const char *begin = getenv("FF8_GTE_TRACE_FIRST");
        const char *end = getenv("FF8_GTE_TRACE_LAST");
        const char *maximum = getenv("FF8_GTE_TRACE_LIMIT");
        initialized = 1;
        first = begin ? (unsigned)strtoul(begin, NULL, 10) : 0;
        last = end ? (unsigned)strtoul(end, NULL, 10) : UINT32_MAX;
        limit = maximum ? strtoul(maximum, NULL, 10) : 131072ul;
        if (path) {
            if (first > last || !limit) {
                fputs("GTE trace requires FIRST <= LAST and a positive LIMIT\n", stderr);
                exit(2);
            }
            trace = fopen(path, "wb");
            if (!trace || fwrite("FF8GTE1", 1, 8, trace) != 8) {
                perror("GTE trace open"); exit(2);
            }
            atexit(finish);
        }
    }
    capture = trace && vb >= first && vb <= last;
    if (capture) {
        if (count == limit) {
            fputs("GTE trace command limit reached; select a shorter VBlank window\n", stderr);
            exit(2);
        }
        record[0] = vb; record[1] = c->pc; record[2] = opcode;
        memcpy(record + 3, c->gte_data, 128);
        memcpy(record + 35, c->gte_ctrl, 128);
    }
    return capture;
}

static void end_record(cpu_t *c, uint32_t record[131], int capture) {
    --depth;
    if (capture) {
        memcpy(record + 67, c->gte_data, 128);
        memcpy(record + 99, c->gte_ctrl, 128);
        if (fwrite(record, 131 * sizeof(*record), 1, trace) != 1) {
            perror("GTE trace write"); exit(2);
        }
        ++count;
    }
}

void __real_gte_cmd(cpu_t *c, uint32_t opcode);
void __wrap_gte_cmd(cpu_t *c, uint32_t opcode) {
    uint32_t record[131];
    int capture = begin_record(c, opcode, record);
    __real_gte_cmd(c, opcode);
    end_record(c, record, capture);
}

#ifdef FF8_GTE_SPECIALIZED_TRACE
/* Reconstruct meaningful fields; nested wrappers record the command once. */
#define GTE_TRACE_PLAIN(name, command) \
void __real_gte_##name(cpu_t *c); \
void __wrap_gte_##name(cpu_t *c) { \
    uint32_t record[131]; \
    int capture = begin_record(c, 0x4a000000u | (command), record); \
    __real_gte_##name(c); \
    end_record(c, record, capture); \
}
#define GTE_TRACE_OPTIONS(name, command) \
void __real_gte_##name(cpu_t *c, unsigned shift, int lm); \
void __wrap_gte_##name(cpu_t *c, unsigned shift, int lm) { \
    uint32_t record[131]; \
    uint32_t word = 0x4a000000u | (command) | ((shift == 12) << 19) | ((unsigned)lm << 10); \
    int capture = begin_record(c, word, record); \
    __real_gte_##name(c, shift, lm); \
    end_record(c, record, capture); \
}
GTE_TRACE_PLAIN(nclip, 0x06)
GTE_TRACE_PLAIN(avsz3, 0x2d)
GTE_TRACE_PLAIN(avsz4, 0x2e)
GTE_TRACE_OPTIONS(rtps, 0x01)
GTE_TRACE_OPTIONS(op, 0x0c)
GTE_TRACE_OPTIONS(dpcs, 0x10)
GTE_TRACE_OPTIONS(intpl, 0x11)
GTE_TRACE_OPTIONS(ncds, 0x13)
GTE_TRACE_OPTIONS(cdp, 0x14)
GTE_TRACE_OPTIONS(ncdt, 0x16)
GTE_TRACE_OPTIONS(nccs, 0x1b)
GTE_TRACE_OPTIONS(cc, 0x1c)
GTE_TRACE_OPTIONS(ncs, 0x1e)
GTE_TRACE_OPTIONS(nct, 0x20)
GTE_TRACE_OPTIONS(sqr, 0x28)
GTE_TRACE_OPTIONS(dcpl, 0x29)
GTE_TRACE_OPTIONS(dpct, 0x2a)
GTE_TRACE_OPTIONS(rtpt, 0x30)
GTE_TRACE_OPTIONS(gpf, 0x3d)
GTE_TRACE_OPTIONS(gpl, 0x3e)
GTE_TRACE_OPTIONS(ncct, 0x3f)

void __real_gte_mvmva(cpu_t *c, unsigned shift, int lm, unsigned mx, unsigned vs, unsigned cv);
void __wrap_gte_mvmva(cpu_t *c, unsigned shift, int lm, unsigned mx, unsigned vs, unsigned cv) {
    uint32_t record[131];
    uint32_t word = 0x4a000012u | ((shift == 12) << 19) | ((unsigned)lm << 10) |
                    (mx << 17) | (vs << 15) | (cv << 13);
    int capture = begin_record(c, word, record);
    __real_gte_mvmva(c, shift, lm, mx, vs, cv);
    end_record(c, record, capture);
}
#undef GTE_TRACE_PLAIN
#undef GTE_TRACE_OPTIONS
#endif
