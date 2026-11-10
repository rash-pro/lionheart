// The player's first run with the compiler step, driven by a GUI script: a
// synthetic disc, a fake compiler archive this case serves on the loopback
// address, no network and no game data. Downloading with its progress, a
// cancel and a resume, a failed download and its retry, an archive chosen in
// the file browser, skipping the step, coming back to it from the hub while
// the game runs, the one offer an older installation gets, and the report of
// a session that ended abnormally.
#include "synthetic_player.h"
#include "host_fixtures.h"
#include "network.h"
#include "lh_json.h"
#include "tar_xz.h"
#include <algorithm>
#include <atomic>
#include <cctype>

namespace {
// stall: the first answer stops halfway until the player hangs up.
// fail: the first answer is an error. Later answers resume from a range.
enum class Serve { whole, stall, fail };
class Archive {
    lh::Socket socket_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
public:
    std::atomic<unsigned> requests{0}, ranges{0};
    std::string url;
    Archive(std::string bytes, Serve how) : socket_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        lh::socket_check(!bind(socket_.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)), "bind");
        socklen_t n = sizeof(a);
        getsockname(socket_.get(), reinterpret_cast<sockaddr *>(&a), &n);
        url = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/zig-test.tar.xz";
        listen(socket_.get(), 8);
        worker_ = std::thread([this, bytes, how] {
            while (!stop_) {
                pollfd p{socket_.get(), POLLIN, 0};
                if (poll(&p, 1, 100) <= 0) continue;
                lh::Socket peer(accept(socket_.get(), nullptr, nullptr));
                if (peer.get() < 0) continue;
                const auto request = peer.receive(2000);
                const auto number = ++requests;
                try {
                    if (how == Serve::fail && number == 1) {
                        peer.send("HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        continue;
                    }
                    size_t offset = 0;
                    const auto range = request.find("Range: bytes=");
                    if (range != std::string::npos) { offset = std::stoull(request.substr(range + 13)); ++ranges; }
                    std::string header = offset ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(offset) + "-" +
                                                      std::to_string(bytes.size() - 1) + "/" + std::to_string(bytes.size()) + "\r\n"
                                                : "HTTP/1.1 200 OK\r\n";
                    header += "Content-Length: " + std::to_string(bytes.size() - offset) + "\r\nConnection: close\r\n\r\n";
                    peer.send(header);
                    if (how == Serve::stall && number == 1) {
                        peer.send(bytes.substr(0, bytes.size() / 2));
                        for (int waited = 0; waited < 600 && !stop_; ++waited) {
                            pollfd q{peer.get(), POLLIN, 0};
                            char byte;
                            if (poll(&q, 1, 100) > 0 && recv(peer.get(), &byte, 1, MSG_DONTWAIT) <= 0) break;
                        }
                        continue;
                    }
                    peer.send(bytes.substr(offset));
                } catch (...) {}
            }
        });
    }
    ~Archive() { stop_ = true; worker_.join(); }
};

struct Setup {
    lh::Case &t;
    lh::fs::path binary, media, disc, archive, manifest, ini, data, shots;
    std::string bytes;
    explicit Setup(lh::Case &c) : t(c), binary(synthetic::player(c)), media(c.temp() / "media"), ini(c.temp() / "lionheart.ini"),
                                  data(c.temp() / "data") {
        lh::fs::create_directories(media);
        disc = synthetic::disc(media / "disc1.bin");
        // As the Linux player's pinned archive is packed: tar in xz.
        bytes = tar_xz::xz(tar_xz::tar({tar_xz::member({"zig-test/zig", '0', lh::read_file(t.harness("fake_zig").program())})}));
        archive = media / "zig-test.tar.xz";
        lh::write_file(archive, bytes);
        lh::write_file(ini, "");
        // Pictures of each state, for people to look at; nothing compares them.
        const char *folder = getenv("LIONHEART_NATIVE_GUI_SHOTS");
        shots = lh::fs::absolute(folder ? lh::fs::path(folder) : c.temp() / "shots");
        lh::fs::create_directories(shots);
        manifest = c.temp() / "manifest.txt";
        address("");
    }
    // The archive's identity, and where it downloads from; none means the
    // player has only the offline path.
    void address(const std::string &url) {
        lh::write_file(manifest, "version=test-1\nprefix=zig-test/\nexecutable=zig\nsha256=" + lh::sha256(bytes) +
                                     "\nsize=" + std::to_string(bytes.size()) + "\nfile=zig\n" + (url.empty() ? "" : "url=" + url + "\n"));
    }
    // "shot NAME" in steps captures the screen to the pictures folder.
    lh::Result run(std::string steps, const std::string &name = "gui", bool pinned = false) {
        const auto script = t.temp() / (name + ".txt");
        for (size_t at = 0; (at = steps.find("\nshot ", at)) != std::string::npos;) {
            const auto end = steps.find('\n', ++at);
            const auto replacement = "capture \"" + (shots / (steps.substr(at + 5, end - at - 5) + ".png")).string() + "\"";
            steps.replace(at, end - at, replacement);
            at += replacement.size();
        }
        lh::write_file(script, steps);
        auto command = lh::dummy(t.target(binary)).args({"--config", ini, "--gui-test", script, "--present-size", "1280x720"})
                           .cwd(t.temp()).env("SDL_RENDER_DRIVER", "software").env("HOME", t.temp().string())
                           .env("XDG_DATA_HOME", data.string()).env("XDG_CONFIG_HOME", (t.temp() / "config").string())
                           .timeout(120);
        if (!pinned) command.env("LIONHEART_TOOLCHAIN_TEST_MANIFEST", manifest.string());
        auto result = t.run(command);
        lh::write_file(t.temp() / (name + ".log"), result.out + result.err);
        LH_CHECK_MSG(result.rc == 0 && !result.timed_out, result.out + result.err);
        LH_CONTAINS(result.err, "GUI test complete:");
        return result;
    }
    lh::fs::path folder() const { return data / "lionheart"; }
    bool installed() const { return lh::fs::exists(folder() / "toolchain/test-1/receipt"); }
};

// Disc 1 and a new card, to the compiler step.
std::string to_compiler_step(const Setup &s) {
    return "wait 10\ndrop \"" + s.disc.string() + "\"\n"
           "wait-text disc.1.status \"SLUS-00892 · verified\" 600\nassert-focus btn.continue\nselect\nwait 3\n"
           "assert-focus btn.continue\nselect\nwait 3\nassert-text setup.native.title \"Compiler\"\n";
}
// As test_host_gui's, this harness links partial ELF objects.
lh::Command gui_harness(lh::Case &t) {
    if (lh::internal::build_info().windows_paths) lh::skip("the GUI harness links partial ELF objects");
    return lh::dummy(t.harness("gui"));
}
std::string strip(std::string s) { while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s; }
const char *const kPlayFromHub = "wait 3\nassert-focus btn.play\nselect\nwait 30\n";
}

// The hub row and the reasons the installer gives, in the words of the player.
namespace {
struct NativeRow { std::string id; std::vector<std::string> args; std::string expected; };
// compiler step done total build sets ready failed seconds_left -> alert, second line of the hub row
static std::vector<NativeRow> native_rows() {
    return {{"missing",{"0","0","0","0","0","0","0","0","-1"},"0 Compiler not installed"},
            {"downloading",{"1","0","12582912","59586314","0","0","0","0","-1"},"0 Downloading · 12 of 57 MB"},
            {"unpacking",{"1","2","3","50","0","0","0","0","-1"},"0 Unpacking"},
            {"install-failed",{"2","0","0","0","0","0","0","0","-1"},"1 The download failed."},
            {"installed-before-the-game",{"3","0","0","0","0","0","0","0","-1"},"0 Compiler installed"},
            {"building-without-estimate",{"3","0","0","0","1","428","12","0","-1"},"0 12 of 428 ready"},
            {"building-minutes",{"3","0","0","0","1","428","200","0","1500"},"0 200 of 428 ready · about 25 min left"},
            {"building-last-minute",{"3","0","0","0","1","428","427","0","20"},"0 427 of 428 ready · about 1 min left"},
            {"building-hours",{"3","0","0","0","1","428","20","0","4000"},"0 20 of 428 ready · about 1 h 10 min left"},
            {"done",{"3","0","0","0","2","428","428","0","0"},"0 428 of 428 ready"},
            {"done-with-failures",{"3","0","0","0","2","428","426","2","0"},"1 426 of 428 ready · 2 failed"},
            {"stopped",{"3","0","0","0","3","428","40","0","-1"},"1 40 of 428 ready · stopped"}};
}
}
LH_TEST_P(test_native_row_copy, native_rows()) {
    auto command=gui_harness(t).arg("native-summary");
    for(const auto &arg:p.args) command.arg(arg);
    LH_TEXT_EQ(strip(t.ok(command).out),p.expected);
}
struct Reason { std::string id, message, expected; };
static std::vector<Reason> native_errors() {
    std::vector<Reason> out;
    for(auto row: std::vector<std::pair<std::string,std::string>>{
      {"Cancelled","native.err.cancelled"},{"Download failed; retry to resume","native.err.download"},
      {"No download URL configured for this host","native.err.no_url"},{"Archive size mismatch","native.err.archive"},
      {"Archive SHA-256 mismatch","native.err.archive"},{"Cannot read archive","native.err.read"},
      {"Invalid archive or missing retained file","native.err.damaged"},{"Compiler version self-check failed","native.err.check"},
      {"Another installer is active; retry","native.err.busy"},{"Cannot publish toolchain receipt","native.err.disk"},
      {"something new","native.err.other"}})
        out.push_back({row.second+"-"+std::to_string(out.size()),row.first,row.second});
    return out;
}
LH_TEST_P(test_native_install_reason_copy, native_errors()) {
    LH_TEXT_EQ(strip(t.ok(gui_harness(t).args({"native-reason",p.message})).out),p.expected);
}
LH_TEST(test_no_copy_offers_the_interpreter) {
    // Native code is how the game runs; what it falls back to is not a mode to choose.
    auto header=lh::read_file(t.root()/"backends/host/gui_strings.h");
    std::transform(header.begin(),header.end(),header.begin(),[](unsigned char c){ return char(std::tolower(c)); });
    LH_NOT_CONTAINS(header,"interpret");
}

LH_TEST(test_download_shows_progress_resumes_after_cancel_and_reaches_the_game) {
    Setup s(t);
    Archive server(s.bytes, Serve::stall);
    s.address(server.url);
    const auto r = s.run(to_compiler_step(s) +
        "assert-focus setup.native.download\nselect\n"
        "wait-text native.step \"Downloading\" 600\nassert-text native.amount \"0 of 1 MB\"\nassert-focus native.cancel\nshot step-downloading\n"
        "select\nwait-text native.error \"Download cancelled.\" 600\nassert-focus native.retry\nshot step-cancelled\n"
        "select\nwait-text native.installed \"Installed\" 1800\nassert-focus btn.finish\nshot step-installed\nselect\n"
        "wait-text hub.native.sub \"Compiler installed\" 60\nshot hub-installed\n" + std::string(kPlayFromHub) +
        "wait 60\nopen\nwait 3\nassert-text hub.native \"Native code\"\nshot overlay-stopped\nclose\nquit\n");
    LH_CHECK(s.installed());
    LH_EQ(server.ranges.load(), 1u);
    LH_CONTAINS(r.err, "Compiler installed:");
    // With the compiler there at boot, building starts with the game; these
    // discs are not ones it can build from, which it says.
    LH_CONTAINS(r.err, "native: building stopped:");
    LH_CONTAINS(lh::read_file(s.folder() / "gui-state.ini"), "native-offered=1\n");
}

LH_TEST(test_failed_download_names_the_reason_and_retries) {
    Setup s(t);
    Archive server(s.bytes, Serve::fail);
    s.address(server.url);
    s.run(to_compiler_step(s) +
        "select\nwait-text native.error \"The download failed.\" 600\nshot step-failed\n"
        "assert-focus native.retry\n"
        "select\nwait-text native.installed \"Installed\" 1800\nselect\nwait 3\nquit\n");
    LH_CHECK(s.installed());
    LH_EQ(server.requests.load(), 2u);
}

LH_TEST(test_archive_chosen_in_the_file_browser_installs_offline) {
    Setup s(t);
    s.run(to_compiler_step(s) +
        "assert-text setup.native.download.sub \"Not available in this build\"\nassert-focus setup.native.archive\n"
        "assert-text setup.native.archive.sub \"tar.xz · 1 MB\"\nshot step-offline\nselect\nwait 3\n"
        "assert-focus browser.entry.0\nassert-text browser.entry.0 \"zig-test.tar.xz\"\nshot browser-archive\nselect\n"
        "wait-text native.installed \"Installed\" 1800\nassert-focus btn.finish\nselect\nwait 3\nquit\n");
    LH_CHECK(s.installed());
}

LH_TEST(test_pinned_compiler_download_is_enabled_and_shows_manifest_size) {
    Setup s(t);
    auto entry = lh::Json::parse(lh::read_file(t.root()/"data/toolchain/manifest.json"))["platforms"]["x86_64-linux"];
    LH_CHECK(entry["urls"].size() > 0);
    auto mb = (entry["size"].integer() + 1048575) / 1048576;
    s.run(to_compiler_step(s) + "assert-focus setup.native.download\n"
          "assert-text setup.native.download.sub \"" + std::to_string(mb) + " MB\"\nquit\n", "pinned", true);
}

LH_TEST(test_skipping_the_step_still_reaches_the_game) {
    Setup s(t);
    const auto r = s.run(to_compiler_step(s) +
        "assert-text btn.not_now \"Not now\"\npad start\n"
        "wait-text hub.native.sub \"Compiler not installed\" 60\nshot hub-missing\n" + std::string(kPlayFromHub) +
        "open\nwait 3\nassert-text hub.native.sub \"Compiler not installed\"\nshot overlay-missing\nclose\nquit\n");
    LH_CHECK(!s.installed());
    LH_NOT_CONTAINS(r.err, "native: building");
    LH_CONTAINS(lh::read_file(s.folder() / "gui-state.ini"), "native-offered=1\n");
}

LH_TEST(test_compiler_installed_from_the_hub_starts_building_in_the_game) {
    Setup s(t);
    Archive server(s.bytes, Serve::whole);
    s.address(server.url);
    const auto r = s.run(to_compiler_step(s) + "pad start\n" + std::string(kPlayFromHub) +
        "open\nwait 3\nassert-focus btn.resume\n"
        "nav down\nnav down\nnav down\nnav down\nnav down\nnav down\nassert-focus hub.native\nselect\nwait 3\n"
        "assert-focus setup.native.download\nshot dialog-install\nselect\n"
        // Once installed the dialog has nothing left to install and shows the build.
        "wait-text nav.native \"Native code\" 1800\nwait 60\nshot dialog-details\nback\nwait 3\nclose\nwait 120\nquit\n");
    LH_CHECK(s.installed());
    LH_CONTAINS(r.err, "Compiler installed:");
    LH_CONTAINS(r.err, "native: building stopped:");
}

LH_TEST(test_an_existing_installation_is_offered_the_step_once) {
    Setup s(t);
    lh::write_file(s.ini, "disc1=" + s.disc.string() + "\n");
    const auto first = s.run("wait 60\nassert-text setup.native.title \"Compiler\"\nassert-focus setup.native.archive\nshot step-offered\npad start\nwait 30\nquit\n", "first");
    LH_CONTAINS(first.out, "Hardware:");
    // Straight into the game: a script cannot schedule a frame while a menu shows.
    s.run("at 30\nopen\nwait 3\nassert-text hub.native.sub \"Compiler not installed\"\nclose\nquit\n", "second");
}

LH_TEST(test_a_killed_session_is_reported_with_its_log) {
    Setup s(t);
    lh::write_file(s.ini, "disc1=" + s.disc.string() + "\n");
    lh::fs::create_directories(s.folder());
    lh::write_file(s.folder() / "gui-state.ini", "last-folder=\nlast-input=keyboard\nnative-offered=1\n");
    // The game runs until the case gives up on it and kills it.
    auto killed = synthetic::play(t, {"--config", s.ini, "--fast", "--mute", "--max-vblanks", "0", "--log", "/dev/null"}, false, s.binary)
                      .env("XDG_DATA_HOME", s.data.string()).timeout(3);
    // A timeout here is the point, so the runner's own rule does not apply.
    const auto first = lh::internal::run_process(killed);
    LH_CHECK_MSG(first.timed_out, first.out + first.err);
    const auto r = s.run("wait-text crash.line \"The last session ended unexpectedly · lionheart.1.log\" 600\nwait 2\n"
                         "assert-focus btn.play\nshot hub-crash\nnav right\nassert-focus crash.open\nselect\nwait 3\nquit\n", "after");
    LH_CONTAINS(r.err, "GUI test: open file://" + (s.folder() / "logs/lionheart.1.log").string());
    // That session ended as it meant to: the next one starts straight in.
    s.run("at 30\nopen\nwait 3\nclose\nquit\n", "clean");
}
