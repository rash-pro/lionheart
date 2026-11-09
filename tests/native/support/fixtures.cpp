// Case state, file helpers, comparison text and owned-disc discovery.
#include "lh_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <dlfcn.h>
#include <unistd.h>

namespace lh {

using internal::build_info;

void fail_at(const char *file, int line, const std::string &message) {
    throw Failure(std::string(fs::path(file).filename()) + ":" + std::to_string(line) + ": " + message);
}

void skip(const std::string &reason) { throw Skip(reason); }

Case::Case(std::string name) : name_(std::move(name)), stem_(internal::stem_of(name_)) {}

Case::~Case() {
    if (!temp_.empty() && !keep_temp_ && !std::getenv("LH_KEEP_TEMP")) {
        std::error_code ignored;
        fs::remove_all(temp_, ignored);
    }
}

const fs::path &Case::temp() {
    if (temp_.empty()) {
        std::string prefix = name_;
        for (char &c : prefix) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.') c = '_';
        }
        temp_ = mkdtemp_under(fs::temp_directory_path() / "lionheart-tests", prefix.substr(0, 100) + "-");
    }
    return temp_;
}

fs::path Case::root() const { return build_info().root; }

Command Case::harness(const std::string &name) const {
    const std::string key = stem_ + "." + name;
    const auto found = build_info().harnesses.find(key);
    if (found == build_info().harnesses.end()) {
        std::string declared;
        for (const auto &entry : build_info().harnesses) {
            if (entry.first.rfind(stem_ + ".", 0) == 0) declared += "\n  " + entry.first;
        }
        LH_FAIL("harness " + key + " is not declared in " + stem_ + ".cmake; declared:" + declared);
    }
    if (!fs::exists(found->second)) LH_FAIL("harness " + key + " was not built (" + found->second + ")");
    return Command(found->second, true);
}

// Libraries are only ever built for the machine the runner is on: a cross build
// has no way to load a Windows library into a Linux process, so a case asking for
// one there records why and skips instead of pretending to have run it.
Library Case::library(const std::string &name) const {
    const std::string key = stem_ + "." + name;
    const auto found = build_info().harnesses.find(key);
    if (found == build_info().harnesses.end()) {
        if (build_info().windows_paths || !build_info().emulator.empty()) {
            skip("library " + key + " is only built for a native runner");
        }
        std::string declared;
        for (const auto &entry : build_info().harnesses) {
            if (entry.first.rfind(stem_ + ".", 0) == 0) declared += "\n  " + entry.first;
        }
        LH_FAIL("library " + key + " is not declared in " + stem_ + ".cmake; declared:" + declared);
    }
    if (!fs::exists(found->second)) LH_FAIL("library " + key + " was not built (" + found->second + ")");
    void *handle = dlopen(found->second.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) LH_FAIL("cannot load " + found->second + ": " + dlerror());
    return Library(handle, key);
}

void *Library::address(const char *symbol) const {
    void *found = dlsym(handle_, symbol);
    if (!found) LH_FAIL("library " + name_ + " does not export " + symbol);
    return found;
}

Command Case::player() const {
    const char *path = std::getenv("FF8_PLAYER_HOST");
    if (!path || !*path) LH_FAIL("set FF8_PLAYER_HOST to the player binary");
    return Command(path, true);
}

Result Case::run(const Command &command) {
    Result result = internal::run_process(command);
    if (result.timed_out) {
        throw Failure("command timed out after " + std::to_string(result.timeout) + " s\n  command: " + result.command +
                "\n--- stdout ---\n" + excerpt(result.out) + "\n--- stderr ---\n" + excerpt(result.err));
    }
    return result;
}

Result Case::ok(const Command &command) {
    Result result = run(command);
    if (result.rc != 0) {
        std::string status = result.rc < 0 ? "killed by signal " + std::to_string(-result.rc)
                                           : "exit status " + std::to_string(result.rc);
        throw Failure(status + "\n  command: " + result.command + "\n--- stdout ---\n" + excerpt(result.out) +
                "\n--- stderr ---\n" + excerpt(result.err));
    }
    return result;
}

std::string read_file(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) LH_FAIL("cannot read " + path.string());
    std::ostringstream data;
    data << stream.rdbuf();
    return data.str();
}

void write_file(const fs::path &path, const std::string &contents) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream) LH_FAIL("cannot write " + path.string());
}

fs::path mkdtemp_under(const fs::path &parent, const std::string &prefix) {
    fs::create_directories(parent);
    std::string pattern = (parent / (prefix + "XXXXXX")).string();
    if (!mkdtemp(pattern.data())) LH_FAIL("cannot create a directory under " + parent.string());
    return pattern;
}

std::string capture(const std::string &text, const std::string &pattern, int group) {
    std::smatch match;
    if (!std::regex_search(text, match, std::regex(pattern)) ) {
        LH_FAIL("no match for /" + pattern + "/ in:\n" + excerpt(text));
    }
    return match[group].str();
}

std::string excerpt(const std::string &text) {
    const size_t limit = 4000;
    if (text.size() <= limit) return text;
    return "... " + std::to_string(text.size() - limit) + " earlier bytes omitted ...\n" + text.substr(text.size() - limit);
}

std::string describe_bytes_diff(const std::string &actual, const std::string &expected) {
    size_t common = std::min(actual.size(), expected.size());
    size_t at = 0;
    while (at < common && actual[at] == expected[at]) ++at;
    const auto dump = [at](const std::string &data) {
        size_t begin = at >= 8 ? at - 8 : 0;
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (size_t i = begin; i < std::min(data.size(), begin + 24); ++i) {
            if (i == at) out << "[";
            out << std::setw(2) << (unsigned)static_cast<unsigned char>(data[i]);
            out << (i == at ? "] " : " ");
        }
        return out.str();
    };
    std::ostringstream out;
    out << "first difference at byte " << at << " (sizes: actual " << actual.size() << ", expected " << expected.size() << ")"
        << "\n  actual:   " << dump(actual) << "\n  expected: " << dump(expected);
    return out.str();
}

std::string describe_text_diff(const std::string &actual, const std::string &expected) {
    std::istringstream a(actual), b(expected);
    std::string left, right;
    int line = 1;
    for (;; ++line) {
        bool has_left = static_cast<bool>(std::getline(a, left));
        bool has_right = static_cast<bool>(std::getline(b, right));
        if (!has_left && !has_right) break;
        if (has_left != has_right || left != right) {
            return "first differing line " + std::to_string(line) +
                   "\n  actual:   " + (has_left ? left : "<end of text>") +
                   "\n  expected: " + (has_right ? right : "<end of text>");
        }
    }
    return describe_bytes_diff(actual, expected);
}

// SHA-256, FIPS 180-4. Tests keep regression hashes of frames and cards.
std::string sha256(const std::string &bytes) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::string data = bytes;
    const uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back('\0');
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t at = 0; at < data.size(); at += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto *p = reinterpret_cast<const unsigned char *>(data.data() + at + i * 4);
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],j=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t t1 = j + s1 + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
            j=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=j;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint32_t word : h) out << std::setw(8) << word;
    return out.str();
}

std::string sha256_file(const fs::path &path) { return sha256(read_file(path)); }

namespace {

struct Record { uint32_t lba, size; std::string name; };

Record parse_record(const std::string &raw, size_t at) {
    const auto word = [&](size_t offset) {
        const auto *p = reinterpret_cast<const unsigned char *>(raw.data() + at + offset);
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    };
    const unsigned length = static_cast<unsigned char>(raw[at + 32]);
    std::string name = raw.substr(at + 33, length);
    return {word(2), word(10), name.substr(0, name.find(';'))};
}

// The file names in the root of a raw MODE2/2352 disc described by a .cue.
std::set<std::string> root_names(const fs::path &cue) {
    std::string text = read_file(cue), line, bin;
    std::istringstream lines(text);
    while (std::getline(lines, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, 4, "FILE") != 0) continue;
        size_t open = line.find('"'), close = line.find('"', open + 1);
        if (open == std::string::npos || close == std::string::npos) LH_FAIL("malformed FILE line in " + cue.string());
        bin = line.substr(open + 1, close - open - 1);
        break;
    }
    if (bin.empty()) LH_FAIL("no FILE line in " + cue.string());
    std::ifstream image(cue.parent_path() / bin, std::ios::binary);
    if (!image) LH_FAIL("cannot read " + (cue.parent_path() / bin).string());
    const auto sector = [&](uint32_t lba) {
        std::string raw(2352, '\0');
        image.seekg(static_cast<std::streamoff>(lba) * 2352);
        image.read(raw.data(), 2352);
        return raw.substr(24, 2048);
    };
    const std::string volume = sector(16);
    const Record root = parse_record(volume, 156);
    std::string directory;
    for (uint32_t lba = root.lba; directory.size() < root.size; ++lba) directory += sector(lba);
    directory.resize(root.size);
    std::set<std::string> names;
    for (size_t at = 0; at < directory.size();) {
        const unsigned length = static_cast<unsigned char>(directory[at]);
        if (length == 0) { at = (at / 2048 + 1) * 2048; continue; }
        names.insert(parse_record(directory, at).name);
        at += length;
    }
    return names;
}

const struct { int number; const char *exe, *image; } kUsaDiscs[] = {
    {1, "SLUS_008.92", "FF8DISC1.IMG"}, {2, "SLUS_009.08", "FF8DISC2.IMG"},
    {3, "SLUS_009.09", "FF8DISC3.IMG"}, {4, "SLUS_009.10", "FF8DISC4.IMG"},
};

void scan_folder(const fs::path &folder, std::set<fs::path> &seen, std::map<int, fs::path> &found) {
    // Disc folders may hold symlinks to each other, so guard against cycles.
    if (!seen.insert(fs::canonical(folder)).second) return;
    std::vector<fs::directory_entry> entries(fs::directory_iterator{folder}, fs::directory_iterator{});
    std::sort(entries.begin(), entries.end());
    for (const fs::directory_entry &entry : entries) {
        std::string extension = entry.path().extension().string();
        for (char &c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (entry.is_directory()) { scan_folder(entry.path(), seen, found); continue; }
        if (extension != ".cue") continue;
        std::set<std::string> names = root_names(entry.path());
        std::vector<int> matches;
        for (const auto &disc : kUsaDiscs) {
            if (names.count(disc.exe) && names.count(disc.image)) matches.push_back(disc.number);
        }
        if (matches.size() != 1) continue;
        if (found.count(matches[0])) LH_FAIL("duplicate owned disc " + std::to_string(matches[0]));
        found[matches[0]] = fs::canonical(entry.path());
    }
}

const std::map<int, fs::path> &owned_discs() {
    static std::map<int, fs::path> found;
    static bool scanned = false;
    if (scanned) return found;
    const char *folder = std::getenv("LIONHEART_DISCS");
    if (!folder || !*folder) skip("set LIONHEART_DISCS to run owned-game tests");
    std::set<fs::path> seen;
    scan_folder(folder, seen, found);
    if (!found.count(1)) LH_FAIL("LIONHEART_DISCS does not contain USA disc 1 (SLUS-00892)");
    scanned = true;
    return found;
}

} // namespace

fs::path owned_disc(int number) {
    const auto &discs = owned_discs();
    const auto found = discs.find(number);
    if (found == discs.end()) skip("owned USA disc " + std::to_string(number) + " is not supplied");
    return found->second;
}

fs::path owned_path(const std::string &relative) {
    owned_discs();
    if (relative.rfind("saves/", 0) == 0) {
        const char *saves = std::getenv("LIONHEART_SAVES");
        fs::path folder = saves && *saves ? fs::path(saves) : fs::path(std::getenv("LIONHEART_DISCS")) / "saves";
        fs::path path = folder / relative.substr(6);
        if (!fs::is_regular_file(path)) skip("optional owned memory card is not supplied (LIONHEART_SAVES)");
        return path;
    }
    fs::path path = build_info().root / "build/generated/extracted" / relative;
    if (!fs::is_regular_file(path)) LH_FAIL("run python tools/setup.py --disc1 DISC before owned-game tests");
    return path;
}

} // namespace lh
