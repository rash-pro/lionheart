#pragma once
#include "../../third_party/nlohmann/json.hpp"
#include "metadata_io.h"
#include "process.h"
#include <iostream>
#include <memory>
#include <regex>
#include <set>
namespace metadata {
using J = nlohmann::ordered_json;
namespace fs = std::filesystem;
inline void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
inline std::string text(const J &v) { return v.get<std::string>(); }
inline int64_t num(const J &v) { return v.get<int64_t>(); }
inline std::string lower(std::string s) {
  for (auto &c : s)
    c = std::tolower((unsigned char)c);
  return s;
}
inline std::string trim(std::string s) {
  auto a = s.find_first_not_of(" \r\n\t/");
  if (a == s.npos)
    return {};
  return s.substr(a, s.find_last_not_of(" \r\n\t/") - a + 1);
}
inline std::string hash(const std::string &s) {
  char h[41];
  metadata_hash(s.data(), s.size(), h);
  return h;
}
inline std::string bytes(void *p, size_t n, const char *error) {
  require(p, error[0] ? error : "out of memory");
  std::unique_ptr<void, decltype(&free)> owner(p, free);
  return std::string((char *)p, n);
}
inline std::string decode(const std::string &s) {
  char error[1024] = {};
  size_t n = 0;
  void *p = metadata_decode(s.data(), s.size(), &n, error);
  return bytes(p, n, error);
}
inline uint32_t u32(const std::string &s, size_t p) {
  require(p <= s.size() && s.size() - p >= 4, "truncated table");
  const auto *b = (const unsigned char *)s.data() + p;
  return b[0] | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 |
         uint32_t(b[3]) << 24;
}
inline J load(const fs::path &p) { return J::parse(dt::read(p)); }
inline void write(const fs::path &p, const J &j) {
  fs::create_directories(p.parent_path());
  dt::write(p, j.dump(2, ' ', true) + "\n");
}
inline J obj(std::initializer_list<J::value_type> v) { return J(v); }
struct Image {
  metadata_image *m;
  int number;
  int64_t lba, size;
  std::string top;
  std::map<int, std::string> modules;
  J layout_cache;
  explicit Image(const fs::path &cue) {
    char e[1024] = {};
    m = metadata_open(cue.c_str(), e);
    require(m, e);
    number = metadata_number(m);
    lba = metadata_lba(m);
    size = metadata_size(m);
    top = read(0, 2048);
  }
  ~Image() { metadata_close(m); }
  Image(const Image &) = delete;
  std::string read(int64_t sector, int64_t n) {
    char e[1024] = {};
    auto p = metadata_read(m, sector, n, e);
    return bytes(p, n, e);
  }
  std::string absolute(int64_t sector, int64_t n) {
    return read(sector - lba, n);
  }
  std::string exe(const std::string &name) {
    char e[1024] = {};
    size_t n = 0;
    auto p = metadata_exe(m, name.c_str(), &n, e);
    return bytes(p, n, e);
  }
  bool compressed(int i) { return i == 1 || i == 2 || i == 23 || i == 26; }
  int64_t stored(int i) {
    return compressed(i) ? u32(absolute(u32(top, i * 8), 4), 0) + 4
                         : u32(top, i * 8 + 4);
  }
  const std::string &module(int i) {
    if (!modules.count(i)) {
      auto raw = absolute(u32(top, i * 8), stored(i));
      modules[i] = compressed(i) ? decode(raw) : raw;
    }
    return modules.at(i);
  }
  J layout() {
    if (!layout_cache.is_null())
      return layout_cache;
    const auto &f = module(2);
    size_t off = 0x28900;
    int64_t next = -1;
    while (off + 8 <= f.size()) {
      auto a = u32(f, off), b = u32(f, off + 4);
      if (a < lba || a >= lba + size / 2048 || (next >= 0 && a != next))
        break;
      next = a + (b + 2047) / 2048;
      off += 8;
    }
    require((off - 0x28900) % 24 == 0,
            "field table is not a sequence of MIM/DAT/LZK triples");
    int count = (off - 0x28900) / 24;
    J ids = J::array();
    for (size_t i = 0; i < 0x71c; i += 2) {
      require(off + i + 2 <= f.size(), "truncated field lookup");
      int id = (unsigned char)f[off + i] | (unsigned char)f[off + i + 1] << 8;
      require(id < count, "field lookup references an invalid triple");
      ids.push_back(id);
    }
    size_t movie = off + 0x71c, sound = 0;
    for (size_t p = movie + 8; p + 16 < f.size(); p += 8)
      if (u32(f, p + 4) == 31792 && u32(f, p + 12) == 47792 &&
          u32(f, p + 8) == u32(f, p) + 16 && absolute(u32(f, p), 4) == "AKAO") {
        sound = p;
        break;
      }
    require(sound, "could not identify field sound table");
    layout_cache = {{"field_count", count},
                    {"field_lookup_offset", off},
                    {"field_ids", ids},
                    {"movie_offset", movie},
                    {"movie_slots", (sound - movie) / 8},
                    {"sound_offset", sound},
                    {"sound_slots", 88}};
    return layout_cache;
  }
  std::pair<int64_t, int64_t> pair(const J &loc) {
    auto kind = text(loc.at("table"));
    int i = loc.value("index", 0);
    if (kind == "img")
      return {lba, 2048};
    if (kind == "top") {
      require(i >= 0 && i < 133, "invalid top index");
      return {u32(top, i * 8), stored(i)};
    }
    auto l = layout();
    std::map<std::string, std::pair<int, int64_t>> bases = {
        {"intro", {1, 0x10b8}},
        {"world", {26, 0x2cca4}},
        {"battle", {25, 0x499b4}},
        {"models", {2, 0x286a0}},
        {"field", {2, 0x28900}},
        {"movie", {2, num(l["movie_offset"])}},
        {"sound", {2, num(l["sound_offset"])}}};
    auto b = bases.at(kind);
    const auto &s = module(b.first);
    auto a = u32(s, b.second + i * 8), n = u32(s, b.second + i * 8 + 4);
    return {a, kind == "sound" ? (n + 2047) / 2048 * 2048 : n};
  }
  J locators() {
    J out = J::array({J{{"table", "img"}}});
    auto l = layout();
    for (auto kv : std::vector<std::pair<std::string, int>>{
             {"top", 133},
             {"intro", 36},
             {"world", 12},
             {"battle", 1118},
             {"models", 76},
             {"field", num(l["field_count"]) * 3},
             {"movie", num(l["movie_slots"])},
             {"sound", 88}})
      for (int i = 0; i < kv.second; ++i) {
        J loc = {{"table", kv.first}, {"index", i}};
        auto p = pair(loc);
        if (p.second) {
          read(p.first - lba, 0);
          out.push_back(loc);
        }
      }
    return out;
  }
};
inline std::string id(const J &loc) {
  return text(loc.at("table")) + ":" + std::to_string(loc.value("index", 0));
}
inline bool overlay(const J &e) {
  auto p = text(e.at("path"));
  return p.size() >= 4 && p.substr(p.size() - 4) == ".ovl";
}
inline std::pair<std::string, std::string> raw_overlay(Image &im, const J &e) {
  auto raw = im.read(num(e.at("sector")), num(e.at("size")));
  return {raw, overlay(e) ? (e.at("compressed").get<bool>() ? decode(raw) : raw)
                          : std::string{}};
}
inline J inventory(Image &im, const fs::path &dir) {
  auto j = load(dir / ("files-disc" + std::to_string(im.number) + ".json"));
  require(j["image"]["lba"] == im.lba && j["image"]["size"] == im.size,
          "disc IMG does not match inventory layout");
  return j;
}
inline J select(const J &inv, std::string query, const fs::path &dir) {
  auto q = lower(trim(query));
  J matches = J::array();
  if (!q.empty() && q[0] == '@') {
    for (auto &e : inv["entries"])
      if (id(e["locator"]) == q.substr(1))
        matches.push_back(e);
    require(matches.size() == 1, "expected one table record for '" + query +
                                     "', found " +
                                     std::to_string(matches.size()));
    return matches[0];
  }
  std::set<std::string> hashes;
  for (auto &e : inv["entries"])
    if (lower(text(e["path"])) == q) {
      matches.push_back(e);
      hashes.insert(text(e["sha1"]));
    }
  if (!matches.empty()) {
    require(hashes.size() == 1, "path has different contents at multiple "
                                "locations; select @table:index");
    return matches[0];
  }
  auto catalog = load(dir / "overlays-all.json");
  std::set<std::string> paths;
  for (auto &o : catalog["overlays"])
    for (auto &l : o["locations"]) {
      fs::path p = text(l["path"]);
      if (l["disc"] == inv["disc"] &&
          (q == lower(text(l["name"])) || q == lower(p.filename()) ||
           q == lower(p.stem())))
        paths.insert(p);
    }
  for (auto &e : inv["entries"])
    if (paths.count(text(e["path"])))
      matches.push_back(e);
  require(matches.size() == 1, "expected one file for '" + query + "', found " +
                                   std::to_string(matches.size()));
  return matches[0];
}
inline J coverage(Image &im, const J &entries) {
  std::vector<std::pair<int64_t, int64_t>> spans;
  for (auto &e : entries)
    spans.emplace_back(num(e["sector"]),
                       num(e["sector"]) + (num(e["size"]) + 2047) / 2048);
  std::sort(spans.begin(), spans.end());
  spans.emplace_back(im.size / 2048, im.size / 2048);
  J out = J::array();
  int64_t end = 0;
  for (auto p : spans) {
    require(p.first >= end,
            "overlapping physical file allocations at IMG sector " +
                std::to_string(p.first));
    if (p.first > end) {
      auto s = im.read(end, (p.first - end) * 2048);
      require(s.find_first_not_of('\0') == s.npos,
              "unindexed nonzero data at IMG sector " + std::to_string(end));
      out.push_back(J{
          {"sector", end}, {"size", s.size()}, {"sha1", hash(s)}, {"fill", 0}});
    }
    end = p.second;
  }
  return out;
}
inline std::string image_hash(Image &im) { return hash(im.read(0, im.size)); }
inline void verify(Image &im, const J &inv, const fs::path &dir) {
  int count = 0, overlays = 0;
  std::map<std::string, J> entries;
  for (auto &e : inv["entries"]) {
    auto p = im.pair(e["locator"]);
    auto path = text(e["path"]);
    require(p.first - im.lba == e["sector"] && p.second == e["size"],
            "table location mismatch: " + path);
    auto r = raw_overlay(im, e);
    require(hash(r.first) == text(e["sha1"]), "file SHA-1 mismatch: " + path);
    if (overlay(e)) {
      require(hash(r.second) == text(e["overlay_sha1"]) &&
                  r.second.size() == e["overlay_size"],
              "overlay SHA-1/size mismatch: " + path);
      auto loaded = e["compressed"].get<bool>() &&
                            num(e["read_size"]) < (int64_t)r.first.size()
                        ? decode(r.first.substr(0, num(e["read_size"])))
                        : r.second;
      require(hash(loaded) == text(e["loaded_sha1"]) &&
                  loaded.size() == e["loaded_size"],
              "loader-visible overlay SHA-1/size mismatch: " + path);
      ++overlays;
      entries[path] = e;
    }
    ++count;
  }
  std::set<std::string> seen;
  auto catalog = load(dir / "overlays-all.json");
  for (auto &o : catalog["overlays"])
    for (auto &l : o["locations"])
      if (l["disc"] == im.number) {
        auto path = text(l["path"]);
        require(entries.count(path), "overlay catalog mismatch: " + path);
        auto &e = entries.at(path);
        bool ok = !seen.count(path);
        for (auto k : {"sector", "size", "read_size", "compressed",
                       "loaded_sha1", "loaded_size"})
          ok = ok && l[k] == e[k];
        require(ok && l["sha1_stored"] == text(e["sha1"]) &&
                    o["sha1"] == text(e["overlay_sha1"]) &&
                    o["size"] == e["overlay_size"],
                "overlay catalog mismatch: " + path);
        seen.insert(path);
      }
  require(seen.size() == entries.size(), "overlay catalog coverage mismatch");
  require(coverage(im, inv["entries"]) == inv["padding"],
          "IMG padding coverage mismatch");
  require(image_hash(im) == text(inv["image"]["sha1"]),
          "logical IMG SHA-1 mismatch");
  require(hash(im.exe(text(inv["exe"]["name"]))) == text(inv["exe"]["sha1"]),
          "main executable SHA-1 mismatch");
  std::cout << "disc " << im.number << ": verified " << count
            << " file SHA-1s/table locations and " << overlays
            << " overlay images/catalog records\n";
}
inline void scan(Image &im, const J &inv, const fs::path &cue,
                 const fs::path &dir) {
  std::map<std::string, std::set<std::pair<int64_t, std::string>>> fingerprints;
  {
    Image ref(cue);
    require(ref.number == 1, "--scan-disc1 must name disc 1");
    auto r = inventory(ref, dir);
    for (auto &e : r["entries"])
      if (num(e["size"]) >= 2048) {
        auto h = ref.read(num(e["sector"]), 2048);
        if (std::set<char>(h.begin(), h.end()).size() >= 8)
          fingerprints[hash(h)].emplace(num(e["size"]), text(e["sha1"]));
      }
  }
  using Extent = std::tuple<int64_t, int64_t, std::string>;
  std::set<Extent> found, indexed, expected;
  for (int64_t s = 0; s < im.size / 2048; ++s) {
    auto i = fingerprints.find(hash(im.read(s, 2048)));
    if (i != fingerprints.end())
      for (auto p : i->second)
        if (s * 2048 + p.first <= im.size &&
            hash(im.read(s, p.first)) == p.second)
          found.emplace(s, p.first, p.second);
  }
  size_t count = 0;
  for (auto &e : inv["entries"]) {
    Extent x = {num(e["sector"]), num(e["size"]), text(e["sha1"])};
    indexed.insert(x);
    auto h = hash(
        im.read(num(e["sector"]), std::min<int64_t>(num(e["size"]), 2048)));
    if (fingerprints[h].count({num(e["size"]), text(e["sha1"])}))
      require(found.count(x),
              "sector-boundary content search missed an inventory entry");
  }
  for (auto &x : found)
    if (indexed.count(x))
      ++count;
  std::cout << "disc " << im.number << ": sector scan found " << count
            << " indexed extents identical to disc 1; " << found.size() - count
            << " additional content matches inside/past indexed extents\n";
}
} // namespace metadata
