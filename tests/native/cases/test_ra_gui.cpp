// Render actual sign-in and achievements interactions against the loopback
// mock.
#include "host_fixtures.h"
#include "ra_service.h"
#include "synthetic_disc.h"
#include <cmath>
#include <sys/stat.h>

namespace {
std::string click(int x, int y, double scale = 1) {
    return "mouse " + std::to_string(int(std::nearbyint(x * scale))) + " " +
           std::to_string(int(std::nearbyint(y * scale))) + "\nmouse-down\nmouse-up\nwait 2\n";
}
std::string signin(double scale = 1, const std::string &password = "test-password") {
    return "open\nwait 3\nsection prev\nwait 3\nassert-text ra.off "
           "\"Off\"\ncapture {shots}-off.png\n"
           "select\nwait 2\ncapture {shots}-warning.png\nnav right\nselect\nwait "
           "3\n" +
           click(420, 245, scale) + "text \"TestUser\"\n" + click(420, 340, scale) + "text \"" + password +
           "\"\nwait 3\ncapture {shots}-form.png\n" + click(420, 454, scale) + "wait 90\n";
}
struct Output {
    lh::fs::path shots, data;
};
Output run_gui(lh::Case &t, const lh::fs::path &tmp, lh::RaService &service, std::string steps,
               const std::string &name, const std::string &renderer = "soft", int width = 1280,
               int height = 720, bool token = false, int unlock = 0) {
    const char *supplied = getenv("LIONHEART_RA_GUI_HOST");
    auto binary = supplied ? lh::fs::absolute(supplied)
                           : lh::fs::path(t.player().program()).parent_path() / "ra_gui_contract";
    LH_CHECK_MSG(lh::fs::exists(binary), "build the top-level ra_gui_contract "
                                         "target and set LIONHEART_RA_GUI_HOST");
    const char *shots = getenv("LIONHEART_RA_GUI_SHOTS");
    auto out = lh::fs::absolute(shots ? lh::fs::path(shots) : tmp / "shots");
    lh::fs::create_directories(out);
    auto data = tmp / "data";
    if (token) {
        auto credentials = data / "ra/credentials";
        lh::fs::create_directories(credentials.parent_path());
        lh::write_file(credentials, "ra-user=TestUser\nra-token=test-token\n");
        LH_EQ(chmod(credentials.c_str(), 0600), 0);
    }
    for (size_t at = 0; (at = steps.find("{shots}", at)) != std::string::npos;) {
        auto replacement = (out / name).string();
        steps.replace(at, 7, replacement);
        at += replacement.size();
    }
    auto script = tmp / "gui.txt";
    lh::write_file(script, steps);
    auto image = synthetic::disc(tmp / "synthetic.bin");
    auto cmd = lh::dummy(t.target(binary))
                   .args({"--gui-test", script, data, image, renderer,
                          std::to_string(width) + "x" + std::to_string(height), unlock})
                   .env("LIONHEART_RA_TEST_HOST", service.url())
                   .env("LIONHEART_RA_TEST_TIMEOUT_MS", "500")
                   .env("LIONHEART_RA_GUI_ENABLED", token ? "1" : "0")
                   .timeout(90);
    if (renderer == "vulkan") {
        const lh::fs::path icd("/usr/share/vulkan/icd.d/lvp_icd.json");
        if (!lh::fs::exists(icd))
            lh::skip("lavapipe is required for headless Vulkan GUI capture");
        cmd.env("VK_DRIVER_FILES", icd.string());
    }
    auto result = t.run(cmd);
    lh::write_file(out / (name + ".log"), result.out + result.err);
    LH_CHECK_MSG(result.rc == 0, result.out + result.err);
    LH_CONTAINS(result.err, "GUI test complete:");
    LH_NOT_CONTAINS(result.err, "falling back");
    LH_NOT_CONTAINS(result.err, "using native soft");
    return {out, data};
}
struct View {
    const char *id, *renderer;
    int width, height;
};
const View views[] = {{"soft-size0", "soft", 1280, 720},
                      {"soft-size1", "soft", 1920, 1080},
                      {"vulkan-size2", "vulkan", 1280, 720}};
} // namespace
LH_TEST_P(test_signin_view_search_filters_unlock_and_signout, views) {
    double scale = p.height / 720.;
    lh::RaService service;
    auto steps = signin(scale) +
                 "assert-text ra.signed_in \"Signed in as TestUser\"\ncapture "
                 "{shots}-signed-in.png\nselect\nwait 25\n"
                 "close\nat 10\nopen achievements\nwait 15\nassert-text ach.stats \"0 / 3 "
                 "unlocked · 0 / 17 points\"\n"
                 "assert-text ach.102.status \"5 / 10\"\ncapture {shots}-view.png\nnav "
                 "right\nwait 2\n"
                 "assert-text ach.filters \"Unlocked 0\"\nnav left\n" +
                 click(920, 100, scale) +
                 "text \"Measured\"\nwait 3\n"
                 "assert-text ach.102.title \"Measured signal\"\ncapture "
                 "{shots}-search.png\nclose\nat 55\ncapture {shots}-toast.png\nopen "
                 "achievements\nwait 15\n"
                 "assert-text ach.stats \"1 / 3 unlocked · 5 / 17 points\"\ncapture "
                 "{shots}-unlocked.png\nsection prev\nsection prev\nwait 3\n" +
                 click(420, 515, scale) +
                 "wait 3\nassert-text ra.off \"Off\"\ncapture "
                 "{shots}-signed-out.png\nquit\n";
    auto name = std::string(p.renderer) + "-" + std::to_string(p.height);
    auto out = run_gui(t, t.temp(), service, steps, name, p.renderer, p.width, p.height, false, 40);
    LH_CHECK(!lh::fs::exists(out.data / "ra/credentials"));
    bool award = false;
    for (const auto &r : service.requests()) {
        award |= r.params.at("r") == "awardachievement";
        LH_EQ(r.params.count("h") ? r.params.at("h") : "0", "0");
    }
    LH_CHECK(award);
    bool png = false;
    for (const auto &file : lh::fs::directory_iterator(out.shots))
        png |= file.path().filename().string().rfind(name + "-", 0) == 0 && file.path().extension() == ".png";
    LH_CHECK(png);
}
LH_TEST(test_offline_cached_view_and_pending_unlock) {
    lh::RaService s;
    s.configure([](auto &c) { c.award_mode = "offline"; });
    run_gui(t, t.temp(), s,
            "open achievements\nwait 90\nclose\nat 55\nopen achievements\nwait 20\n"
            "assert-text ach.stats \"1 / 3 unlocked · 5 / 17 points\"\nassert-text "
            "ach.pending \"· not sent yet\"\ncapture {shots}-view.png\n"
            "section prev\nsection prev\nwait 3\nassert-text ra.offline \"Offline, "
            "retrying\"\ncapture {shots}-pane.png\nquit\n",
            "offline", "soft", 1280, 720, true, 40);
}
static const char *const modes[] = {"blocked", "expired", "offline"};
LH_TEST_P(test_stored_token_failure_panes, modes) {
    lh::RaService s;
    s.configure([&](auto &c) { c.mode = p; });
    std::string state = std::string(p) == "offline" ? "ra.offline \"Offline, retrying\""
                                                    : "ra.rejected \"Signed out by retroachievements.org\"";
    run_gui(t, t.temp(), s,
            "open\nsection prev\nwait 90\nassert-text " + state + "\ncapture {shots}-pane.png\nquit\n", p,
            "soft", 1280, 720, true);
}
LH_TEST(test_wrong_password_and_load_failure) {
    {
        lh::RaService s;
        run_gui(t, t.temp(), s,
                signin(1, "wrong") + "assert-text ra.err.credentials \"Wrong username or "
                                     "password.\"\ncapture {shots}-pane.png\nquit\n",
                "bad-password");
    }
    auto retry = t.temp() / "retry";
    lh::fs::create_directory(retry);
    lh::RaService s;
    s.configure([](auto &c) { c.set_failure = true; });
    run_gui(t, retry, s,
            "open achievements\nwait 90\nassert-text ach.load.title \"Achievements "
            "could not be loaded\"\ncapture {shots}-dialog.png\nquit\n",
            "load-failure", "soft", 1280, 720, true);
}
