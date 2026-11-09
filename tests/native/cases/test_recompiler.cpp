// Differential tests of generated code against the interpreter, one shared
// program for every case. Ported functions of tests/test_recompiler.py; the
// rest are marked LH_PENDING below and stay authoritative in pytest.
#include "cases/recompiler_fixture.h"

#include <set>

using namespace lh::gen;
using namespace lh::recompiler;

namespace {

struct NameRow { std::string id, variant, name; };

std::vector<NameRow> differential_rows() {
    const std::set<std::string> excluded = {"loop", "call_alt_link", "fallback", "fallback_entry", "deep_recursion",
                                            "nonlocal_loop", "nonlocal_child", "unexpected_child", "bios_longjmp", "longjmp_child"};
    std::vector<NameRow> out;
    for (const char *variant : kVariants) {
        for (const std::string &name : table().order) {
            if (!excluded.count(name)) out.push_back({std::string(variant) + "-" + name, variant, name});
        }
    }
    return out;
}

std::vector<NameRow> random_rows() {
    std::vector<NameRow> out;
    for (const char *variant : kVariants) {
        for (const std::string &name : table().order) {
            if (name.rfind("random_", 0) == 0) out.push_back({std::string(variant) + "-" + name, variant, name});
        }
    }
    return out;
}

struct HiloRow { std::string id, variant, name; uint32_t a, b; };

std::vector<HiloRow> hilo_rows() {
    static const std::array<std::pair<uint32_t, uint32_t>, 8> pairs = {{
        {0, 0}, {1, 0}, {0xFFFFFFFF, 0}, {0x80000000, 0}, {0x80000000, 0xFFFFFFFF}, {0xFFFFFFF9, 3}, {7, 0xFFFFFFFD}, {0xFFFFFFFF, 7}}};
    std::vector<HiloRow> out;
    for (const char *variant : kVariants) {
        for (const char *name : {"div", "divu", "mult", "multu"}) {
            for (const auto &pair : pairs) {
                out.push_back({std::string(variant) + "-" + name + "-" + std::to_string(pair.first) + "-" + std::to_string(pair.second),
                               variant, name, pair.first, pair.second});
            }
        }
    }
    return out;
}

struct VariantRow { const char *id; };
const std::array<VariantRow, 4> kVariantRows = {{{"trace"}, {"fast"}, {"compact"}, {"precise"}}};

LH_TEST_P(test_differential, differential_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    const Pairs registers = (p.name == "syscall" || p.name == "pending_syscall") ? Pairs{{4, 1}, {8, 5}, {9, 5}} : Pairs{};
    diff(t, h, p.name, registers);
}

LH_TEST_P(test_load_delay_expected, kVariantRows) {
    Compiled lib = compiled_library(t, p.id);
    const Harness h = functions_of(lib);
    Outcome state = diff(t, h, "load_delay");
    LH_EQ(state.state[9], 0xAABBCCDDu);
    LH_EQ(state.state[10], 0x44332211u);
    state = diff(t, h, "load_overwrite");
    LH_EQ(state.state[8], 99u);
    state = diff(t, h, "load_twice");
    LH_EQ(state.state[9], 0xAABBCCDDu);
    LH_EQ(state.state[10], 0x88776655u);
    state = diff(t, h, "load_in_slot");
    LH_EQ(state.state[9], 0xAABBCCDDu);
    LH_EQ(state.state[10], 0x44332211u);
}

LH_TEST_P(test_hilo_edges, hilo_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    Outcome state = diff(t, h, p.name, {{8, p.a}, {9, p.b}});
    const bool is_signed = p.name == "div" || p.name == "mult";
    const int64_t x = is_signed && (p.a >> 31) ? int64_t(p.a) - (int64_t(1) << 32) : int64_t(p.a);
    const int64_t y = is_signed && (p.b >> 31) ? int64_t(p.b) - (int64_t(1) << 32) : int64_t(p.b);
    uint32_t hi, lo;
    if (p.name.rfind("mult", 0) == 0) {
        const uint64_t product = static_cast<uint64_t>(x) * static_cast<uint64_t>(y);
        hi = static_cast<uint32_t>(product >> 32);
        lo = static_cast<uint32_t>(product);
    } else if (y == 0) {
        hi = p.a;
        lo = is_signed && x < 0 ? 1 : 0xFFFFFFFFu;
    } else {
        const int64_t quotient = (std::llabs(x) / std::llabs(y)) * (((x < 0) != (y < 0)) ? -1 : 1);
        hi = static_cast<uint32_t>(x - quotient * y);
        lo = static_cast<uint32_t>(quotient);
    }
    LH_EQ(state.state[32], hi);
    LH_EQ(state.state[33], lo);
}

struct JumpRow { std::string id, variant; int index; uint32_t expected; };

static std::vector<JumpRow> jump_rows() {
    std::vector<JumpRow> out;
    for (const char *variant : kVariants) {
        for (const auto &row : std::vector<std::pair<int, uint32_t>>{{0, 42}, {1, 77}}) {
            out.push_back({std::string(variant) + "-" + std::to_string(row.first) + "-" + std::to_string(row.second),
                           variant, row.first, row.second});
        }
    }
    return out;
}

LH_TEST_P(test_jump_table, jump_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    char key[20];
    std::snprintf(key, sizeof key, "0x%x", table().table_pc + 24);
    LH_CHECK(lib.manifest().at("jump_tables").contains(key));
    Outcome state = diff(t, h, "jump_table", {{4, static_cast<uint32_t>(p.index)}});
    LH_EQ(state.state[2], p.expected);
    LH_EQ(state.counts[0], 0u);
}

LH_TEST_P(test_random_register_states, random_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    lh::PyRandom rng(p.name);
    Pairs registers;
    for (uint32_t reg = 1; reg < 19; ++reg) registers.emplace_back(reg, static_cast<uint32_t>(rng.getrandbits(32)));
    diff(t, h, p.name, registers);
}

struct CachedRow { std::string id, variant; uint32_t address; unsigned width; };

static std::vector<CachedRow> cached_rows() {
    const uint32_t callee = table().pcs.at("callee");
    const std::array<std::pair<uint32_t, unsigned>, 4> rows = {{{callee, 4}, {callee + 2, 2}, {callee + 3, 1}, {callee + 0x200000, 4}}};
    std::vector<CachedRow> out;
    for (const char *variant : kVariants) {
        for (const auto &row : rows) {
            out.push_back({std::string(variant) + "-" + std::to_string(row.first) + "-" + std::to_string(row.second),
                           variant, row.first, row.second});
        }
    }
    return out;
}

LH_TEST_P(test_cached_source_invalidated_and_restored, cached_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    const CaseTable &tables = table();
    Cpu cpu(h);
    h.test_image(cpu.get(), BASE, tables.data.data(), tables.data.size());
    h.test_register(cpu.get(), 8, 99);
    const uint32_t callee = tables.pcs.at("callee");
    h.test_run(cpu.get(), callee, 0);
    LH_EQ(h.test_count(cpu.get(), 0), 0u);
    const uint32_t original = h.test_read(cpu.get(), p.address, p.width);
    const uint32_t replacement = imm(9, 0, 9, 123) >> ((p.address & 3) * 8);
    h.test_write(cpu.get(), p.address, replacement, p.width);
    h.test_run(cpu.get(), callee, 0);
    const uint64_t interpreted = h.test_count(cpu.get(), 0);
    LH_CHECK(interpreted > 0);
    h.test_write(cpu.get(), p.address, original, p.width);
    h.test_run(cpu.get(), callee, 0);
    LH_EQ(h.test_count(cpu.get(), 0), interpreted);
}

LH_TEST_P(test_overlay_function_selects_replacements_and_rejects_unknown, kVariantRows) {
    Compiled lib = compiled_library(t, p.id);
    const Harness h = functions_of(lib);
    const CaseTable &tables = table();
    Cpu cpu(h);
    h.test_image(cpu.get(), BASE, tables.data.data(), tables.data.size());
    for (int index : {0, 1, 2, 32, 64, 0}) {
        std::string blob = pack(Words{r(33, 31, 0, 25), j(3, tables.pcs.at("callee")), 0,
                                      imm(9, 0, 2, 42 + index % 32), r(33, 25, 0, 31)} + RET);
        blob.append(16, '\0');
        h.test_image(cpu.get(), 0x80098000, blob.data(), blob.size());
        const uint64_t before = h.test_count(cpu.get(), 0);
        h.test_run(cpu.get(), 0x80098000, 0);
        std::array<uint32_t, 135> snapshot{};
        h.test_snapshot(cpu.get(), snapshot.data());
        LH_EQ(snapshot[2], static_cast<uint32_t>(42 + index % 32));
        LH_EQ(h.test_count(cpu.get(), 0) > before, index == 2);
    }
    // Mutable overlay data must not disable an unchanged function.
    h.test_write(cpu.get(), 0x80098028, 1, 1);
    const uint64_t before = h.test_count(cpu.get(), 0);
    h.test_run(cpu.get(), 0x80098000, 0);
    LH_EQ(h.test_count(cpu.get(), 0), before);
}

struct ShaRow { std::string id, variant, data; };

static std::vector<ShaRow> sha_rows() {
    std::string all_bytes;
    for (int repeat = 0; repeat < 30; ++repeat) for (int value = 0; value < 256; ++value) all_bytes.push_back(static_cast<char>(value));
    const std::vector<std::pair<std::string, std::string>> rows = {
        {"", ""}, {"abc", "abc"},
        {std::string(55, 'x'), std::string(55, 'x')},
        {std::string(56, 'x'), std::string(56, 'x')},
        {std::string(64, 'x'), std::string(64, 'x')},
        {"data5", all_bytes}};
    std::vector<ShaRow> out;
    for (const char *variant : kVariants) {
        for (const auto &row : rows) out.push_back({std::string(variant) + "-" + row.first, variant, row.second});
    }
    return out;
}

// The digest comes from the generator's SHA-1, which shares no code with the
// runtime function under test.
LH_TEST_P(test_overlay_sha1_matches_standard, sha_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    uint8_t digest[20], expected[20];
    h.code_sha1(p.data.data(), p.data.size(), digest);
    lhg_sha1(reinterpret_cast<const uint8_t *>(p.data.data()), p.data.size(), expected);
    LH_BYTES_EQ(std::string(reinterpret_cast<const char *>(digest), 20), std::string(reinterpret_cast<const char *>(expected), 20));
}

LH_TEST(test_decoder_coverage) {
    const std::map<int, const char *> special = {
        {0, "sll"}, {2, "srl"}, {3, "sra"}, {4, "sllv"}, {6, "srlv"}, {7, "srav"},
        {8, "jr"}, {9, "jalr"}, {12, "syscall"}, {13, "break"}, {16, "mfhi"},
        {17, "mthi"}, {18, "mflo"}, {19, "mtlo"}, {24, "mult"}, {25, "multu"},
        {26, "div"}, {27, "divu"}, {32, "add"}, {33, "addu"}, {34, "sub"},
        {35, "subu"}, {36, "and"}, {37, "or"}, {38, "xor"}, {39, "nor"},
        {42, "slt"}, {43, "sltu"}};
    const std::map<int, const char *> primary = {
        {2, "j"}, {3, "jal"}, {4, "beq"}, {5, "bne"}, {6, "blez"}, {7, "bgtz"},
        {8, "addi"}, {9, "addiu"}, {10, "slti"}, {11, "sltiu"}, {12, "andi"},
        {13, "ori"}, {14, "xori"}, {15, "lui"}, {32, "lb"}, {33, "lh"},
        {34, "lwl"}, {35, "lw"}, {36, "lbu"}, {37, "lhu"}, {38, "lwr"},
        {40, "sb"}, {41, "sh"}, {42, "swl"}, {43, "sw"}, {46, "swr"},
        {50, "lwc2"}, {58, "swc2"}};
    for (const auto &entry : special) LH_TEXT_EQ(decoded_name(entry.first, BASE), entry.second);
    for (const auto &entry : primary) LH_TEXT_EQ(decoded_name(static_cast<uint32_t>(entry.first) << 26, BASE), entry.second);
    for (uint32_t op : {16u, 18u}) {
        for (const auto &row : std::vector<std::pair<uint32_t, std::string>>{{0, "mfc"}, {2, "cfc"}, {4, "mtc"}, {6, "ctc"}}) {
            LH_TEXT_EQ(decoded_name(cop(op, row.first, 2, 3), BASE), row.second + std::to_string(op - 16));
        }
    }
}

LH_TEST(test_discovery_excludes_buffers_and_outside_code_ranges) {
    std::string data(512, '\0');
    data.replace(0, 16, pack(Words{j(3, BASE + 128), 0} + RET));
    data.replace(256, 4, pack({j(3, BASE + 384)}));
    Image image(data, BASE, BASE);
    image.code_ranges = {{BASE, BASE + 128}};
    Discovery functions(image);
    LH_CHECK(functions.entries() == std::set<uint32_t>{BASE});
    for (uint32_t entry : functions.entries()) {
        for (uint32_t pc : functions.instructions(entry)) LH_CHECK(BASE <= pc && pc < BASE + 128);
    }
    LH_CHECK(Discovery(Image(std::string(128, '\0'), BASE, BASE)).entries().empty());
}

LH_TEST(test_versioned_indirect_entry_points_anchor_discovery) {
    std::string data(128, '\0');
    data.replace(0, 8, pack(RET));
    data.replace(64, 12, pack(Words{imm(9, 0, 2, 77)} + RET));
    Image image(data, BASE, BASE);
    image.entry_points = {BASE + 64};
    Discovery functions(image, {}, false);
    LH_CHECK(functions.entries() == (std::set<uint32_t>{BASE, BASE + 64}));
    LH_CHECK(functions.instructions(BASE + 64) == (std::set<uint32_t>{BASE + 64, BASE + 68, BASE + 72}));
}

struct UnalignedRow { std::string id, variant; unsigned offset; };
static std::vector<UnalignedRow> unaligned_rows() {
    std::vector<UnalignedRow> rows;
    for (const char *variant : kVariants) for (unsigned offset = 0; offset < 4; ++offset)
        rows.push_back({std::string(variant) + "-" + std::to_string(offset), variant, offset});
    return rows;
}
LH_TEST_P(test_unaligned_expected, unaligned_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    const unsigned offset = p.offset;
    Outcome state = diff(t, h, "unaligned_" + std::to_string(offset));
    const unsigned char bytes[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint32_t expected = 0;
    for (unsigned i = 0; i < 4; ++i) expected |= uint32_t(bytes[offset + i]) << (8 * i);
    LH_EQ(state.state[10], expected);
    LH_EQ(state.state[9], 0xAABBCCDDu);
    state = diff(t, h, "unaligned_reverse_" + std::to_string(offset));
    LH_EQ(state.state[8], expected);
    for (const std::string name : {"swl", "swr"}) {
        state = diff(t, h, name + "_" + std::to_string(offset));
        std::string expected_bytes("\x11\x22\x33\x44", 4);
        const std::string value("\xdd\xcc\xbb\xaa", 4);
        if (name == "swl") expected_bytes.replace(0, offset + 1, value.substr(3 - offset));
        else expected_bytes.replace(offset, 4 - offset, value.substr(0, 4 - offset));
        LH_BYTES_EQ(state.memory.substr(DATA & 0x1FFFFF, 4), expected_bytes);
    }
}
LH_TEST_P(test_branch_operands_captured_before_slot, kVariantRows) {
    Compiled lib = compiled_library(t, p.id);
    const Harness h = functions_of(lib);
    Outcome state = diff(t, h, "branch_delay", {{8, 5}, {9, 5}});
    LH_EQ(state.state[8], 17u); LH_EQ(state.state[10], 0u);
    state = diff(t, h, "branch_delay", {{8, 4}, {9, 5}});
    LH_EQ(state.state[10], 22u);
    state = diff(t, h, "load_branch", {{8, 5}, {9, 5}});
    LH_EQ(state.state[10], 0x44332211u); LH_EQ(state.state[11], 0u);
    state = diff(t, h, "loop", {{8, 17}, {9, 0}});
    LH_EQ(state.state[8], 0u); LH_EQ(state.state[9], 17u);
}
static std::vector<NameRow> boundary_rows() {
    std::vector<NameRow> rows;
    for (const char *variant : kVariants) for (const char *name : {"call_direct", "call_indirect"})
        rows.push_back({std::string(variant) + "-" + name, variant, name});
    return rows;
}
LH_TEST_P(test_call_boundary_load, boundary_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    const Outcome state = diff(t, h, p.name);
    LH_EQ(state.state[9], 0xAABBCCDDu); LH_EQ(state.state[10], 0x44332211u);
    LH_CHECK(state.counts[1] >= 2); LH_EQ(state.counts[0], 0u);
}
LH_TEST_P(test_dispatch_miss_hands_back_to_compiled, kVariantRows) {
    Compiled lib = compiled_library(t, p.id);
    const Harness h = functions_of(lib);
    const uint32_t start = 0x800F0000;
    const Pairs memory = {{start, j(2, table().pcs.at("callee"))}, {start + 4, imm(9, 0, 8, 99)}};
    Outcome state = diff(t, h, "callee", {}, memory, start);
    LH_EQ(state.state[9], 99u); LH_EQ(state.state[10], 99u);
    LH_EQ(state.counts[0], 2u); LH_CHECK(state.counts[1] >= 1); LH_EQ(state.counts[2], 1u);
    state = diff(t, h, "fallback", {{21, start}}, memory);
    LH_EQ(state.state[9], 99u); LH_EQ(state.state[10], 99u);
    LH_EQ(state.counts[0], 2u); LH_CHECK(state.counts[1] >= 2); LH_EQ(state.counts[2], 1u);
}
LH_TEST_P(test_jalr_alternate_link, kVariantRows) {
    Compiled lib = compiled_library(t, p.id);
    const Harness h = functions_of(lib);
    const uint32_t start = 0x800F0000;
    const Outcome state = diff(t, h, "call_alt_link", {{21, start}}, {{start, r(8, 22)}, {start + 4, 0}});
    LH_EQ(state.state[8], 66u); LH_EQ(state.state[22], table().pcs.at("call_alt_link") + 8);
}
struct ConditionalRow { std::string id, variant, name; int value; };
static std::vector<ConditionalRow> conditional_rows() {
    std::vector<ConditionalRow> rows;
    for (const char *variant : kVariants) for (const char *name : {"link_branch", "link_branch_negative"})
        for (int value : {4, -4}) rows.push_back({std::string(variant) + "-" + name + "-" + std::to_string(value), variant, name, value});
    return rows;
}
LH_TEST_P(test_conditional_link, conditional_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    const Outcome state = diff(t, h, p.name, {{8, static_cast<uint32_t>(p.value)}});
    LH_EQ(state.state[12], 77u);
}
struct ExceptionRow { std::string id, variant, name; Pairs regs; uint32_t code; };
static std::vector<ExceptionRow> exception_rows() {
    const std::vector<ExceptionRow> cases = {
        {"overflow_add-regs0-12", "", "overflow_add", {{8, 0x7FFFFFFF}, {9, 1}}, 12},
        {"overflow_sub-regs1-12", "", "overflow_sub", {{8, 0x80000000}, {9, 1}}, 12},
        {"overflow_addi-regs2-12", "", "overflow_addi", {{8, 0x7FFFFFFF}}, 12},
        {"misaligned_load-regs3-4", "", "misaligned_load", {}, 4},
        {"misaligned_store-regs4-5", "", "misaligned_store", {}, 5},
        {"break-regs5-9", "", "break", {}, 9}, {"reserved-regs6-10", "", "reserved", {}, 10},
        {"cop_unusable-regs7-11", "", "cop_unusable", {}, 11}, {"exception_slot-regs8-4", "", "exception_slot", {}, 4}};
    std::vector<ExceptionRow> rows;
    for (const char *variant : kVariants) for (auto row : cases) {
        row.id = std::string(variant) + "-" + row.id; row.variant = variant; rows.push_back(row);
    }
    return rows;
}
LH_TEST_P(test_exception_expected, exception_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    const Outcome state = diff(t, h, p.name, p.regs);
    LH_EQ(state.state[37], 1u); LH_EQ((state.state[52] >> 2) & 31, p.code);
    LH_EQ(state.state[53], table().pcs.at(p.name));
    LH_EQ(bool(state.state[52] >> 31), p.name == "exception_slot");
}
LH_TEST_P(test_delay_paths_and_isolation, kVariantRows) {
    Compiled lib = compiled_library(t, p.id);
    const Harness h = functions_of(lib);
    Outcome state = diff(t, h, "load_slot_taken");
    LH_EQ(state.state[10], 0xAABBCCDDu); LH_EQ(state.state[11], 0x44332211u);
    state = diff(t, h, "load_slot_fallthrough");
    LH_EQ(state.state[9], 0xAABBCCDDu); LH_EQ(state.state[10], 0x44332211u);
    state = diff(t, h, "load_slot_overwrite"); LH_EQ(state.state[8], 77u);
    state = diff(t, h, "load_slot_twice");
    LH_EQ(state.state[9], 0xAABBCCDDu); LH_EQ(state.state[10], 0x88776655u);
    state = diff(t, h, "call_return_load");
    LH_EQ(state.state[9], 0xAABBCCDDu); LH_EQ(state.state[10], 0x44332211u);
    state = diff(t, h, "pending_overflow", {{9, 0x7FFFFFFF}, {10, 1}});
    LH_EQ(state.state[8], 0x44332211u); LH_CHECK(state.state[37]);
    state = diff(t, h, "isolation", {{8, 0x10000}, {9, 0x12345678}});
    LH_EQ(state.state[10], 0x44332211u); LH_EQ(state.state[11], 0x12345678u);
    LH_BYTES_EQ(state.memory.substr(0x100000, 8), std::string("\x11\x22\x33\x44\x78\x56\x34\x12", 8));
    state = diff(t, h, "call_indirect", {{21, table().pcs.at("incoming_load_twice")}});
    LH_EQ(state.state[9], 0xAABBCCDDu); LH_EQ(state.state[10], 0x88776655u);
    state = diff(t, h, "load_merge_address", {{8, DATA}}, {{DATA, 0x12345678}});
    LH_EQ(state.state[8], 0x12345678u);
}
struct MirrorRow { std::string id, variant; uint32_t base; };
static std::vector<MirrorRow> mirror_rows() {
    std::vector<MirrorRow> rows;
    for (const char *variant : kVariants) for (uint32_t base : {0x00200000u, 0x80400000u, 0xA0600000u, 0x1F800000u})
        rows.push_back({std::string(variant) + "-" + std::to_string(base), variant, base});
    return rows;
}
LH_TEST_P(test_generated_memory_mirrors_and_scratchpad, mirror_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    diff(t, h, "memory", {{20, p.base + 0x100}});
}
struct BoolRow { std::string id, variant; bool value; };
static std::vector<BoolRow> bool_rows() {
    std::vector<BoolRow> rows;
    for (const char *variant : kVariants) for (bool value : {false, true})
        rows.push_back({std::string(variant) + (value ? "-True" : "-False"), variant, value});
    return rows;
}
LH_TEST_P(test_stop_unwinds_before_following_instruction, bool_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    Cpu cpu(h);
    h.test_image(cpu.get(), BASE, table().data.data(), table().data.size());
    h.test_register(cpu.get(), 8, 0x7FFFFFFF); h.test_register(cpu.get(), 10, 123);
    if (p.value) h.test_run_legacy(cpu.get(), table().pcs.at("overflow_addi"));
    else h.test_run(cpu.get(), table().pcs.at("overflow_addi"), 0);
    std::array<uint32_t, 135> snapshot{}; h.test_snapshot(cpu.get(), snapshot.data());
    LH_CHECK(snapshot[37]); LH_EQ(snapshot[10], 123u);
    LH_EQ(snapshot[34], table().pcs.at("overflow_addi")); LH_EQ(h.test_count(cpu.get(), 4), 1u);
}
LH_TEST_P(test_instruction_budget_stops_loop, bool_rows()) {
    Compiled lib = compiled_library(t, p.variant);
    const Harness h = functions_of(lib);
    Cpu cpu(h);
    h.test_image(cpu.get(), BASE, table().data.data(), table().data.size());
    h.test_register(cpu.get(), 8, 1000000); h.test_limits(cpu.get(), 10000, 0);
    h.test_run(cpu.get(), table().pcs.at("loop"), p.value);
    LH_TEXT_EQ(h.test_reason(cpu.get()), "instruction budget exhausted");
    const uint64_t count = h.test_count(cpu.get(), 0) + h.test_count(cpu.get(), 4);
    LH_CHECK(count >= 10000 && count < 10004);
}
LH_PENDING(test_modified_source_is_interpreted);
LH_PENDING(test_store_to_current_function_exits_before_stale_instruction);
LH_PENDING(test_gte_store_to_current_function_exits_before_stale_instruction);
LH_PENDING(test_dispatch_cache_follows_registry_changes);
LH_PENDING(test_jump_table_words_invalidate_compiled_function);
LH_PENDING(test_overlay_data_write_does_not_change_code_epoch);
LH_PENDING(test_deep_guest_recursion_resumes_compiled_returns);
LH_PENDING(test_nonlocal_reentry_discards_abandoned_call_frames);
LH_PENDING(test_bios_longjmp_resumes_compiled_return_site);

} // namespace
