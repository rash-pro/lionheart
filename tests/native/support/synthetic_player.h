#pragma once
#include "synthetic_disc.h"
#include "generated.h"
#include "lh_internal.h"
#include <algorithm>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
namespace synthetic {
inline void empty_catalog(const std::string &executable, const lh::fs::path &out) {
    lh::fs::create_directories(out);
    auto *session = lhgt_open();
    LH_CHECK(session != nullptr);
    lhg_image image{};
    image.data = reinterpret_cast<const uint8_t *>(executable.data() + 2048);
    auto word = [&](size_t at) {
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(uint8_t(executable.at(at+i))) << (8*i);
        return value;
    };
    image.size = word(28);
    image.base = word(24); image.entry = word(16);
    lhg_functions functions{};
    lhg_emit_options options{};
    options.host_mode = LHG_HOST_FAST;
    int rc = lhgt_emit(session, &image, &functions, out.c_str(), nullptr, 0, &options);
    std::string error = lhgt_error(session);
    lhgt_close(session);
    LH_CHECK_MSG(rc == 0, error);
}
inline lh::fs::path player(lh::Case &t, bool dev = false) {
    if (lh::internal::build_info().windows_paths)
        lh::skip("synthetic player fixture requires an on-demand native build");
    std::string inputs = t.root().string();
    for (auto folder : {"runtime", "backends/host", "generator", "games/ff8-usa"}) {
        std::vector<lh::fs::path> files;
        for (auto &e : lh::fs::recursive_directory_iterator(t.root()/folder))
            if (e.is_regular_file()) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (auto &f : files) inputs += f.string() + lh::read_file(f);
    }
    inputs += lh::read_file(t.root()/"CMakeLists.txt") + lh::read_file(t.root()/"tests/native/support/synthetic_player.h");
    inputs += lh::read_file(t.root()/"data/toolchain/manifest.json");
    auto dir = lh::fs::temp_directory_path()/("lionheart-synthetic-"+lh::sha256(inputs)+(dev?"-dev":"-player"));
    lh::fs::create_directories(dir);
    int fd = open((dir/"lock").c_str(), O_CREAT|O_RDWR, 0600);
    LH_CHECK(fd >= 0); LH_EQ(flock(fd, LOCK_EX), 0);
    struct Lock { int fd; ~Lock() { flock(fd, LOCK_UN); close(fd); } } lock{fd};
    auto binary = dir/"host"/(dev?"ff8":"lionheart");
    if (!lh::fs::exists(dir/"ready")) {
        auto cmd = t.host("cmake").args({"-S", t.root(), "-B", dir/"host", "-DCMAKE_BUILD_TYPE=Release",
            "-DFF8_RCHEEVOS=OFF", "-DFF8_STATE_BRIDGE=OFF"}).timeout(300);
        if (dev) {
            lh::write_file(dir/"guest.exe", exe());
            empty_catalog(exe(), dir/"generated");
            cmd.args({"-DFF8_PLAYER=OFF", "-DFF8_HOST_INTERPRETER_ONLY=ON", "-DGENERATED_DIR="+(dir/"generated").string()});
        } else cmd.arg("-DFF8_PLAYER=ON");
        t.ok(cmd);
        if (!dev) lh::fs::copy_file(lh::fs::path(LH_SYNTHETIC_METADATA)/"disc_identity.h", dir/"host/disc_identity.h", lh::fs::copy_options::overwrite_existing);
        t.ok(t.host("cmake").args({"--build", dir/"host", "--target", "ff8", "-j4"}).timeout(300));
        lh::write_file(dir/"ready", "");
    }
    return binary;
}
inline lh::Command play(lh::Case &t, std::initializer_list<lh::Arg> args = {}, bool automatic = false, lh::fs::path binary = {}) {
    if (binary.empty()) binary = player(t);
    auto cmd = t.target(binary).arg("--headless").cwd(t.temp()).timeout(30)
        .env("HOME", t.temp().string()).env("XDG_CONFIG_HOME", (t.temp()/"config").string())
        .env("XDG_DATA_HOME", (t.temp()/"data").string()).env("SDL_VIDEODRIVER", "dummy").env("SDL_AUDIODRIVER", "dummy");
    bool config = false;
    for (auto &a : args) if (a.text() == "--config") config = true;
    if (!automatic && !config) cmd.args({"--config", "/dev/null"});
    return cmd.args(args);
}
inline lh::Command &run(lh::Command &cmd) {
    return cmd.args({"--headless", "--fast", "--mute", "--max-vblanks", "2", "--log", "/dev/null"});
}
inline lh::Command running(lh::Case &t, std::initializer_list<lh::Arg> args = {}) { auto cmd=play(t,args); run(cmd); return cmd; }
}
