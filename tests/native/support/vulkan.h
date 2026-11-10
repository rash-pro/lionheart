// Build the same isolated host contracts as pytest, outside the harness project:
// its FF8_PLAYER definition must never reach a test harness.
#pragma once
#include "lh_test.h"
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace lh {
inline bool executable_on_path(const std::string &name) {
    const char *path = std::getenv("PATH");
    std::istringstream parts(path ? path : "");
    for (std::string folder; std::getline(parts, folder, ':');)
        if (access((fs::path(folder.empty() ? "." : folder) / name).c_str(), X_OK) == 0) return true;
    return false;
}
class FixtureLock {
public:
    explicit FixtureLock(const fs::path &path) : fd_(open(path.c_str(), O_CREAT | O_RDWR, 0600)) {
        if (fd_ < 0 || flock(fd_, LOCK_EX)) LH_FAIL("cannot lock host fixture " + path.string());
    }
    ~FixtureLock() { if (fd_ >= 0) close(fd_); }
    FixtureLock(const FixtureLock &) = delete;
    FixtureLock &operator=(const FixtureLock &) = delete;
private:
    int fd_;
};
inline fs::path host_contract(Case &t, const std::string &target, bool replay = true,
                              fs::path folder = {}) {
    if (!executable_on_path("cmake"))
        skip(replay ? "CMake is required for the optional Vulkan gate" : "CMake is required for the CRT presentation gate");
    if (folder.empty()) folder = fs::read_symlink("/proc/self/exe").parent_path() / "fixtures" / (replay ? "vulkan" : "crt");
    fs::create_directories(folder / "gen");
    FixtureLock lock(folder / "build.lock");
    if (!fs::exists(folder / "gen/sources.cmake")) {
        write_file(folder / "gen/sources.cmake", "set(GENERATED_SOURCES \"\")\n");
        write_file(folder / "gen/manifest.json", "{}\n");
    }
    if (!fs::exists(folder / "host/CMakeCache.txt")) {
        auto configure = t.host("cmake").args({"-S", t.root(), "-B", folder / "host",
            "-DGENERATED_DIR=" + (folder / "gen").string(), "-DFF8_PLAYER=OFF", "-DFF8_VULKAN=ON", "-DCMAKE_BUILD_TYPE=Release"});
        if (replay) configure.args({"-DFF8_VULKAN_TESTING=ON", "-DFF8_PGXP=ON"});
        t.ok(configure.timeout(120));
    }
    // Check dependencies on every use, so a source edit cannot leave a stale
    // cached contract. The lock also protects Makefiles during parallel CTest.
    t.ok(t.host("nice").args({"-n", "10", "cmake", "--build", folder / "host", "--target", target, "-j4"}).timeout(300));
    return folder / "host" / target;
}
inline fs::path vulkan_replay(Case &t) {
    auto binary = host_contract(t, "gpu_replay");
    host_contract(t, "gpu_wsi_contract");
    auto probe = t.run(t.host(binary.string()).args({"--self-test", "1"}).timeout(120));
    if (probe.err.find("Vulkan warning: using native soft presentation") != std::string::npos) {
        const char *driver = std::getenv("VK_DRIVER_FILES"), *icd = std::getenv("VK_ICD_FILENAMES");
        if ((driver && *driver) || (icd && *icd))
            LH_FAIL("The explicitly selected Vulkan driver is unavailable:\n" + probe.err);
        skip("No usable Vulkan device: " + probe.err);
    }
    LH_CHECK_MSG(probe.rc == 0, probe.err);
    return binary;
}
inline fs::path crt_preview(Case &t) {
    vulkan_replay(t);
    return host_contract(t, "crt_preview", false);
}
}
