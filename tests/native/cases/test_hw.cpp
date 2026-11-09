// Synthetic controller/kernel tests; no copyrighted game data required.
#include "lh_test.h"

static const char *const kCases[] = {"pending_sr", "masked_deadlines", "scheduler", "dma_irq_deadline", "cd", "dma", "cd_status", "no_disc", "disc_swap", "gpu_dma", "events", "kernel_ram_chains", "kernel_ram_threads", "kernel_ram_threads_syscall", "kernel_ram_threads_branch", "timers", "pal_timing", "timer_modes", "wait_event", "bios_pad", "misc"};

LH_TEST_P(test_hardware, kCases) {
    std::string raw;
    for (int sector = 0; sector < 8; ++sector) {
        std::string payload(2352, '\0');
        for (int i = 0; i < 2352; ++i) payload[i] = char((i + sector) & 255);
        payload[15] = 2;
        raw += payload;
    }
    lh::write_file(t.temp() / "synthetic.bin", raw);
    const auto cue = t.temp() / "synthetic.cue";
    lh::write_file(cue, "FILE \"synthetic.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n");
    auto command = t.harness("main").args({p, cue}).timeout(30);
    if (std::string(p) == "disc_swap") {
        const auto replacement = t.temp() / "replacement.bin";
        raw[24] = char(0xa5);
        lh::write_file(replacement, raw);
        command.arg(replacement);
    }
    t.ok(command);
}

LH_TEST(test_default_stats_off_preserves_stall_progress) {
    t.ok(t.harness("default").arg("stats_off").timeout(30));
}
