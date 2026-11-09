// Optional real-disc boot regression. All disc-derived output stays in build/.
#include "lh_test.h"

#include <cstdio>
#include <map>
#include <string>

namespace {

struct RealHost {
    lh::fs::path disc, exe, binary, integration;
};

RealHost real_host(lh::Case &t) {
    RealHost host{lh::owned_disc(1), lh::owned_path("SLUS_008.92"), t.root() / "build/dev/ff8", t.root() / "build/integration"};
    LH_CHECK_MSG(lh::fs::is_regular_file(host.binary), "run python tools/setup.py --disc1 DISC first");
    return host;
}

std::string frame_name(int frame) {
    char name[32];
    std::snprintf(name, sizeof name, "frames/frame-%06d.png", frame);
    return name;
}

void expect_recompiled_code_ran(const std::string &output) {
    const std::string count = lh::capture(output, R"(Instructions: interpreter=\d+ recompiled=(\d+))");
    LH_CHECK_MSG(std::stoull(count) > 0, "the boot gate must execute compiled code with statistics enabled");
}

} // namespace

LH_GAME_TEST(test_recompiled_boot_frame_hashes) {
    const RealHost host = real_host(t);
    const lh::fs::path output = lh::mkdtemp_under(host.integration, "boot-regression-");
    const auto result = t.ok(t.target(host.binary).args({
        host.exe, "--disc", host.disc, "--config", lh::fs::path("/dev/null"), "--headless", "--mute",
        "--stall", "0", "--max-instructions", "0", "--max-vblanks", "2850", "--frame-step", "30",
        "--log", lh::fs::path("/dev/null"),
        "--input-script", t.root() / "tests/scripts/to-title.txt",
        "--dump-frames", output / "frames"}).timeout(180));
    lh::write_file(output / "summary.txt", result.out + result.err);
    LH_CONTAINS(result.out, "Stop: VBlank limit reached");
    LH_CONTAINS(result.out, "VBlanks=2850 ");
    expect_recompiled_code_ran(result.out);
    const std::map<int, std::string> expected = {
        {300, "53f470202aae9839511410ccb736f4e9222908135d749594c863fe5f70550225"},
        {600, "26e09e018f28f65f1f73a700e605e2adb1c4c31786a6543f6cb938551ee1c374"},
        {750, "1c3280234cc725537df614d82f6771c1f8d4ddccee802b8f232b24f7dad7966f"},
        {2820, "4b847ef9e45607b9ebf9f12bc6242b124e8de7661e63fbed5fa36e6dd7e7c8bd"},
    };
    for (const auto &[frame, digest] : expected) {
        LH_EQ(lh::sha256_file(output / frame_name(frame)), digest);
    }
}

LH_GAME_TEST(test_recompiled_real_card_save_list) {
    const RealHost host = real_host(t);
    const lh::fs::path source = lh::owned_path("saves/disc1-early.mcr");
    const std::string original = lh::sha256_file(source);
    const lh::fs::path output = lh::mkdtemp_under(host.integration, "card-regression-");
    const lh::fs::path card = output / "card1.mcr";
    lh::fs::copy_file(source, card);
    const auto result = t.ok(t.target(host.binary).args({
        host.exe, "--disc", host.disc, "--config", lh::fs::path("/dev/null"), "--headless", "--mute",
        "--stall", "0", "--max-instructions", "0", "--max-vblanks", "3960",
        "--frame-step", "60", "--frames-from", "3900", "--log", lh::fs::path("/dev/null"),
        "--input-script", t.root() / "tests/scripts/load-card.txt",
        "--card1", card, "--dump-frames", output / "frames"}).timeout(240));
    lh::write_file(output / "summary.txt", result.out + result.err);
    LH_CONTAINS(result.out, "Stop: VBlank limit reached");
    expect_recompiled_code_ran(result.out);
    LH_EQ(lh::sha256_file(output / frame_name(3900)),
          std::string("4687f3273bad6cc67962d52a35c647e53ee855b4b22ae0ace11be550a2e06ed8"));
    LH_EQ(lh::sha256_file(source), original);
}
