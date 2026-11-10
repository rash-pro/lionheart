// Host review fixes use synthetic media and the unchanged runtime CUE parser.
#include "host_fixtures.h"
#include "synthetic_disc.h"
namespace {
lh::Result identify(lh::Case &t, const lh::fs::path &p, const char *action = "quick", bool runtime = false) {
    return t.run(t.harness(runtime ? "runtime-identify" : "identify").args({action, p}).timeout(20));
}
struct Cue { const char *id, *suffix; bool bom; };
const Cue cues[] = {{"False-.cue", ".cue", false}, {"False-.CUE", ".CUE", false}, {"False-.CuE", ".CuE", false},
 {"True-.cue", ".cue", true}, {"True-.CUE", ".CUE", true}, {"True-.CuE", ".CuE", true}};
}
LH_TEST_P(test_cue_suffix_and_bom_reaches_real_runtime, cues) {
    synthetic::Disc d; d.offset = 150; auto image = synthetic::disc(t.temp()/"game.bin", d);
    auto cue = t.temp()/(std::string("game")+p.suffix);
    std::string original = (p.bom ? "\xef\xbb\xbf" : "") + std::string("FILE \"") + image.filename().string() +
        "\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:02:00\n";
    lh::write_file(cue, original); auto r = identify(t, cue, "quick", true);
    if (std::string(p.suffix) == ".CuE") { LH_EQ(r.rc, 2); LH_CONTAINS(r.err, "unsupported image"); }
    else {
        LH_EQ(r.rc, 0); LH_TEXT_EQ(r.out, "disc=1\n");
        r = t.run(t.harness("runtime-identify").args({"runtime", cue}).env("TMPDIR", t.temp().string()).timeout(20));
        LH_EQ(r.rc, 0); LH_TEXT_EQ(r.out, "runtime offset=150 sectors=832 temporary="+std::to_string(p.bom)+"\n");
        LH_CHECK(lh::files_starting(t.temp(), "lionheart-cue-").empty());
    }
    LH_BYTES_EQ(lh::read_file(cue), original);
}
static const char *boots[] = {"slus_008.92", "SlUs_008.92"};
LH_TEST_P(test_lowercase_boot_matches_root_file, boots) {
    synthetic::Disc d; d.boot = "BOOT = cdrom:\\"+std::string(p)+";1\r\n";
    auto image = synthetic::disc(t.temp()/"game.bin", d); auto r = identify(t, image);
    LH_EQ(r.rc, 0); LH_TEXT_EQ(r.out, "disc=1\n"); LH_EQ(identify(t, image, "exe").rc, 0);
}
LH_TEST(test_lowercase_iso_root_names) {
    synthetic::Disc d; d.name = "slus_008.92"; auto image = synthetic::disc(t.temp()/"game.bin", d);
    auto data = lh::read_file(image); size_t at = 0;
    while ((at = data.find("SYSTEM.CNF;1", at)) != std::string::npos) data.replace(at, 12, "system.cnf;1");
    lh::write_file(image, data); auto r = identify(t, image); LH_EQ(r.rc, 0); LH_TEXT_EQ(r.out, "disc=1\n");
}
struct Separator { const char *id, *text; };
static const Separator separators[] = {{"\\\\", "\\"}, {"/", "/"}};
LH_TEST_P(test_subdirectory_boot_is_another_game, separators) {
    synthetic::Disc d; d.boot = "BOOT = cdrom:\\DIR"+std::string(p.text)+"SLUS_008.92;1\r\n";
    auto r = identify(t, synthetic::disc(t.temp()/"game.bin", d));
    LH_EQ(r.rc, 2); LH_TEXT_EQ(r.err, "Another game: DIR?SLUS_008.92\n");
}
struct Serial { const char *id, *serial, *expected; };
static const Serial serials[] = {{"BAD\\x1b[31m_%\\xe9-BAD??31m_???", "BAD\x1b[31m_%é", "BAD??31m_???"},
 {"ab-CD_09.xyz-ab-CD_09.xyz", "ab-CD_09.xyz", "ab-CD_09.xyz"}};
LH_TEST_P(test_disc_derived_serial_diagnostics_are_sanitized, serials) {
    synthetic::Disc d; d.name = p.serial; d.image = false;
    auto r = identify(t, synthetic::disc(t.temp()/"game.bin", d));
    LH_EQ(r.rc, 2); LH_TEXT_EQ(r.err, "Another game: "+std::string(p.expected)+"\n");
}
LH_TEST(test_exe_diagnostic_sanitizes_boot_name) {
    synthetic::Disc d; d.boot = "BOOT = cdrom:\\BAD\x1b[31m;1\r\n";
    auto r = identify(t, synthetic::disc(t.temp()/"game.bin", d), "exe");
    LH_EQ(r.rc, 2); LH_TEXT_EQ(r.err, "FF8 USA EXE missing (BAD??31m)\n");
}
LH_TEST(test_missing_bin_names_bin_instead_of_cue) {
    auto cue = t.temp()/"game.cue";
    lh::write_file(cue, "FILE \"lost.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n");
    auto r = identify(t, cue); LH_EQ(r.rc, 2);
    LH_CONTAINS(r.err, "lost.bin: No such file");
    LH_NOT_CONTAINS(r.err, cue.string());
}
LH_TEST(test_missing_exe_reports_missing) {
    synthetic::Disc d; d.name = "OTHER"; d.boot = "BOOT = cdrom:\\SLUS_008.92;1\r\n";
    auto r = identify(t, synthetic::disc(t.temp()/"game.bin", d));
    LH_EQ(r.rc, 2); LH_TEXT_EQ(r.err, "FF8 USA EXE missing (SLUS_008.92)\n");
}
LH_TEST(test_exe_allocation_failure_reports_out_of_memory) {
    auto image = synthetic::disc(t.temp()/"game.bin");
    auto r = t.run(t.harness("runtime-identify").args({"quick", image}).env("FF8_TEST_FAIL_ALLOC", "1").timeout(20));
    LH_EQ(r.rc, 2); LH_TEXT_EQ(r.err, "Out of memory reading EXE SLUS_008.92\n");
}
