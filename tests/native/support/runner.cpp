// Case registry and the command line that CTest and developers use.
#include "lh_internal.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#define setenv(k,v,o) _putenv_s(k,v)
#ifndef SIGBUS
#define SIGBUS 7
#endif
#endif

namespace lh {

namespace internal {

BuildInfo &build_info() {
    static BuildInfo info;
    return info;
}

std::string stem_of(const std::string &case_name) {
    return case_name.substr(0, case_name.find("::"));
}

namespace {

struct Registered {
    std::string name;
    Tier tier;
    std::function<void(Case &)> body;
    std::string unavailable;
};

std::vector<Registered> &registry() {
    static std::vector<Registered> cases;
    return cases;
}

const char *tier_name(Tier tier) {
    switch (tier) {
    case Tier::game: return "game";
    case Tier::realtime: return "realtime";
    default: return "standard";
    }
}

fs::path executable_path() {
#ifdef _WIN32
    wchar_t path[32768];
    DWORD size = GetModuleFileNameW(nullptr, path, 32768);
    if (!size || size == 32768) throw Failure("cannot locate test runner");
    return fs::path(std::wstring(path, size));
#else
    return fs::read_symlink("/proc/self/exe");
#endif
}

void read_build_info(const fs::path &path) {
    std::ifstream stream(path);
    if (!stream) {
        std::cerr << "cannot read " << path.string() << "; configure and build tests/native first\n";
        std::exit(2);
    }
    BuildInfo &info = build_info();
    std::string line;
    while (std::getline(stream, line)) {
        const size_t split = line.find('=');
        if (split == std::string::npos) continue;
        const std::string key = line.substr(0, split), value = line.substr(split + 1);
        if (key == "root") info.root = fs::absolute(path.parent_path() / fs::path(value));
        else if (key == "windows_paths") info.windows_paths = value == "1";
        else if (key == "launcher") info.launcher = value;
        else if (key == "emulator") {
            std::istringstream parts(value);
            std::string part;
            while (std::getline(parts, part, ';')) if (!part.empty()) info.emulator.push_back(part);
        } else if (key.rfind("unavailable.", 0) == 0) {
            info.unavailable[key.substr(12)] = value;
        } else if (key.rfind("harness.", 0) == 0) {
            info.harnesses[key.substr(8)] = fs::absolute(path.parent_path() / fs::path(value)).string();
        }
    }
}

// A loaded library can take the process down from inside a C function, so the
// failure text has to come from the signal itself or CTest shows nothing but
// an exit code. Write that one line, then die of the same signal.
char crash_note[512];

extern "C" void on_fatal_signal(int number) {
    const char *name = number == SIGSEGV ? "SIGSEGV" : number == SIGABRT ? "SIGABRT"
                     : number == SIGFPE ? "SIGFPE" : number == SIGBUS ? "SIGBUS" : "fatal signal";
    (void)!write(1, crash_note, std::strlen(crash_note));
    (void)!write(1, name, std::strlen(name));
    (void)!write(1, "\n", 1);
    signal(number, SIG_DFL);
    raise(number);
}

void announce_crashes_for(const std::string &name) {
    std::snprintf(crash_note, sizeof(crash_note), "FAIL %s: the case process crashed with ", name.c_str());
    for (int number : {SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL}) signal(number, on_fatal_signal);
}

Registered *find_case(const std::string &name) {
    for (Registered &entry : registry()) if (entry.name == name) return &entry;
    return nullptr;
}

int run_case(const std::string &name) {
    Registered *entry = find_case(name);
    if (!entry) {
        std::cerr << "no such case: " << name << "\n";
        return 1;
    }
    // Relative paths in the original tests are relative to the repository.
    fs::current_path(build_info().root);
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    setenv("SDL_AUDIODRIVER", "dummy", 0);
    Case test(name);
    announce_crashes_for(name);
    try {
        if (build_info().windows_paths) {
            const auto stem = stem_of(name);
            if (stem == "module_loader") skip("native module fixtures require ELF freestanding libraries");
            if (stem == "test_frame_probe") skip("sampler requires Linux x86-64 processes");
            if (stem == "test_generator_parity" || stem == "test_generator_rules" || stem == "test_setup_cache")
                skip("generator test programs are built for the host");
            if (stem == "test_frame_bench") skip("benchmark fixture requires POSIX process and script conventions");
            if (stem == "test_state_bridge") skip("state bridge fixture requires POSIX sockets and syscalls");
            if (stem == "test_metadata_tool") skip("metadata utility requires POSIX process support");
            if (stem == "test_ra_gui" || stem == "test_host_gui" || stem == "test_host_gui_vulkan" ||
                stem == "test_host_crt" || stem == "test_host_hotkeys" || stem == "test_vulkan")
                skip("display/GPU fixture requires a native interactive session");
        }
        if (entry->tier == Tier::game) {
            const char *discs = std::getenv("LIONHEART_DISCS");
            if (!discs || !*discs) skip("set LIONHEART_DISCS to run owned-game tests");
        }
        entry->body(test);
        std::cout << "PASS " << name << "\n";
        return 0;
    } catch (const Skip &reason) {
        std::cout << "SKIP: " << reason.what() << "\n";
        return 77;
    } catch (const std::exception &error) {
        std::cout << "FAIL " << name << "\n" << error.what() << "\n";
    } catch (...) {
        std::cout << "FAIL " << name << ": unknown exception\n";
    }
    if (!test.temp_if_created().empty()) {
        test.keep_temp();
        std::cout << "temporary files kept in " << test.temp_if_created().string() << "\n";
    }
    return 1;
}

int emit_ctest(const fs::path &output, const fs::path &info_path) {
    std::ofstream stream(output);
    const std::string runner = executable_path().string();
    std::vector<const Registered *> cases;
    for (const Registered &entry : registry()) cases.push_back(&entry);
    std::sort(cases.begin(), cases.end(), [](auto *a, auto *b) { return a->name < b->name; });
    for (const Registered *entry : cases) {
        const std::string guard = "[==[" + entry->name + "]==]";
        std::string label;
        if (entry->tier != Tier::standard) label = tier_name(entry->tier);
        else if (entry->name.rfind("test_", 0) != 0) label = "infra";
        stream << "add_test(" << guard << " [==[" << runner << "]==] --build-info [==[" << info_path.string()
               << "]==] --run " << guard << ")\n"
               << "set_tests_properties(" << guard << " PROPERTIES SKIP_RETURN_CODE 77";
        if (!label.empty()) stream << " LABELS " << label;
        stream << ")\n";
    }
    return stream ? 0 : 1;
}

struct BaselineRow {
    std::string nodeid, native, tier, disposition;
};

std::vector<BaselineRow> read_baseline() {
    const fs::path path = build_info().root / "tests/native/identities.tsv";
    std::ifstream stream(path);
    if (!stream) LH_FAIL("cannot read " + path.string());
    std::vector<BaselineRow> rows;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream cols(line);
        BaselineRow row;
        std::getline(cols, row.nodeid, '\t');
        std::getline(cols, row.tier, '\t');
        std::getline(cols, row.disposition, '\t');
        const std::string prefix = "tests/", suffix = ".py::";
        const size_t split = row.nodeid.find(suffix);
        if (row.nodeid.rfind(prefix, 0) != 0 || split == std::string::npos) {
            LH_FAIL("malformed identity in identities.tsv: " + row.nodeid);
        }
        row.native = row.nodeid.substr(prefix.size(), split - prefix.size()) + "::" +
                     row.nodeid.substr(split + suffix.size());
        rows.push_back(std::move(row));
    }
    return rows;
}

std::set<std::string> ported_stems(const std::vector<std::string> &requested) {
    if (!requested.empty()) return {requested.begin(), requested.end()};
    std::set<std::string> stems;
    // The frozen inventory defines migration coverage. New native suites have
    // no historical identities; missing historical suites must still be checked.
    for (const BaselineRow &row : read_baseline()) stems.insert(stem_of(row.native));
    return stems;
}

bool compare_results(const fs::path &report, const std::vector<std::string> &requested) {
    const std::string xml = read_file(report);
    const auto attribute = [](const std::string &element, const std::string &name) {
        const std::string marker = " " + name + "=\"";
        size_t at = element.find(marker);
        if (at == std::string::npos) return std::string();
        at += marker.size();
        std::string value = element.substr(at, element.find('"', at) - at);
        static const std::pair<const char *, char> entities[] = {
            {"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}};
        for (const auto &entity : entities) {
            for (size_t hit; (hit = value.find(entity.first)) != std::string::npos;) {
                value.replace(hit, std::strlen(entity.first), 1, entity.second);
            }
        }
        return value;
    };
    std::map<std::string, std::string> statuses;
    for (size_t at = 0; (at = xml.find("<testcase", at)) != std::string::npos; ++at) {
        const std::string element = xml.substr(at, xml.find('>', at) - at);
        statuses[attribute(element, "name")] = attribute(element, "status");
    }
    const std::set<std::string> stems = ported_stems(requested);
    bool ok = true;
    size_t compared = 0;
    for (const BaselineRow &row : read_baseline()) {
        if (!stems.count(stem_of(row.native))) continue;
        const auto found = statuses.find(row.native);
        const std::string status = found == statuses.end() ? "missing" : found->second;
        const bool passed = status == "run", skipped = status == "notrun";
        const bool match = row.disposition == "pass" ? passed
                         : row.disposition == "skip" ? skipped
                         : passed || skipped || status == "missing";
        ++compared;
        if (!match) {
            ok = false;
            std::cout << "MISMATCH " << row.native << ": baseline " << row.disposition
                      << ", native " << status << "\n";
        }
    }
    std::cout << compared << " identities compared with the baseline: "
              << (ok ? "all match" : "differences found") << "\n";
    return ok;
}

bool duplicates_found() {
    std::set<std::string> seen;
    bool found = false;
    for (const Registered &entry : registry()) {
        if (!seen.insert(entry.name).second) {
            std::cerr << "duplicate case name: " << entry.name << "\n";
            found = true;
        }
    }
    return found;
}

} // namespace

bool check_identities(const std::vector<std::string> &requested) {
    const std::vector<BaselineRow> baseline = read_baseline();
    bool ok = true;
    for (const std::string &stem : ported_stems(requested)) {
        std::set<std::string> known, native;
        std::map<std::string, std::string> tiers;
        for (const Registered &entry : registry()) if (stem_of(entry.name) == stem) native.insert(entry.name);
        for (const BaselineRow &row : baseline) {
            if (stem_of(row.native) == stem) {
                known.insert(row.native);
                tiers[row.native] = row.tier;
            }
        }
        std::vector<std::string> problems;
        if (known.empty() && native.empty()) problems.push_back("unknown file stem: " + stem);
        for (const std::string &name : known) {
            if (!native.count(name)) problems.push_back("missing " + name);
        }
        for (const std::string &name : native) if (!known.count(name)) problems.push_back("extra   " + name);
        for (const std::string &name : native) {
            const char *tier = tier_name(find_case(name)->tier);
            if (known.count(name) && tiers[name] != tier) {
                problems.push_back("tier    " + name + ": native " + tier + ", baseline " + tiers[name]);
            }
        }
        std::cout << stem << ": " << native.size() << " native, " << known.size() << " baseline";
        if (problems.empty()) {
            std::cout << ", identical\n";
        } else {
            std::cout << ", MISMATCH\n";
            for (const std::string &problem : problems) std::cout << "  " << problem << "\n";
            ok = false;
        }
    }
    return ok;
}

} // namespace internal

void add_case(const char *file, const std::string &name, Tier tier, std::function<void(Case &)> body) {
    internal::registry().push_back({fs::path(file).stem().string() + "::" + name, tier, std::move(body), {}});
}
void add_unavailable_case(const char *file, const std::string &name, Tier tier, const std::string &reason) {
    add_case(file, name, tier, [reason](Case &) { skip(reason); });
    internal::registry().back().unavailable = reason;
}


} // namespace lh

int main(int argc, char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    using namespace lh;
    using namespace lh::internal;
    if (duplicates_found()) return 2;
    fs::path info_path = executable_path().parent_path() / "lh_build_info.txt";
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() >= 2 && args[0] == "--build-info") {
        info_path = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    const std::string mode = args.empty() ? "" : args[0];
    const std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
    if (mode == "--list") {
        for (const Registered &entry : registry()) std::cout << tier_name(entry.tier) << "\t" << entry.name << "\n";
        return 0;
    }
    if (mode == "--emit-ctest" && rest.size() == 1) return emit_ctest(rest[0], info_path);
    read_build_info(info_path);
    try {
        if (mode == "--suite") {
            unsigned passed = 0, skipped = 0, failed = 0;
            std::map<std::string, unsigned> reasons;
            for (const auto &entry : registry()) {
                if (entry.tier == Tier::game || entry.tier == Tier::realtime) continue;
                if (!entry.unavailable.empty()) {
                    ++skipped; ++reasons["SKIP: " + entry.unavailable + "\n"];
                    std::cout << "SKIP " << entry.name << ": " << entry.unavailable << "\n";
                    continue;
                }
                auto result = run_process(Command(executable_path().string(), false)
                    .args({"--build-info", info_path, "--run", entry.name}).timeout(300));
                if (result.rc == 77) std::cout << "SKIP " << entry.name << "\n";
                std::cout << result.out << result.err;
                if (result.rc == 77) { ++skipped; ++reasons[result.out]; }
                else if (result.rc == 0 && !result.timed_out) ++passed;
                else ++failed;
            }
            std::cout << "RESULT passed=" << passed << " skipped=" << skipped << " failed=" << failed << "\n";
            for (const auto &reason : reasons) std::cout << reason.second << " " << reason.first;
            return failed ? 1 : 0;
        }
        if (mode == "--run" && rest.size() == 1) return run_case(rest[0]);
        if (mode == "--check-identities") return check_identities(rest) ? 0 : 1;
        if (mode == "--compare-results" && !rest.empty()) {
            return compare_results(rest[0], {rest.begin() + 1, rest.end()}) ? 0 : 1;
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    std::cerr << "usage: lionheart_tests [--build-info FILE] --list | --emit-ctest FILE | --run NAME |\n"
                 "       --check-identities [STEM...] | --compare-results JUNIT.xml [STEM...]\n";
    return 2;
}
