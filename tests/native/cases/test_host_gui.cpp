// Host GUI contracts: persistence, media, scaling and real SDL script events.
#include "host_fixtures.h"
#include "synthetic_disc.h"
#include <regex>
#include <set>
#include <sys/stat.h>
namespace {
lh::Result helper(lh::Case &t, std::initializer_list<lh::Arg> args, int expected = 0, const char *binary = "gui") {
    auto r = t.run(lh::dummy(t.harness(binary)).args(args).timeout(30));
    LH_CHECK_MSG(r.rc == expected, "expected exit " + std::to_string(expected) + "\n" + r.out + r.err); return r;
}
lh::Result script(lh::Case &t, const std::string &text, int expected = 0) {
    auto path = t.temp()/"steps.txt"; lh::write_file(path, text); return helper(t, {path}, expected, "script");
}
int mode(const lh::fs::path &path) { struct stat s{}; LH_EQ(stat(path.c_str(), &s), 0); return s.st_mode & 07777; }
std::string strip(std::string s) { while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s; }
}
LH_TEST(test_writer_keeps_unknown_comments_order_spacing_and_crlf) {
    auto ini = t.temp()/"lionheart.ini";
    std::string original = "# keep this\r\nunknown = untouched # comment\r\n  fullscreen = 0  ; note\r\nrumble=1\r\n";
    lh::write_file(ini, original); LH_EQ(chmod(ini.c_str(), 0640), 0); helper(t, {"write", ini, "fullscreen", "1"});
    original.replace(original.find("fullscreen = 0"), 14, "fullscreen = 1");
    LH_BYTES_EQ(lh::read_file(ini), original); LH_EQ(mode(ini), 0640);
    for (auto &e : lh::fs::directory_iterator(t.temp())) LH_NOT_CONTAINS(e.path().filename().string(), ".tmp-");
}
LH_TEST(test_writer_appends_header_and_missing_newline) {
    auto ini = t.temp()/"new/nested/lionheart.ini"; helper(t, {"write", ini, "rumble", "0"});
    LH_TEXT_EQ(lh::read_file(ini), "# Lionheart settings\nrumble=0\n"); LH_EQ(mode(ini), 0600);
    lh::write_file(ini, "unknown=untouched"); helper(t, {"write", ini, "fullscreen", "1"});
    LH_TEXT_EQ(lh::read_file(ini), "unknown=untouched\n# Lionheart settings\nfullscreen=1\n");
}
struct Prefix { const char *id, *text; };
static const Prefix prefixes[] = {{"", ""}, {"# user comment\\r\\nunknown = untouched\\r\\n", "# user comment\r\nunknown = untouched\r\n"}};
LH_TEST_P(test_writer_uses_one_header_across_saved_settings, prefixes) {
    auto ini = t.temp()/"lionheart.ini"; lh::write_file(ini, p.text);
    helper(t, {"write", ini, "renderer", "vulkan"}); helper(t, {"write", ini, "internal-scale", "8"});
    helper(t, {"write", ini, "renderer", "soft"}); helper(t, {"write", ini, "filter", "linear"});
    LH_BYTES_EQ(lh::read_file(ini), std::string(p.text)+"# Lionheart settings\nrenderer=soft\ninternal-scale=8\nfilter=linear\n");
}
struct Invalid { const char *id, *key, *value; };
static const Invalid invalid[] = {{"fullscreen-2", "fullscreen", "2"}, {"internal-scale-9", "internal-scale", "9"},
 {"renderer-metal", "renderer", "metal"}, {"not-a-key-1", "not-a-key", "1"},
 {"screenshot-dir-bad\\nfullscreen=1", "screenshot-dir", "bad\nfullscreen=1"}};
LH_TEST_P(test_writer_rejects_invalid_values_without_replacing_file, invalid) {
    auto ini = t.temp()/"lionheart.ini"; std::string original = "# no mutation\nfullscreen=0\n";
    lh::write_file(ini, original); helper(t, {"write", ini, p.key, p.value}, 2); LH_BYTES_EQ(lh::read_file(ini), original);
}
LH_TEST(test_writer_updates_duplicates_and_keeps_media_hash_name) {
    auto ini = t.temp()/"lionheart.ini";
    lh::write_file(ini, "fullscreen=0\n# middle\nfullscreen = 0 ; effective\ndisc1=FF8 #1.cue # note\n");
    helper(t, {"write", ini, "fullscreen", "1"}); LH_CHECK(lh::read_file(ini).rfind("fullscreen=1\n# middle\nfullscreen = 1 ; effective\n", 0) == 0);
    helper(t, {"write", ini, "disc1", "FF8 #2.cue"}); auto text = lh::read_file(ini); std::string tail = "disc1=FF8 #2.cue # note\n";
    LH_EQ(text.substr(text.size()-tail.size()), tail);
}
LH_TEST(test_write_failure_keeps_valid_session_value) {
    auto blocker = t.temp()/"file"; lh::write_file(blocker, "keep");
    LH_CONTAINS(helper(t, {"write", blocker/"lionheart.ini", "fullscreen", "1"}, 2).out, "ok=0 fullscreen=1");
    LH_TEXT_EQ(lh::read_file(blocker), "keep");
}
struct Scale { const char *id; int height; double expected; };
static const Scale scales[] = {{"100-0.75",100,.75},{"540-0.75",540,.75},{"720-1",720,1},{"1080-1.5",1080,1.5},
 {"1440-2",1440,2},{"2160-3",2160,3},{"4000-4",4000,4}};
LH_TEST_P(test_scale_math, scales) { LH_EQ(std::stod(helper(t, {"scale",p.height}).out), p.expected); }
LH_TEST(test_card_summary_counts_blocks_and_ff8_first_directory_frames) {
    std::string data(131072, '\0'); data.replace(0,2,"MC"); for(int i=1;i<16;++i) data[i*128]=char(0xa0);
    struct Row { int block, flags; const char *name; };
    const Row rows[]={{1,0x51,"BASLUS-00892FF8"},{2,0x52,"BASLUS-00892CHAIN"},{3,0x53,""},{4,0x51,"BASLUS-00908OTHER"},
      {5,0x51,"BASLUS-0089XBAD"},{6,0xa1,"BASLUS-00892DELETED"},{7,0x51,"BASLUS-00899SAVE"}};
    for(auto r:rows) { data[r.block*128]=char(r.flags); data.replace(r.block*128+10,std::string(r.name).size(),r.name); }
    auto card=t.temp()/"synthetic.mcr"; lh::write_file(card,data); LH_TEXT_EQ(helper(t,{"card-summary",card}).out,"6 2\n");
}
static const int sizes[]={0,1,131071,131073,262144};
LH_TEST_P(test_card_import_rejects_every_other_size, sizes) {
    auto source=t.temp()/"source.mcr", data=t.temp()/"data"; std::string bytes(p,'\0'); lh::write_file(source,bytes);
    helper(t,{"card-import",source,data},2); LH_BYTES_EQ(lh::read_file(source),bytes);
    if(lh::fs::exists(data)) LH_CHECK(lh::fs::recursive_directory_iterator(data)==lh::fs::recursive_directory_iterator());
}
LH_TEST(test_card_import_exact_size_any_extension_is_copy_and_collision_safe) {
    auto source=t.temp()/"source.anything", data=t.temp()/"data"; std::string original;
    for(int i=0;i<512;++i) for(int j=0;j<256;++j) original+=char(j);
    lh::write_file(source,original); lh::fs::path dest=strip(helper(t,{"card-import",source,data}).out);
    LH_EQ(dest,data/"cards/source.anything"); LH_BYTES_EQ(lh::read_file(dest),original); LH_BYTES_EQ(lh::read_file(source),original);
    lh::write_file(dest,std::string(131072,'\0')); lh::fs::path second=strip(helper(t,{"card-import",source,data}).out);
    LH_CHECK(second!=dest); LH_BYTES_EQ(lh::read_file(second),original); LH_BYTES_EQ(lh::read_file(dest),std::string(131072,'\0'));
    LH_BYTES_EQ(lh::read_file(source),original);
}
LH_TEST(test_new_card_is_formatted_and_existing_card_is_preserved) {
    auto data=t.temp()/"data"; lh::fs::path card=strip(helper(t,{"card-new",data,0}).out); LH_EQ(card,data/"cards/card1.mcr");
    auto original=lh::read_file(card); LH_EQ(original.size(),131072u); LH_EQ(original.substr(0,2),"MC");
    auto runtime=t.temp()/"runtime.mcr"; helper(t,{"runtime-card",runtime}); LH_BYTES_EQ(original,lh::read_file(runtime));
    LH_TEXT_EQ(helper(t,{"card-summary",card}).out,"0 0\n"); helper(t,{"card-new",data,0}); LH_BYTES_EQ(lh::read_file(card),original);
}
struct Browser { const char *id; int kind; const char *expected; };
static const Browser browsers[]={{"0-expected0",0,"f\ta.cue\nf\tB.BIN\n"},{"1-expected1",1,"f\tc.mcd\nf\td.MCR\nf\te.srm\n"},
 {"2-expected2",2,""},{"3-expected3",3,""}};
LH_TEST_P(test_browser_folders_first_case_insensitive_filters, browsers) {
    lh::fs::create_directory(t.temp()/"z-folder"); lh::fs::create_directory(t.temp()/"A-folder");
    for(auto name:{"a.cue","B.BIN","c.mcd","d.MCR","e.srm","hidden.iso"}) lh::write_file(t.temp()/name,"test");
    auto rows=lh::lines(helper(t,{"browser",t.temp(),p.kind,0}).out); std::string pairs;
    for(auto row:rows) { auto at=row.find('\t'); auto end=row.find('\t',at+1); pairs+=row.substr(0,end)+"\n"; }
    LH_TEXT_EQ(pairs,std::string("d\tA-folder\nd\tz-folder\n")+p.expected);
    auto all=helper(t,{"browser",t.temp(),p.kind,1}).out;
    if(p.kind<2) { LH_CONTAINS(all,"hidden.iso"); } else { LH_NOT_CONTAINS(all,"hidden.iso"); }
}
LH_TEST(test_browser_up_and_state_roundtrip) {
    auto folder=t.temp()/"space name/nested"; lh::fs::create_directories(folder);
    LH_EQ(strip(helper(t,{"browser-up",folder}).out),folder.parent_path().string()); auto data=t.temp()/"data";
    LH_TEXT_EQ(helper(t,{"gui-state",data,folder,2}).out,"2\t"+folder.string()+"\n");
    LH_TEXT_EQ(lh::read_file(data/"gui-state.ini"),"last-folder="+folder.string()+"\nlast-input=pad\n");
}
struct Error { std::string id, message, expected; };
static std::vector<Error> errors() {
    std::vector<Error> out;
    for(auto row: std::vector<std::pair<std::string,std::string>>{
      {"x: No such file or directory","disc.err.missing"},{"x: unsupported image","disc.err.unsupported"},
      {"x: not a PlayStation disc","disc.err.not_psx"},{"Another game: SCUS_123.45","disc.err.other_game"},
      {"Only the USA release is supported","disc.err.region"},{"EXE FNV-1a check failed","disc.err.hash"},
      {"IMG SHA-1 check failed","disc.err.img_hash"},{"IMG size check failed","disc.err.size"},{"out of memory","disc.err.memory"}})
        out.push_back({row.first+"-"+row.second,row.first,row.second});
    return out;
}
LH_TEST_P(test_disc_error_copy_mapping, errors()) { LH_EQ(strip(helper(t,{"disc-error",p.message}).out),p.expected); }
static const int numbers[]={1,2,3,4};
LH_TEST_P(test_media_worker_delivers_identified_serial_and_generation, numbers) {
    synthetic::Disc d; d.number=p; auto path=synthetic::disc(t.temp()/"image.bin",d);
    auto out=helper(t,{"identify",path,0}).out;
    std::istringstream fields(out); std::string number, generation, status, file;
    std::getline(fields,number,'\t'); std::getline(fields,generation,'\t');
    std::getline(fields,status,'\t'); std::getline(fields,file,'\t');
    LH_EQ(number,std::to_string(p)); LH_EQ(generation,"91"); LH_EQ(status,"2"); LH_EQ(file,path.string());
}
LH_TEST(test_folder_scan_identifies_each_filtered_file) {
    for(int n:{4,2,1}) { synthetic::Disc d; d.number=n; synthetic::disc(t.temp()/("disc"+std::to_string(n)+".bin"),d); }
    lh::write_file(t.temp()/"ignored.iso","not scanned"); auto rows=lh::lines(helper(t,{"scan",t.temp(),3}).out);
    std::vector<int> discs;
    for(auto row:rows) { auto at=row.find('\t'); discs.push_back(std::stoi(row.substr(0,at))); LH_CHECK(row.substr(at,6)=="\t37\t2\t"); }
    std::sort(discs.begin(),discs.end()); LH_CHECK((discs==std::vector<int>{1,2,4}));
}
LH_TEST(test_string_table_covers_each_static_ui_reference) {
    auto header=lh::read_file(t.root()/"backends/host/gui_strings.h");
    std::regex definition(R"re(\{\s*"([a-z][a-z0-9_.-]+)"\s*,)re"), use(R"re((?:host_gui_string|S)\("([a-z][a-z0-9_.-]+)"\))re");
    std::set<std::string> ids; size_t count=0;
    for(std::sregex_iterator it(header.begin(),header.end(),definition),end;it!=end;++it) { ++count; ids.insert((*it)[1]); }
    LH_CHECK(count>0); LH_EQ(count,ids.size());
    for(auto e:lh::fs::directory_iterator(t.root()/"backends/host")) {
        auto name=e.path().filename().string(), ext=e.path().extension().string();
        if(name.rfind("gui",0)!=0 || (ext!=".c" && ext!=".cpp")) continue;
        auto text=lh::read_file(e.path());
        for(std::sregex_iterator it(text.begin(),text.end(),use),end;it!=end;++it)
            LH_CHECK_MSG(ids.count((*it)[1]), "missing string: "+(*it)[1].str());
    }
    LH_TEXT_EQ(helper(t,{"string","app.name"}).out,"Lionheart\n");
    LH_TEXT_EQ(helper(t,{"string","ra.err.unavailable"}).out,"Not available in this build.\n");
}
LH_TEST(test_gui_test_injects_balanced_real_sdl_events) {
    auto r=script(t,"at 3\nopen\nnav down\nnav up\nnav left\nnav right\nselect\nsection next\nsection prev\ntext \"héllo\"\ndrop \"/tmp/path with spaces.cue\"\nwait 2\nassert-focus btn.browse\nassert-text app.name \"Lionheart\"\ncapture \""+t.temp().string()+"/capture.png\"\nclose\n");
    LH_CONTAINS(r.out,"keys=9 ups=9 text=6 drops=1"); LH_CONTAINS(r.out,"finished=1 failed=0 visible=0");
}
struct Step { std::string id, text; };
static std::vector<Step> invalid_steps() {
    std::vector<Step> out;
    for(std::string s:{"not-a-command\n","open\nnav diagonal\n","wait -1\n","open\nassert-focus wrong\n",
      "open\nassert-text app.name \"Wrong\"\n","capture /tmp/hidden.png\n","open\ntext \"unterminated\n",
      "open\nmouse nope 1\n","open\nmouse 1 2 junk\n","select-up\n","mouse-up\n","open\nmouse-down\n",
      "open\nselect-down\n","nav down\n","section next\n","text \"hidden\"\n","drop \"/tmp/hidden.cue\"\n"}) {
        std::string id; for(char c:s) id+=(c=='\n' ? "\\n" : std::string(1,c)); out.push_back({id,s});
    } return out;
}
LH_TEST_P(test_gui_test_fails_closed_on_invalid_steps_or_observations, invalid_steps()) {
    auto r=script(t,p.text,2); LH_CONTAINS(r.out,"failed=1"); LH_CONTAINS(r.err,"GUI test");
}
LH_TEST(test_gui_test_queues_balanced_held_and_mouse_input) {
    auto r=script(t,"open\nmouse 400 200\nmouse-down\nwait 2\nmouse-up\nselect-down\nwait 2\nselect-up\nclose\n");
    LH_CONTAINS(r.out,"finished=1 failed=0 visible=0");
}
LH_TEST(test_audio_pause_preserves_device_queue_preroll_and_resumed_playback) {
    auto r=t.ok(lh::dummy(t.harness("audio")).timeout(20)); LH_CONTAINS(r.out,"resumed playback stayed started");
}
LH_TEST(test_gui_pause_real_controller_events_and_guest_sio_wire) {
    auto r=t.ok(lh::dummy(t.harness("input")).timeout(30));
    LH_CONTAINS(r.out,"Guide taps opened; Guide+Cross and F10 opened achievements; paused SIO replies were released; held input cleared");
}
LH_TEST(test_controller_held_on_entry_cannot_activate_resume) {
    auto r=t.ok(lh::dummy(t.harness("held-entry")).env("SDL_VIDEODRIVER","offscreen").env("SDL_RENDER_DRIVER","software").timeout(30));
    LH_CONTAINS(r.out,"actions_from_button_held_on_entry=0"); LH_CONTAINS(r.out,"actions_after_release_and_fresh_press=1");
}
LH_TEST(test_keyboard_navigation_stays_in_pane_and_pad_text_switch_opens_keyboard) {
    auto r=t.ok(lh::dummy(t.harness("navigation")).env("SDL_VIDEODRIVER","offscreen").env("SDL_RENDER_DRIVER","software").timeout(30));
    LH_CONTAINS(r.out,"down_from_renderer=filter"); LH_CONTAINS(r.out,"same_field_pad_callbacks=1");
}
LH_TEST(test_hidden_gui_assertion_fails_during_event_tick) {
    auto r=script(t,"assert-focus btn.browse\n",2); LH_CONTAINS(r.err,"interaction or assertion requires a visible GUI"); LH_CONTAINS(r.out,"ticks=1 ");
}
