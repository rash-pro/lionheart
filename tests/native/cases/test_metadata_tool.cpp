#include "../../../tools/devtools/metadata_yaml.h"
#include "lh_test.h"
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
