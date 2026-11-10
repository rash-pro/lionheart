#pragma once
#include "metadata_yaml.h"
namespace metadata {
inline J field_ids(const J &layout) {
  J out = J::object();
  int g = 0;
  for (auto &v : layout["field_ids"]) {
    int local = num(v);
    if (local || g == 0) {
      auto k = std::to_string(local);
      if (!out.contains(k))
        out[k] = J::array();
      out[k].push_back(g);
    }
    ++g;
  }
  return out;
}
inline J ids_at(const J &ids, int i) {
  auto k = std::to_string(i);
  return ids.contains(k) ? ids[k] : J::array();
}
inline J header(Image &im, const J &source) {
  std::string exe = "SLUS_";
  const char *names[] = {"SLUS_008.92", "SLUS_009.08", "SLUS_009.09",
                         "SLUS_009.10"};
  exe = names[im.number - 1];
  auto layout = im.layout();
  layout.erase("field_ids");
  return J{{"schema_version", 1},
           {"source", source},
           {"disc", im.number},
           {"exe", J{{"name", exe}, {"sha1", hash(im.exe(exe))}}},
           {"image", J{{"name", "FF8DISC" + std::to_string(im.number) + ".IMG"},
                       {"lba", im.lba},
                       {"sector_size", 2048},
                       {"size", im.size}}},
           {"tables", layout}};
}
inline void overlay_fields(Image &im, J &e) {
  auto r = raw_overlay(im, e);
  require(hash(r.first) == text(e["sha1"]),
          "disc-1 SHA-1 mismatch: " + text(e["path"]));
  if (overlay(e)) {
    e["overlay_sha1"] = hash(r.second);
    e["overlay_size"] = r.second.size();
    auto loaded = e["compressed"].get<bool>() &&
                          num(e["read_size"]) < (int64_t)r.first.size()
                      ? decode(r.first.substr(0, num(e["read_size"])))
                      : r.second;
    e["loaded_sha1"] = hash(loaded);
    e["loaded_size"] = loaded.size();
  }
}
inline bool compressed(Image &im, const std::string &path, const J &loc) {
  auto t = text(loc["table"]);
  return path.size() >= 4 && path.substr(path.size() - 4) == ".ovl" &&
         (t == "battle" || t == "movie" ||
          (t == "top" && im.compressed(loc["index"])));
}
inline J reference(Image &im, const J &lock, const J &source) {
  std::map<std::pair<int64_t, int64_t>, J> by_extent;
  for (auto &loc : im.locators()) {
    auto p = im.pair(loc);
    if (!by_extent.count(p))
      by_extent[p] = loc;
  }
  J entries = J::array();
  auto ids = field_ids(im.layout());
  for (auto e : lock["entries"]) {
    auto p = std::make_pair(im.lba + num(e["sector"]), num(e["size"]));
    require(by_extent.count(p), "no table locator for " + text(e["path"]));
    auto loc = by_extent.at(p);
    e["locator"] = loc;
    e["id"] = id(loc);
    if (loc["table"] == "field")
      e["field_ids"] = ids_at(ids, num(loc["index"]) / 3);
    e["path_source"] = "upstream";
    e["differs_from_disc1"] = false;
    e["compressed"] = compressed(im, text(e["path"]), loc);
    e["read_size"] = loc["table"] == "top"
                         ? u32(im.top, num(loc["index"]) * 8 + 4)
                         : num(e["size"]);
    overlay_fields(im, e);
    entries.push_back(e);
  }
  auto out = header(im, source);
  out["entries"] = entries;
  out["missing_from_disc1"] = J::array();
  auto h = image_hash(im);
  require(h == text(lock["image"]["sha1"]),
          "disc-1 logical IMG SHA-1 mismatch");
  out["image"]["sha1"] = h;
  return out;
}
inline J other(Image &im, const J &ref, const J &refids,
               const std::map<std::string, std::string> &heads) {
  std::map<std::string, J> paths, locs;
  std::map<std::pair<int, int>, J> fields;
  std::map<std::string, std::vector<J>> byhead;
  for (auto &e : ref["entries"]) {
    auto path = text(e["path"]);
    paths[path] = e;
    byhead[heads.at(path)].push_back(e);
    auto loc = e["locator"];
    if (loc["table"] == "field")
      for (auto g : ids_at(refids, num(loc["index"]) / 3))
        fields[{num(g), num(loc["index"]) % 3}] = e;
    else if (loc["table"] != "movie")
      locs[id(loc)] = e;
  }
  auto ids = field_ids(im.layout());
  std::set<std::string> used;
  std::set<std::pair<int64_t, int64_t>> extents;
  J entries = J::array();
  for (auto loc : im.locators()) {
    auto p = im.pair(loc);
    int64_t sector = p.first - im.lba, size = p.second;
    if (!extents.emplace(sector, size).second)
      continue;
    auto raw = im.read(sector, size), digest = hash(raw),
         head = hash(raw.substr(0, 2048));
    std::vector<J> candidates;
    for (auto &e : byhead[head])
      if (e["size"] == size && e["sha1"] == digest)
        candidates.push_back(e);
    J original;
    auto kind = text(loc["table"]);
    int index = loc.value("index", 0);
    if (kind == "field") {
      std::set<std::string> names;
      for (auto g : ids_at(ids, index / 3))
        if (fields.count({num(g), index % 3}))
          names.insert(text(fields.at({num(g), index % 3})["path"]));
      require(names.size() < 2,
              "global field ID aliases disagree on a disc-1 name");
      if (!names.empty())
        original = paths.at(*names.begin());
    } else if (locs.count(id(loc)))
      original = locs.at(id(loc));
    bool content = false;
    if (original.is_null() && kind != "field" && candidates.size() == 1 &&
        !used.count(text(candidates[0]["path"]))) {
      original = candidates[0];
      content = true;
    }
    std::string source = content              ? "content-disc1"
                         : original.is_null() ? "generated"
                                              : "table-disc1",
                path;
    if (!original.is_null())
      path = text(original["path"]);
    else if (kind == "field") {
      auto f = ids_at(ids, index / 3);
      std::ostringstream tag;
      if (!f.empty())
        tag << "id_" << std::setw(4) << std::setfill('0') << num(f[0]);
      else
        tag << "disc" << im.number << "_slot_" << std::setw(4)
            << std::setfill('0') << index / 3;
      auto s = tag.str();
      path = "data/eng/field/mapdata/_unidentified/" + s + "/" + s +
             std::vector<std::string>{".mim", ".dat", ".lzk"}[index % 3];
    } else if (kind == "movie") {
      if (index == 0) {
        path = "data/movie/movie.ovl";
        original = paths.at(path);
        source = "table-disc1";
      } else {
        std::ostringstream s;
        s << "data/movie/disc" << im.number << "_fmv_" << std::setw(3)
          << std::setfill('0') << index - 1 << ".bin";
        path = s.str();
      }
    } else
      throw std::runtime_error("no path for table record " + loc.dump());
    used.insert(path);
    J e = {{"id", id(loc)},
           {"path", path},
           {"sector", sector},
           {"size", size},
           {"sha1", digest},
           {"locator", loc},
           {"path_source", source},
           {"differs_from_disc1",
            original.is_null() || digest != text(original["sha1"])},
           {"compressed", compressed(im, path, loc)},
           {"read_size", kind == "top" ? u32(im.top, index * 8 + 4) : size}};
    if (kind == "field")
      e["field_ids"] = ids_at(ids, index / 3);
    if (!candidates.empty()) {
      std::set<std::string> matches;
      for (auto &c : candidates)
        matches.insert(text(c["path"]));
      e["content_matches_disc1"] = matches;
    }
    overlay_fields(im, e);
    entries.push_back(e);
  }
  std::sort(entries.begin(), entries.end(), [](const J &a, const J &b) {
    return std::make_pair(num(a["sector"]), text(a["path"])) <
           std::make_pair(num(b["sector"]), text(b["path"]));
  });
  auto out = header(im, ref["source"]);
  out["entries"] = entries;
  J missing = J::array();
  for (auto &p : paths)
    if (!used.count(p.first))
      missing.push_back(p.first);
  out["missing_from_disc1"] = missing;
  return out;
}
inline std::map<std::string, J> specs(const J &config, const J &ref) {
  std::map<std::string, J> out;
  for (auto &b : config["binaries"]) {
    auto name = text(b["name"]);
    if (name == "SLUS_008.92")
      continue;
    J code;
    for (auto &s : b["segments"])
      if (s.is_object() && s.value("type", "") == "code") {
        code = s;
        break;
      }
    J address;
    if (!code.is_null()) {
      std::ostringstream h;
      h << "0x" << std::hex << std::setw(8) << std::setfill('0')
        << num(code["vram"]);
      address = h.str();
    }
    int count = 0;
    for (auto &e : ref["entries"])
      if (overlay(e) && e["overlay_sha1"] == b["sha1"]) {
        auto path = text(e["path"]);
        if (name.rfind("effect_", 0) == 0 &&
            fs::path(path).filename() != "mag" + name.substr(7) + ".ovl")
          continue;
        J s = {
            {"name", name},
            {"load_address", address},
            {"code_configured", !code.is_null()},
            {"symbols", b["options"].value("symbol_addrs_path", J::array())}};
        require(!out.count(path) || out[path] == s,
                "ambiguous upstream overlay identity for " + path);
        out[path] = s;
        ++count;
      }
    require(count, "upstream binary has no verified disc-1 overlay: " + name);
  }
  require(out.size() == 368,
          "expected 368 overlay paths, got " + std::to_string(out.size()));
  return out;
}
inline J collect(const J &inventories, const std::map<std::string, J> &spec,
                 const J &source) {
  std::map<std::string, J> groups;
  for (auto &inv : inventories)
    for (auto &e : inv["entries"])
      if (overlay(e)) {
        auto s = spec.at(text(e["path"]));
        auto digest = text(e["overlay_sha1"]);
        if (!groups.count(digest))
          groups[digest] = J{{"name", s["name"]},
                             {"names", J::array()},
                             {"paths", J::array()},
                             {"discs", J::array()},
                             {"sha1", digest},
                             {"size", e["overlay_size"]},
                             {"code_configured", s["code_configured"]},
                             {"load_address", s["load_address"]},
                             {"load_addresses", J::array()},
                             {"compressed", e["compressed"]},
                             {"locations", J::array()}};
        auto &g = groups[digest];
        require(g["size"] == e["overlay_size"],
                "overlay digest has inconsistent sizes");
        for (auto kv : std::vector<std::pair<std::string, J>>{
                 {"names", s["name"]},
                 {"paths", e["path"]},
                 {"discs", inv["disc"]},
                 {"load_addresses", s["load_address"]}})
          if (std::find(g[kv.first].begin(), g[kv.first].end(), kv.second) ==
              g[kv.first].end())
            g[kv.first].push_back(kv.second);
        if (g["load_addresses"].size() > 1)
          g["load_address"] = nullptr;
        if (g["compressed"] != e["compressed"])
          g["compressed"] = nullptr;
        J loc = {
            {"disc", inv["disc"]}, {"name", s["name"]}, {"path", e["path"]}};
        for (auto k : {"sector", "size", "read_size", "compressed",
                       "loaded_size", "loaded_sha1"})
          loc[k] = e[k];
        loc["load_address"] = s["load_address"];
        loc["sha1_stored"] = e["sha1"];
        g["locations"].push_back(loc);
      }
  J a = J::array();
  for (auto &g : groups)
    a.push_back(g.second);
  std::sort(a.begin(), a.end(), [](const J &a, const J &b) {
    return std::make_pair(text(a["name"]), text(a["sha1"])) <
           std::make_pair(text(b["name"]), text(b["sha1"]));
  });
  return J{{"schema_version", 1},
           {"source", source},
           {"size_unit",
            "decompressed guest overlay image bytes, including embedded data"},
           {"overlays", a}};
}
inline J symbols(const std::string &s) {
  J out = J::object();
  std::istringstream in(s);
  std::string line;
  std::regex declaration(R"(^\s*([\w.$]+)\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)\s*;)");
  while (std::getline(in, line)) {
    auto t = Yaml::space(line);
    if (t.rfind("//", 0) == 0 || t.rfind("#", 0) == 0)
      continue;
    std::smatch m;
    if (std::regex_search(line, m, declaration)) {
      std::ostringstream key;
      key << "0x" << std::hex << std::setw(8) << std::setfill('0')
          << std::stoll(m[2], nullptr, 0);
      if (!out.contains(key.str()))
        out[key.str()] = J::array();
      auto &names = out[key.str()];
      if (std::find(names.begin(), names.end(), m[1].str()) == names.end())
        names.push_back(m[1].str());
    } else
      require(line.substr(0, line.find("//")).find('=') == line.npos ||
                  t.rfind("/*", 0) == 0 || t.rfind("*", 0) == 0,
              "unparsed symbol declaration: " + line);
  }
  return out;
}
inline void convert(const fs::path &up, const J &config, const fs::path &dest,
                    const J &source) {
  std::map<std::string, J> parsed;
  J files = J::object(), sets = J::object();
  for (auto &f : fs::directory_iterator(up / "config")) {
    auto n = f.path().filename().string();
    if (n.rfind("symbols.", 0) == 0 && f.path().extension() == ".txt")
      parsed["config/" + n] = symbols(dt::read(f.path()));
  }
  require(!parsed.empty(), "upstream config/symbols.*.txt files are missing");
  for (auto &p : parsed)
    files[p.first] = hash(dt::read(up / p.first));
  auto emit = [&](const std::string &name, const J &merged, J record) {
    std::map<std::string, J> sorted;
    J aliases = J::object();
    for (auto it = merged.begin(); it != merged.end(); ++it) {
      sorted[it.key()] = it.value()[0];
      if (it.value().size() > 1)
        aliases[it.key()] = it.value();
    }
    J names = J::object();
    for (auto &p : sorted)
      names[p.first] = p.second;
    write(dest / (name + ".json"), names);
    record["count"] = merged.size();
    record["aliases"] = aliases;
    sets[name] = record;
  };
  for (auto &b : config["binaries"]) {
    J merged = J::object();
    auto deps = b["options"].value("symbol_addrs_path", J::array());
    for (auto &d : deps) {
      auto path = text(d);
      require(parsed.count(path), "missing upstream symbols: " + path);
      for (auto it = parsed[path].begin(); it != parsed[path].end(); ++it) {
        auto names = it.value();
        if (merged.contains(it.key()))
          for (auto &n : merged[it.key()])
            if (std::find(names.begin(), names.end(), n) == names.end())
              names.push_back(n);
        merged[it.key()] = names;
      }
    }
    auto n = text(b["name"]);
    emit(n == "SLUS_008.92" ? "main" : n, merged,
         J{{"binary_sha1", b["sha1"]}, {"sources", deps}});
  }
  for (auto &p : parsed) {
    auto n = fs::path(p.first).filename().string();
    n = n.substr(8, n.size() - 12);
    if (!sets.contains(n))
      emit(n, p.second, J{{"sources", J::array({p.first})}});
  }
  write(
      dest / "index.json",
      J{{"source", source},
        {"scope", "disc 1 only; other-disc addresses and names are unverified"},
        {"source_files", files},
        {"sets", sets}});
}
inline void import_all(const fs::path &up, std::string commit,
                       const fs::path &dest, bool only, bool unverified,
                       const std::vector<std::string> &cues) {
  require(!unverified || (commit.empty() && only),
          "--unverified-cache requires --inventory-only and no --commit");
  if (commit.empty() && !unverified) {
    auto log = fs::path(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") /
               ("metadata-commit-" + std::to_string(getpid()));
    int status =
        dt::run({"git", "-C", up.string(), "rev-parse", "HEAD"}, {}, log);
    auto s = dt::read(log);
    fs::remove(log);
    require(status == 0, "git rev-parse failed");
    commit = Yaml::space(s.substr(0, s.find('\n')));
  }
  require(unverified || std::regex_match(commit, std::regex("[0-9a-f]{40}")),
          "--commit must be a full 40-character commit SHA");
  auto lb = dt::read(up / "config/SLUS_008.92_FF8DISC1.IMG.lock.yaml"),
       cb = dt::read(up / "config/ff8.yaml");
  auto lock = Yaml(lb).parse(), config = Yaml(cb).parse();
  J source = {{"repository", "https://github.com/roengstrom/ff8-decomp"},
              {"commit", unverified ? J() : J(commit)},
              {"license", "CC0-1.0"},
              {"lock_sha1", hash(lb)},
              {"config_sha1", hash(cb)}};
  if (unverified)
    source["status"] =
        "unverified local cache; upstream commit and symbols unavailable";
  std::set<std::string> missing;
  for (auto &b : config["binaries"])
    for (auto &p : b["options"].value("symbol_addrs_path", J::array()))
      if (!fs::is_regular_file(up / text(p)))
        missing.insert(text(p));
  require(missing.empty() || only,
          "missing " + std::to_string(missing.size()) +
              " upstream symbol files (first: " +
              (missing.empty() ? "" : *missing.begin()) + ")");
  require(cues.size() == 4,
          "import requires disc 1, 2, 3, 4 CUEs in that order");
  J all = J::array(), ref, refids;
  std::map<std::string, J> spec;
  std::map<std::string, std::string> heads;
  for (int i = 0; i < 4; ++i) {
    Image im(cues[i]);
    require(im.number == i + 1, "expected disc " + std::to_string(i + 1) +
                                    ", got " + std::to_string(im.number));
    J inv;
    if (i == 0) {
      ref = reference(im, lock, source);
      spec = specs(config, ref);
      refids = field_ids(im.layout());
      for (auto &e : ref["entries"])
        heads[text(e["path"])] = hash(
            im.read(num(e["sector"]), std::min<int64_t>(num(e["size"]), 2048)));
      inv = ref;
    } else
      inv = other(im, ref, refids, heads);
    inv["padding"] = coverage(im, inv["entries"]);
    inv["image"]["sha1"] = image_hash(im);
    all.push_back(inv);
    write(dest / ("files-disc" + std::to_string(i + 1) + ".json"), inv);
    int changed = 0, generated = 0;
    for (auto &e : inv["entries"]) {
      changed += e["differs_from_disc1"].get<bool>();
      generated += e["path_source"] == "generated";
    }
    std::cout << "disc " << i + 1 << ": " << inv["entries"].size() << " files, "
              << changed << " changed/new, " << generated << " generated names"
              << std::endl;
  }
  auto overlays = collect(all, spec, source);
  write(dest / "overlays-all.json", overlays);
  if (!only)
    convert(up, config, dest / "symbols", source);
  else
    write(dest / "symbols/index.json",
          J{{"source", source},
            {"status", "not_imported"},
            {"sets", J::object()},
            {"reason", "symbol conversion was skipped; no address-to-name maps "
                       "have been imported"},
            {"missing_source_files", missing}});
  int64_t size = 0;
  for (auto &o : overlays["overlays"])
    size += num(o["size"]);
  std::cout << "unique overlays: " << overlays["overlays"].size()
            << "; guest image bytes: " << size << '\n';
}
} // namespace metadata
