// Disc extraction can keep a shared image tree entirely read-only.
#include "generated.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <vector>

using namespace lh::gen;

namespace {

namespace {

void put32(std::string &out, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<char>(value >> (8 * i));
}
// replaced the parser with a mock, and C has no such seam.
std::string minimal_exe() {
    std::string exe(0x800 + 16, '\0');
    std::memcpy(exe.data(), "PS-X EXE", 8);
    put32(exe, 0x10, 0x80010000); // entry
    put32(exe, 0x18, 0x80010000); // base
    put32(exe, 0x1C, 16);         // text size
    exe.replace(0x800, 16, pack({0x03E00008, 0, 0, 0}));
    return exe;
}

std::string sha1_hex(const std::string &data) {
    char text[41];
    lhg_sha1_hex(reinterpret_cast<const uint8_t *>(data.data()), data.size(), text);
    return text;
}

// MODE2/2352 sectors: the reader needs sector 16, the root directory it points
// to, and the sectors named by the directory and the top table.
class DiscImage {
public:
    void set(uint32_t lba, const std::string &user) {
        std::string raw(2352, '\0');
        raw.replace(24, std::min<size_t>(user.size(), 2048), user, 0, 2048);
        if (sectors_.size() <= lba) sectors_.resize(lba + 1, std::string(2352, '\0'));
        sectors_[lba] = raw;
    }
    void write(const lh::fs::path &bin) const {
        std::string all;
        for (const std::string &sector : sectors_) all += sector;
        lh::write_file(bin, all);
    }
private:
    std::vector<std::string> sectors_;
};

std::string directory_record(const std::string &name, uint32_t lba, uint32_t size, bool directory) {
    std::string record(33, '\0');
    const uint8_t length = static_cast<uint8_t>((33 + name.size() + 1) & ~1u);
    record.resize(length);
    record[0] = static_cast<char>(length);
    put32(record, 2, lba);
    put32(record, 10, size);
    record[25] = directory ? 2 : 0;
    record[32] = static_cast<char>(name.size());
    record.replace(33, name.size(), name);
    return record;
}
// so the C reader does its real work, and a wrong LBA finds other bytes.
void build_disc(const lh::fs::path &directory, const std::string &exe, const std::string &payload, bool external, int index) {
    DiscImage image;
    std::string pvd(2048, '\0');
    pvd[0] = 1;
    std::memcpy(pvd.data() + 1, "CD001", 5);
    const std::string root = directory_record(std::string(1, '\0'), 18, 2048, true);
    pvd.replace(156, root.size(), root);
    image.set(16, pvd);
    std::string listing;
    listing += directory_record("SLUS_008.92;1", 30, static_cast<uint32_t>(exe.size()), false);
    listing += directory_record("FF8DISC1.IMG;1", 20, 2048, false);
    listing.resize(2048, '\0');
    image.set(18, listing);
    std::string table(2048, '\0');
    put32(table, index * 8, 77);
    put32(table, index * 8 + 4, static_cast<uint32_t>(payload.size()));
    image.set(20, table);
    for (size_t offset = 0; offset < exe.size(); offset += 2048) {
        image.set(30 + static_cast<uint32_t>(offset / 2048), exe.substr(offset, 2048));
    }
    // has the payload, so reading the other fails the catalog's SHA-1 check.
    const std::string other(payload.size(), '\xff');
    image.set(77, external ? other : payload);
    image.set(108, external ? payload : other);
    image.write(directory / "disc.bin");
    lh::write_file(directory / "disc.cue", "FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
}

struct External { const char *id; bool external; };
const External kExternal[] = {{"False", false}, {"True", true}};

} // namespace

LH_TEST_P(test_assets_directory_and_overlay_source, kExternal) {
    const std::string exe = minimal_exe(), payload(32, '\0');
    const int index = p.external ? 27 : 2;

    const lh::fs::path catalog = t.temp() / "catalog", shared = t.temp() / "shared", assets = t.temp() / "build/assets";
    lh::fs::create_directories(catalog);
    lh::fs::create_directories(shared);

    build_disc(shared, exe, payload, p.external, index);

    std::ostringstream row;
    row << "{\"index\":" << index << ",\"name\":\"" << (p.external ? "movie" : "field")
        << "\",\"load_address\":\"0x80098000\",\"compressed\":false,\"size\":" << payload.size()
        << ",\"sha1\":\"" << sha1_hex(payload) << "\"";
    if (p.external) row << ",\"img_sector\":88,\"stored_size\":" << payload.size();
    row << "}";
    lh::write_file(catalog / "overlays.json",
                   "{\"exe_sha1\":\"" + sha1_hex(exe) + "\",\"main_code_ranges\":[[\"0x80010000\",\"0x80010010\"]],"
                   "\"discs\":{\"1\":{\"exe\":\"SLUS_008.92\",\"image\":\"FF8DISC1.IMG\",\"overlays\":[" + row.str() + "]}}}");

    // The shared tree is read-only for the whole extraction, which is what the
    const auto list = [](const lh::fs::path &root) {
        std::set<std::string> names;
        for (const auto &entry : lh::fs::recursive_directory_iterator(root)) names.insert(entry.path().string());
        return names;
    };
    const std::set<std::string> before = list(shared), catalog_before = list(catalog);
    LH_EQ(chmod(shared.c_str(), 0555), 0);

    lhgt_session *session = lhgt_open();
    LH_CHECK(session != nullptr);
    lhg_program program{};
    const int status = lhgt_load_legacy_disc(session, catalog.c_str(), (shared / "disc.cue").c_str(), assets.c_str(), &program);
    const std::string error = status ? lhgt_error(session) : "";
    chmod(shared.c_str(), 0755);
    LH_CHECK_MSG(status == 0, error);

    LH_BYTES_EQ(std::string(reinterpret_cast<const char *>(program.main->data), program.main->size), exe.substr(0x800, 16));
    LH_EQ(program.overlay_count, 1u);
    LH_EQ(program.overlays[0].index, index);
    lhgt_close(session);

    LH_BYTES_EQ(lh::read_file(assets / "SLUS_008.92"), exe);
    char name[32];
    std::snprintf(name, sizeof name, "overlays/disc1/%02d.bin", index);
    LH_BYTES_EQ(lh::read_file(assets / name), payload);
    LH_CHECK(list(shared) == before);
    LH_CHECK(list(catalog) == catalog_before);
}

LH_TEST(test_movie_metadata_matches_existing_catalog) {
    const Json metadata = Json::file(t.root() / "games/ff8-usa/overlays.json");
    const Json catalog = Json::file(t.root() / "games/ff8-usa/overlays-all.json");
    const Json overlays = catalog.at("overlays");
    bool found = false;
    Json movie;
    for (size_t index = 0; index < overlays.size(); ++index) {
        if (overlays.at(index).at("name").str() == "movie") {
            movie = overlays.at(index);
            found = true;
            break;
        }
    }
    LH_CHECK(found);
    const Json locations = movie.at("locations");
    for (size_t index = 0; index < locations.size(); ++index) {
        const Json location = locations.at(index);
        const Json rows = metadata.at("discs").at(std::to_string(location.at("disc").integer())).at("overlays");
        bool matched = false;
        for (size_t at = 0; at < rows.size() && !matched; ++at) {
            const Json row = rows.at(at);
            if (row.at("index").integer() != 27) continue;
            LH_TEXT_EQ(row.at("load_address").str(), movie.at("load_address").str());
            LH_TEXT_EQ(row.at("sha1").str(), movie.at("sha1").str());
            LH_EQ(row.at("size").integer(), movie.at("size").integer());
            LH_EQ(row.at("img_sector").integer(), location.at("sector").integer());
            LH_EQ(row.at("stored_size").integer(), location.at("read_size").integer());
            matched = true;
        }
        LH_CHECK(matched);
    }
}

} // namespace
