// The same wholly synthetic raw ISO fixture as test_host_player.disc's
// defaults.
#pragma once
#include "lh_test.h"
#include "py_random.h"
namespace lh {
inline fs::path synthetic_disc(const fs::path &path, unsigned number = 1) {
    std::string raw(832 * 2352, '\0'), exe(4096, '\0');
    auto le = [](std::string &s, size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            s.at(at + i) = char(v >> (8 * i));
    };
    exe.replace(0, 8, "PS-X EXE");
    le(exe, 16, 0x80010000);
    le(exe, 24, 0x80010000);
    le(exe, 28, 2048);
    PyRandom random(14);
    exe.replace(2048, 2048, random.randbytes(2048));
    le(exe, 2048, 0x1000ffff);
    le(exe, 2052, 0);
    auto sector = [&](unsigned lba, const std::string &s) {
        raw.at(lba * 2352 + 15) = 2;
        raw.replace(lba * 2352 + 24, s.size(), s);
    };
    auto record = [&](unsigned lba, unsigned size, const std::string &name) {
        std::string r(33 + name.size() + (name.size() % 2 == 0), '\0');
        r[0] = char(r.size());
        le(r, 2, lba);
        le(r, 10, size);
        r[32] = char(name.size());
        r.replace(33, name.size(), name);
        return r;
    };
    const char *serials[] = {"SLUS_008.92", "SLUS_009.08", "SLUS_009.09", "SLUS_009.10"};
    const std::string serial = serials[number - 1], config = "BOOT = cdrom:\\" + serial + ";1\r\n";
    std::string root(2048, '\0'), volume(2048, '\0');
    auto records = record(21, config.size(), "SYSTEM.CNF;1") + record(30, exe.size(), serial + ";1") +
                   record(826, 2048, "FF8DISC" + std::to_string(number) + ".IMG;1");
    root.replace(0, records.size(), records);
    volume.replace(0, 7, std::string("\1CD001\1", 7));
    le(volume, 158, 20);
    le(volume, 166, 2048);
    sector(16, volume);
    sector(20, root);
    sector(21, config);
    sector(30, exe.substr(0, 2048));
    sector(31, exe.substr(2048));
    sector(826, std::string(2048, char(number)));
    write_file(path, raw);
    return path;
}
} // namespace lh
