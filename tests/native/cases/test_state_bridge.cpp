// Host-only telemetry transport and local rcheevos; no disc or RA account
// needed.
#include "host_fixtures.h"
#include "lh_json.h"
#include "network.h"
#include <array>
#include <csignal>
#include <sys/stat.h>
#include <utime.h>

namespace {
using lh::Json;
Json json(const std::string &s) { return Json::parse(s); }
Json body(const std::string &s) {
    auto at = s.find("\r\n\r\n");
    LH_CHECK_MSG(at != std::string::npos, s);
    return json(s.substr(at + 4));
}
struct Bridge {
    lh::Library lib;
#define FN(ret, name, ...) ret (*name)(__VA_ARGS__) = lib.fn<ret(__VA_ARGS__)>(#name)
    FN(void *, test_state_cpu);
    FN(void, test_state_free, void *);
    FN(void, test_state_write, void *, unsigned, unsigned, unsigned);
    FN(void *, host_state_bridge_open, const char *);
    FN(void, host_state_bridge_frame, void *, void *, uint64_t);
    FN(void, host_state_bridge_restore, void *, void *, uint64_t);
    FN(void, host_state_bridge_close, void *);
    FN(void, host_state_bridge_event, void *, const char *);
    FN(void *, test_achievements_open, const char *, const char *, const char *);
    FN(void, host_achievements_frame, void *, void *);
    FN(int, host_achievements_close, void *);
    FN(uint32_t, host_achievements_read, uint32_t, void *, uint32_t, void *);
    FN(const char *, test_last_event);
    FN(unsigned, test_events);
    FN(void, test_clock, double);
    FN(void, test_network, int, int);
    FN(void, test_client, int);
    FN(void, test_send_mode, int);
    FN(void, test_sync_fail, int, int);
    FN(void, test_operations_clear);
    FN(const char *, test_operations);
    FN(int, test_reuse_before_bind);
    FN(int, test_sigpipe_is_default);
    FN(void, host_ff8_state_json, void *, void *, size_t);
#undef FN
    void *cpu = nullptr, *b = nullptr, *a = nullptr;
    lh::Socket local;
    int original = -1;
    explicit Bridge(lh::Case &t) : lib(t.library("bridge")), cpu(test_state_cpu()) { LH_CHECK(cpu); }
    ~Bridge() {
        if (a)
            host_achievements_close(a);
        if (b)
            host_state_bridge_close(b);
        test_state_free(cpu);
        test_clock(-1);
        test_network(0, 0);
        test_send_mode(0);
        test_sync_fail(0, 0);
    }
    void put(unsigned address, unsigned value, unsigned size = 4) {
        test_state_write(cpu, address, value, size);
    }
    void frame(uint64_t v) { host_state_bridge_frame(b, cpu, v); }
    void achievement_frame() { host_achievements_frame(a, cpu); }
    int close_achievements() {
        auto old = a;
        a = nullptr;
        return host_achievements_close(old);
    }
    void open_achievements(const lh::fs::path &patch, const lh::fs::path &progress) {
        a = test_achievements_open(patch.c_str(), progress.c_str(), nullptr);
        LH_CHECK(a);
    }
    void open(const std::string &address) {
        b = host_state_bridge_open(address.c_str());
        LH_CHECK(b);
    }
    unsigned tcp() {
        lh::StderrCapture capture;
        b = host_state_bridge_open("0");
        auto err = capture.take();
        if (!b && err.find("Operation not permitted") != std::string::npos)
            lh::skip("sandbox forbids listening TCP sockets; inherited transport has "
                     "separate coverage");
        LH_CHECK_MSG(b, err);
        return unsigned(std::stoul(lh::capture(err, R"(127\.0\.0\.1:(\d+))")));
    }
    void inherited() {
        auto pair = lh::socket_pair();
        local = std::move(pair.first);
        local.nonblocking();
        original = pair.second.release();
        open("fd:" + std::to_string(original));
    }
    void request(const std::string &path = "/state", const std::string &host = "localhost") {
        local.send("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
    }
    Json snapshot() {
        std::array<char, 16384> buffer{};
        host_ff8_state_json(cpu, buffer.data(), buffer.size());
        return json(buffer.data());
    }
    Json event() { return json(test_last_event()); }
};
lh::fs::path patch_file(lh::Case &t, const std::string &achievements, const std::string &extra = "") {
    auto path = t.temp() / "patch.json";
    lh::write_file(path, "{\"PatchData\": {\"Achievements\": " + achievements + extra + "}}");
    return path;
}
const std::string achievement42 = "[{\"ID\": 42, \"MemAddr\": \"0xH000100=7\"}]";
} // namespace

LH_TEST(test_unlock_and_persistence) {
    Bridge c(t);
    auto patch = patch_file(t, achievement42), progress = t.temp() / "progress.json";
    c.open_achievements(patch, progress);
    c.achievement_frame();
    c.put(0x100, 7, 1);
    c.achievement_frame();
    LH_EQ(c.event(), json(R"({"type":"achievement","id":42,"persisted":true})"));
    LH_EQ(json(lh::read_file(progress))["unlocked"], json("[42]"));
    c.achievement_frame();
    LH_EQ(c.test_events(), 1u);
    LH_CHECK(c.close_achievements());
    c.open_achievements(patch, progress);
    c.put(0x100, 0, 1);
    c.achievement_frame();
    c.put(0x100, 7, 1);
    c.achievement_frame();
    LH_EQ(c.test_events(), 0u);
}
LH_TEST(test_ps1_linear_memory) {
    Bridge c(t);
    c.put(0x1fffff, 0xfe, 1);
    c.put(0x200000, 0x12345678, 4);
    std::array<char, 5> data{};
    LH_EQ(c.host_achievements_read(0x1fffff, data.data(), 5, c.cpu), 5u);
    LH_BYTES_EQ(std::string(data.data(), 5), std::string("\xfe\x78\x56\x34\x12", 5));
    LH_EQ(c.host_achievements_read(0x200400, data.data(), 4, c.cpu), 0u);
    LH_EQ(c.host_achievements_read(0xffffffff, data.data(), 4, c.cpu), 0u);
}
static const char *const bad_patches[] = {
    "{", R"({"PatchData":{"Achievements":[{"ID":1,"MemAddr":"bad"}]}})",
    R"({"PatchData":{"Achievements":[{"ID":1,"MemAddr":"0=1"},{"ID":1,"MemAddr":"0=2"}]}})"};
LH_TEST_P(test_bad_patch_fails, bad_patches) {
    Bridge c(t);
    auto patch = t.temp() / "bad.json", progress = t.temp() / "progress.json";
    lh::write_file(patch, p);
    LH_CHECK(!c.test_achievements_open(patch.c_str(), progress.c_str(), nullptr));
}
LH_TEST(test_corrupt_progress_is_preserved) {
    Bridge c(t);
    auto patch = patch_file(t, R"([{"ID": 1, "MemAddr": "0xH000100=7"}])"),
         progress = t.temp() / "progress.json";
    lh::write_file(progress, "broken");
    LH_CHECK(!c.test_achievements_open(patch.c_str(), progress.c_str(), nullptr));
    LH_TEXT_EQ(lh::read_file(progress), "broken");
}
LH_TEST(test_leaderboard_and_rich_presence) {
    Bridge c(t);
    auto patch = patch_file(t, "[]",
                            ", \"Leaderboards\": [{\"ID\": 8, \"Mem\": "
                            "\"STA:0xH000100=1::CAN:0xH000100=2::SUB:0xH000100=3::VAL:0xH000101\"}], "
                            "\"RichPresencePatch\": \"Display:\\nLocal \\\"test\\\"\\n\"");
    c.open_achievements(patch, t.temp() / "progress.json");
    c.achievement_frame();
    LH_EQ(c.event(), json(R"({"type":"rich_presence","text":"Local \"test\""})"));
    c.put(0x100, 1, 1);
    c.achievement_frame();
    LH_EQ(c.event()["type"], Json("leaderboard"));
    c.put(0x101, 9, 1);
    c.put(0x100, 3, 1);
    c.achievement_frame();
    LH_EQ(c.event()["value"], Json(9));
}
LH_TEST(test_http_sse_and_changed_snapshots) {
    Bridge c(t);
    auto port = c.tcp();
    auto events = lh::http_get(port, "/events");
    c.frame(1);
    auto initial = events.receive(1000);
    LH_CONTAINS(initial, "text/event-stream");
    LH_CONTAINS(initial, "event: state");
    {
        auto s = lh::http_get(port, "/state");
        c.frame(2);
        auto snapshot = body(s.receive(1000));
        LH_EQ(snapshot["vblank"], Json(2));
        LH_EQ(snapshot["schema"], Json(1));
    }
    c.host_state_bridge_event(c.b, R"({"type":"achievement","id":9})");
    LH_CONTAINS(events.receive(1000), "\"id\":9");
    lh::sleep_ms(260);
    c.frame(3);
    LH_BYTES_EQ(events.receive(50), "");
    auto s = lh::http_get(port, "/wrong");
    c.frame(4);
    LH_CONTAINS(s.receive(1000), "404 Not Found");
}
LH_TEST(test_http_and_sse_inherited_socket) {
    Bridge c(t);
    c.inherited();
    for (int v : {1, 2}) {
        c.request();
        c.frame(v);
        LH_EQ(body(c.local.receive())["vblank"], Json(v));
    }
    c.request("/events");
    c.frame(3);
    LH_CONTAINS(c.local.receive(), "event: state");
    c.host_state_bridge_event(c.b, R"({"type":"achievement","id":7})");
    LH_CONTAINS(c.local.receive(), "\"id\":7");
    lh::sleep_ms(260);
    c.frame(4);
    LH_BYTES_EQ(c.local.receive(), "");
}
LH_TEST(test_snapshot_contract_and_cumulative_kills) {
    Bridge c(t);
    LH_EQ(c.snapshot()["available"], Json(false));
    c.put(0x7737a, 0x08ff, 2);
    c.put(0x781a4, 1, 1);
    c.put(0x5f158, 1, 2);
    c.put(0x5f14e, 180, 2);
    c.put(0xc2568 + 180 * 2, 73, 2);
    c.put(0xc0908 + 73 * 24, 45474);
    c.put(0x77e84, 123456);
    c.put(0x78048, 3481);
    c.put(0x78054, 49);
    c.put(0x7805a, 4, 2);
    c.put(0x77808, 1286, 2);
    c.put(0x7780a, 9999, 2);
    c.put(0x7780c, 99000);
    c.put(0x7789c, 15, 1);
    for (unsigned i = 0; i < 8; ++i)
        c.put(0x77898 + 0x98 * i, i + 1, 2);
    auto state = c.snapshot();
    LH_EQ(state["field"], json(R"({"id":180,"verified":true,"name":"bghoke_2","name_verified":true})"));
    LH_EQ(state["total_kills"], json(R"({"value":36,"verified":true})"));
    LH_EQ(state["battles_won"]["value"], Json(49));
    LH_EQ(state["battles_escaped"]["value"], Json(4));
    LH_EQ(state["gil"]["value"], Json(123456));
    LH_EQ(state["play_time_seconds"]["value"], Json(3481));
    const auto &squall = state["characters"][size_t(0)];
    LH_EQ(squall["level"], json(R"({"value":100,"verified":true})"));
    LH_EQ(squall["hp"], json(R"({"value":1286,"verified":true})"));
    LH_EQ(squall["max_hp"]["verified"], Json(false));
    LH_EQ(state["draw_count"], json(R"({"value":null,"verified":false})"));
}
LH_TEST(test_sse_coalesces_changed_fields_at_four_hz) {
    Bridge c(t);
    c.inherited();
    c.request("/events");
    c.frame(1);
    LH_CONTAINS(c.local.receive(), "event: state");
    for (int i = 1; i <= 3; ++i) {
        c.put(0x5f158, i, 2);
        c.frame(i + 1);
        LH_BYTES_EQ(c.local.receive(), "");
    }
    lh::sleep_ms(260);
    c.frame(5);
    auto message = c.local.receive();
    auto at = message.find("data: ");
    LH_CHECK_MSG(at != std::string::npos, message);
    auto state = json(message.substr(at + 6, message.find("\n\n", at) - at - 6));
    LH_EQ(state["vblank"], Json(5));
    LH_EQ(state["state"]["mode"]["value"], Json("battle"));
    c.frame(6);
    LH_BYTES_EQ(c.local.receive(), "");
}
LH_TEST(test_listener_reuses_port_before_bind) {
    Bridge c(t);
    c.test_network(1, 0);
    for (int i = 0; i < 2; ++i) {
        auto b = c.host_state_bridge_open("12345");
        LH_CHECK(b);
        LH_CHECK(c.test_reuse_before_bind());
        c.host_state_bridge_close(b);
    }
}
LH_TEST(test_inherited_client_survives_idle_and_owns_fd) {
    Bridge c(t);
    c.test_clock(100);
    c.inherited();
    struct stat st{};
    errno = 0;
    LH_EQ(fstat(c.original, &st), -1);
    LH_EQ(errno, EBADF);
    c.test_clock(120);
    c.frame(1);
    LH_CHECK(c.local.would_block());
    c.request();
    c.frame(2);
    LH_CONTAINS(c.local.receive(), "200 OK");
    c.host_state_bridge_close(c.b);
    c.b = nullptr;
    c.test_state_free(c.cpu);
    c.cpu = nullptr;
    LH_BYTES_EQ(c.local.read_one(), "");
}
LH_TEST(test_invalid_addresses_warn_once_after_activation) {
    Bridge c(t);
    auto patch = patch_file(t, R"([{"ID": 44, "MemAddr": "0xX300000=1"}])");
    lh::StderrCapture capture;
    c.open_achievements(patch, t.temp() / "progress.json");
    for (int i = 0; i < 600; ++i)
        c.achievement_frame();
    auto err = capture.take();
    std::string needle = "invalid address: id=44";
    size_t count = 0;
    for (size_t at = 0; (at = err.find(needle, at)) != std::string::npos; at += needle.size())
        ++count;
    LH_EQ(count, 1u);
    LH_EQ(c.test_events(), 0u);
}
LH_TEST(test_stalled_partial_writes_release_all_listener_slots) {
    Bridge c(t);
    c.test_network(1, 0);
    c.test_clock(100);
    c.test_send_mode(2);
    c.open("12345");
    std::vector<lh::Socket> peers;
    for (int i = 0; i < 16; ++i) {
        auto pair = lh::socket_pair();
        pair.first.nonblocking();
        c.test_client(pair.second.release());
        pair.first.send("GET /state HTTP/1.1\r\nHost: localhost\r\n\r\n");
        peers.push_back(std::move(pair.first));
    }
    c.frame(1);
    for (size_t i = 0; i < peers.size(); ++i) {
        auto part = peers[i].receive();
        if (!i)
            LH_EQ(part.size(), 7u);
        else
            LH_CHECK(part.empty());
    }
    for (const auto &peer : peers)
        LH_CHECK(peer.would_block());
    c.test_clock(106);
    c.frame(2);
    for (const auto &peer : peers) {
        peer.receive();
        LH_BYTES_EQ(peer.read_one(), "");
    }
    c.test_send_mode(0);
    auto pair = lh::socket_pair();
    pair.first.nonblocking();
    c.test_client(pair.second.release());
    peers.push_back(std::move(pair.first));
    peers.back().send("GET /state HTTP/1.1\r\nHost: 127.0.0.1:12345\r\n\r\n");
    c.frame(3);
    LH_CONTAINS(peers.back().receive(), "200 OK");
}
LH_TEST(test_pipelining_closes_with_an_explicit_error) {
    Bridge c(t);
    c.inherited();
    const std::string message = "GET /state HTTP/1.1\r\nHost: localhost\r\n\r\n";
    c.local.send(message + message);
    c.frame(1);
    auto response = c.local.receive();
    LH_CONTAINS(response, "400 Bad Request");
    LH_CONTAINS(response, "Connection: close");
    LH_BYTES_EQ(c.local.read_one(), "");
}
struct Host {
    const char *id, *value;
};
static const Host bad_hosts[] = {{"evil.example", "evil.example"},
                                 {"localhost.evil.example", "localhost.evil.example"},
                                 {"127.0.0.1:65536", "127.0.0.1:65536"},
                                 {"localhost@evil.example", "localhost@evil.example"},
                                 {"localhost:bad", "localhost:bad"},
                                 {"localhost\\r\\nHost: localhost", "localhost\r\nHost: localhost"}};
LH_TEST_P(test_host_header_rejects_rebinding_and_ambiguity, bad_hosts) {
    Bridge c(t);
    c.inherited();
    c.request("/state", p.value);
    c.frame(1);
    LH_CONTAINS(c.local.receive(), "403 Forbidden");
    LH_BYTES_EQ(c.local.read_one(), "");
}
static const char *const good_hosts[] = {"localhost", "LOCALHOST:8765", "127.0.0.1:12345"};
LH_TEST_P(test_local_host_headers_are_accepted, good_hosts) {
    Bridge c(t);
    c.inherited();
    c.request("/state", p);
    c.frame(1);
    LH_CONTAINS(c.local.receive(), "200 OK");
}
LH_TEST(test_socket_disconnect_preserves_sigpipe_disposition) {
    std::signal(SIGPIPE, SIG_DFL);
    Bridge c(t);
    c.inherited();
    LH_CHECK(c.test_sigpipe_is_default());
    c.request();
    c.local = lh::Socket();
    c.frame(1);
    LH_CHECK(c.test_sigpipe_is_default());
}
LH_TEST(test_progress_directory_sync_mode_cleanup_and_recovery) {
    Bridge c(t);
    auto patch =
             patch_file(t, R"([{"ID": 42, "MemAddr": "0xH000100=7"}, {"ID": 43, "MemAddr": "0xH000101=7"}])"),
         progress = t.temp() / "progress.json";
    auto stale = t.temp() / "progress.json.tmp.ABCDEF",
         dead = t.temp() / "progress.json.tmp.2147483647.ZYXWVU",
         fresh = t.temp() / "progress.json.tmp.GHIJKL", unrelated = t.temp() / "other.json.tmp.ABCDEF";
    auto active = t.temp() / ("progress.json.tmp." + std::to_string(getpid()) + ".MNOPQR"),
         link = t.temp() / "progress.json.tmp.STUVWX";
    for (const auto &p : {stale, dead, fresh, unrelated, active})
        lh::write_file(p, "temporary");
    lh::fs::create_symlink(unrelated, link);
    utimbuf times{std::time(nullptr) - 172800, std::time(nullptr) - 172800};
    for (const auto &p : {stale, unrelated, active})
        LH_EQ(utime(p.c_str(), &times), 0);
    c.test_operations_clear();
    c.open_achievements(patch, progress);
    LH_EQ(std::string(c.test_operations()), "FRD");
    LH_CHECK(!lh::fs::exists(stale));
    LH_CHECK(!lh::fs::exists(dead));
    LH_CHECK(lh::fs::exists(fresh));
    LH_CHECK(lh::fs::exists(unrelated));
    LH_CHECK(lh::fs::exists(active));
    LH_CHECK(lh::fs::is_symlink(link));
    c.close_achievements();
    LH_EQ(chmod(progress.c_str(), 0640), 0);
    c.open_achievements(patch, progress);
    struct stat st{};
    LH_EQ(stat(progress.c_str(), &st), 0);
    LH_EQ(st.st_mode & 0777, 0640u);
    c.achievement_frame();
    c.test_sync_fail(2, 1);
    c.put(0x100, 7, 1);
    c.achievement_frame();
    c.achievement_frame();
    LH_EQ(c.event()["persisted"], Json(false));
    c.put(0x101, 7, 1);
    c.achievement_frame();
    LH_EQ(c.event()["persisted"], Json(true));
    LH_EQ(json(lh::read_file(progress))["unlocked"], json("[42, 43]"));
    LH_EQ(stat(progress.c_str(), &st), 0);
    LH_EQ(st.st_mode & 0777, 0640u);
    LH_CHECK(c.close_achievements());
}
LH_TEST(test_progress_file_sync_failure_keeps_previous_file) {
    Bridge c(t);
    auto patch = patch_file(t, achievement42), progress = t.temp() / "progress.json";
    c.open_achievements(patch, progress);
    auto previous = lh::read_file(progress);
    c.achievement_frame();
    c.test_sync_fail(1, 1);
    c.put(0x100, 7, 1);
    c.achievement_frame();
    LH_BYTES_EQ(lh::read_file(progress), previous);
    LH_CHECK(lh::files_starting(t.temp(), "progress.json.tmp.").empty());
    LH_CHECK(!c.close_achievements());
}
LH_TEST(test_rich_presence_sse_coalesces_at_four_hz) {
    Bridge c(t);
    c.test_clock(100);
    c.inherited();
    c.request("/events");
    c.frame(1);
    c.local.receive();
    c.host_state_bridge_event(c.b, R"({"type":"rich_presence","text":"first"})");
    LH_CONTAINS(c.local.receive(), "\"first\"");
    for (int i = 1; i < 5; ++i) {
        c.test_clock(100 + i * .04);
        auto event = "{\"type\":\"rich_presence\",\"text\":\"" + std::to_string(i) + "\"}";
        c.host_state_bridge_event(c.b, event.c_str());
        LH_BYTES_EQ(c.local.receive(), "");
    }
    c.test_clock(100.26);
    c.frame(2);
    auto result = c.local.receive();
    LH_CONTAINS(result, "\"text\":\"4\"");
    LH_NOT_CONTAINS(result, "\"text\":\"1\"");
}
struct Presence {
    const char *id, *prefix, *unit;
    int count;
};
static const Presence presences[] = {
    {"text0", "", "🐱", 400}, {"text1", "x", "€", 400}, {"text2", "a", "é", 600}};
LH_TEST_P(test_rich_presence_truncation_keeps_valid_utf8, presences) {
    Bridge c(t);
    std::string text = p.prefix;
    for (int i = 0; i < p.count; ++i)
        text += p.unit;
    auto patch = patch_file(t, "[]", ", \"RichPresencePatch\": " + Json::quote("Display:\n" + text + "\n"));
    c.open_achievements(patch, t.temp() / "progress.json");
    c.achievement_frame();
    auto value = c.event()["text"].text();
    LH_CHECK(text.rfind(value, 0) == 0);
    LH_CHECK(value.size() <= 1023);
    LH_CHECK(!value.empty());
}
LH_GAME_TEST(test_bridge_bind_failure_does_not_stop_game) {
    auto binary = t.root() / "build/dev/ff8";
    if (!lh::fs::exists(binary))
        lh::skip("opt-in real host/executable not built");
    auto exe = lh::owned_path("SLUS_008.92");
    if (!lh::fs::exists(exe))
        lh::skip("opt-in real host/executable not built");
    auto shim = lh::fs::path(t.harness("bind_failure").program());
    auto result = t.run(lh::dummy(t.target(binary))
                            .args({exe, "--config", "/dev/null", "--headless", "--mute", "--fast",
                                   "--state-bridge", "12345", "--max-vblanks", "2"})
                            .env("LD_PRELOAD", shim.string())
                            .timeout(30));
    LH_EQ(result.rc, 0);
    LH_CONTAINS(result.out, "VBlank limit reached");
    LH_CONTAINS(result.err, "Warning: state bridge disabled: Address already in use");
}
LH_TEST(test_title_hides_stale_savemap_and_levels_ignore_unknown_flags) {
    Bridge c(t);
    c.put(0x7737a, 0x08ff, 2);
    c.put(0x781a4, 1, 1);
    c.put(0x5f158, 1, 2);
    c.put(0x7780c, 99000, 4);
    for (unsigned flag : {0, 1, 15, 254}) {
        c.put(0x7789c, flag, 1);
        auto state = c.snapshot();
        LH_EQ(state["available"], Json(true));
        LH_EQ(state["characters"][size_t(0)]["level"], json(R"({"value":100,"verified":true})"));
        LH_EQ(state["disc"]["verified"], Json(false));
    }
    c.put(0x5f158, 8, 2);
    auto state = c.snapshot();
    LH_EQ(state["available"], Json(false));
    LH_CHECK(!state.contains("gil"));
    LH_CHECK(!state.contains("characters"));
}
struct Field {
    const char *id;
    unsigned field, local, lba;
    const char *name;
};
static const Field fields[] = {{"315-136-55638-dogate_1", 315, 136, 55638, "dogate_1"},
                               {"733-215-69654-glgateb1", 733, 215, 69654, "glgateb1"}};
LH_TEST_P(test_additional_replay_field_ids_and_resource_names, fields) {
    Bridge c(t);
    c.put(0x7737a, 0x08ff, 2);
    c.put(0x781a4, 1, 1);
    c.put(0x5f158, 1, 2);
    c.put(0x5f14e, p.field, 2);
    c.put(0xc2568 + 2 * p.field, p.local, 2);
    c.put(0xc0908 + 24 * p.local, p.lba, 4);
    LH_EQ(c.snapshot()["field"],
          Json(Json::Object{
              {"id", Json(int(p.field))}, {"verified", true}, {"name", p.name}, {"name_verified", true}}));
}
LH_TEST(test_tcp_restart_after_server_closed_state_reply) {
    Bridge c(t);
    auto port = c.tcp();
    {
        auto peer = lh::http_get(port, "/state");
        c.frame(1);
        LH_CONTAINS(peer.receive(1000), "200 OK");
        LH_BYTES_EQ(peer.read_one(), "");
    }
    c.host_state_bridge_close(c.b);
    c.b = nullptr;
    c.open(std::to_string(port));
}
LH_TEST(test_restore_publishes_same_fields_at_earlier_vblank) {
    Bridge c(t);
    c.inherited();
    c.request("/events");
    c.frame(500);
    c.local.receive();
    c.host_state_bridge_restore(c.b, c.cpu, 100);
    LH_CONTAINS(c.local.receive(), "\"vblank\":100");
}
LH_TEST(test_unlock_survives_replacing_guest_session) {
    Bridge c(t);
    auto patch = patch_file(t, achievement42), progress = t.temp() / "progress.json";
    c.open_achievements(patch, progress);
    c.achievement_frame();
    c.put(0x100, 7, 1);
    c.achievement_frame();
    LH_EQ(c.test_events(), 1u);
    c.test_state_free(c.cpu);
    c.cpu = c.test_state_cpu();
    LH_CHECK(c.cpu);
    c.achievement_frame();
    c.put(0x100, 7, 1);
    c.achievement_frame();
    LH_EQ(c.test_events(), 1u);
    LH_EQ(json(lh::read_file(progress))["unlocked"], json("[42]"));
}
