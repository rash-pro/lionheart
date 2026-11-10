// Controller chords on the guest wire and asynchronous presented screenshots.
#include "presentation.h"
#include "host_fixtures.h"
#include <set>
namespace {
static lh::Command environment(lh::Command command) {
    return lh::dummy(command).env("SDL_RENDER_DRIVER","software").env("TZ","UTC");
}
static std::vector<lh::fs::path> pngs(const lh::fs::path &folder) {
    auto paths=lh::files_starting(folder,"");
    paths.erase(std::remove_if(paths.begin(),paths.end(),[](const auto &path){return path.extension()!=".png";}),paths.end());return paths;
}
static auto png(const lh::fs::path &path) {return lh::oracle::read_png(lh::read_file(path),true);}
LH_TEST(test_controller_hotkeys_guest_wire) {
    auto run=t.ok(environment(t.harness("hotkeys").arg(t.temp()/"shots")).timeout(60));
    LH_CONTAINS(run.err,"Disc 2 missing; skipping empty slot");LH_CONTAINS(run.err,"Disc 3 missing; skipping empty slot");
    LH_EQ(pngs(t.temp()/"shots").size(),size_t(2));
}
struct Capture {std::string id,backend;int preset,width,height,mode,linear;};
static std::vector<Capture> captures() {
    std::vector<Capture> result;
    for(int linear:{0,1}) for(int mode:{0,2}) for(int size:{0,1}) for(auto backend:{"soft","vulkan"}) for(int preset:{0,2})
        result.push_back({std::to_string(linear)+"-"+std::to_string(mode)+"-size"+std::to_string(size)+"-"+backend+"-"+std::to_string(preset),
            backend,preset,size?3840:1920,size?2160:1080,mode,linear});
    return result;
}
LH_TEST_P(test_screenshot_is_presented_crop_without_osd,captures()) {
    auto command=t.harness("soft_capture");
    if(p.backend=="vulkan") {lh::crt_preview(t);command=t.host(lh::host_contract(t,"screenshot_capture",false).string());}
    t.ok(environment(lh::Command(command).args({p.backend,p.width,p.height,p.mode,p.preset,p.linear,t.temp(),"0"})).timeout(90));
    auto baseline=t.temp()/"no-osd";
    t.ok(environment(lh::Command(command).args({p.backend,p.width,p.height,p.mode,p.preset,p.linear,baseline,"2"})).timeout(90));
    const auto reference=png(baseline/"presented/frame-000001.png"),with_osd=png(t.temp()/"presented/frame-000001.png");
    LH_CHECK(reference.width!=with_osd.width || reference.height!=with_osd.height || reference.rgb!=with_osd.rgb);
    const lh::Timing timing{lh::ntsc.horizontal,16u|(240u<<10),lh::ntsc.mode};
    const auto view=lh::viewport(t,p.width,p.height,timing,320,224,p.mode);
    const auto files=pngs(t.temp()/"shots");LH_EQ(files.size(),size_t(2));
    std::set<std::string> names;for(const auto &file:files) names.insert(file.filename().string());
    LH_CHECK((names==std::set<std::string>{"ff8-20231114-221320.png","ff8-20231114-221320-2.png"}));
    const auto expected=lh::oracle::crop(reference,view.frame);
    for(const auto &file:files) {auto actual=png(file);LH_EQ(actual.width,expected.width);LH_EQ(actual.height,expected.height);LH_BYTES_EQ(actual.rgb,expected.rgb);}
}
LH_TEST(test_unwritable_directory_reports_failure) {
    t.ok(environment(t.harness("soft_capture").args({"soft","1920","1080","0","0","0",t.temp(),"1"})).timeout(30));
    LH_CHECK(!lh::fs::exists(t.temp()/"shots"));
}
static lh::Command options(lh::Case &t) {
    return environment(t.harness("options").args({"unused","--config","/dev/null"})).env("FF8_TEST_HOTKEYS","1");
}
static void ends(const std::string &text,const std::string &suffix) {LH_CHECK_MSG(text.size()>=suffix.size() && text.compare(text.size()-suffix.size(),suffix.size(),suffix)==0,text);}
LH_TEST(test_hotkey_options_and_classification) {
    auto result=t.ok(options(t));ends(result.out,"hotkey=0 directory= frames=");
    for(auto key:{"pad-hotkey","screenshot-dir","screenshot-at"}) LH_CONTAINS(result.out,std::string(key)+" P\n");
    ends(t.run(options(t).args({"--pad-hotkey","off","--screenshot-dir","/tmp/screens","--screenshot-at","0","--screenshot-at","20"})).out,"hotkey=1 directory=/tmp/screens frames=0\n20\n");
    for(auto args:{std::vector<std::string>{"--pad-hotkey","home"},{"--screenshot-at","-1"},{"--screenshot-at","1x"},
        {"--screenshot-at","18446744073709551616"},{"--screenshot-at"},{"--pad-hotkey"},{"--screenshot-dir"}}) {
        auto command=options(t);for(auto &arg:args) command.arg(arg);LH_EQ(t.run(command).rc,2);
    }
    auto ini=t.temp()/"ff8.ini";lh::write_file(ini,"pad-hotkey=off\nscreenshot-dir=/ini\nscreenshot-at=3\n");
    ends(t.run(options(t).args({"--config",ini,"--pad-hotkey","guide"})).out,"hotkey=0 directory=/ini frames=3\n");
}
static const char *sources[]={"fallback","env","config","home-config","escaped","malicious"};
LH_TEST_P(test_xdg_resolution,sources) {
    auto home=t.temp()/"home",config=t.temp()/"config";lh::fs::create_directory(home);lh::fs::create_directory(config);
    setenv("HOME",home.c_str(),1);setenv("XDG_CONFIG_HOME",config.c_str(),1);unsetenv("XDG_PICTURES_DIR");
    auto expected=home/"Pictures/FF8";const std::string source=p;
    if(source=="env") {auto photos=t.temp()/"photos";setenv("XDG_PICTURES_DIR",photos.c_str(),1);expected=photos/"FF8";}
    else if(source=="home-config") {unsetenv("XDG_CONFIG_HOME");config=home/".config";lh::fs::create_directory(config);}
    if(source=="config" || source=="home-config" || source=="escaped") {
        const std::string value=source=="escaped"?"$HOME/My \\\"Photos\\\"":"$HOME/My Photos";
        lh::write_file(config/"user-dirs.dirs","XDG_PICTURES_DIR=\""+value+"\"\n");
        expected=home/(source=="escaped"?"My \"Photos\"/FF8":"My Photos/FF8");
    }
    if(source=="malicious") lh::write_file(config/"user-dirs.dirs","XDG_PICTURES_DIR=\"$(touch /tmp/host-must-not-execute)\"\n");
    auto resolve=t.library("directory").fn<int(char *,unsigned)>("host_screenshot_directory");
    char buffer[4096]{};LH_CHECK(resolve(buffer,sizeof buffer));LH_EQ(std::string(buffer),expected.string());
}
LH_TEST(test_worker_is_async_and_queue_is_bounded) {
    t.ok(environment(t.harness("queue").arg(t.temp()/"shots")).timeout(30));
    auto files=pngs(t.temp()/"shots");LH_EQ(files.size(),size_t(2));
    const unsigned char colors[]={255,0,0,0,255,0,0,0,255,255,255,255};
    for(const auto &file:files) {auto image=png(file);LH_EQ(image.width,2u);LH_EQ(image.height,2u);LH_BYTES_EQ(image.rgb,std::string(reinterpret_cast<const char *>(colors),sizeof colors));}
}
} // namespace
