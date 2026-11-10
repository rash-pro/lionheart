// Player legal checks use complete recompiler overlay images and compiler inputs.
#include "lh_test.h"
#include "../../../tools/player-check/scan.h"
#include <random>

namespace pc = player_check;
static std::string payload(size_t length,unsigned seed=16) {
    std::mt19937 random(seed); std::string data;
    for(size_t i=0;i<length;++i) data+=char(random()&255);
    return data;
}
static void rejects(const std::function<void()> &run,const std::string &message) {
    try { run(); } catch(const std::runtime_error &e) { LH_CONTAINS(std::string(e.what()),message); return; }
    LH_FAIL("expected rejection: "+message);
}
static pc::Json field(const pc::Json &j,const std::string &key) { return j.at(key); }
static lh::fs::path database(lh::Case &t,std::vector<std::string> arguments={},std::string source="main.c",const std::string &command="") {
    auto unit=pc::Json::dict().set("directory",t.temp().string()).set("file",source);
    if(command.empty()) {
        auto args=pc::Json::list(); args.items.emplace_back("cc");
        for(auto &a:arguments) args.items.emplace_back(a);
        args.items.emplace_back("-c"); args.items.emplace_back(source); unit.set("arguments",std::move(args));
    } else unit.set("command",command);
    auto units=pc::Json::list(); units.items.push_back(std::move(unit)); auto path=t.temp()/"compile_commands.json";
    lh::write_file(path,units.dump()); return path;
}
static pc::Extraction synthetic() {
    return {[](const lh::fs::path &,const lh::fs::path &){return std::vector<pc::Image>{{0,std::string(64,'\0')}};},
            [](const std::vector<lh::fs::path> &,const lh::fs::path &){return std::vector<pc::Image>{{128,std::string(64,'\0')}};}};
}
static std::vector<lh::fs::path> discs(const std::string &suffix="cue") {
    std::vector<lh::fs::path> paths; for(int i=1;i<=4;++i) paths.emplace_back("disc"+std::to_string(i)+"."+suffix); return paths;
}
static const int lengths[]={0,1,63,64,65,127,200};
LH_TEST_P(test_binary_index_matches_direct_scan_and_first_match_offset,lengths) {
    auto text=payload(p,1900+p),binary=std::string(9,'\0')+text+std::string(3,'\0')+text;
    pc::BinaryIndex index(binary);
    if(p<64) LH_EQ(pc::check_image(binary,text,&index).dump(),pc::check_image(binary,text).dump());
    else {
        rejects([&]{pc::check_image(binary,text);},"binary offset 9");
        rejects([&]{pc::check_image(binary,text,&index);},"binary offset 9");
    }
    std::string clean(200,'\0'); pc::BinaryIndex empty(clean);
    LH_EQ(pc::check_image(clean,text,&empty).dump(),pc::check_image(clean,text).dump());
}
LH_TEST(test_overlay_scans_every_disc_and_full_image) {
    auto data=payload(193); auto extraction=synthetic();
    std::vector<std::pair<lh::fs::path,lh::fs::path>> calls;
    extraction.legacy=[&](const lh::fs::path &disc,const lh::fs::path &work) {
        calls.emplace_back(disc,work); lh::fs::create_directories(work); lh::write_file(work/"private.bin",data);
        return std::vector<pc::Image>{{calls.size()*32,data}};
    };
    auto paths=discs(); auto work=t.temp()/"extracted";
    auto r=pc::check_discs(std::string(200,'\0'),paths,work,extraction);
    LH_EQ(field(r,"legacy_full_images").integer(),4u); LH_EQ(field(r,"catalog_loaded_images").integer(),1u); LH_EQ(field(r,"overlay_images").integer(),5u);
    for(size_t i=0;i<calls.size();++i) {
        LH_EQ(calls[i].first,paths[i]); LH_CONTAINS(calls[i].second.string(),work.string()); LH_CHECK(!lh::fs::exists(calls[i].second));
        LH_EQ(r.at("discs").items[i].at("overlays").items[0].at("image_bytes").integer(),data.size());
    }
    rejects([&]{pc::check_discs(std::string(9,'\0')+data.substr(101,64),paths,work,extraction);},"disc1.cue, overlay 160: guest text window found at binary offset 9");
    LH_CHECK(lh::fs::is_empty(work));
}
LH_TEST(test_overlay_match_on_last_disc_is_detected) {
    auto extraction=synthetic(); auto data=payload(64,17); size_t calls=0;
    extraction.legacy=[&](const lh::fs::path &,const lh::fs::path &) { return std::vector<pc::Image>{{96,++calls==4?data:std::string(64,'\0')}}; };
    rejects([&]{pc::check_discs(data,discs("bin"),t.temp(),extraction);},"disc4.bin, overlay 96"); LH_EQ(calls,4u);
}
LH_TEST(test_empty_overlay_extraction_fails_closed) {
    auto extraction=synthetic(); extraction.legacy=[](const lh::fs::path &,const lh::fs::path &){return std::vector<pc::Image>{};};
    rejects([&]{pc::check_discs(std::string(64,'\0'),{"disc.cue"},t.temp(),extraction);},"no overlay images");
}
LH_TEST(test_default_catalog_adds_effect_images_and_uses_all_discs) {
    auto extraction=synthetic(); auto data=payload(193,19); auto paths=discs();
    std::vector<lh::fs::path> used; lh::fs::path directory;
    extraction.catalog=[&](const std::vector<lh::fs::path> &cues,const lh::fs::path &work) {
        used=cues; directory=work; lh::fs::create_directories(work); lh::write_file(work/"effect.bin",data);
        return std::vector<pc::Image>{{128,data},{129,std::string(80,'\0')}};
    };
    auto work=t.temp()/"extracted"; auto r=pc::check_discs(std::string(200,'\0'),paths,work,extraction);
    LH_EQ(r.at("legacy_full_images").integer(),4u); LH_EQ(r.at("catalog_loaded_images").integer(),2u); LH_EQ(r.at("overlay_images").integer(),6u);
    LH_EQ(r.at("catalog").items[0].at("index").integer(),128u); LH_EQ(r.at("catalog").items[1].at("index").integer(),129u);
    LH_CHECK(used==paths); LH_CHECK(!lh::fs::exists(directory));
    rejects([&]{pc::check_discs(std::string(9,'\0')+data.substr(101,64),paths,work,extraction);},"catalog overlay 128: guest text window found at binary offset 9");
    LH_CHECK(lh::fs::is_empty(work));
}
LH_TEST(test_empty_default_catalog_extraction_fails_closed) {
    auto extraction=synthetic(); extraction.catalog=[](const std::vector<lh::fs::path> &,const lh::fs::path &){return std::vector<pc::Image>{};};
    rejects([&]{pc::check_discs(std::string(64,'\0'),{"disc.cue"},t.temp(),extraction);},"default recompiler catalog extraction produced no overlay images");
}
struct Include { std::string id,flag; bool attached; };
static std::vector<Include> includes() {
    std::vector<Include> rows;
    for(bool attached:{false,true}) for(auto flag:{"-I","-isystem","-iquote","-idirafter","-include","-imacros","-isystem-after","-iframework","-F","--include-directory","/I","/FI","/external:I"})
        rows.push_back({std::string(attached?"True-":"False-")+flag,flag,attached});
    return rows;
}
LH_TEST_P(test_catalog_include_paths_are_rejected,includes()) {
    std::string path=p.flag=="-include"||p.flag=="-imacros"||p.flag=="/FI"?"generated-exact/catalog.h":"generated-exact/headers";
    auto args=p.attached?std::vector<std::string>{p.flag+(p.flag=="--include-directory"?"=":"")+path}:std::vector<std::string>{p.flag,path};
    rejects([&]{pc::check_compile_commands(database(t,args));},"generated catalog include");
}
static const char *const sources[]={"generated-exact/catalog.c","gen/overlay_2_001.c","dispatch_table.c","interpreter_table.c","functions_001.c"};
LH_TEST_P(test_catalog_sources_are_rejected,sources) {
    rejects([&]{pc::check_compile_commands(database(t,{},p));},"generated catalog source");
}
LH_TEST(test_player_module_cache_paths_are_rejected) {
    const std::string cache="data/lionheart/native/0123456789abcdef0123456789abcdef/fedcba9876543210fedcba9876543210";
    rejects([&]{pc::check_compile_commands(database(t,{},cache+"/work/1-1/unit_000.c"));},"generated catalog source");
    rejects([&]{pc::check_compile_commands(database(t,{"-I",cache+"/include"}));},"generated catalog include");
    // Other folders that happen to be called native stay allowed.
    LH_CHECK(pc::check_compile_commands(database(t,{"-Itests/native/support"},"tests/native/cases/x.c")).at("passed").truth());
}
LH_TEST(test_named_catalog_directory_and_symlink_are_rejected) {
    auto catalog=t.temp()/"catalog"; lh::fs::create_directory(catalog); lh::fs::create_directory_symlink(catalog,t.temp()/"alias");
    rejects([&]{pc::check_compile_commands(database(t,{"-Ialias"}),{catalog});},"generated catalog include");
    lh::write_file(t.temp()/"CMakeCache.txt","GENERATED_DIR:PATH="+catalog.string()+"\n");
    rejects([&]{pc::check_compile_commands(database(t,{},"alias/module.c"));},"generated catalog source");
}
LH_TEST(test_response_and_forwarded_include_options_are_checked) {
    lh::write_file(t.temp()/"outer.rsp","@inner.rsp");
    lh::write_file(t.temp()/"inner.rsp","-Xclang -include -Xclang 'generated-exact/with spaces.h'");
    rejects([&]{pc::check_compile_commands(database(t,{"@outer.rsp"}));},"generated catalog include");
    rejects([&]{pc::check_compile_commands(database(t,{"-Wp,-I,generated-exact"}));},"generated catalog include");
}
LH_TEST(test_unreadable_and_recursive_responses_fail_closed) {
    rejects([&]{pc::check_compile_commands(database(t,{"@absent.rsp"}));},"cannot read compiler response file");
    lh::write_file(t.temp()/"cycle.rsp","@cycle.rsp");
    rejects([&]{pc::check_compile_commands(database(t,{"@cycle.rsp"}));},"recursive compiler response file");
}
LH_TEST(test_clean_commands_and_response_count) {
    lh::write_file(t.temp()/"headers.rsp","-I \"include with spaces\" -isystem /usr/include -include config.h");
    auto r=pc::check_compile_commands(database(t,{"@headers.rsp"},"player_catalog.c"));
    LH_CHECK(r.at("passed").truth()); LH_EQ(r.at("include_paths_checked").integer(),3u); LH_EQ(r.at("response_files_checked").integer(),1u);
    LH_CHECK(pc::check_compile_commands(database(t,{},"main.c","cc -I'include with spaces' -c main.c")).at("passed").truth());
}
LH_TEST(test_cli_keeps_exe_check_and_adds_disc_and_compile_checks) {
    auto data=payload(80,18); auto exe=std::string(2048,'\0')+data;
    exe.replace(0,8,"PS-X EXE"); exe[28]=char(data.size());
    auto path=t.temp()/"game.exe",binary=t.temp()/"player"; lh::write_file(path,exe); lh::write_file(binary,std::string(200,'\0'));
    auto extraction=synthetic(); extraction.legacy=[&](const lh::fs::path &,const lh::fs::path &){return std::vector<pc::Image>{{0,data}};};
    auto result=pc::scan_cli({binary.string(),path.string(),"--disc","disc.cue","--compile-commands",database(t).string(),"--extract-dir",(t.temp()/"extracted").string()},extraction,t.root());
    LH_CHECK(result.at("passed").truth()); LH_EQ(result.at("overlays").at("legacy_full_images").integer(),1u);
    LH_EQ(result.at("overlays").at("catalog_loaded_images").integer(),1u); LH_CHECK(result.at("compile_commands").at("passed").truth());
    LH_NOT_CONTAINS(result.dump(),"sha"); LH_NOT_CONTAINS(result.dump(),"hash"); LH_CHECK(pc::check_image(lh::read_file(binary),data).at("passed").truth());
    auto cli=t.ok(t.harness("check_player_binary").args({binary,path,"--compile-commands",database(t)}));
    LH_CHECK(pc::parse(cli.out).at("compile_commands").at("passed").truth());
    auto reordered=t.ok(t.harness("check_player_binary").args({"--compile-commands="+database(t).string(),binary,path}));
    LH_EQ(pc::parse(reordered.out).dump(),pc::parse(cli.out).dump());
    lh::write_file(path,"bad"); auto bad=t.run(t.harness("check_player_binary").args({binary,path}));
    LH_CHECK(bad.rc!=0); LH_CONTAINS(bad.err,"not a PS-X EXE");
}
LH_TEST(test_windows_drive_paths_match_explicit_catalog_root) {
    rejects([&]{pc::check_compile_commands(database(t,{"-IC:\\game\\catalog"}),{"C:\\game\\catalog"});},"generated catalog include");
}
LH_TEST(test_player_compiles_imgui_without_its_unused_default_font) {
    auto binary=lh::fs::path(t.player().program()); auto units=pc::load_json((binary.parent_path()/"compile_commands.json").string()); size_t count=0;
    for(auto &u:units.values()) if(lh::fs::path(u.at("file").str()).filename()=="imgui_draw.cpp") {
        ++count; std::vector<std::string> args;
        if(auto a=u.find("arguments")) { for(auto &v:a->values()) args.push_back(v.str()); }
        else args=pc::shell_words(u.at("command").str());
        LH_CHECK(std::find(args.begin(),args.end(),"-DIMGUI_DISABLE_DEFAULT_FONT")!=args.end());
    }
    LH_EQ(count,1u); auto source=lh::read_file(t.root()/"third_party/imgui/imgui_draw.cpp"); std::smatch match;
    LH_CHECK(std::regex_search(source,match,std::regex("proggy_clean_ttf_compressed_data\\[(\\d+)\\]\\s*=\\s*\\{([^}]*)\\}")));
    auto body=match[2].str(); std::string font; std::regex digits("\\b\\d+\\b");
    for(std::sregex_iterator i(body.begin(),body.end(),digits),end;i!=end;++i) font+=char(std::stoi(i->str()));
    LH_EQ(font.size(),std::stoul(match[1].str())); LH_CHECK(lh::read_file(binary).find(font)==std::string::npos);
}
