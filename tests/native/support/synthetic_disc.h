// Match the synthetic ISO and seeded EXE used by the host pytest fixtures.
#pragma once
#include "lh_test.h"
#include "py_random.h"
#include <cstdint>
#include <filesystem>
#include <string>
namespace synthetic {
inline void put32(std::string &s, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) s[at + i] = char(v >> (8 * i));
}
inline std::string exe() {
    std::string s(4096, '\0'); s.replace(0, 8, "PS-X EXE");
    put32(s, 16, 0x80010000); put32(s, 24, 0x80010000); put32(s, 28, 2048);
    lh::PyRandom random(14);
    s.replace(2048, 2048, random.randbytes(2048));
    put32(s, 2048, 0x1000ffff); put32(s, 2052, 0); return s;
}
inline const char *serial(int n) {
    static const char *names[] = {"SLUS_008.92", "SLUS_009.08", "SLUS_009.09", "SLUS_009.10"};
    return names[n-1];
}
struct Disc {
    int number = 1, offset = 0;
    std::string name, boot;
    bool image = true;
    int mode = 2, exe_size = -1, img_size = 2048, img_lba = 826;
    bool cnf = true, pvd = true, bad_record = false;
    std::string executable = exe();
};
inline std::filesystem::path disc(const std::filesystem::path &path, Disc d = {}) {
    std::string raw((832 + d.offset) * 2352, '\0');
    auto sector = [&](int lba, const std::string &data) {
        size_t at = (lba + d.offset) * 2352; raw[at+15] = char(d.mode); raw.replace(at+(d.mode == 1 ? 16 : 24), data.size(), data);
    };
    auto record = [](int lba, int size, const std::string &name) {
        std::string r(33 + name.size() + (name.size()%2 == 0), '\0'); r[0] = char(r.size());
        put32(r, 2, lba); put32(r, 10, size); r[32] = char(name.size()); r.replace(33, name.size(), name); return r;
    };
    if (d.name.empty()) d.name = serial(d.number);
    if (d.boot.empty()) d.boot = "BOOT = cdrom:\\" + d.name + ";1\r\n";
    auto e = d.executable; std::string root(2048, '\0'), volume(2048, '\0');
    auto records = (d.cnf ? record(21, d.boot.size(), "SYSTEM.CNF;1") : "") + record(30, d.exe_size < 0 ? e.size() : d.exe_size, d.name + ";1");
    if (d.image) records += record(d.img_lba, d.img_size, "FF8DISC" + std::to_string(d.number) + ".IMG;1");
    root.replace(0, records.size(), records); if (d.bad_record) root[0] = 33;
    if (d.pvd) volume.replace(0, 7, std::string("\1CD001\1", 7));
    put32(volume, 158, 20); put32(volume, 166, 2048);
    sector(16, volume); sector(20, root); sector(21, d.boot);
    sector(30, e.substr(0, 2048)); sector(31, e.substr(2048)); sector(d.img_lba, std::string(std::min(d.img_size, 2048), char(d.number)));
    lh::write_file(path, raw); return path;
}
}
