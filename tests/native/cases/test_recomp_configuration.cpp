// Small executable checks for the stats-off runtime and aligned guest loads.
#include "generated.h"

#include <array>
#include <map>
#include <vector>

using namespace lh::gen;

namespace {

using Replacements = std::vector<std::pair<std::string, std::string>>;

struct BoolRow { const char *id; bool value; };
static const BoolRow kDirect[] = {{"False", false}, {"True", true}};

LH_TEST_P(test_compact_deadlines_preserve_loads_branches_and_resume, kDirect) {
    // A load feeds a branch, its slot loads again, and the target calls a helper.
    // MMIO can shorten the deadline after compiled execution has already begun.
    const Words words = {0x03E0B021, 0x8E880000, 0x01004821, 0x8EAA0000,
                         0x11400002, 0x8E8B0004, 0x240C0063, 0x01606821,
                         0x0C004010, 0x01607021, 0x02C0F821, 0x03E00008, 0,
                         0, 0, 0, 0x25EF0001, 0x03E00008, 0};
    Program program{Image::words(words, 0x80010000)};
    const std::string data = program.image.data;
    Compiled library = Build(t)
        .program(program)
        .probe("test_recomp_configuration_boundary.c", {{"DATA_SIZE", std::to_string(data.size())}})
        .flags({"-std=c99", "-O2", "-DFF8_STATS", p.value ? "-DFF8_COMPACT" : "-DFF8_COMPACT=1"})
        .runtime({"cpu.c", "interp.c"})
        .library();
    auto check = library.fn<int(const void *, unsigned, unsigned, unsigned, uint32_t *)>("check");
    for (unsigned deadline = 1; deadline < 40; ++deadline) {
        for (unsigned scheduled : {0u, 2u, 4u, 6u, 10u, 30u}) {
            std::array<uint32_t, 117> compiled{}, reference{};
            LH_EQ(check(data.data(), 1, deadline, scheduled, reference.data()), 1);
            LH_EQ(check(data.data(), 0, deadline, scheduled, compiled.data()), 1);
            for (size_t index = 0; index < 115; ++index) {
                LH_CHECK_MSG(compiled[index] == reference[index],
                             "deadline=" + std::to_string(deadline) + " scheduled=" + std::to_string(scheduled));
            }
            LH_EQ(compiled[115] + compiled[116], reference[115]);
            LH_EQ(reference[115], 15u);
            if (deadline == 39 && scheduled == 0) {
                LH_EQ(compiled[115], 0u);
                LH_EQ(compiled[116], 15u);
            }
            if (deadline == 2) LH_CHECK(0 < compiled[115] && compiled[115] < 15 && compiled[116] > 0);
        }
    }
}

// Program-to-emit arguments shared by the fast/compact/trace/precise cases.
static Program emit_for(const Words &words, const std::string &mode) {
    Program program{Image::words(words, 0x80010000)};
    program.mode = mode == "trace" ? "trace" : "fast";
    program.host_mode = mode == "precise" ? "precise" : "fast";
    return program;
}

static void compact_flag(Build &build, const std::string &mode) {
    if (mode == "compact") build.flag("-DFF8_COMPACT");
}

struct IrqRow { std::string id, mode, kind; };

static std::vector<IrqRow> irq_rows() {
    std::vector<IrqRow> out;
    for (const char *kind : {"delay_store", "alu", "bios", "enable", "rfe"}) {
        for (const char *mode : {"fast", "compact", "trace", "precise"}) {
            out.push_back({std::string(kind) + "-" + mode, mode, kind});
        }
    }
    return out;
}

LH_TEST_P(test_irq_observes_state_before_next_instruction, irq_rows()) {
    const std::map<std::string, Words> programs = {
        {"delay_store", {0x24080063, 0x10000001, 0xAE880000, 0x03E00008, 0}},
        {"alu", {0x24080063, 0x2409004D, 0x03E00008, 0}},
        {"bios", {0x03E0F021, 0x0C00002C, 0, 0x2409004D, 0x03C00008, 0}},
        {"rfe", {0x24080063, 0x42000010, 0x2409004D, 0x03E00008, 0}},
        {"enable", {0x24080063, 0x408C6000, 0x2409004D, 0x03E00008, 0}},
    };
    const std::map<std::string, int> irq_cycle = {{"delay_store", 4}, {"alu", 2}, {"bios", 8}, {"enable", 2}, {"rfe", 2}};
    const Words &words = programs.at(p.kind);
    Program program = emit_for(words, p.mode);
    const std::string data = program.image.data;
    std::string upper = p.kind;
    for (char &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    Build build(t);
    build.program(program)
        .probe("test_recomp_configuration_slot.c", {{"DATA_SIZE", std::to_string(data.size())},
                                                    {"IRQ_CYCLE", std::to_string(irq_cycle.at(p.kind))}})
        .flags({"-std=c99", "-O2", "-DFF8_STATS"})
        .flag("-DFF8_TEST_IRQ_" + upper)
        .runtime({"cpu.c", "interp.c"});
    compact_flag(build, p.mode);
    Compiled library = build.library();
    auto check = library.fn<int(const void *, unsigned)>("check");
    LH_EQ(check(data.data(), 1), 1);
    LH_EQ(check(data.data(), 0), 1);
}

struct DmaRow { std::string id, mode, access; };

static std::vector<DmaRow> dma_rows() {
    std::vector<DmaRow> out;
    for (const char *access : {"read", "write", "gtewrite"}) {
        for (const char *mode : {"fast", "compact", "trace", "precise"}) {
            out.push_back({std::string(access) + "-" + mode, mode, access});
        }
    }
    return out;
}

LH_TEST_P(test_async_dma_is_visible_before_compiled_ram_access, dma_rows()) {
    const bool write = p.access != "read", gte = p.access == "gtewrite";
    Words words = write ? Words{0x24080063, 0x24090007, 0xAE880000, 0x03E00008, 0}
                        : Words{0x24090007, 0x240A0008, 0x8E880000, 0, 0xAE880004, 0x03E00008, 0};
    if (gte) words[2] = 0xEA880000;
    Program program = emit_for(words, p.mode);
    const std::string data = program.image.data;
    Build build(t);
    build.program(program)
        .probe("test_recomp_configuration_dma.c",
               {{"DATA_BYTES", std::to_string(data.size())},
                {"EXPECTED_CYCLES", std::to_string(words.size() * 2)},
                {"CHECK_ADDRESS", write ? "0x80100000" : "0x80100004"},
                {"EXPECTED_VALUE", write && !gte ? "99" : "123"}})
        .flags({"-std=c99", "-O2", "-DFF8_STATS"})
        .runtime({"cpu.c", "interp.c"});
    compact_flag(build, p.mode);
    Compiled library = build.library();
    auto check = library.fn<int(const void *, unsigned)>("check");
    LH_EQ(check(data.data(), 1), 1);
    LH_EQ(check(data.data(), 0), 1);
}

struct StatsRow { std::string id; bool stats; std::string host_mode; };

static std::vector<StatsRow> stats_rows() {
    std::vector<StatsRow> out;
    for (const char *host_mode : {"fast", "precise"}) {
        for (bool stats : {false, true}) {
            out.push_back({std::string(host_mode) + "-" + (stats ? "True" : "False"), stats, host_mode});
        }
    }
    return out;
}

LH_TEST_P(test_instruction_limit_without_statistics, stats_rows()) {
    // addiu t0,t0,-1; bne t0,zero,loop; addiu t1,t1,1; jr ra; nop
    const Words words = {0x2508FFFF, 0x1500FFFE, 0x25290001, 0x03E00008, 0};
    Program program{Image::words(words, 0x80010000)};
    program.host_mode = p.host_mode;
    const std::string data = program.image.data;
    Build build(t);
    build.program(program).probe("test_recomp_configuration_stats.c")
        .flags({"-std=c99", "-O2", "-fsanitize=undefined", "-fno-sanitize-recover=all"})
        .runtime({"cpu.c", "interp.c"});
    if (p.stats) build.flag("-DFF8_STATS");
    Compiled library = build.library();
    auto probe = library.fn<int(const void *)>("probe");
    LH_EQ(probe(data.data()), 1);
}

LH_TEST(test_compiled_events_resume_and_mmio_uses_static_cycle_offset) {
    // Loop, an ALU instruction, then a clock-valued MMIO load and RAM store.
    Program program{Image::words({0x2508FFFF, 0x1500FFFE, 0, 0x240A0007, 0x8E890000, 0, 0xAEA90000, 0x03E00008, 0, 0},
                                  0x80010000)};
    const std::string data = program.image.data;
    Compiled library = Build(t)
        .program(program)
        .probe("test_recomp_configuration_clock.c")
        .flags({"-std=c99", "-O2", "-DFF8_STATS"})
        .runtime({"cpu.c", "interp.c"})
        .library();
    auto check = library.fn<int(const void *, unsigned)>("check");
    LH_EQ(check(data.data(), 1), 1);
    LH_EQ(check(data.data(), 0), 1);
}

struct IrqDispatchRow { std::string id; bool nonlocal_handler, cop2_load, compact, observe_pending; };

static std::vector<IrqDispatchRow> irq_dispatch_rows() {
    const auto name = [](bool value) { return std::string(value ? "True" : "False"); };
    std::vector<IrqDispatchRow> out;
    for (bool observe_pending : {false, true}) {
        for (bool compact : {false, true}) {
            for (bool cop2_load : {false, true}) {
                for (bool nonlocal_handler : {false, true}) {
                    out.push_back({name(observe_pending) + "-" + name(compact) + "-" + name(cop2_load) + "-" +
                                       name(nonlocal_handler),
                                   nonlocal_handler, cop2_load, compact, observe_pending});
                }
            }
        }
    }
    return out;
}

LH_TEST_P(test_mmio_irq_dispatch_returns_to_compiled_caller, irq_dispatch_rows()) {
    // MMIO schedules an IRQ at retirement of the load. The compiled handler
    // observes the intermediate t1, then the interrupted function continues.
    Words words = {0x24090001, 0x8E880000, 0x25290001, 0xAEA90000, 0x03E00008, 0};
    if (p.cop2_load) words[1] = 0xCA880000;
    Words handler = {0xAEA90004, 0x24090063, 0x03E00008, 0};
    if (p.nonlocal_handler) {
        // Skip a second call by returning to its compiled continuation. The
        // callback must unwind without discarding hw_event's saved registers.
        handler = {0x03E07821, 0xAEA90004, 0x0C004018, 0, 0x0C004018, 0,
                   0x24090063, 0x01E0F821, 0x03E00008, 0, 0, 0,
                   0x3C1F8001, 0x37FF0048, 0x03E00008, 0};
    }
    if (p.observe_pending) handler[p.nonlocal_handler ? 1 : 0] = 0xAEA80004;
    const Words blob = words + Words(6, 0) + handler;
    Program program{Image::words(blob, 0x80010000)};
    program.seeds = {0x80010030};
    const std::string data = program.image.data;
    const Replacements replacements = {
        {"data, 64", "data, " + std::to_string(data.size())},
        {"c->r[8] == 123", p.cop2_load ? "c->gte_data[8] == 123" : "c->r[8] == 123"},
        {"mem_read32(c, 0x80100004) == 1", "mem_read32(c, 0x80100004) == " + std::to_string(p.observe_pending ? 0 : 1)},
        {"== 10", p.nonlocal_handler ? "== 18" : "== 10"},
        {"== 20", p.nonlocal_handler ? "== 36" : "== 20"},
    };
    Build build(t);
    build.program(program).probe("test_recomp_configuration_irq.c", replacements)
        .flags({"-std=c99", "-O2", "-DFF8_STATS"}).runtime({"cpu.c", "interp.c"});
    if (p.compact) build.flag("-DFF8_COMPACT");
    Compiled library = build.library();
    auto check = library.fn<int(const void *, unsigned)>("check");
    LH_EQ(check(data.data(), 1), 1);
    LH_EQ(check(data.data(), 0), 1);
}

} // namespace
