// Emit only synthetic inputs; the production metadata converter writes the header.
#include "synthetic_disc.h"
#include <iomanip>
#include <sstream>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::filesystem::path out(argv[1]); std::filesystem::create_directories(out);
    auto exe = synthetic::exe(); synthetic::write(out / "exe", exe);
    uint32_t hash = 2166136261u;
    for (size_t i = 2048; i < exe.size(); ++i) hash = (hash ^ uint8_t(exe[i])) * 16777619u;
    std::ostringstream hex; hex << std::hex << std::setfill('0') << std::setw(8) << hash;
    synthetic::write(out / "fnv", hex.str());
    for (int n = 1; n <= 4; ++n) synthetic::write(out / ("img" + std::to_string(n)), std::string(2048, char(n)));
}
