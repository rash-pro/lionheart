// Service-client contracts. Every request is confined to a loopback mock.
#include "lh_test.h"
#include "ra_service.h"
#include "synthetic_disc.h"
#include <cmath>
#include <set>
#include <sys/stat.h>

namespace {
constexpr int OFF = 0, SIGNED_OUT = 1, SIGNING_IN = 2, CONNECTED = 3, OFFLINE = 4, REJECTED = 5,
              UNAVAILABLE = 6;
struct Client {
    lh::RaService service;
    lh::Env host, timeout;
    lh::Library lib;
    lh::fs::path data;
    explicit Client(lh::Case &t)
        : host("LIONHEART_RA_TEST_HOST", service.url()), timeout("LIONHEART_RA_TEST_TIMEOUT_MS", "200"),
          lib(t.library("client")), data(t.temp() / "data") {
        reset();
        configure(0);
    }
    ~Client() { reset(); }
    void reset() { lib.fn<void()>("ra_test_reset")(); }
    void configure(int enabled = 1) {
        lib.fn<void(const char *, int, int)>("host_ra_configure")(data.c_str(), enabled, 1);
    }
    void idle() { lib.fn<void()>("host_ra_idle")(); }
    int state() { return lib.fn<int()>("ra_test_state")(); }
    std::string error() { return lib.fn<const char *()>("ra_test_error")(); }
    unsigned total() { return lib.fn<unsigned()>("ra_test_total")(); }
    unsigned unlocked() { return lib.fn<unsigned()>("ra_test_unlocked")(); }
    int value(const char *name) { return lib.fn<int()>(name)(); }
    int indexed(const char *name, unsigned i) { return lib.fn<int(unsigned)>(name)(i); }
    void call(const char *name) { lib.fn<void()>(name)(); }
    void put(const char *name, unsigned v) { lib.fn<void(unsigned)>(name)(v); }
    void frame(unsigned v) { put("ra_test_frame", v); }
    void advance(unsigned v) { put("ra_test_advance", v); }
    std::string hash() { return lib.fn<const char *()>("ra_test_hash")(); }
    float progress() { return lib.fn<float(unsigned)>("ra_test_progress")(1); }
    std::string progress_text() { return lib.fn<const char *(unsigned)>("ra_test_progress_text")(1); }
    double toast(double now) { return lib.fn<double(double)>("ra_test_toast")(now); }
    lh::fs::path token() const { return data / "ra/credentials"; }
    template <class F> void until(F predicate) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < end) {
            idle();
            if (predicate())
                return;
            lh::sleep_ms(5);
        }
        LH_FAIL("client did not settle: state=" + std::to_string(state()) + ", error=" + error());
    }
    void login(const char *password = "test-password") {
        lib.fn<void(int)>("host_ra_enable")(1);
        lib.fn<void(const char *, const char *)>("host_ra_login")("TestUser", password);
        until([&] { return state() != SIGNING_IN; });
    }
    void disc(const lh::fs::path &p) { lib.fn<void(const char *)>("host_ra_disc")(p.c_str()); }
    lh::fs::path load(lh::Case &t) {
        login();
        LH_EQ(state(), CONNECTED);
        frame(0);
        auto path = synthetic::disc(t.temp() / "synthetic.bin");
        disc(path);
        until([&] {
            return total() == 3 || value("ra_test_failed") || value("ra_test_unknown");
        });
        return path;
    }
    bool requested(const std::string &op) {
        for (const auto &r : service.requests())
            if (r.params.at("r") == op)
                return true;
        return false;
    }
};
unsigned mode(const lh::fs::path &p) {
    struct stat st{};
    LH_EQ(stat(p.c_str(), &st), 0);
    return st.st_mode & 0777;
}
} // namespace

LH_TEST(test_opt_in_login_token_permissions_and_signout) {
    Client c(t);
    c.idle();
    c.lib.fn<void(const char *, const char *)>("host_ra_login")("TestUser", "test-password");
    LH_CHECK(c.service.requests().empty());
    LH_CHECK(!lh::fs::exists(c.data));
    c.login();
    LH_EQ(c.state(), CONNECTED);
    LH_TEXT_EQ(lh::read_file(c.token()), "ra-user=TestUser\nra-token=test-token\n");
    LH_EQ(mode(c.token()), 0600u);
    for (const auto &p : lh::fs::recursive_directory_iterator(c.data))
        if (p.is_regular_file())
            LH_NOT_CONTAINS(lh::read_file(p.path()), "test-password");
    c.configure();
    c.until([&] { return c.state() == CONNECTED; });
    LH_EQ(c.service.requests().back().params.at("t"), "test-token");
    c.call("host_ra_signout");
    LH_EQ(c.state(), OFF);
    LH_CHECK(!lh::fs::exists(c.token()));
    auto count = c.service.requests().size();
    c.advance(120000);
    c.idle();
    LH_EQ(c.service.requests().size(), count);
    for (const auto &r : c.service.requests()) {
        LH_CHECK(r.agent.rfind("Lionheart/", 0) == 0);
        LH_CONTAINS(r.agent, "(Linux)");
        LH_CONTAINS(r.agent, "rcheevos/");
    }
}
LH_TEST(test_wrong_password) {
    Client c(t);
    c.login("wrong");
    LH_EQ(c.state(), SIGNED_OUT);
    LH_EQ(c.error(), "ra.err.credentials");
    LH_CHECK(!lh::fs::exists(c.token()));
}
LH_TEST(test_new_login_replaces_saved_token) {
    Client c(t);
    c.login();
    c.service.configure([](auto &s) { s.token = "renewed-test-token"; });
    c.login();
    LH_TEXT_EQ(lh::read_file(c.token()), "ra-user=TestUser\nra-token=renewed-test-token\n");
    LH_EQ(mode(c.token()), 0600u);
    std::vector<lh::fs::path> files;
    for (const auto &p : lh::fs::directory_iterator(c.token().parent_path()))
        files.push_back(p.path());
    LH_EQ(files.size(), 1u);
    LH_EQ(files[0], c.token());
    c.configure();
    c.until([&] { return c.state() == CONNECTED; });
    LH_EQ(c.service.requests().back().params.at("t"), "renewed-test-token");
}
static const char *const rejected_modes[] = {"expired", "blocked", "blocked-json"};
LH_TEST_P(test_rejected_token_clears_credentials_and_stops, rejected_modes) {
    Client c(t);
    c.login();
    c.service.configure([&](auto &s) { s.mode = p; });
    c.configure();
    c.until([&] { return c.state() == REJECTED; });
    LH_CHECK(!lh::fs::exists(c.token()));
    auto count = c.service.requests().size();
    c.service.configure([](auto &s) { s.mode = "ok"; });
    c.advance(120000);
    for (int i = 0; i < 10; ++i)
        c.idle();
    LH_EQ(c.service.requests().size(), count);
    c.login();
    LH_EQ(c.state(), CONNECTED);
}
struct Timeout {
    const char *id;
    bool enabled;
};
static const Timeout timeouts[] = {{"False", false}, {"True", true}};
LH_TEST_P(test_offline_and_timeout_login, timeouts) {
    Client c(t);
    c.service.configure([&](auto &s) {
        s.delay_ms = p.enabled ? 400 : 0;
        s.mode = p.enabled ? "ok" : "offline";
    });
    c.login();
    LH_EQ(c.state(), OFFLINE);
    LH_CHECK(!lh::fs::exists(c.token()));
    c.service.configure([](auto &s) {
        s.delay_ms = 0;
        s.mode = "ok";
    });
    c.login();
    LH_EQ(c.state(), CONNECTED);
}
LH_TEST(test_token_offline_retries_without_password) {
    Client c(t);
    c.login();
    c.service.configure([](auto &s) { s.mode = "offline"; });
    c.configure();
    c.until([&] { return c.state() == OFFLINE; });
    LH_CHECK(lh::fs::exists(c.token()));
    c.service.configure([](auto &s) { s.mode = "ok"; });
    c.advance(60001);
    c.until([&] { return c.state() == CONNECTED; });
    LH_EQ(c.service.requests().back().params.at("t"), "test-token");
}
LH_TEST(test_unknown_game_and_set_retry) {
    Client c(t);
    c.service.configure([](auto &s) { s.set_failure = true; });
    c.load(t);
    LH_CHECK(c.value("ra_test_failed"));
    c.service.configure([](auto &s) {
        s.set_failure = false;
        s.unknown = true;
    });
    c.call("host_ra_retry");
    c.until([&] { return c.value("ra_test_unknown"); });
    LH_EQ(c.total(), 0u);
    c.service.configure([](auto &s) { s.unknown = false; });
    c.call("host_ra_retry");
    c.until([&] { return c.total() == 3; });
}
LH_TEST(test_casual_unlock_pending_retry_badges_progress_toast) {
    Client c(t);
    c.load(t);
    c.frame(0);
    LH_EQ(c.progress_text(), "0 / 10");
    c.frame(5);
    LH_EQ(c.progress(), 50);
    LH_EQ(c.progress_text(), "5 / 10");
    c.until([&] { return c.indexed("ra_test_badge", 0); });
    LH_BYTES_EQ(lh::read_file(c.data / "ra/badges/101.png"), lh::badge_png());
    c.service.configure([](auto &s) { s.mode = "offline"; });
    c.frame(7);
    c.until([&] { return c.state() == OFFLINE; });
    LH_EQ(c.unlocked(), 1u);
    LH_CHECK(c.indexed("ra_test_pending", 0));
    LH_EQ(c.toast(0), 0);
    LH_CHECK(std::abs(c.toast(60) - .5) <= .5e-6);
    LH_EQ(c.toast(120), 1);
    LH_EQ(c.toast(1920), 1);
    LH_CHECK(std::abs(c.toast(2070) - .5) <= .5e-6);
    LH_EQ(c.toast(2220), -1);
    c.service.configure([](auto &s) { s.mode = "ok"; });
    c.advance(120000);
    c.until([&] { return !c.service.awards().empty(); });
    c.until([&] { return c.state() == CONNECTED; });
    LH_CHECK(!c.indexed("ra_test_pending", 0));
    c.call("host_ra_reset");
    c.frame(0);
    c.frame(7);
    LH_EQ(c.service.awards().size(), 1u);
    for (const auto &r : c.service.requests()) {
        LH_EQ(r.params.count("h") ? r.params.at("h") : "0", "0");
        LH_CHECK(r.params.at("r") != "submitlbentry");
    }
}
LH_TEST(test_badge_cache_hit_and_corruption) {
    Client c(t);
    auto path = c.load(t);
    c.until([&] { return c.indexed("ra_test_badge", 0); });
    auto count = c.service.badge_requests();
    c.configure();
    c.disc(path);
    c.until([&] { return c.total() == 3; });
    c.until([&] { return c.indexed("ra_test_badge", 0); });
    LH_EQ(c.service.badge_requests(), count);
    lh::write_file(c.data / "ra/badges/101.png", "broken");
    c.configure();
    c.disc(path);
    c.until([&] { return c.total() == 3; });
    c.until([&] { return c.indexed("ra_test_badge", 0); });
    LH_EQ(c.service.badge_requests(), count + 1);
}
LH_TEST(test_logout_during_login_does_not_restore_token) {
    Client c(t);
    c.service.configure([](auto &s) { s.delay_ms = 300; });
    c.lib.fn<void(int)>("host_ra_enable")(1);
    c.lib.fn<void(const char *, const char *)>("host_ra_login")("TestUser", "test-password");
    c.until([&] { return !c.service.requests().empty(); });
    c.call("host_ra_signout");
    lh::sleep_ms(350);
    c.idle();
    LH_EQ(c.state(), OFF);
    LH_CHECK(!lh::fs::exists(c.token()));
}
LH_TEST(test_diagnostic_override_fails_closed) {
    lh::Env host("LIONHEART_RA_TEST_HOST", "https://example.invalid");
    auto lib = t.library("client");
    lib.fn<void(const char *, int, int)>("host_ra_configure")(t.temp().c_str(), 1, 1);
    LH_EQ(lib.fn<int()>("ra_test_state")(), UNAVAILABLE);
    lib.fn<void()>("ra_test_reset")();
}
LH_TEST(test_png_validation) {
    auto lib = t.library("client");
    auto check = lib.fn<int(const void *, size_t)>("ra_test_png");
    const auto data = lh::badge_png();
    LH_CHECK(check(data.data(), data.size()));
    for (const auto &broken : {std::string(), data.substr(0, 30), data.substr(0, data.size() - 12),
                               data.substr(0, 40) + "X" + data.substr(41)})
        LH_CHECK(!check(broken.data(), broken.size()));
}
LH_TEST(test_insecure_or_symlinked_credentials_are_not_used) {
    Client c(t);
    c.login();
    LH_EQ(chmod(c.token().c_str(), 0644), 0);
    auto count = c.service.requests().size();
    c.configure();
    c.idle();
    LH_EQ(c.state(), SIGNED_OUT);
    LH_EQ(c.service.requests().size(), count);
    auto external = t.temp() / "external";
    lh::fs::rename(c.token(), external);
    LH_EQ(chmod(external.c_str(), 0600), 0);
    lh::fs::create_symlink(external, c.token());
    c.configure();
    LH_EQ(c.state(), SIGNED_OUT);
    LH_EQ(c.service.requests().size(), count);
    c.call("host_ra_signout");
    LH_CHECK(!lh::fs::is_symlink(c.token()));
    LH_CHECK(lh::fs::exists(external));
}
LH_TEST(test_signout_with_an_award_in_flight_and_notification_off) {
    Client c(t);
    c.load(t);
    c.lib.fn<void(int)>("host_ra_notify")(0);
    c.frame(0);
    c.service.configure([](auto &s) { s.delay_ms = 300; });
    c.frame(7);
    c.until([&] { return c.requested("awardachievement"); });
    LH_EQ(c.toast(120), -1);
    c.call("host_ra_signout");
    lh::sleep_ms(350);
    c.idle();
    LH_EQ(c.state(), OFF);
    LH_CHECK(!lh::fs::exists(c.token()));
}
static const char *const award_modes[] = {"blocked", "blocked-json", "expired"};
LH_TEST_P(test_award_rejection_is_terminal, award_modes) {
    Client c(t);
    c.load(t);
    c.frame(0);
    c.service.configure([&](auto &s) { s.mode = p; });
    c.frame(7);
    c.until([&] { return c.state() == REJECTED; });
    auto count = c.service.requests().size();
    c.service.configure([](auto &s) { s.mode = "ok"; });
    c.advance(120000);
    for (int i = 0; i < 5; ++i) {
        c.frame(0);
        c.frame(7);
    }
    LH_EQ(c.service.requests().size(), count);
    LH_CHECK(!lh::fs::exists(c.token()));
}
LH_TEST(test_disc_change_hashes_each_loaded_disc) {
    Client c(t);
    c.load(t);
    std::vector<std::string> hashes{c.hash()};
    for (unsigned n : {2, 3, 4}) {
        auto path = synthetic::disc(t.temp() / ("synthetic-" + std::to_string(n) + ".bin"), synthetic::Disc{int(n), 0, {}, {}, true});
        c.disc(path);
        c.until([&] { return c.hash() != hashes.back(); });
        hashes.push_back(c.hash());
        c.until([&] {
            for (const auto &r : c.service.requests())
                if (r.params.at("r") == "gameid" && r.params.count("m") && r.params.at("m") == hashes.back())
                    return true;
            return false;
        });
    }
    LH_EQ(std::set<std::string>(hashes.begin(), hashes.end()).size(), 4u);
}
LH_TEST(test_idle_does_not_evaluate_changed_memory) {
    Client c(t);
    c.load(t);
    c.frame(0);
    c.put("ra_test_memory", 7);
    for (int i = 0; i < 30; ++i)
        c.idle();
    LH_EQ(c.unlocked(), 0u);
    LH_EQ(c.progress(), 0);
    c.frame(7);
    LH_EQ(c.unlocked(), 1u);
}
LH_TEST(test_pending_unlock_survives_disc_change) {
    Client c(t);
    c.load(t);
    c.service.configure([](auto &s) { s.award_mode = "offline"; });
    c.frame(0);
    c.frame(7);
    c.until([&] { return c.state() == OFFLINE; });
    LH_CHECK(c.indexed("ra_test_pending", 0));
    c.disc(synthetic::disc(t.temp() / "disc-two.bin", synthetic::Disc{2, 0, {}, {}, true}));
    c.until([&] { return !c.value("ra_test_loading"); });
    LH_EQ(c.unlocked(), 1u);
    LH_CHECK(c.indexed("ra_test_pending_id", 101));
    c.service.configure([](auto &s) { s.award_mode = "ok"; });
    c.advance(120000);
    c.until([&] { return !c.service.awards().empty() && !c.indexed("ra_test_pending_id", 101); });
}
LH_TEST(test_reset_clears_measured_display_and_toast_handles_clock_restart) {
    Client c(t);
    c.load(t);
    c.frame(0);
    c.frame(5);
    LH_EQ(c.progress(), 50);
    c.call("host_ra_reset");
    LH_EQ(c.progress_text(), "0 / 10");
    LH_EQ(c.progress(), 0);
    c.frame(0);
    c.frame(7);
    LH_EQ(c.toast(5000), 0);
    LH_EQ(c.toast(5120), 1);
    LH_EQ(c.toast(0), 0);
    LH_EQ(c.toast(120), 1);
    LH_EQ(c.toast(2220), -1);
}
LH_TEST(test_worker_start_failure_can_sign_out_and_reconfigure) {
    Client c(t);
    c.call("ra_test_fail_worker");
    c.configure();
    LH_EQ(c.state(), UNAVAILABLE);
    c.call("host_ra_signout");
    c.call("host_ra_shutdown");
    LH_CHECK(c.service.requests().empty());
    c.configure();
    c.login();
    LH_EQ(c.state(), CONNECTED);
}
