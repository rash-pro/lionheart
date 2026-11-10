#include "../../../tools/devtools/metadata_yaml.h"
#include "lh_test.h"
extern "C" {
#include "sha1.h"
}
namespace {
void word(std::string &s, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i)
    s.at(at + i) = char(value >> (i * 8));
}
std::string record(const std::string &name, int lba, int size) {
  std::string r(33 + name.size() + (name.size() % 2 == 0), 0);
  r[0] = char(r.size());
  word(r, 2, lba);
  word(r, 10, size);
  r[32] = char(name.size());
  r.replace(33, name.size(), name);
  return r;
}
struct Fixture {
  lh::fs::path cue, dir;
  metadata::J inv;
  explicit Fixture(const lh::fs::path &p) : cue(p / "disc.cue"), dir(p) {
    using namespace metadata;
    std::string img(2048, 0);
    for (int i = 0; i < 133; ++i) {
      word(img, i * 8, 22);
      word(img, i * 8 + 4, 1);
    }
    std::string bin(24 * 2352, 0), root = record(std::string(1, 0), 20, 2048);
    root[25] = 2;
    bin.replace(16 * 2352 + 24 + 156, root.size(), root);
    auto records =
        record("FF8DISC1.IMG;1", 22, 2048) + record("SLUS_008.92;1", 23, 4);
    bin.replace(20 * 2352 + 24, records.size(), records);
    bin.replace(22 * 2352 + 24, img.size(), img);
    bin.replace(23 * 2352 + 24, 4, "EXE!");
    lh::write_file(p / "disc.bin", bin);
    lh::write_file(
        cue,
        "FILE \"disc.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n");
    inv = {{"disc", 1},
           {"image", J{{"lba", 22},
                       {"size", 2048},
                       {"sha1", "dd41b7bee1b81b6137fda9283d5a902d42306f4a"}}},
           {"exe", J{{"name", "SLUS_008.92"},
                     {"sha1", "db04e64467ce00713a487958458d0b72df3bfca1"}}},
           {"entries",
            J::array({J{{"path", "toc.bin"},
                        {"sector", 0},
                        {"size", 2048},
                        {"sha1", "dd41b7bee1b81b6137fda9283d5a902d42306f4a"},
                        {"locator", J{{"table", "img"}}},
                        {"compressed", false}}})},
           {"padding", J::array()}};
    write(p / "files-disc1.json", inv);
    write(p / "overlays-all.json", J{{"overlays", J::array()}});
  }
};
} // namespace
LH_TEST(test_yaml_subset) {
  using namespace metadata;
  auto j =
      Yaml("image:\n  sha1: abc012\nentries:\n- path: 'a#b' # comment\n  "
           "sector: 0x10\n  size: 12\nbinaries:\n- name: test\n  options:\n    "
           "symbol_addrs_path: [a, b]\n  segments:\n  - type: code\n    vram: "
           "0x80010000\n    subsegments:\n    - [0, .text, x]\n")
          .parse();
  LH_EQ(num(j["entries"][0]["sector"]), 16);
  LH_TEXT_EQ(text(j["entries"][0]["path"]), "a#b");
  LH_EQ(j["binaries"][0]["options"]["symbol_addrs_path"].size(), 2u);
  for (auto s :
       {"x: &anchor value\n", "x: |\n  text\n", "x: [a\n", "x: 1\nx: 2\n"}) {
    bool failed = false;
    try {
      Yaml(s).parse();
    } catch (const std::exception &) {
      failed = true;
    }
    LH_CHECK(failed);
  }
}
LH_TEST(test_list_synthetic_disc) {
  Fixture f(t.temp());
  auto tool = [&] { return t.harness("ff8files"); };
  auto r = t.ok(tool().arg("list").arg(f.cue).arg("--metadata").arg(f.dir));
  LH_CONTAINS(r.out, "toc.bin");
  auto j = t.ok(
      tool().arg("list").arg(f.cue).arg("--metadata").arg(f.dir).arg("--json"));
  LH_CONTAINS(j.out, "\"table\": \"img\"");
  auto empty = t.ok(
      tool().arg("list").arg(f.cue).arg("absent").arg("--metadata").arg(f.dir));
  LH_TEXT_EQ(empty.out, "");
}
LH_TEST(test_get_synthetic_disc) {
  Fixture f(t.temp());
  auto out = f.dir / "out.bin";
  auto tool = [&] { return t.harness("ff8files"); };
  auto r = t.ok(tool()
                    .arg("get")
                    .arg(f.cue)
                    .arg("@img:0")
                    .arg(out)
                    .arg("--metadata")
                    .arg(f.dir)
                    .arg("--raw"));
  LH_CONTAINS(r.out, "wrote 2048 bytes");
  LH_EQ(lh::read_file(out).size(), 2048u);
  auto bad = t.run(tool()
                       .arg("get")
                       .arg(f.cue)
                       .arg("unknown")
                       .arg(out)
                       .arg("--metadata")
                       .arg(f.dir));
  LH_EQ(bad.rc, 1);
  LH_CONTAINS(bad.err, "found 0");
}
LH_TEST(test_verify_synthetic_disc_and_corruption) {
  Fixture f(t.temp());
  auto tool = [&] { return t.harness("ff8files"); };
  auto r = t.ok(tool().arg("verify").arg(f.cue).arg("--metadata").arg(f.dir));
  LH_CONTAINS(r.out, "verified 1 file SHA-1s/table locations and 0 overlay "
                     "images/catalog records");
  f.inv["entries"][0]["sha1"] = std::string(40, '0');
  metadata::write(f.dir / "files-disc1.json", f.inv);
  auto bad =
      t.run(tool().arg("verify").arg(f.cue).arg("--metadata").arg(f.dir));
  LH_EQ(bad.rc, 1);
  LH_CONTAINS(bad.err, "file SHA-1 mismatch: toc.bin");
}
LH_TEST(test_get_compressed_overlay_modes) {
  using namespace metadata;
  Fixture f(t.temp());
  // The first top-table pair is also a valid bounded LZS prefix: a literal
  // zero followed by a three-byte zero-ring reference.
  std::string raw(8, 0);
  raw[0] = 22;
  raw[4] = 1;
  J e = {{"path", "data/mag001.ovl"},
         {"sector", 0},
         {"size", 8},
         {"sha1", "e3278f8bd947e68f85ecde940de8c95ee4588186"},
         {"locator", J{{"table", "top"}, {"index", 0}}},
         {"compressed", true},
         {"read_size", 7},
         {"overlay_sha1", "9069ca78e7450a285173431b3e52c5c25299e473"},
         {"overlay_size", 4},
         {"loaded_sha1", "5ba93c9db0cff93f52b521d7420e43f6eda2784f"},
         {"loaded_size", 1}};
  f.inv["entries"] = J::array({e});
  write(f.dir / "files-disc1.json", f.inv);
  write(
      f.dir / "overlays-all.json",
      J{{"overlays",
         J::array({J{{"locations", J::array({J{{"disc", 1},
                                               {"path", e["path"]},
                                               {"name", "effect_001"}}})}}})}});
  for (auto mode : std::vector<std::pair<std::string, std::string>>{
           {"--raw", raw},
           {"--loaded", std::string(1, 0)},
           {"--decompress", std::string(4, 0)}}) {
    auto out = f.dir / "decoded";
    t.ok(t.harness("ff8files")
             .arg("get")
             .arg(f.cue)
             .arg("effect_001")
             .arg(out)
             .arg(mode.first)
             .arg("--metadata")
             .arg(f.dir));
    LH_BYTES_EQ(lh::read_file(out), mode.second);
  }
}
LH_TEST(test_verify_set_entry_points) {
  using namespace metadata;
  Fixture f(t.temp());
  // Top slot 4 names the table sector itself as an uncompressed overlay, so
  // the words after the 133 slots are the overlay's code: one return.
  auto bin = lh::read_file(f.dir / "disc.bin");
  std::string sector = bin.substr(22 * 2352 + 24, 2048);
  word(sector, 4 * 8 + 4, 2048);
  word(sector, 1100, 0x03e00008);
  bin.replace(22 * 2352 + 24, 2048, sector);
  lh::write_file(f.dir / "disc.bin", bin);
  char digest[41];
  lhg_sha1_hex(reinterpret_cast<const uint8_t *>(sector.data()), sector.size(),
               digest);
  std::string sha1 = digest;
  f.inv["image"]["sha1"] = sha1;
  f.inv["entries"] = J::array({J{{"path", "data/x.ovl"},
                                 {"sector", 0},
                                 {"size", 2048},
                                 {"sha1", sha1},
                                 {"locator", J{{"table", "top"}, {"index", 4}}},
                                 {"compressed", false},
                                 {"read_size", 2048},
                                 {"overlay_sha1", sha1},
                                 {"overlay_size", 2048},
                                 {"loaded_sha1", sha1},
                                 {"loaded_size", 2048}}});
  write(f.dir / "files-disc1.json", f.inv);
  J location = {{"disc", 1},          {"name", "x"},
                {"path", "data/x.ovl"}, {"sector", 0},
                {"size", 2048},       {"read_size", 2048},
                {"compressed", false}, {"loaded_size", 2048},
                {"loaded_sha1", sha1}, {"load_address", "0x80100000"},
                {"sha1_stored", sha1}};
  write(f.dir / "overlays-all.json",
        J{{"overlays", J::array({J{{"name", "x"},
                                   {"sha1", sha1},
                                   {"size", 2048},
                                   {"locations", J::array({location})}}})}});
  auto verify = [&](J points, std::string name = "x") {
    write(f.dir / "overlays.json",
          J{{"set_entry_points", J::array({J{{"name", name},
                                             {"sha1", sha1},
                                             {"entry_points", points}}})}});
    return t.run(t.harness("ff8files")
                     .arg("verify")
                     .arg(f.cue)
                     .arg("--metadata")
                     .arg(f.dir));
  };
  auto good = verify(J::array({"0x8010044c"}));
  LH_EQ(good.rc, 0);
  LH_CONTAINS(good.out, "disc 1: verified 1 set entry points");
  for (auto bad : std::vector<std::pair<J, std::string>>{
           {J::array({"0x80100450"}), "not an instruction of data/x.ovl"},
           {J::array({"0x80100800"}), "not an instruction of data/x.ovl"},
           {J::array({"0x8010044C"}), "spelled"},
           {J::array({"0x8010044c", "0x80100000"}), "ascending"},
           {J::array(), "list nothing"}}) {
    auto r = verify(bad.first);
    LH_EQ(r.rc, 1);
    LH_CONTAINS(r.err, bad.second);
  }
  auto renamed = verify(J::array({"0x8010044c"}), "y");
  LH_EQ(renamed.rc, 1);
  LH_CONTAINS(renamed.err, "do not name one catalog set");
}
LH_TEST(test_code_prefixes) {
  using namespace metadata;
  Fixture f(t.temp());
  // A literal-only LZS asset whose code (a prologue, a return and its delay
  // slot) is followed by data; top slot 5 names it in a second IMG sector.
  std::string plain(64, 0), asset(4, 0);
  word(plain, 0, 0x27bdfff8);
  word(plain, 4, 0x03e00008);
  word(plain, 8, 0x27bd0008);
  word(plain, 32, 0x12345678);
  for (size_t at = 0; at < plain.size(); at += 8)
    asset += char(0xff) + plain.substr(at, 8);
  word(asset, 0, uint32_t(asset.size() - 4));
  auto bin = lh::read_file(f.dir / "disc.bin");
  bin.resize(26 * 2352, 0);
  std::string top = bin.substr(22 * 2352 + 24, 2048);
  word(top, 5 * 8, 23);
  word(top, 5 * 8 + 4, uint32_t(asset.size()));
  bin.replace(22 * 2352 + 24, 2048, top);
  std::string sector(2048, 0);
  sector.replace(0, asset.size(), asset);
  bin.replace(23 * 2352 + 24, 2048, sector);
  // The executable moves past the image, which now spans two sectors.
  bin.replace(24 * 2352 + 24, 4, "EXE!");
  auto image = record("FF8DISC1.IMG;1", 22, 4096), exe = record("SLUS_008.92;1", 24, 4);
  bin.replace(20 * 2352 + 24, image.size() + exe.size(), image + exe);
  lh::write_file(f.dir / "disc.bin", bin);
  auto digest = [](const std::string &s) {
    char out[41];
    lhg_sha1_hex(reinterpret_cast<const uint8_t *>(s.data()), s.size(), out);
    return std::string(out);
  };
  f.inv["image"]["size"] = 4096;
  f.inv["image"]["sha1"] = digest(top + sector);
  f.inv["entries"][0]["sha1"] = digest(top);
  f.inv["entries"].push_back(J{{"path", "data/stage0.x"},
                               {"sector", 1},
                               {"size", asset.size()},
                               {"sha1", digest(asset)},
                               {"locator", J{{"table", "top"}, {"index", 5}}},
                               {"compressed", false}});
  write(f.dir / "files-disc1.json", f.inv);
  auto metadata = [&](int64_t size, int limit) {
    write(f.dir / "overlays.json",
          J{{"code_prefixes",
             J::array({J{{"name", "stage"},
                         {"load_address", "0x8013e000"},
                         {"limit", limit},
                         {"variants", J::array({J{{"sha1", digest(plain.substr(0, size))},
                                                  {"size", size},
                                                  {"paths", J::array({"data/stage0.x"})}}})}}})}});
  };
  auto tool = [&](const char *command) {
    return t.run(t.harness("ff8files").arg(command).arg(f.cue).arg("--metadata").arg(f.dir));
  };
  metadata(12, 32);
  auto good = tool("verify");
  LH_EQ(good.rc, 0);
  LH_CONTAINS(good.out, "disc 1: verified 1 code prefix assets");
  // A wrong record is reported with what the disc holds, and the prefixes
  // command prints the record the disc bears out.
  metadata(16, 32);
  auto bad = tool("verify");
  LH_EQ(bad.rc, 1);
  LH_CONTAINS(bad.err, "stage: data/stage0.x starts with 12 bytes of code with SHA-1 " + digest(plain.substr(0, 12)));
  auto printed = tool("prefixes");
  LH_EQ(printed.rc, 0);
  auto records = J::parse(printed.out);
  LH_EQ(num(records[0]["variants"][0]["size"]), 12);
  LH_TEXT_EQ(text(records[0]["variants"][0]["sha1"]), digest(plain.substr(0, 12)));
}
