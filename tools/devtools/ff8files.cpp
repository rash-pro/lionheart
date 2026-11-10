#include "metadata_import.h"
int main(int argc, char **argv) {
  using namespace metadata;
  try {
    std::map<std::string, std::string> options;
    std::set<std::string> flags;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
      std::string s = argv[i];
      if (s == "--help" || s == "-h") {
        std::cout << "Usage: ff8files {import,list,get,verify,prefixes}\nimport "
                     "--upstream DIR [--commit SHA] [--output DIR] "
                     "[--inventory-only] [--unverified-cache] CUE...\nlist CUE "
                     "[PATTERN] [--overlays] [--json] [--metadata DIR]\nget "
                     "CUE QUERY OUT [--raw|--loaded|--decompress] [--metadata "
                     "DIR]\nverify CUE [--scan-disc1 CUE] [--metadata DIR]\nprefixes CUE "
                     "[--metadata DIR]\n";
        return 0;
      }
      auto eq = s.find('=');
      std::string key = s.substr(0, eq);
      if (key == "--metadata" || key == "--upstream" || key == "--commit" ||
          key == "--output" || key == "--scan-disc1") {
        if (eq != s.npos)
          options[key] = s.substr(eq + 1);
        else {
          if (i + 1 == argc) {
            std::cerr << "ff8files: expected argument for " << key << '\n';
            return 2;
          }
          options[key] = argv[++i];
        }
      } else if (s == "--raw" || s == "--loaded" || s == "--decompress" ||
                 s == "--overlays" || s == "--json" ||
                 s == "--inventory-only" || s == "--unverified-cache")
        flags.insert(s);
      else if (s.rfind("--", 0) == 0) {
        std::cerr << "ff8files: unrecognized argument: " << s << '\n';
        return 2;
      } else
        pos.push_back(s);
    }
    fs::path root = LH_TOOLS_ROOT, dir = options.count("--metadata")
                                             ? fs::path(options["--metadata"])
                                             : root / "games/ff8-usa";
    if (pos.empty() || (pos[0] != "import" && pos[0] != "list" &&
                        pos[0] != "get" && pos[0] != "verify" &&
                        pos[0] != "prefixes")) {
      std::cerr << "ff8files: expected import, list, get, verify or prefixes\n";
      return 2;
    }
    auto command = pos[0];
    if (command == "import") {
      if (!options.count("--upstream"))
        return 2;
      import_all(
          options["--upstream"], options["--commit"],
          options.count("--output") ? fs::path(options["--output"]) : dir,
          flags.count("--inventory-only"), flags.count("--unverified-cache"),
          {pos.begin() + 1, pos.end()});
      return 0;
    }
    if (pos.size() < 2 || (command == "get" && pos.size() != 4) ||
        (command == "list" && pos.size() > 3) ||
        ((command == "verify" || command == "prefixes") && pos.size() != 2))
      return 2;
    Image im(pos[1]);
    auto inv = inventory(im, dir);
    if (command == "list") {
      auto pattern = pos.size() == 3 ? lower(pos[2]) : "";
      J entries = J::array();
      for (auto &e : inv["entries"])
        if (lower(text(e["path"])).find(pattern) != std::string::npos &&
            (!flags.count("--overlays") || overlay(e)))
          entries.push_back(e);
      if (flags.count("--json"))
        std::cout << entries.dump(2, ' ', true) << '\n';
      else
        for (auto &e : entries)
          std::cout << std::setw(7) << num(e["sector"]) << ' ' << std::setw(9)
                    << num(e["size"]) << ' ' << text(e["sha1"]) << ' '
                    << text(e["path"]) << '\n';
    } else if (command == "prefixes") {
      std::cout << prefixes(im, inv, dir).dump(2, ' ', true) << '\n';
    } else if (command == "verify") {
      verify(im, inv, dir);
      if (options.count("--scan-disc1"))
        scan(im, inv, options["--scan-disc1"], dir);
    } else {
      auto e = select(inv, pos[2], dir);
      auto r = raw_overlay(im, e);
      require(hash(r.first) == text(e["sha1"]),
              "file SHA-1 mismatch: " + text(e["path"]));
      require(flags.count("--raw") + flags.count("--loaded") +
                      flags.count("--decompress") <=
                  1,
              "--raw, --loaded and --decompress are mutually exclusive");
      auto data = flags.count("--raw") ? r.first
                  : overlay(e)         ? r.second
                                       : r.first;
      if (flags.count("--loaded")) {
        require(overlay(e), "--loaded requires a code overlay");
        data = e["compressed"].get<bool>()
                   ? decode(r.first.substr(0, num(e["read_size"])))
                   : r.first;
        require(hash(data) == text(e["loaded_sha1"]) &&
                    data.size() == e["loaded_size"],
                "loader-visible overlay hash/size mismatch");
      }
      if (flags.count("--decompress") && !overlay(e)) {
        require(r.first.size() >= 4 &&
                    uint64_t(u32(r.first, 0)) + 4 <= r.first.size(),
                "file does not contain a complete LZS stream");
        data = decode(r.first);
      }
      if (overlay(e))
        require(hash(r.second) == text(e["overlay_sha1"]),
                "decompressed overlay hash mismatch");
      auto out = fs::weakly_canonical(pos[3]);
      auto inside = [](const fs::path &p, const fs::path &base) {
        auto r = p.lexically_relative(fs::weakly_canonical(base));
        return !r.empty() && *r.begin() != "..";
      };
      require(
          !inside(out, root) || inside(out, root / "work") ||
              inside(out, root / "build"),
          "extraction inside the repository is restricted to work/ or build/");
      dt::write(pos[3], data);
      std::cout << "wrote " << data.size() << " bytes; SHA-1 " << hash(data)
                << '\n';
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ff8files: " << e.what() << '\n';
    return 1;
  }
}
