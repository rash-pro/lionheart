// The shared program of tests/test_recompiler.py and its `compiled` fixture, used
// by test_recompiler and by the files that import that fixture (test_runtime).
#pragma once

#include "generated.h"

#include "py_random.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace lh::recompiler {

using namespace lh::gen;

inline constexpr uint32_t BASE = 0x80010000, DATA = 0x80100000;

// Python's dict keeps insertion order, and PCS depends on it.
struct CaseTable {
    std::vector<std::string> order;
    std::map<std::string, Words> cases;
    std::map<std::string, uint32_t> pcs;
    uint32_t table_pc = 0, table = 0;
    std::string data;

    void set(const std::string &name, Words words) {
        if (!cases.count(name)) order.push_back(name);
        cases[name] = std::move(words);
    }
};

inline const CaseTable &table() {
    static const CaseTable built = [] {
        CaseTable t;
        const Words R = RET;
        t.set("load_delay", Words{imm(35, 20, 8, 0), r(33, 8, 0, 9), r(33, 8, 0, 10)} + R);
        t.set("load_overwrite", Words{imm(35, 20, 8, 0), imm(9, 0, 8, 99)} + R);
        t.set("load_twice", Words{imm(35, 20, 8, 0), imm(35, 20, 8, 4), r(33, 8, 0, 9), r(33, 8, 0, 10)} + R);
        t.set("load_zero", Words{imm(35, 20, 0, 0), r(33, 0, 0, 9)} + R);
        t.set("branch_delay", Words{imm(4, 8, 9, 2), imm(9, 0, 8, 17), imm(9, 0, 10, 22)} + R);
        t.set("load_branch", Words{imm(35, 20, 8, 0), imm(4, 8, 9, 2), r(33, 8, 0, 10), imm(9, 0, 11, 22)} + R);
        t.set("load_in_slot", Words{imm(4, 0, 0, 1), imm(35, 20, 8, 0), r(33, 8, 0, 9), r(33, 8, 0, 10)} + R);
        t.set("return_load", Words{r(8, 31), imm(35, 20, 8, 0)});
        t.set("load_slot_taken", Words{imm(4, 8, 8, 3), imm(35, 20, 8, 0), r(33, 8, 0, 9), 0,
                                       r(33, 8, 0, 10), r(33, 8, 0, 11)} + R);
        t.set("load_slot_fallthrough", Words{imm(5, 8, 8, 3), imm(35, 20, 8, 0), r(33, 8, 0, 9), 0,
                                             r(33, 8, 0, 10)} + R);
        t.set("load_slot_overwrite", Words{imm(4, 0, 0, 1), imm(35, 20, 8, 0), imm(9, 0, 8, 77)} + R);
        t.set("load_slot_twice", Words{imm(4, 0, 0, 1), imm(35, 20, 8, 0), imm(35, 20, 8, 4),
                                       r(33, 8, 0, 9), r(33, 8, 0, 10)} + R);
        t.set("pending_fault", Words{imm(35, 20, 8, 0), imm(35, 20, 8, 1)} + R);
        t.set("pending_overflow", Words{imm(35, 20, 8, 0), r(32, 9, 10, 8)} + R);
        t.set("isolation", Words{cop(16, 4, 8, 12), imm(43, 20, 9, 0), imm(35, 20, 10, 0), 0,
                                 cop(16, 4, 0, 12), imm(43, 20, 9, 4), imm(35, 20, 11, 4), 0} + R);
        t.set("pending_syscall", Words{imm(35, 20, 8, 0), 12} + R);
        t.set("load_merge_address", Words{imm(35, 20, 8, 0), imm(34, 8, 8, 3), 0} + R);

        t.set("jr_capture", Words{r(8, 31), imm(9, 0, 31, 123)});
        t.set("load_return", Words{imm(35, 20, 31, 0), r(8, 31), 0});
        t.set("loop", Words{imm(9, 8, 8, -1), imm(5, 8, 0, -2), imm(9, 9, 9, 1)} + R);
        t.set("div", Words{r(26, 8, 9), r(18, 0, 0, 10), r(16, 0, 0, 11)} + R);
        t.set("divu", Words{r(27, 8, 9), r(18, 0, 0, 10), r(16, 0, 0, 11)} + R);
        t.set("mult", Words{r(24, 8, 9), r(18, 0, 0, 10), r(16, 0, 0, 11)} + R);
        t.set("multu", Words{r(25, 8, 9), r(18, 0, 0, 10), r(16, 0, 0, 11)} + R);
        t.set("hilo", Words{r(17, 8), r(19, 9), r(16, 0, 0, 10), r(18, 0, 0, 11)} + R);
        t.set("memory", Words{imm(40, 20, 8, 8), imm(41, 20, 9, 10), imm(43, 20, 10, 12),
                              imm(32, 20, 11, 8), imm(36, 20, 12, 8), imm(33, 20, 13, 10),
                              imm(37, 20, 14, 10), imm(35, 20, 15, 12), 0} + R);
        t.set("overflow_add", Words{r(32, 8, 9, 10)} + R);
        t.set("overflow_sub", Words{r(34, 8, 9, 10)} + R);
        t.set("overflow_addi", Words{imm(8, 8, 10, 1)} + R);
        t.set("misaligned_load", Words{imm(35, 20, 8, 1)} + R);
        t.set("misaligned_store", Words{imm(41, 20, 8, 1)} + R);
        t.set("exception_slot", Words{imm(4, 0, 0, 1), imm(35, 20, 8, 1)} + R);
        t.set("break", Words{13});
        t.set("reserved", Words{0xFC000000u});
        t.set("cop_unusable", Words{17u << 26});
        t.set("syscall", Words{12} + R);
        t.set("cop0", Words{cop(16, 4, 8, 12), cop(16, 0, 9, 12), r(33, 9, 0, 10),
                            r(33, 9, 0, 11), 0x42000010u} + R);
        t.set("gte", Words{cop(18, 4, 8, 6), cop(18, 6, 9, 7), cop(18, 0, 10, 6),
                           r(33, 10, 0, 11), cop(18, 2, 12, 7), 0, imm(50, 20, 5, 0),
                           imm(58, 20, 5, 4), 0x4A000001u} + R);
        // A pending load must not replace the operand of a GTE transfer.
        for (uint32_t control = 0; control < 2; ++control) {
            for (uint32_t reg = 0; reg < 32; ++reg) {
                t.set("gte_transfer_" + std::to_string(control) + "_" + std::to_string(reg),
                      Words{imm(35, 20, 8, 0), cop(18, control ? 6 : 4, 8, reg),
                            cop(18, control ? 2 : 0, 9, reg), r(33, 9, 0, 10), r(33, 9, 0, 11)} + R);
            }
        }
        // GTE commands preserve instruction retirement, pending loads, and FLAG updates.
        for (uint32_t command : {1u, 6u, 12u, 16u, 17u, 18u, 19u, 20u, 22u, 27u, 28u, 30u, 32u, 40u, 41u, 42u, 45u, 46u, 48u, 61u, 62u, 63u, 0u, 58u}) {
            for (uint32_t sf = 0; sf < 2; ++sf) {
                for (uint32_t lm = 0; lm < 2; ++lm) {
                    const uint32_t word = 0x4A000000u | command | sf << 19 | lm << 10 | 0x7E000u;
                    t.set("gte_command_" + std::to_string(command) + "_" + std::to_string(sf) + "_" + std::to_string(lm),
                          Words{cop(18, 6, 9, 31), cop(18, 4, 8, 9), cop(18, 4, 9, 10),
                                cop(18, 4, 8, 11), word, cop(18, 2, 10, 31),
                                r(33, 10, 0, 11), r(33, 10, 0, 12)} + R);
                }
            }
        }
        for (uint32_t offset = 0; offset < 4; ++offset) {
            const std::string s = std::to_string(offset);
            t.set("lwl_" + s, Words{imm(34, 20, 8, offset), 0} + R);
            t.set("lwr_" + s, Words{imm(38, 20, 8, offset), 0} + R);
            t.set("unaligned_" + s, Words{imm(34, 20, 8, offset + 3), imm(38, 20, 8, offset),
                                          r(33, 8, 0, 9), r(33, 8, 0, 10)} + R);
            t.set("unaligned_reverse_" + s, Words{imm(38, 20, 8, offset), imm(34, 20, 8, offset + 3), 0} + R);
            t.set("swl_" + s, Words{imm(42, 20, 8, offset)} + R);
            t.set("swr_" + s, Words{imm(46, 20, 8, offset)} + R);
        }
        for (uint32_t op : {4u, 5u, 6u, 7u}) {
            t.set("branch_" + std::to_string(op),
                  Words{imm(op, 8, op < 6 ? 9 : 0, 2), imm(9, 0, 10, 10), imm(9, 0, 11, 11)} + R);
        }
        for (uint32_t rt : {0u, 1u}) {
            t.set("regimm_" + std::to_string(rt), Words{imm(1, 8, rt, 2), imm(9, 0, 10, 10), imm(9, 0, 11, 11)} + R);
        }

        // Calls are assigned absolute addresses after the case list has been laid out.
        t.set("callee", Words{r(33, 8, 0, 9), r(33, 8, 0, 10)} + R);
        for (const char *name : {"call_direct", "call_indirect", "callee_return_load", "call_return_load",
                                 "incoming_load_twice", "call_alt_link", "link_branch", "link_branch_negative",
                                 "tail_jump", "fallback", "fallback_entry", "jump_table"}) t.set(name, {});
        t.set("self_modify", Words{imm(43, 20, 8, 0), imm(9, 0, 9, 1)} + R);
        t.set("gte_self_modify", Words{cop(18, 4, 8, 2), imm(58, 20, 2, 0), imm(9, 0, 9, 1)} + R);
        for (const char *name : {"deep_recursion", "nonlocal_loop", "nonlocal_child", "unexpected_child",
                                 "bios_longjmp", "longjmp_child"}) t.set(name, {});

        lh::PyRandom rng(0xFF8);
        const std::vector<uint32_t> register_ops = {0, 2, 3, 4, 6, 7, 33, 35, 36, 37, 38, 39, 42, 43};
        const std::vector<uint32_t> immediate_ops = {9, 10, 11, 12, 13, 14, 15};
        for (int index = 0; index < 64; ++index) {
            Words words;
            for (int word = 0; word < 40; ++word) {
                const uint32_t rs = static_cast<uint32_t>(rng.randrange(1, 19));
                const uint32_t rt = static_cast<uint32_t>(rng.randrange(1, 19));
                const uint32_t rd = static_cast<uint32_t>(rng.randrange(1, 19));
                if (rng.randrange(2)) {
                    const uint32_t fn = rng.choice(register_ops);
                    const uint32_t shift = (fn == 0 || fn == 2 || fn == 3) ? static_cast<uint32_t>(rng.randrange(32)) : 0;
                    words.push_back(r(fn, rs, rt, rd, shift));
                } else {
                    const uint32_t op = rng.choice(immediate_ops);
                    const int64_t value = rng.randrange(65536);
                    words.push_back(imm(op, rs, rt, value));
                }
            }
            t.set("random_" + std::to_string(index), words + R);
        }
        const std::vector<uint32_t> memory_ops = {32, 33, 34, 35, 36, 37, 38, 40, 41, 43};
        for (int index = 0; index < 16; ++index) {
            Words words;
            for (int word = 0; word < 32; ++word) {
                const uint32_t rt = static_cast<uint32_t>(rng.randrange(8, 13));
                if (rng.randrange(3)) {
                    const uint32_t op = rng.choice(memory_ops);
                    int64_t offset = rng.randrange(16);
                    if (op == 33 || op == 37 || op == 41) offset &= ~int64_t(1);
                    if (op == 35 || op == 43) offset &= ~int64_t(3);
                    words.push_back(imm(op, 20, rt, offset));
                } else {
                    const uint32_t a = static_cast<uint32_t>(rng.randrange(8, 13));
                    const uint32_t b = static_cast<uint32_t>(rng.randrange(8, 13));
                    words.push_back(r(33, rt, a, b));
                }
            }
            t.set("memory_random_" + std::to_string(index), words + R);
        }

        uint32_t pc = BASE;
        for (const std::string &name : t.order) {
            t.pcs[name] = pc;
            pc += 0x200;
        }
        const auto PCS = [&](const char *name) { return t.pcs.at(name); };
        t.cases["call_direct"] = Words{r(33, 31, 0, 30), j(3, PCS("callee")), imm(35, 20, 8, 0), r(33, 30, 0, 31)} + R;
        t.cases["callee_return_load"] = Words{r(8, 31), imm(35, 20, 8, 0)};
        t.cases["call_return_load"] = Words{r(33, 31, 0, 30), j(3, PCS("callee_return_load")), 0, r(33, 8, 0, 9),
                                            r(33, 8, 0, 10), r(33, 30, 0, 31)} + R;
        t.cases["incoming_load_twice"] = Words{imm(35, 20, 8, 4), r(33, 8, 0, 9), r(33, 8, 0, 10)} + R;
        t.cases["call_indirect"] = Words{r(33, 31, 0, 30), r(9, 21, 0, 31), imm(35, 20, 8, 0), r(33, 30, 0, 31)} + R;
        t.cases["call_alt_link"] = Words{r(9, 21, 0, 22), imm(9, 0, 8, 66)} + R;
        for (const auto &row : std::vector<std::pair<std::string, uint32_t>>{{"link_branch", 17}, {"link_branch_negative", 16}}) {
            const uint32_t at = PCS(row.first.c_str());
            t.cases[row.first] = Words{r(33, 31, 0, 30),
                                        imm(1, 8, row.second, (static_cast<int64_t>(PCS("callee")) - (static_cast<int64_t>(at) + 8)) / 4),
                                        imm(9, 0, 12, 77), r(33, 30, 0, 31)} + R;
        }
        t.cases["tail_jump"] = Words{j(2, PCS("callee")), imm(9, 0, 8, 55)};
        t.cases["fallback"] = Words{r(8, 21), imm(35, 20, 8, 0)};
        t.cases["fallback_entry"] = Words{j(2, PCS("callee")), r(33, 8, 0, 12)};
        t.table_pc = PCS("jump_table");
        t.table = t.table_pc + 0x80;
        t.cases["jump_table"] = Words{imm(15, 0, 8, (t.table + 0x8000) >> 16), imm(9, 8, 8, t.table & 65535),
                                      r(0, 0, 4, 9, 2), r(33, 8, 9, 8), imm(35, 8, 8, 0),
                                      0, r(8, 8), 0, imm(9, 0, 2, 42)} + R + Words{imm(9, 0, 2, 77)} + R;
        const uint32_t recursion = PCS("deep_recursion");
        t.cases["deep_recursion"] = Words{imm(9, 29, 29, -8), imm(43, 29, 31, 0), imm(9, 8, 8, -1),
                                          imm(6, 8, 0, 3), 0, j(3, recursion), 0, imm(35, 29, 31, 0),
                                          imm(9, 29, 29, 8)} + R;
        t.cases["nonlocal_loop"] = Words{r(33, 31, 0, 30), j(3, PCS("nonlocal_child")), 0,
                                         imm(9, 8, 8, -1), imm(7, 8, 0, -4), 0, r(33, 30, 0, 31)} + R;
        t.cases["nonlocal_child"] = Words{r(8, 21), 0};
        t.cases["unexpected_child"] = Words{r(33, 21, 0, 31)} + R;
        t.cases["bios_longjmp"] = Words{j(3, PCS("longjmp_child")), 0, imm(9, 0, 10, 77), r(33, 30, 0, 31)} + R;
        t.cases["longjmp_child"] = Words{imm(9, 0, 9, 0x14), j(3, 0xa0), 0, 13};

        // data = bytearray(len(CASES) * 0x200); each case's words at its offset;
        // the two table words of jump_table written last.
        t.data.assign(t.order.size() * 0x200, '\0');
        for (const std::string &name : t.order) {
            const std::string words = pack(t.cases.at(name));
            t.data.replace(t.pcs.at(name) - BASE, words.size(), words);
        }
        const std::string table_words = pack({t.table_pc + 32, t.table_pc + 44});
        t.data.replace(t.table - BASE, table_words.size(), table_words);
        return t;
    }();
    return built;
}

struct Harness {
    void *(*test_new)();
    void (*test_free)(void *);
    void (*test_register)(void *, unsigned, uint32_t);
    void (*test_write)(void *, uint32_t, uint32_t, unsigned);
    uint32_t (*test_read)(void *, uint32_t, unsigned);
    void (*test_run)(void *, uint32_t, unsigned);
    void (*test_image)(void *, uint32_t, const void *, size_t);
    void (*test_snapshot)(void *, uint32_t *);
    uint64_t (*test_count)(void *, unsigned);
    const char *(*test_reason)(void *);
    uint8_t *(*test_ram)(void *);
    void (*code_sha1)(const void *, size_t, void *);
    void (*test_run_legacy)(void *, uint32_t);
    void (*test_limits)(void *, uint64_t, uint64_t);
    void (*test_registry_count)(void *, unsigned, size_t);
    uint64_t (*test_code_epoch)(void *);
    void (*test_step)(void *, uint32_t);
    void (*test_bios)(void *, unsigned);
    void (*test_syscall)(void *);
    void (*test_logger)(void *, void (*)(void *, const char *));
    void (*test_gte_write)(void *, unsigned, uint32_t, int);
    uint32_t (*test_gte_read)(void *, unsigned, int);
    void (*test_poll)(void *, uint32_t, uint64_t);
};

// The session-scoped `compiled` fixture. With the shared cache one compile
// serves every case of a variant, in every process.
inline Compiled compiled_library(lh::Case &t, const std::string &variant) {
    const CaseTable &tables = table();
    Program program{Image(tables.data, BASE, BASE)};
    for (const std::string &name : tables.order) if (name != "fallback_entry") program.seeds.push_back(tables.pcs.at(name));
    for (int index : {0, 1, 32, 64}) {
        Words words = Words{r(33, 31, 0, 25), j(3, tables.pcs.at("callee")), 0,
                            imm(9, 0, 2, 42 + index % 32), r(33, 25, 0, 31)} + RET;
        std::string blob = pack(words);
        blob.append(16, '\0');
        program.overlays.push_back(Overlay{index, Image(blob, 0x80098000, 0x80098000)});
    }
    program.mode = (variant == "compact" || variant == "precise") ? "fast" : variant;
    program.host_mode = (variant == "precise" || variant == "compact") ? "precise" : "fast";
    Build build(t);
    build.program(std::move(program))
        .source("tests/harness.c")
        .flags({"-std=c99", "-DFF8_STATS", "-O1", "-g", "-Wall", "-Wextra",
                "-Wno-unused-variable", "-Wno-unused-label", "-Wno-tautological-compare", "-DFF8_TEST_HARDWARE"})
        .runtime_all();
    if (variant == "compact") build.flag("-DFF8_COMPACT");
    if (const char *sanitize = std::getenv("FF8_TEST_SANITIZE"); sanitize && *sanitize) {
        build.flag("-fsanitize=undefined").flag("-fno-sanitize-recover=all");
    }
    return build.library();
}

inline Harness functions_of(const Compiled &lib) {
    Harness h{};
    h.test_new = lib.fn<void *()>("test_new");
    h.test_free = lib.fn<void(void *)>("test_free");
    h.test_register = lib.fn<void(void *, unsigned, uint32_t)>("test_register");
    h.test_write = lib.fn<void(void *, uint32_t, uint32_t, unsigned)>("test_write");
    h.test_read = lib.fn<uint32_t(void *, uint32_t, unsigned)>("test_read");
    h.test_run = lib.fn<void(void *, uint32_t, unsigned)>("test_run");
    h.test_image = lib.fn<void(void *, uint32_t, const void *, size_t)>("test_image");
    h.test_snapshot = lib.fn<void(void *, uint32_t *)>("test_snapshot");
    h.test_count = lib.fn<uint64_t(void *, unsigned)>("test_count");
    h.test_reason = lib.fn<const char *(void *)>("test_reason");
    h.test_ram = lib.fn<uint8_t *(void *)>("test_ram");
    h.code_sha1 = lib.fn<void(const void *, size_t, void *)>("code_sha1");
    h.test_run_legacy = lib.fn<void(void *, uint32_t)>("test_run_legacy");
    h.test_limits = lib.fn<void(void *, uint64_t, uint64_t)>("test_limits");
    h.test_registry_count = lib.fn<void(void *, unsigned, size_t)>("test_registry_count");
    h.test_code_epoch = lib.fn<uint64_t(void *)>("test_code_epoch");
    h.test_step = lib.fn<void(void *, uint32_t)>("test_step");
    h.test_bios = lib.fn<void(void *, unsigned)>("test_bios");
    h.test_syscall = lib.fn<void(void *)>("test_syscall");
    h.test_logger = lib.fn<void(void *, void (*)(void *, const char *))>("test_logger");
    h.test_gte_write = lib.fn<void(void *, unsigned, uint32_t, int)>("test_gte_write");
    h.test_gte_read = lib.fn<uint32_t(void *, unsigned, int)>("test_gte_read");
    h.test_poll = lib.fn<void(void *, uint32_t, uint64_t)>("test_poll");
    return h;
}

// A Python dict: later keys replace in place, otherwise append, keeping order.
using Pairs = std::vector<std::pair<uint32_t, uint32_t>>;
inline Pairs dict_update(Pairs base, const Pairs &updates) {
    for (const auto &entry : updates) {
        bool replaced = false;
        for (auto &existing : base) if (existing.first == entry.first) { existing.second = entry.second; replaced = true; }
        if (!replaced) base.push_back(entry);
    }
    return base;
}

struct Outcome {
    std::array<uint32_t, 135> state{};
    std::string memory;
    std::array<uint64_t, 6> counts{};
    std::string reason;
    bool has_reason = false;
};

class Cpu {
public:
    explicit Cpu(const Harness &h) : h_(h), c_(h.test_new()) { LH_CHECK(c_ != nullptr); }
    ~Cpu() { h_.test_free(c_); }
    Cpu(const Cpu &) = delete;
    void *get() const { return c_; }
private:
    const Harness &h_;
    void *c_;
};

inline Outcome run(const Harness &h, const std::string &name, const Pairs &registers, const Pairs &memory, unsigned interpret, uint32_t start) {
    const CaseTable &tables = table();
    Cpu cpu(h);
    h.test_image(cpu.get(), BASE, tables.data.data(), tables.data.size());
    const Pairs initial = dict_update({{8, 0xAABBCCDD}, {9, 0x11111111}, {20, DATA}, {21, tables.pcs.at("callee")}}, registers);
    for (const auto &entry : initial) h.test_register(cpu.get(), entry.first, entry.second);
    const Pairs mem = dict_update({{DATA, 0x44332211}, {DATA + 4, 0x88776655}}, memory);
    for (const auto &entry : mem) h.test_write(cpu.get(), entry.first, entry.second, 4);
    h.test_run(cpu.get(), start ? start : tables.pcs.at(name), interpret);
    Outcome out;
    h.test_snapshot(cpu.get(), out.state.data());
    out.memory.assign(reinterpret_cast<const char *>(h.test_ram(cpu.get())), 0x200000 + 1024 + 65536);
    for (unsigned index = 0; index < 6; ++index) out.counts[index] = h.test_count(cpu.get(), index);
    if (const char *reason = h.test_reason(cpu.get())) {
        out.reason = reason;
        out.has_reason = true;
    }
    return out;
}

// diff() runs both engines and asserts equivalence; a failure lists the state
// words that differ, as the Python message does.
inline Outcome diff(lh::Case &t, const Harness &h, const std::string &name, const Pairs &registers = {},
             const Pairs &memory = {}, uint32_t start = 0) {
    Outcome a = run(h, name, registers, memory, 0, start);
    Outcome b = run(h, name, registers, memory, 1, start);
    if (a.state != b.state) {
        std::string differs;
        for (size_t index = 0; index < a.state.size(); ++index) {
            if (a.state[index] != b.state[index]) {
                char line[80];
                std::snprintf(line, sizeof line, "(%zu, 0x%x, 0x%x) ", index, a.state[index], b.state[index]);
                differs += line;
            }
        }
        lh::fail_at(__FILE__, __LINE__, name + ": state differs: " + differs);
    }
    LH_CHECK_MSG(a.memory == b.memory, name + ": memory differs");
    LH_CHECK_MSG(a.has_reason == b.has_reason && a.reason == b.reason, name + ": stop reason differs");
    LH_EQ(a.counts[0] + a.counts[4], b.counts[0]);
    LH_EQ(a.counts[3], b.counts[3]);
    LH_EQ(a.counts[5], b.counts[5]);
    (void)t;
    return a;
}

inline const std::array<const char *, 4> kVariants = {"trace", "fast", "compact", "precise"};

} // namespace lh::recompiler
