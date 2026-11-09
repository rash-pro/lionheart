// Native cases that run the C generator on a synthetic program. A case builds
// the program the way the pytest file does, asks for it compiled against the
// runtime, and calls the result. See "Generated programs" in tests/README.md.
#pragma once

#include "lh_test.h"

#include <memory>
#include <set>

extern "C" {
#include "generator_shim.h"
}

namespace lh::gen {

// The assemblers of tests/test_recompiler.py. Python's keyword arguments become
// positions: r(18, rd=10) is r(18, 0, 0, 10).
constexpr uint32_t r(uint32_t fn, uint32_t rs = 0, uint32_t rt = 0, uint32_t rd = 0, uint32_t sh = 0) {
    return rs << 21 | rt << 16 | rd << 11 | sh << 6 | fn;
}
constexpr uint32_t imm(uint32_t op, uint32_t rs, uint32_t rt, int64_t value) {
    return op << 26 | rs << 21 | rt << 16 | (static_cast<uint32_t>(value) & 65535);
}
constexpr uint32_t j(uint32_t op, uint32_t target) { return op << 26 | ((target >> 2) & 0x3FFFFFF); }
constexpr uint32_t cop(uint32_t op, uint32_t rs, uint32_t rt, uint32_t rd) {
    return op << 26 | rs << 21 | rt << 16 | rd << 11;
}

// A Python list of words: `a + b`, `Words(n, 0)` for `[0] * n`.
struct Words : std::vector<uint32_t> {
    using std::vector<uint32_t>::vector;
    Words() = default;
    Words(std::initializer_list<uint32_t> values) : std::vector<uint32_t>(values) {}
};
inline Words operator+(Words a, const Words &b) { a.insert(a.end(), b.begin(), b.end()); return a; }
inline const Words RET = {r(8, 31), 0};

// struct.pack('<%dI', *words): little-endian bytes of the words, which is what
// both generators are given.
std::string pack(const Words &words);

// recomp.discover.Image.
struct Image {
    std::string data;
    uint32_t base, entry;
    std::vector<std::pair<int64_t, int64_t>> code_ranges;
    std::vector<uint32_t> entry_points;
    Image(std::string bytes, uint32_t image_base, uint32_t entry_point)
        : data(std::move(bytes)), base(image_base), entry(entry_point) {}
    static Image words(const Words &program, uint32_t base) { return Image(pack(program), base, base); }
};

// discover(image, seeds, scan_calls) results, in the C library's own form.
class Discovery {
public:
    Discovery(const Image &image, const std::vector<uint32_t> &seeds = {}, bool scan_calls = true);
    ~Discovery();
    Discovery(const Discovery &) = delete;
    std::set<uint32_t> entries() const;          // set(functions)
    std::set<uint32_t> instructions(uint32_t entry) const; // set(functions[entry].instructions)
private:
    Image image_;
    std::vector<uint32_t> seeds_;
    lhgt_session *session_;
    lhg_functions functions_{};
};

// decode(word, pc).name
std::string decoded_name(uint32_t word, uint32_t pc);

// effects(decode(word, pc)) as (lines, dst, value, load, loaded).
struct Effects {
    std::vector<std::string> lines;
    int dst, load;
    std::string value, loaded;
};
Effects effects(uint32_t word, uint32_t pc);

// A parsed JSON value that keeps its parse alive, for the generated manifest
// and the checked-in catalogs.
class Json {
public:
    Json() = default;
    bool contains(const std::string &key) const;   // Python's `key in value`
    Json at(const std::string &key) const;
    Json at(size_t index) const;
    Json at(int index) const { return at(static_cast<size_t>(index)); }
    size_t size() const;
    std::string str() const;
    int64_t integer() const;
    static Json parse(const std::string &text, const std::string &name);
    static Json file(const fs::path &path);
    static Json own(std::shared_ptr<lhgt_session> owner, const lhg_json *value);
private:
    Json(std::shared_ptr<lhgt_session> owner, const lhg_json *value) : owner_(std::move(owner)), value_(value) {}
    std::shared_ptr<lhgt_session> owner_;
    const lhg_json *value_ = nullptr;
};

// infer_effect_base(data); fails by raising the same message Python raises.
struct EffectBase {
    uint32_t base;
    Json evidence;
};
EffectBase infer_effect_base(const std::string &data);

// Lines a ValueError carried, or "" if the callable did not fail. Python's
// pytest.raises(ValueError, match=...) becomes LH_CONTAINS on this.
std::string error_of(const std::function<void()> &body);

// One program to emit: the arguments of discover() and emit() in one value.
struct Overlay {
    int index;
    Image image;
    std::vector<uint32_t> seeds = {};
    bool scan_calls = true;
};

struct Program {
    Image image;
    std::vector<uint32_t> seeds = {};
    bool scan_calls = true;
    // Python's defaults for emit(); the CLI's differ.
    std::string mode = "fast", host_mode = "fast", timing = "exact";
    std::vector<Overlay> overlays = {};
    explicit Program(Image program) : image(std::move(program)) {}
    std::string canonical() const;
};

// The compiled result: a Library plus where the generated files are.
class Compiled : public Library {
public:
    Compiled(void *handle, fs::path directory) : Library(handle, directory.string()), directory_(std::move(directory)) {}
    Json manifest() const;
    const fs::path &directory() const { return directory_; }
private:
    fs::path directory_;
};

// Generates C for a program and compiles it, like the cc line of a pytest case.
// Equal programs and equal flags are built once per build directory, however
// many cases or processes ask, so a case pays for dlopen rather than for a
// compile. Flags omit -shared and -fPIC, which are always present.
class Build {
public:
    explicit Build(Case &t) : t_(t) {}
    Build &program(Program program);
    Build &flags(std::initializer_list<std::string> values);
    Build &flag(std::string value);
    // Names under runtime/, or every runtime/*.c. The objects are shared.
    Build &runtime(std::initializer_list<std::string> names);
    Build &runtime_all();
    // Compiled with each program, for sources that include "generated.h".
    Build &source(fs::path repository_relative);
    // A C file under tests/native/cases, after Python's .replace() calls in order.
    Build &probe(const std::string &name, std::vector<std::pair<std::string, std::string>> replacements = {});
    Compiled library();
private:
    Case &t_;
    std::unique_ptr<Program> program_;
    std::vector<std::string> flags_, sources_;
    std::vector<std::string> runtime_;
    bool runtime_all_ = false;
    std::vector<std::pair<std::string, std::string>> probes_;
};

} // namespace lh::gen
