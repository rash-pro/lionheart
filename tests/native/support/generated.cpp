// Program building, the generator library calls and the compile cache behind
// lh::gen::Build. Why the cache exists is in tests/README.md.
#include "generated.h"

#include "lh_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace lh::gen {

using internal::build_info;

namespace {

// Everything the library allocates lives until the session closes; the C++
// side converts results before that, or keeps the session alive with them.
std::shared_ptr<lhgt_session> open_session() {
    std::shared_ptr<lhgt_session> session(lhgt_open(), lhgt_close);
    if (!session) LH_FAIL("cannot start a generator session");
    return session;
}

void check(int status, const lhgt_session *session, const char *what) {
    if (status != 0) LH_FAIL(std::string(what) + ": " + lhgt_error(session));
}

// A file's position in the cache depends on these, not on a recompute, so a
// case that cannot change what the compiler sees pays one stat per input.
std::string stat_signature(const fs::path &path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) LH_FAIL("cannot stat " + path.string());
    return std::to_string(info.st_size) + ":" + std::to_string(info.st_mtim.tv_sec) + ":" +
           std::to_string(info.st_mtim.tv_nsec);
}

// Every runtime source and header. Header changes reach any object, and the
// C compiler's own include list would be a build step of its own.
std::string runtime_fingerprint() {
    static const std::string fingerprint = [] {
        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(build_info().root / "runtime")) {
            const std::string extension = entry.path().extension().string();
            if (extension == ".c" || extension == ".h") files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        std::string out;
        for (const fs::path &file : files) out += file.filename().string() + "=" + stat_signature(file) + ";";
        return out;
    }();
    return fingerprint;
}

std::string generator_fingerprint() {
    const auto &harnesses = build_info().harnesses;
    const auto found = harnesses.find("lh_generator_support");
    if (found == harnesses.end()) LH_FAIL("the runner was built without lh_generator_support");
    return stat_signature(found->second);
}

std::string compiler() {
    if (const char *chosen = std::getenv("FF8_TEST_CC"); chosen && *chosen) return chosen;
    return LH_TEST_CC;
}

fs::path cache_root() {
    if (const char *chosen = std::getenv("LH_GENERATED_CACHE"); chosen && *chosen) return chosen;
    return fs::read_symlink("/proc/self/exe").parent_path() / "generated-cache";
}

// Up to this many compiler processes per Build. A program with few translation
// units never reaches it; the large shared program of test_recompiler does.
unsigned compile_jobs() {
    if (const char *chosen = std::getenv("LH_COMPILE_JOBS"); chosen && *chosen) {
        const int value = std::atoi(chosen);
        if (value > 0) return static_cast<unsigned>(value);
    }
    return 2;
}

class Lock {
public:
    explicit Lock(const fs::path &path) : fd_(open(path.c_str(), O_CREAT | O_RDWR, 0644)) {
        if (fd_ < 0) LH_FAIL("cannot open " + path.string() + ": " + std::strerror(errno));
        while (flock(fd_, LOCK_EX) != 0) {
            if (errno != EINTR) LH_FAIL("cannot lock " + path.string() + ": " + std::strerror(errno));
        }
    }
    ~Lock() { close(fd_); }
private:
    int fd_;
};

// Publishes a finished directory: a process that sees `done` exist also sees
// everything in it, and one killed halfway leaves only a .tmp to replace.
class Publication {
public:
    Publication(const fs::path &done) : done_(done), temporary_(done.string() + ".tmp." + std::to_string(getpid())) {
        std::error_code ignored;
        fs::remove_all(temporary_, ignored);
        fs::create_directories(temporary_);
    }
    const fs::path &directory() const { return temporary_; }
    void publish() {
        fs::rename(temporary_, done_);
    }
    ~Publication() {
        std::error_code ignored;
        fs::remove_all(temporary_, ignored);
    }
private:
    fs::path done_, temporary_;
};

std::string hex_of(const std::string &bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (char c : bytes) {
        const auto value = static_cast<unsigned char>(c);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 15]);
    }
    return out;
}

std::string words_of(const std::vector<uint32_t> &values) {
    std::ostringstream out;
    for (uint32_t value : values) out << value << ',';
    return out.str();
}

std::string image_text(const Image &image, const std::vector<uint32_t> &seeds, bool scan_calls) {
    std::ostringstream out;
    out << image.base << '|' << image.entry << '|';
    for (const auto &range : image.code_ranges) out << range.first << '-' << range.second << ',';
    out << '|' << words_of(image.entry_points) << '|' << words_of(seeds) << '|' << (scan_calls ? 1 : 0) << '|'
        << hex_of(image.data);
    if (!image.timing_entry_points.empty()) out << "|timing-entry-points|" << words_of(image.timing_entry_points);
    return out.str();
}

void fill_image(lhg_image &raw, const Image &image, std::vector<lhg_range> &ranges) {
    std::memset(&raw, 0, sizeof raw);
    raw.data = reinterpret_cast<const uint8_t *>(image.data.data());
    raw.size = image.data.size();
    raw.base = image.base;
    raw.entry = image.entry;
    for (const auto &range : image.code_ranges) ranges.push_back({range.first, range.second});
    raw.code_ranges = ranges.empty() ? nullptr : ranges.data();
    raw.code_range_count = ranges.size();
    raw.entry_points = image.entry_points.empty() ? nullptr : image.entry_points.data();
    raw.entry_point_count = image.entry_points.size();
    raw.timing_entry_points = image.timing_entry_points.data();
    raw.timing_entry_point_count = image.timing_entry_points.size();
}

void emit_program(const Program &program, const fs::path &directory) {
    const std::shared_ptr<lhgt_session> session = open_session();
    lhg_image main{};
    std::vector<lhg_range> main_ranges;
    fill_image(main, program.image, main_ranges);
    lhg_functions functions{};
    if (program.discover_functions)
        check(lhgt_discover(session.get(), &main, program.seeds.data(), program.seeds.size(), program.scan_calls, &functions),
              session.get(), "discover");
    std::vector<lhg_image> overlay_images(program.overlays.size());
    std::vector<std::vector<lhg_range>> overlay_ranges(program.overlays.size());
    std::vector<lhg_overlay_set> sets(program.overlays.size());
    for (size_t index = 0; index < program.overlays.size(); ++index) {
        const Overlay &overlay = program.overlays[index];
        fill_image(overlay_images[index], overlay.image, overlay_ranges[index]);
        std::memset(&sets[index], 0, sizeof sets[index]);
        sets[index].index = overlay.index;
        sets[index].image = &overlay_images[index];
        check(lhgt_discover(session.get(), &overlay_images[index], overlay.seeds.data(), overlay.seeds.size(),
                            overlay.scan_calls, &sets[index].functions), session.get(), "discover overlay");
    }
    lhg_emit_options options{};
    if (program.mode == "trace") options.mode = LHG_MODE_TRACE;
    else if (program.mode != "fast") LH_FAIL("unknown emission mode: " + program.mode);
    if (program.host_mode == "fast") options.host_mode = LHG_HOST_FAST;
    else if (program.host_mode != "precise") LH_FAIL("unknown host mode: " + program.host_mode);
    if (program.timing == "block") options.timing = LHG_TIMING_BLOCK;
    else if (program.timing == "loose") options.timing = LHG_TIMING_LOOSE;
    else if (program.timing != "exact") LH_FAIL("unknown timing policy: " + program.timing);
    if (options.timing != LHG_TIMING_EXACT && options.mode == LHG_MODE_TRACE)
        LH_FAIL("experimental timing requires fast emission");
    std::optional<RegisterCache> selection = program.register_cache;
    if (!selection && options.timing != LHG_TIMING_EXACT)
        selection = read_selection(build_info().root / "games/ff8-usa/play-register-cache.json");
    lhg_register_cache cache{};
    std::vector<const char *> names, regions;
    if (selection) {
        for (const auto &name : selection->functions) names.push_back(name.c_str());
        if (selection->fuse_forward)
            for (const auto &name : *selection->fuse_forward) regions.push_back(name.c_str());
        cache = {selection->max_registers, names.data(), names.size(), regions.data(), regions.size()};
        options.register_cache = &cache;
    }
    check(lhgt_emit(session.get(), &main, &functions, directory.c_str(), sets.data(), sets.size(), &options),
          session.get(), "emit");
}

std::string run_compiler(const std::vector<std::string> &arguments) {
    Command command(compiler(), false);
    for (const std::string &argument : arguments) command.arg(argument);
    command.timeout(600);
    Result result = internal::run_process(command);
    if (result.timed_out) throw Failure("compiler timed out\n  command: " + result.command);
    if (result.rc != 0) {
        throw Failure("compiler failed (exit status " + std::to_string(result.rc) + ")\n  command: " + result.command +
                      "\n--- stderr ---\n" + excerpt(result.err));
    }
    return result.err;
}

// Runs the compile steps with a small pool, because the large shared programs
// would otherwise spend their single process on one translation unit at a time.
void run_compilers(std::vector<std::vector<std::string>> commands) {
    std::mutex mutex;
    size_t next = 0;
    std::string error;
    const auto worker = [&] {
        for (;;) {
            std::vector<std::string> arguments;
            {
                std::lock_guard<std::mutex> hold(mutex);
                if (next >= commands.size() || !error.empty()) return;
                arguments = std::move(commands[next++]);
            }
            try {
                run_compiler(arguments);
            } catch (const std::exception &failure) {
                std::lock_guard<std::mutex> hold(mutex);
                if (error.empty()) error = failure.what();
            }
        }
    };
    std::vector<std::thread> workers;
    const unsigned jobs = std::min<unsigned>(compile_jobs(), static_cast<unsigned>(commands.size()));
    for (unsigned index = 1; index < jobs; ++index) workers.emplace_back(worker);
    worker();
    for (std::thread &thread : workers) thread.join();
    if (!error.empty()) throw Failure(error);
}

bool sanitizer_flag(const std::string &flag) {
    return flag.rfind("-fsanitize", 0) == 0 || flag.rfind("-fno-sanitize", 0) == 0;
}

} // namespace

std::string pack(const Words &words) {
    std::string out;
    out.reserve(words.size() * 4);
    for (uint32_t word : words) {
        for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>(word >> shift));
    }
    return out;
}

Discovery::Discovery(const Image &image, const std::vector<uint32_t> &seeds, bool scan_calls)
    : image_(image), seeds_(seeds), session_(lhgt_open()) {
    if (!session_) LH_FAIL("cannot start a generator session");
    lhg_image raw{};
    std::vector<lhg_range> ranges;
    fill_image(raw, image_, ranges);
    check(lhgt_discover(session_, &raw, seeds_.data(), seeds_.size(), scan_calls, &functions_), session_, "discover");
}

Discovery::~Discovery() { lhgt_close(session_); }

std::set<uint32_t> Discovery::entries() const {
    std::set<uint32_t> out;
    for (size_t index = 0; index < functions_.count; ++index) out.insert(functions_.items[index].entry);
    return out;
}

std::set<uint32_t> Discovery::instructions(uint32_t entry) const {
    std::set<uint32_t> out;
    for (size_t index = 0; index < functions_.count; ++index) {
        const lhg_function &function = functions_.items[index];
        if (function.entry != entry) continue;
        for (uint32_t at = 0; at < function.insn_count; ++at) out.insert(function.insns[at].pc);
    }
    return out;
}

std::string decoded_name(uint32_t word, uint32_t pc) {
    static const char *const names[] = {
#define X(name) #name,
        LHG_OPS(X)
#undef X
    };
    const lhg_insn insn = lhg_decode(word, pc);
    std::string name = names[insn.op];
    for (char &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

Effects effects(uint32_t word, uint32_t pc) {
    const std::shared_ptr<lhgt_session> session = open_session();
    const lhg_insn insn = lhg_decode(word, pc);
    lhg_effect effect;
    check(lhgt_effects(session.get(), &insn, &effect), session.get(), "effects");
    Effects out;
    out.dst = effect.dst;
    out.load = effect.load;
    out.value = effect.value ? effect.value : "";
    out.loaded = effect.loaded ? effect.loaded : "";
    for (uint32_t index = 0; index < effect.lines.count; ++index) {
        out.lines.emplace_back(effect.lines.items[index].text, effect.lines.items[index].length);
    }
    return out;
}

Json Json::own(std::shared_ptr<lhgt_session> owner, const lhg_json *value) { return Json(std::move(owner), value); }

Json Json::parse(const std::string &text, const std::string &name) {
    std::shared_ptr<lhgt_session> session = open_session();
    const lhg_json *value = nullptr;
    check(lhgt_json_parse(session.get(), text.data(), text.size(), name.c_str(), &value), session.get(), "json");
    return Json(std::move(session), value);
}

Json Json::file(const fs::path &path) { return parse(read_file(path), path.string()); }

bool Json::contains(const std::string &key) const {
    if (value_->type == LHG_JSON_OBJECT) return lhg_json_find(value_, key.c_str()) != nullptr;
    if (value_->type == LHG_JSON_ARRAY) {
        for (size_t index = 0; index < value_->as.array.count; ++index) {
            const lhg_json *item = value_->as.array.items[index];
            if (item->type == LHG_JSON_STRING && item->as.string.length == key.size() &&
                !std::memcmp(item->as.string.text, key.data(), key.size())) return true;
        }
    }
    return false;
}

Json Json::at(const std::string &key) const {
    if (value_->type != LHG_JSON_OBJECT) LH_FAIL("not a JSON object");
    const lhg_json *found = lhg_json_find(value_, key.c_str());
    if (!found) LH_FAIL("no JSON member " + key);
    return Json(owner_, found);
}

Json Json::at(size_t index) const {
    if (value_->type != LHG_JSON_ARRAY) LH_FAIL("not a JSON array");
    if (index >= value_->as.array.count) LH_FAIL("JSON array index out of range");
    return Json(owner_, value_->as.array.items[index]);
}

size_t Json::size() const {
    if (value_->type == LHG_JSON_ARRAY) return value_->as.array.count;
    if (value_->type == LHG_JSON_OBJECT) return value_->as.object.count;
    LH_FAIL("not a JSON container");
}

std::string Json::str() const {
    if (value_->type != LHG_JSON_STRING) LH_FAIL("not a JSON string");
    return std::string(value_->as.string.text, value_->as.string.length);
}

int64_t Json::integer() const {
    if (value_->type != LHG_JSON_INT) LH_FAIL("not a JSON integer");
    return value_->as.integer;
}

EffectBase infer_effect_base(const std::string &data) {
    std::shared_ptr<lhgt_session> session = open_session();
    uint32_t base = 0;
    const lhg_json *evidence = nullptr;
    check(lhgt_infer_effect_base(session.get(), reinterpret_cast<const uint8_t *>(data.data()), data.size(),
                                 "probe", &base, &evidence), session.get(), "infer_effect_base");
    return {base, Json::own(session, evidence)};
}

std::string error_of(const std::function<void()> &body) {
    try {
        body();
    } catch (const Failure &failure) {
        return failure.what();
    }
    return "";
}

std::string Program::canonical() const {
    std::ostringstream out;
    out << "program|" << mode << '|' << host_mode << '|' << timing << '|' << image_text(image, seeds, scan_calls);
    for (const Overlay &overlay : overlays) out << "|overlay|" << overlay.index << '|'
        << image_text(overlay.image, overlay.seeds, overlay.scan_calls);
    if (register_cache) {
        out << "|register-cache|" << register_cache->max_registers << "|functions|";
        for (const auto &name : register_cache->functions) out << name << ',';
        out << "|fuse-forward|";
        if (register_cache->fuse_forward)
            for (const auto &name : *register_cache->fuse_forward) out << name << ',';
        else out << "absent";
    }
    if (!discover_functions) out << "|empty-functions";
    return out.str();
}

RegisterCache read_selection(const fs::path &path) {
    const auto session = open_session();
    const lhg_register_cache *raw = nullptr;
    check(lhgt_cache_read(session.get(), path.c_str(), &raw), session.get(), "read_selection");
    RegisterCache selection;
    selection.max_registers = raw->max_registers;
    for (size_t i = 0; i < raw->function_count; ++i) selection.functions.emplace_back(raw->functions[i]);
    if (raw->fuse_forward) {
        selection.fuse_forward.emplace();
        for (size_t i = 0; i < raw->fuse_forward_count; ++i) selection.fuse_forward->emplace_back(raw->fuse_forward[i]);
    }
    return selection;
}

void generate(Case &t, const Program &program, const fs::path &directory) {

    emit_program(program, directory);
}

Json Compiled::manifest() const { return Json::file(directory_ / "manifest.json"); }

Build &Build::program(Program program) { program_ = std::make_unique<Program>(std::move(program)); return *this; }
Build &Build::flags(std::initializer_list<std::string> values) {
    flags_.insert(flags_.end(), values);
    return *this;
}
Build &Build::flag(std::string value) { flags_.push_back(std::move(value)); return *this; }
Build &Build::header_prefix(std::string text) { header_prefix_ = std::move(text); return *this; }
Build &Build::runtime(std::initializer_list<std::string> names) { runtime_.insert(runtime_.end(), names); return *this; }
Build &Build::runtime_all() { runtime_all_ = true; return *this; }
Build &Build::source(fs::path repository_relative) { sources_.push_back(repository_relative.string()); return *this; }
Build &Build::probe(const std::string &name, std::vector<std::pair<std::string, std::string>> replacements) {
    std::string text = read_file(build_info().root / "tests/native/cases" / name);
    for (const auto &replacement : replacements) {
        std::string out;
        size_t at = 0;
        for (size_t hit; (hit = text.find(replacement.first, at)) != std::string::npos; at = hit + replacement.first.size()) {
            out.append(text, at, hit - at);
            out += replacement.second;
        }
        out.append(text, at, std::string::npos);
        text = std::move(out);
    }
    probes_.emplace_back(name, std::move(text));
    return *this;
}

Compiled Build::library() {
    if (build_info().windows_paths)
        skip("generated libraries require a native compiler and loader");
    if (!program_) LH_FAIL("Build::library() needs a program");
    if (build_info().windows_paths || !build_info().emulator.empty()) {
        skip("generated programs are compiled for and loaded into the native runner");
    }
    const std::string canonical = program_->canonical();


    std::vector<std::string> runtime_files = runtime_;
    if (runtime_all_) {
        runtime_files.clear();
        for (const auto &entry : fs::directory_iterator(build_info().root / "runtime")) {
            if (entry.path().extension() == ".c") runtime_files.push_back(entry.path().filename().string());
        }
    }
    // One order everywhere, so two programs taking object locks cannot deadlock.
    std::sort(runtime_files.begin(), runtime_files.end());

    std::vector<std::string> object_flags = flags_;
    object_flags.push_back("-fPIC");
    std::ostringstream flag_text;
    for (const std::string &flag : object_flags) flag_text << flag << '\n';

    std::ostringstream key_text;
    key_text << "v1\n" << compiler() << '\n' << runtime_fingerprint() << '\n' << generator_fingerprint() << '\n'
             << flag_text.str() << canonical << '\n';
    if (program_->timing != "exact" && !program_->register_cache)
        key_text << "default-register-cache=" << stat_signature(build_info().root / "games/ff8-usa/play-register-cache.json") << '\n';
    for (const std::string &file : runtime_files) key_text << "runtime:" << file << '\n';
    for (const std::string &source : sources_) {
        const fs::path path = build_info().root / source;
        key_text << "source:" << source << '=' << stat_signature(path) << '\n';
    }
    for (const auto &probe : probes_) key_text << "probe:" << probe.first << '\n' << probe.second << '\n';
    if (!header_prefix_.empty()) key_text << "header-prefix:" << header_prefix_ << '\n';
    const std::string key = sha256(key_text.str());

    const fs::path root = cache_root();
    const fs::path directory = root / key.substr(0, 2) / key;
    fs::create_directories(directory.parent_path());
    const fs::path library_path = directory / "lib.so";
    if (!fs::exists(library_path)) {
        Lock lock(fs::path(directory.string() + ".lock"));
        if (!fs::exists(library_path)) {
            Publication publication(directory);
            const fs::path &work = publication.directory();
            emit_program(*program_, work);
            if (!header_prefix_.empty())
                write_file(work / "generated.h", header_prefix_ + read_file(work / "generated.h"));

            const fs::path runtime_dir = build_info().root / "runtime";
            const std::string runtime_include = "-I" + runtime_dir.string();

            // Runtime objects are shared by every program with the same flags,
            // which is most of the saving: cpu.c and interp.c take far longer
            // to compile than a small generated program does.
            std::vector<std::string> objects;
            std::vector<std::vector<std::string>> runtime_commands;
            std::vector<std::unique_ptr<Lock>> locks;
            for (const std::string &file : runtime_files) {
                const fs::path source = runtime_dir / file;
                const std::string object_key = sha256(flag_text.str() + compiler() + "\n" + file + "\n" + runtime_fingerprint());
                const fs::path object_dir = root / "objects" / object_key.substr(0, 2) / object_key;
                const fs::path object = object_dir / (file + ".o");
                objects.push_back(object.string());
                if (fs::exists(object)) continue;
                fs::create_directories(object_dir.parent_path());
                locks.push_back(std::make_unique<Lock>(fs::path(object_dir.string() + ".lock")));
                if (fs::exists(object)) continue;
                std::error_code ignored;
                fs::create_directories(object_dir, ignored);
                std::vector<std::string> command = object_flags;
                command.insert(command.end(), {runtime_include, "-c", source.string(), "-o", object.string() + ".tmp"});
                runtime_commands.push_back(std::move(command));
            }
            run_compilers(runtime_commands);
            for (const std::vector<std::string> &command : runtime_commands) {
                const std::string &temporary = command.back();
                fs::rename(temporary, temporary.substr(0, temporary.size() - 4));
            }
            locks.clear();

            std::vector<std::vector<std::string>> commands;
            const std::string generated_include = "-I" + work.string();
            const auto compile = [&](const std::string &source, const std::string &name) {
                std::vector<std::string> command = object_flags;
                const std::string object = (work / (name + ".o")).string();
                command.insert(command.end(), {runtime_include, generated_include, "-c", source, "-o", object});
                objects.push_back(object);
                commands.push_back(std::move(command));
            };
            for (const auto &probe : probes_) {
                const std::string name = fs::path(probe.first).stem().string();
                const fs::path file = work / (name + ".c");
                write_file(file, probe.second);
                compile(file.string(), "probe_" + name);
            }
            for (const std::string &source : sources_) {
                compile((build_info().root / source).string(), "source_" + fs::path(source).stem().string());
            }
            std::vector<fs::path> generated;
            for (const auto &entry : fs::directory_iterator(work)) {
                if (entry.path().extension() == ".c") generated.push_back(entry.path());
            }
            std::sort(generated.begin(), generated.end());
            for (const fs::path &file : generated) {
                bool is_probe = false;
                for (const auto &probe : probes_) {
                    if (file.stem().string() == fs::path(probe.first).stem().string()) is_probe = true;
                }
                if (!is_probe) compile(file.string(), "gen_" + file.stem().string());
            }
            run_compilers(commands);

            std::vector<std::string> link = {"-shared"};
            for (const std::string &flag : flags_) if (sanitizer_flag(flag)) link.push_back(flag);
            link.insert(link.end(), objects.begin(), objects.end());
            link.insert(link.end(), {"-o", (work / "lib.so").string(), "-lm"});
            run_compiler(link);
            publication.publish();
        }
    }
    // A sanitizer's runtime must be loaded before other shared objects; it is
    // the first dependency of a -fsanitize library, so plain dlopen handles it.
    void *handle = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) LH_FAIL("cannot load " + library_path.string() + ": " + dlerror());
    return Compiled(handle, directory);
}

} // namespace lh::gen
