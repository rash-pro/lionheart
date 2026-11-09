// Runner internals shared by the support files; test cases do not use this.
#pragma once

#include "lh_test.h"

namespace lh::internal {

struct BuildInfo {
    fs::path root;
    std::vector<std::string> emulator;
    bool windows_paths = false;
    std::map<std::string, std::string> harnesses;
};

BuildInfo &build_info();
Result run_process(const Command &command);
std::string stem_of(const std::string &case_name);
std::string to_windows_path(const std::string &path);
bool check_identities(const std::vector<std::string> &requested);

} // namespace lh::internal
