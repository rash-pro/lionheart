// Observe the span model against the original word calendar between deadlines.
#include "equiv.h"
#include "../../../tools/devtools/mdec_trace.h"
#include "lh_test.h"
#include "mdec_calendar_model.h"
#include "py_random.h"
#include "values.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct Row {
    std::string id;
    unsigned depth, layout, control;
};

// pytest builds this id from the innermost decorator outward: control, layout, depth.
std::vector<Row> rows() {
    std::vector<Row> out;
    for (unsigned depth = 0; depth < 4; ++depth) {
        for (unsigned layout = 0; layout < 4; ++layout) {
            for (unsigned control = 0; control < 2; ++control) {
                out.push_back({std::to_string(control) + "-" + std::to_string(layout) + "-" + std::to_string(depth),
                               depth, layout, control});
            }
        }
    }
    return out;
}

} // namespace

LH_TEST_P(test_word_calendar_observations, rows()) {
    LH_TEXT_EQ(trace::instrument_hw(lh::read_file(t.root()/"runtime/hw.c")),
               lh::read_file(lh::fs::path(t.harness("model_0").program()).parent_path().parent_path().parent_path()/"generated/test_mdec_calendar/hw.c"));
    const CalendarModel models[2] = {CalendarModel(t.library("model_0")), CalendarModel(t.library("model_1"))};
    void *cpus[2] = {models[0].create(p.depth, p.layout, p.control), models[1].create(p.depth, p.layout, p.control)};
    lh::PyRandom rng(1400 + p.depth * 16 + p.layout * 2 + p.control);

    const auto write_both = [&](uint32_t address, unsigned width, uint32_t value) {
        for (int i = 0; i < 2; ++i) models[i].write(cpus[i], address, width, value);
    };
    const auto ram_write_both = [&](uint32_t address, uint32_t value) {
        for (int i = 0; i < 2; ++i) models[i].ram_write(cpus[i], address, value);
    };
    const auto compare = [&] {
        const int compared = models[1].compare(cpus[0], cpus[1]);
        LH_CHECK_MSG(compared == 0, lh::str("compare=", compared));
        const unsigned count = models[0].irq_count();
        LH_EQ(count, models[1].irq_count());
        for (unsigned i = 0; i < count; ++i) {
            for (unsigned field = 0; field < 3; ++field) {
                const uint64_t a = models[0].irq(i, field), b = models[1].irq(i, field);
                LH_CHECK_MSG(a == b, lh::str("irq ", i, " field ", field, ": ", a, " vs ", b));
            }
        }
    };
    const auto release = [&] {
        models[0].destroy(cpus[0]);
        models[1].destroy(cpus[1]);
    };

    try {
        const unsigned output_words = p.depth < 2 ? 600 * (p.depth == 0 ? 8 : 16) : 100 * (p.depth == 2 ? 192 : 128);
        const uint32_t destination = p.layout == 3 ? 0x2000 : 0x10000;
        write_both(0x1f801090, 4, destination);
        write_both(0x1f801094, 4, 0x10000 | output_words);
        write_both(0x1f801098, 4, 0x01000200);
        uint64_t cycle = 0;
        // Single-cycle observations cover exact before/on/after boundaries;
        // random long gaps exercise the lazy path and overlapping grants.
        for (int step = 0; step < 600; ++step) {
            cycle += step < 80 ? 1 : static_cast<uint64_t>(rng.randrange(1, 1300));
            lh::equiv::record(t, cycle);
            for (int i = 0; i < 2; ++i) models[i].advance(cpus[i], cycle);
            compare();
            for (uint32_t address : {0x1f801824u, 0x1f801080u, 0x1f801084u, 0x1f801088u,
                                     0x1f801090u, 0x1f801094u, 0x1f801098u, 0x1f8010f4u}) {
                const uint32_t a = models[0].read(cpus[0], address, 4), b = models[1].read(cpus[1], address, 4);
                LH_CHECK_MSG(a == b, lh::str("step=", step, " cycle=", cycle, " address=0x", std::hex, address, " values ", a, ", ", b));
            }
            if (step == 100 || step == 200) {
                // A source mutation must be observed at its real DMA read,
                // including when it invalidates the preview or crosses a fence.
                ram_write_both(0x2000u + static_cast<uint32_t>(step) * 4, 0xfe000409);
            }
            if (step == 120) {
                // A competing DMA4 observes SPU state and CPU RAM mid-movie.
                write_both(0x1f801da4, 2, 0x201);
                write_both(0x1f801da6, 2, 0x200);
                write_both(0x1f801daa, 2, 0xc060);
                write_both(0x1f8010c0, 4, 0x2000);
                write_both(0x1f8010c4, 4, 0x10020);
                write_both(0x1f8010c8, 4, 0x01000201);
            }
            if (step % 25 == 0) {
                const uint32_t a = models[0].read(cpus[0], 0x1f801dae, 2), b = models[1].read(cpus[1], 0x1f801dae, 2);
                LH_EQ(a, b);
                LH_EQ(models[1].spu_compare(cpus[0], cpus[1]), 0);
            }
            if (step == 300) {
                // Partial register writes and cancellation retain exact state.
                write_both(0x1f8010f4, 1, 0);
                write_both(0x1f801098, 4, 0);
            }
            if (step == 400) write_both(0x1f801824, 4, 0xe0000000);
            compare();
        }
        if (p.layout == 0) LH_CHECK(models[1].spans(cpus[1]) > 0);
    } catch (...) {
        release();
        throw;
    }
    release();
}
