// Play DMA grants must retain pixels, bus ownership and cancellation safety.
#include "lh_test.h"
#include "mdec_calendar_model.h"
#include "values.h"

#include <array>
#include <cstdint>

namespace {
const char *const kPlayCases[] = {"mdec", "dma", "movie-overlap", "movie-priority", "movie-cancel", "movie-late-gpu"};

struct Row {
    const char *id;
    const char *observer;
    unsigned depth;
};
const Row kRows[] = {{"mdec-2", "mdec", 2}, {"mdec-3", "mdec", 3}, {"spu-2", "spu", 2}, {"spu-3", "spu", 3}};
} // namespace

LH_TEST_P(test_play_grants, kPlayCases) { t.ok(t.harness("play").arg(p).timeout(60)); }

LH_TEST_P(test_aliasing_input_output_keep_word_observations, kRows) {
    const bool spu = std::string(p.observer) == "spu";
    const CalendarModel models[2] = {CalendarModel(t.library("alias_exact")), CalendarModel(t.library("alias_play"))};
    void *cpus[2] = {models[0].create(p.depth, 3, 0), models[1].create(p.depth, 3, 0)};
    const auto release = [&] {
        models[0].destroy(cpus[0]);
        models[1].destroy(cpus[1]);
    };
    try {
        for (int i = 0; i < 2; ++i) {
            // MDEC output or SPU DMA overwrites future compressed input.
            // Observations must retain the original interleaving.
            models[i].write(cpus[i], 0x1f801090, 4, spu ? 0x100000 : 0x2000);
            models[i].write(cpus[i], 0x1f801094, 4, 0x10000 | (p.depth == 2 ? 19200 : 12800));
            models[i].write(cpus[i], 0x1f801098, 4, 0x01000200);
            if (spu) {
                models[i].write(cpus[i], 0x1f8010c0, 4, 0x2000);
                models[i].write(cpus[i], 0x1f8010c4, 4, 0x10000 | 1024);
                models[i].write(cpus[i], 0x1f8010c8, 4, 0x01000200);
            }
        }
        for (uint64_t cycle = 0; cycle < 20000; cycle += 7) {
            models[0].advance(cpus[0], cycle);
            models[1].advance(cpus[1], cycle);
            const int compared = models[1].compare(cpus[0], cpus[1]);
            LH_CHECK_MSG(compared == 0, lh::str("cycle=", cycle, " compare=", compared));
        }
    } catch (...) {
        release();
        throw;
    }
    release();
}
