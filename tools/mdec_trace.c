#include "hw.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

typedef struct {
    uint64_t cpu_cycle, device_cycle;
    uint32_t kind, address, width, value;
} observation_t;
static FILE *stream;
static unsigned initialized, replay;
static uint64_t counts[4], rows;

static void finish(void) {
    if (!stream) return;
    if (replay && fgetc(stream) != EOF) { fprintf(stderr, "MDEC TRACE FAIL: unread oracle rows\n"); exit(2); }
    if (ferror(stream) || fclose(stream)) { fprintf(stderr, "MDEC TRACE FAIL: I/O error\n"); exit(2); }
    stream = NULL;
    fflush(stdout);
    fprintf(stderr, "MDEC TRACE %s reads=%" PRIu64 " writes=%" PRIu64 " irqs=%" PRIu64 " ram=%" PRIu64 " rows=%" PRIu64 "\n",
            replay ? "PASS" : "RECORDED", counts[0], counts[1], counts[2], counts[3], rows);
}
static void record(cpu_t *c, unsigned kind, uint32_t address, unsigned width, uint32_t value) {
    observation_t row = {c->cycles, c->hw->now, kind, address, width, value}, expected;
    if (!stream) return;
    if (replay) {
        if (fread(&expected, sizeof(expected), 1, stream) != 1 || memcmp(&expected, &row, sizeof(row))) {
            fprintf(stderr, "MDEC TRACE FAIL row=%" PRIu64 " cycle=%" PRIu64 " device=%" PRIu64
                    " kind=%u address=%08x width=%u value=%08x\n", rows, row.cpu_cycle,
                    row.device_cycle, kind, address, width, value);
            exit(2);
        }
    } else if (fwrite(&row, sizeof(row), 1, stream) != 1) { perror("MDEC trace write"); exit(2); }
    ++rows; ++counts[kind];
}
void mdec_trace_observe(cpu_t *c, unsigned kind, uint32_t address, unsigned width, uint32_t value) {
    if (!initialized) {
        const char *path = getenv("FF8_MDEC_TRACE"), *oracle = getenv("FF8_MDEC_REPLAY");
        initialized = 1;
        if (path && oracle) { fprintf(stderr, "choose one MDEC trace mode\n"); exit(2); }
        if (oracle) { path = oracle; replay = 1; }
        if (path) {
            char magic[8];
            stream = fopen(path, replay ? "rb" : "wb");
            if (!stream) { perror(path); exit(2); }
            if (replay) {
                if (fread(magic, 8, 1, stream) != 1 || memcmp(magic, "FF8MDCT1", 8)) exit(2);
            } else if (fwrite("FF8MDCT1", 8, 1, stream) != 1) exit(2);
            if (atexit(finish)) exit(2);
        }
    }
    if (!stream) return;
    if (kind < 2 && !((address >= 0x1f801080 && address < 0x1f8010f8) ||
                     address == 0x1f801820 || address == 0x1f801824)) return;
    record(c, kind, address, width, value);
    if (kind == 2 && address == 0) {
        uint64_t hash = UINT64_C(14695981039346656037);
        unsigned i;
        for (i = 0; i < RAM_SIZE; ++i) { hash ^= c->ram[i]; hash *= UINT64_C(1099511628211); }
        record(c, 3, (uint32_t)hash, 0, (uint32_t)(hash >> 32));
    }
}
