// SIO0/card wire transactions and deterministic scripts; synthetic data only.
#include "host_fixtures.h"
static void run(lh::Case &t, const char *name, const lh::fs::path &path) {
    t.ok(t.harness("sio").args({name, path}).timeout(30));
}
static const char *const cases[] = {"pad", "timing", "fifo", "dualshock"};
LH_TEST_P(test_sio, cases) { t.ok(t.harness("sio").arg(p).timeout(30)); }
LH_TEST(test_card_protocol_and_persistence) {
    auto path = t.temp()/"card.mcr";
    run(t, "card", path);
    auto data = lh::read_file(path);
    LH_EQ(data.size(), 131072u); LH_EQ(data.substr(0, 2), "MC");
    for (int frame = 0; frame < 36; ++frame) {
        unsigned checksum = 0;
        for (unsigned char byte : data.substr(frame*128, 128)) checksum ^= byte;
        LH_EQ(checksum, 0u);
    }
    for (int frame = 1; frame < 16; ++frame) {
        LH_EQ(static_cast<unsigned char>(data[frame*128]), 0xa0);
        LH_BYTES_EQ(data.substr(frame*128+8, 2), std::string(2, '\xff'));
    }
    auto block = std::string(1, '\xa0') + std::string(7, 0) + std::string(2, '\xff') + std::string(117, 0) + std::string(1, '\xa0');
    LH_BYTES_EQ(data.substr(7*128,128), block);
    std::string tail;
    for (int i = 0; i < 128; ++i) tail += char(i ^ 0x5a);
    LH_BYTES_EQ(data.substr(data.size()-128), tail);
}
static const int sizes[] = {0,131071,131073,131136};
LH_TEST_P(test_invalid_card_is_not_modified, sizes) {
    auto path=t.temp()/"invalid.mcd"; auto original=std::string(p, '\xab');
    lh::write_file(path, original); run(t,"invalid_card",path); LH_BYTES_EQ(lh::read_file(path),original);
}
LH_TEST(test_existing_raw_card_is_not_reformatted) {
    auto path=t.temp()/"dump.mcd"; std::string original;
    for (int i=0;i<131072;++i) original+=char(i%256);
    lh::write_file(path,original); run(t,"card_existing",path); LH_BYTES_EQ(lh::read_file(path),original);
}
LH_TEST(test_card_write_failure_is_reported) {
    auto path=t.temp()/"card.mcr"; run(t,"card_failed",path); auto data=lh::read_file(path);
    LH_BYTES_EQ(data.substr(data.size()-128),std::string(128,'\xff'));
}
LH_TEST(test_script_exact_frames_and_release) {
    auto path=t.temp()/"input.txt"; lh::write_file(path,"# Script overrides live port 1 input\n10 START+CROSS\n20 -\n"); run(t,"script",path);
}
LH_TEST(test_script_disc_request_preserves_pad_state) {
    auto path=t.temp()/"swap.txt"; lh::write_file(path,"10 CROSS\n15 DISC2\n20 -\n"); run(t,"disc_script",path);
}
struct Script { const char *id, *text; };
static const Script scripts[] = {
{"10 START\\n9 -\\n","10 START\n9 -\n"},
{"10 START\\n10 CROSS\\n","10 START\n10 CROSS\n"},
{"-1 START\\n","-1 START\n"},
{"1 X\\n","1 X\n"},
{"1 START+\\n","1 START+\n"},
{"1 +START\\n","1 +START\n"},
{"1 START CROSS\\n","1 START CROSS\n"},
{"18446744073709551616 START\\n","18446744073709551616 START\n"},
{"1 START junk\\n","1 START junk\n"},
{"1 DISC0\\n","1 DISC0\n"},
{"1 DISC5\\n","1 DISC5\n"},
{"1 DISC2+CROSS\\n","1 DISC2+CROSS\n"},
};
LH_TEST_P(test_invalid_scripts, scripts) {
    auto path=t.temp()/"input.txt"; lh::write_file(path,p.text); run(t,"invalid_script",path);
}
static const char *const drivers[] = {"None","software"};
LH_TEST_P(test_sdl_events_and_virtual_controllers, drivers) {
    auto cmd=t.harness("events").env("SDL_VIDEODRIVER","dummy").timeout(30);
    if (std::string(p)=="software") cmd.env("SDL_RENDER_DRIVER",p);
    t.ok(cmd);
}
LH_TEST(test_dualshock_sdl_wire_and_feedback) {
    auto path=t.temp()/"digital.txt"; lh::write_file(path,"0 CROSS\n");
    t.ok(lh::dummy(t.harness("dualshock")).timeout(30));
    t.ok(lh::dummy(t.harness("dualshock")).arg(path).timeout(30));
}
LH_TEST(test_real_disc_probe_initializes_trigger_rest) { t.ok(lh::dummy(t.harness("probe")).timeout(30)); }
