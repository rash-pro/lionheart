// Helpers shared by the generator case files: synthetic programs and discs,
// emission through the spec tool and comparison with the frozen reference.
#pragma once
#include "lh_json.h"
#include "recompiler_fixture.h"
#include <algorithm>
#include <array>
#include <iomanip>

namespace generator_cases {
using namespace lh::gen;
using J = lh::Json;
using O = J::Object;
using A = J::Array;
constexpr uint32_t BASE = 0x80010000;
inline J num(uint64_t n) { return J(int64_t(n)); }
inline std::string hex(const std::string &s) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
inline std::string symbol(uint32_t pc, const std::string &prefix = "") {
    std::ostringstream s;
    s << prefix << "f_" << std::hex << std::setfill('0') << std::setw(8) << pc;
    return s.str();
}

inline O image(const std::string &data, uint32_t base = BASE) {
    return {{"data", hex(data)}, {"base", num(base)}, {"entry", num(base)}};
}
inline O image(const Words &w, uint32_t base = BASE) { return image(pack(w), base); }
inline O selection(A names, int cap = 8) { return {{"max_registers", cap}, {"functions", names}}; }
inline O main_spec(O main) { return {{"main", main}}; }
inline void put32(std::string &s, size_t at, uint32_t v) { s.replace(at, 4, pack({v})); }
inline lh::fs::path emit(lh::Case &t, const O &spec, const std::string &label) {
    auto path = t.temp() / (label + ".json"), out = t.temp() / (label + "-c");
    lh::write_file(path, J(spec).dump());
    t.ok(t.harness("lionheart-gen-spec").args({path, out}).timeout(120));
    return out;
}
inline void compare(lh::Case &t, const O &spec, const std::string &label, const lh::fs::path &out) {
    std::string key = t.name() + "/" + label;
    auto cmd = t.host("cmake").args({"-DKEY=" + key, "-DOUTPUT=" + out.string(),
                                     "-DREFERENCE=" + (t.root() / "tests/native/generator_reference.tsv").string()});
    if (const char *dir = std::getenv("LH_REWRITE_GENERATOR_REFERENCE")) {
        lh::fs::create_directories(dir);
        cmd.arg("-DREWRITE=" + (lh::fs::path(dir) / (lh::sha256(key) + ".tsv")).string());
    }
    t.ok(cmd.args({"-P", t.root() / "tests/native/generator_reference.cmake"}));
}
inline lh::fs::path parity(lh::Case &t, const O &spec, const std::string &label) {
    auto out = emit(t, spec, label);
    compare(t, spec, label, out);
    return out;
}
inline void spec_error(lh::Case &t, const O &spec, const std::string &message) {
    auto path = t.temp() / "invalid.json";
    lh::write_file(path, J(spec).dump());
    auto res = t.run(t.harness("lionheart-gen-spec").args({path, t.temp() / "invalid-output"}));
    LH_CHECK(res.rc > 0);
    LH_CONTAINS(res.err, message);
    LH_NOT_CONTAINS(res.err, "Sanitizer");
    LH_NOT_CONTAINS(res.err, "runtime error");
}
inline std::string source(const lh::fs::path &out) { return lh::read_file(out / "functions_000.c"); }
inline void contains_if(const std::string &s, const std::string &text, bool expected) {
    LH_EQ(s.find(text) != std::string::npos, expected);
}
inline std::string valid_exe(const std::string &body = std::string(0x400, '\0')) {
    std::string s(0x800, '\0');
    s.replace(0, 8, "PS-X EXE");
    put32(s, 0x10, BASE);
    put32(s, 0x18, BASE);
    put32(s, 0x1c, body.size());
    return s + body;
}
inline std::string disc() {
    std::string s(32 * 2352, '\0');
    size_t d = 16 * 2352 + 24;
    s[d] = 1;
    s.replace(d + 1, 5, "CD001");
    s[d + 156] = 34;
    put32(s, d + 158, 18);
    put32(s, d + 166, 2048);
    s[d + 181] = 2;
    return s;
}
inline std::string directory_record(std::string name, uint32_t lba, uint32_t size) {
    std::string s(33 + name.size() + (1 - name.size() % 2), '\0');
    s[0] = char(s.size());
    put32(s, 2, lba);
    put32(s, 10, size);
    s[32] = char(name.size());
    s.replace(33, name.size(), name);
    return s;
}
inline std::string sha1(const std::string &s) {
    char out[41];
    lhg_sha1_hex(reinterpret_cast<const uint8_t *>(s.data()), s.size(), out);
    return out;
}
inline std::string cli(lh::Case &t, std::initializer_list<lh::Arg> args) {
    auto res = t.run(t.harness("lionheart-gen").args(args).cwd(t.temp()).timeout(120));
    LH_CHECK(res.rc > 0);
    LH_NOT_CONTAINS(res.err, "Sanitizer");
    LH_NOT_CONTAINS(res.err, "runtime error");
    LH_CHECK_MSG(res.err.find("lionheart-gen: ") == 0 || res.err.find("usage") != std::string::npos, res.err);
    return res.err;
}
} // namespace generator_cases
