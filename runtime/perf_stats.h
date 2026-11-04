#ifndef FF8_PERF_STATS_H
#define FF8_PERF_STATS_H

enum { C_MEM_RAM, C_MEM_SCRATCH, C_MEM_MMIO, C_MEM_OTHER,
       C_MEM_GAP, C_MEM_FENCE, C_MEM_WATCHED, C_MEM_COUNT };
enum { C_DISPATCH_CALL, C_DISPATCH_RETURN, C_DISPATCH_TABLE,
       C_DISPATCH_CROSS_PAGE, C_DISPATCH_COUNT };
enum { C_CANDIDATE_BACKEDGE, C_CANDIDATE_IDLE, C_CANDIDATE_POLL };
#define C_CANDIDATE_SLOTS 4096
typedef struct { uint32_t pc, kind; uint64_t hits; } candidate_stat_t;
typedef struct {
    uint64_t memory[C_MEM_COUNT], dispatch[C_DISPATCH_COUNT], candidate_lost;
    uint64_t native_cross_page;
    unsigned native_page;
    candidate_stat_t candidates[C_CANDIDATE_SLOTS];
} perf_stats_t;

#endif
