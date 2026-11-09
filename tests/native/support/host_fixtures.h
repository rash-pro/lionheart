// Common process and file operations for isolated host contracts.
#pragma once
#include "lh_test.h"
#include <algorithm>
namespace lh {
inline Command dummy(Command command) {
    return command.env("SDL_VIDEODRIVER", "dummy").env("SDL_AUDIODRIVER", "dummy");
}
inline std::vector<std::string> lines(const std::string &text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}
inline std::vector<fs::path> files_starting(const fs::path &folder, const std::string &prefix) {
    std::vector<fs::path> out;
    if (fs::exists(folder)) for (const auto &entry : fs::directory_iterator(folder))
        if (entry.path().filename().string().rfind(prefix, 0) == 0) out.push_back(entry.path());
    std::sort(out.begin(), out.end());
    return out;
}
}
