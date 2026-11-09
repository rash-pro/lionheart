// Player distribution boundary and synthetic ISO9660 startup contracts.
#include "synthetic_player.h"
#include "../../../tools/player-check/scan.h"
#include <regex>
#include <iomanip>
#include <sstream>
namespace {
using namespace synthetic;
lh::Result identify(lh::Case &t,const lh::fs::path &path,const char *action="quick",std::initializer_list<lh::Arg> paths={}) {
    return t.run(t.harness("identify").args({action,path}).args(paths).timeout(20));
}
struct DiscRow { std::string id; int number,mode; };
std::vector<DiscRow> rows() { std::vector<DiscRow> r; for(int m:{1,2}) for(int n=1;n<=4;++n) r.push_back({std::to_string(m)+"-"+std::to_string(n),n,m}); return r; }
}
LH_TEST_P(test_disc_identity_and_full_hash,rows()) {
    Disc d; d.number=p.number; d.mode=p.mode; auto path=disc(t.temp()/"game.bin",d);
    for(auto action:{"quick","full"}) { auto r=identify(t,path,action); LH_EQ(r.rc,0); LH_EQ(r.out,"disc="+std::to_string(p.number)+"\n"); }
    LH_EQ(identify(t,path,"exe").out,"EXE verified\n");
}
LH_TEST(test_cue_offset_and_order) {
    std::vector<lh::fs::path> cues;
    for(int n:{4,2,1}) { Disc d; d.number=n; d.offset=150; auto path=disc(t.temp()/("disc"+std::to_string(n)+".bin"),d); auto cue=path; cue.replace_extension(".cue");
        lh::write_file(cue,"FILE \""+path.filename().string()+"\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:02:00\n"); cues.push_back(cue); }
    auto r=identify(t,cues[0],"assign",{cues[1],cues[2]}); LH_EQ(r.rc,0);
    LH_EQ(r.out,"1="+cues[2].string()+"\n2="+cues[1].string()+"\n3=\n4="+cues[0].string()+"\n");
    r=identify(t,cues[0],"assign",{cues[0]}); LH_EQ(r.rc,2); LH_EQ(r.err,"Two images for FF8 USA disc 4\n");
}
namespace {
struct Error { std::string id,message; int index; };
std::vector<Error> errors() { const char *messages[]={"not a PlayStation disc","not a PlayStation disc","not a PlayStation disc","Another game","not a PlayStation disc","Another game: SCUS_123.45","Only the USA release","EXE size check failed","EXE header check failed","EXE FNV-1a check failed","EXE SHA-1 check failed","missing or truncated","IMG size check failed","IMG LBA check failed"};
    std::vector<Error> r; for(int i=0;i<14;++i) r.push_back({"changes"+std::to_string(i)+"-"+messages[i],messages[i],i}); return r; }
}
LH_TEST_P(test_identity_errors,errors()) {
    Disc d; switch(p.index) {
    case 0:d.pvd=false;break; case 1:d.cnf=false;break; case 2:d.boot="BOOT2 = cdrom:\\SLUS_008.92;1";break;
    case 3:d.boot="BOOT = cdrom:\\DIR\\SLUS_008.92;1";break; case 4:d.bad_record=true;break;
    case 5:d.name="SCUS_123.45";d.image=false;break;case 6:d.name="SLES_020.80";break;case 7:d.exe_size=2048;break;
    case 8:d.executable.replace(0,8,"broken!!");break;case 9:d.executable.replace(2048,2048,std::string(2048,'\0'));break;
    case 10:d.executable[100]='!';break;case 11:d.image=false;break;case 12:d.img_size=4096;break;case 13:d.img_lba=825;break;
    }
    auto r=identify(t,disc(t.temp()/"game.bin",d)); LH_EQ(r.rc,2); LH_CONTAINS(r.err,p.message); LH_EQ(std::count(r.err.begin(),r.err.end(),'\n'),1);
}
LH_TEST(test_full_hash_is_opt_in) {
    auto path=disc(t.temp()/"game.bin"); auto data=lh::read_file(path); data[826*2352+24]='!';lh::write_file(path,data);
    LH_EQ(identify(t,path).rc,0); auto r=identify(t,path,"full"); LH_EQ(r.rc,2); LH_CONTAINS(r.err,"IMG SHA-1 check failed");
}
static const char *const suffixes[]={".iso",".chd",".pbp",".bin"};
LH_TEST_P(test_unsupported_formats,suffixes) {
    auto path=t.temp()/(std::string("bad")+p); lh::write_file(path,std::string(2048,'\0')); auto r=identify(t,path); LH_EQ(r.rc,2);
    LH_CONTAINS(r.err,"single-track .cue with MODE2/2352 or MODE1/2352, or raw 2352-byte .bin");
}
namespace {
struct Cue { std::string id,text; };
std::vector<Cue> cues() { std::vector<Cue> rows; for(std::string s:{
    "FILE \"x.bin\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n",
    "FILE \"x.bin\" BINARY\nTRACK 01 MODE2/2352\nTRACK 02 AUDIO\nINDEX 01 00:00:00\n",
    "FILE \"x.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:60:00\n",
    "FILE \"x.bin\" BINARY\nTRACK 01 MODE2/2352\n",
    "FILE \"x.bin\" WAVE\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n",
    "FILE \"missing.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n"}) {
        std::string id; for(char c:s) id+=c=='\n'?"\\n":std::string(1,c); rows.push_back({id,s}); } return rows; }
}
LH_TEST_P(test_cue_errors,cues()) {
    disc(t.temp()/"x.bin");auto path=t.temp()/"bad.cue";lh::write_file(path,p.text);auto r=identify(t,path);LH_EQ(r.rc,2);LH_CONTAINS(r.err,"supported:");LH_EQ(std::count(r.err.begin(),r.err.end(),'\n'),1);
}
LH_TEST(test_unreadable) { auto r=identify(t,t.temp()/"absent.bin");LH_EQ(r.rc,2);LH_CONTAINS(r.err,"No such file");LH_CONTAINS(r.err,"supported:"); }
static const char *const recording[]={"FF8_RECORD_GPU","FF8_SPU_TRACE"};
LH_TEST_P(test_player_rejects_recording_builds,recording) {
    auto r=t.run(t.host("cmake").args({"-S",t.root(),"-B",t.temp(),"-DFF8_PLAYER=ON",std::string("-D")+p+"=ON"})); LH_CHECK(r.rc!=0);LH_CONTAINS(r.err,"The player cannot include");
}
LH_TEST(test_no_arguments) {
    auto r=t.run(play(t,{},true)); LH_EQ(r.rc,2); LH_EQ(r.err,"Provide FF8 USA disc 1 with --disc PATH or disc1=PATH in "+(t.temp()/"config/lionheart/lionheart.ini").string()+"\n");
    LH_CHECK(lh::fs::exists(t.temp()/"data/lionheart/logs/lionheart.log"));
}
LH_TEST(test_disc_boot_default_card_and_states) {
    auto image=disc(t.temp()/"game.bin"),state=t.temp()/"state.bin";
    auto r=t.ok(running(t,{"--disc",image,"--dump-state",state,"--save-at","1","--save-state",t.temp()/"saved.ff8s"}));
    LH_CONTAINS(r.out,"VBlanks=2 ");LH_CONTAINS(r.out,"Stop: VBlank limit reached");
    auto card=t.temp()/"data/lionheart/cards/card1.mcr";auto content=lh::read_file(card);LH_EQ(content.size(),131072u);LH_EQ(content.substr(0,2),"MC");
    LH_CHECK(!lh::fs::exists(card.parent_path()/"card2.mcr"));LH_CHECK(lh::fs::is_directory(t.temp()/"data/lionheart/states"));
    LH_BYTES_EQ(lh::read_file(state).substr(9+135*4+0x10000,2048),exe().substr(2048));
    t.ok(running(t,{"--disc",image}));LH_BYTES_EQ(lh::read_file(card),content);
}
LH_TEST(test_explicit_exe_matches_disc_boot) {
    auto image=disc(t.temp()/"game.bin"),file=t.temp()/"game.exe";lh::write_file(file,exe());
    t.ok(running(t,{"--disc",image,"--dump-state",t.temp()/"disc"}));t.ok(running(t,{file,"--disc",image,"--dump-state",t.temp()/"file"}));LH_FILE_EQ(t.temp()/"disc",t.temp()/"file");
}
LH_TEST(test_ini_relative_paths_cli_and_order) {
    auto cfg=t.temp()/"settings";lh::fs::create_directory(cfg); for(int n=1;n<=4;++n) {Disc d;d.number=n;disc(cfg/(std::to_string(n)+".bin"),d);}
    auto ini=cfg/"lionheart.ini";lh::write_file(ini,"disc1=4.bin\ndisc2=2.bin\ndisc3=1.bin\ndisc4=3.bin\ncard1=one.mcr\ncard2=two.mcr\n");
    t.ok(running(t,{"--config",ini}));for(auto card:{"one.mcr","two.mcr"}) LH_EQ(lh::read_file(cfg/card).substr(0,2),"MC");
    lh::write_file(ini,"disc1=absent.bin\ncard1=absent/one.mcr\ncard2=\n");t.ok(running(t,{"--config",ini,"--disc",cfg/"1.bin","--card1",t.temp()/"cli.mcr"}));LH_CHECK(lh::fs::exists(t.temp()/"cli.mcr"));
}
LH_TEST(test_portable) {
    auto binary=t.temp()/"ff8";lh::fs::copy_file(player(t),binary);lh::fs::permissions(binary,lh::fs::perms::owner_all);lh::write_file(t.temp()/"portable","");
    auto r=t.run(play(t,{},true,binary));LH_EQ(r.rc,2);LH_CONTAINS(r.err,(t.temp()/"lionheart.ini").string());
    disc(t.temp()/"disc.bin");lh::write_file(t.temp()/"lionheart.ini","disc1=disc.bin\n");auto other=t.temp()/"other";lh::fs::create_directory(other);
    auto cmd=play(t,{},true,binary);run(cmd);cmd.cwd(other).env("HOME",other.string()).env("XDG_DATA_HOME",(other/"data").string()).env("XDG_CONFIG_HOME",(other/"config").string());t.ok(cmd);
    LH_EQ(lh::read_file(t.temp()/"cards/card1.mcr").substr(0,2),"MC");LH_CHECK(!lh::fs::exists(other/"data"));
}
LH_TEST(test_home_fallback_and_missing_disc) {
    Disc d;d.number=2;auto path=disc(t.temp()/"two.bin",d);auto r=t.run(running(t,{"--disc",path}).env("XDG_DATA_HOME",""));LH_EQ(r.rc,2);LH_CONTAINS(r.err,"Provide FF8 USA disc 1");
    path=disc(t.temp()/"one.bin");t.ok(running(t,{"--disc",path}).env("XDG_DATA_HOME",""));LH_CHECK(lh::fs::exists(t.temp()/".local/share/lionheart/cards/card1.mcr"));
}
LH_TEST(test_missing_swap_is_not_fatal) {
    auto path=disc(t.temp()/"one.bin"),script=t.temp()/"input.txt";lh::write_file(script,"1 DISC2\n2 -\n");auto r=t.ok(running(t,{"--disc",path,"--input-script",script}));LH_CONTAINS(r.err,"Disc 2 missing");
}
LH_TEST(test_player_full_verification_cli_and_ini) {
    auto path=disc(t.temp()/"one.bin");auto data=lh::read_file(path);data[826*2352+24]='!';lh::write_file(path,data);t.ok(running(t,{"--disc",path}));
    auto r=t.run(running(t,{"--disc",path,"--verify-discs","full"}));LH_EQ(r.rc,2);LH_CONTAINS(r.err,"IMG SHA-1 check failed");LH_EQ(std::count(r.err.begin(),r.err.end(),'\n'),1);
    auto ini=t.temp()/"full.ini";lh::write_file(ini,"disc1=one.bin\nverify-discs=full\n");r=t.run(running(t,{"--config",ini}));LH_EQ(r.rc,2);LH_CONTAINS(r.err,"IMG SHA-1 check failed");t.ok(running(t,{"--config",ini,"--verify-discs","quick"}));
}
LH_TEST(test_player_compiles_no_generated_catalog) {
    auto build=lh::fs::path(t.player().program()).parent_path();auto units=lh::gen::Json::file(build/"compile_commands.json");LH_CHECK(units.size()>0);bool catalog=false;
    for(size_t i=0;i<units.size();++i) {auto file=lh::fs::path(units.at(i).at("file").str());auto name=file.filename().string();catalog|=name=="player_catalog.c";
        auto parent=file.parent_path().string();std::transform(parent.begin(),parent.end(),parent.begin(),[](unsigned char c){return std::tolower(c);});LH_NOT_CONTAINS(parent,"generated");
        LH_CHECK(!std::regex_search(name,std::regex("^(functions_|overlay_|dispatch_table|interpreter_table)")));LH_CHECK(name!="gpu_record.c"&&name!="spu_trace.c");}
    LH_CHECK(catalog);auto cache=lh::read_file(build/"CMakeCache.txt");for(auto option:{"FF8_PLAYER","FF8_VULKAN","FF8_PGXP","FF8_SAVESTATES","FF8_MOVIES","FF8_RCHEEVOS","FF8_STATE_BRIDGE"}) LH_CONTAINS(cache,std::string(option)+":BOOL=ON");LH_CONTAINS(cache,"FF8_STATS:BOOL=OFF");
}
LH_GAME_TEST(test_exe_identity_equals_recompiler) {
    auto file=lh::owned_path("SLUS_008.92");if(!lh::fs::is_regular_file(file)) lh::skip("owned game EXE is absent");auto bytes=lh::read_file(file);empty_catalog(bytes,t.temp());
    auto facts=lh::gen::Json::file(t.root()/"games/ff8-usa/exe-identity.json");auto dispatch=lh::read_file(t.temp()/"dispatch_table.c");
    LH_CONTAINS(dispatch,"generated_hash = 0x"+facts.at("fnv1a").str()+"u");
    std::ostringstream base; base << std::hex << std::setfill('0') << std::setw(8) << facts.at("base").integer();
    LH_CONTAINS(dispatch,"generated_base = 0x"+base.str()+"u, generated_size = "+std::to_string(facts.at("size").integer()));
    char hash[41];lhg_sha1_hex(reinterpret_cast<const uint8_t *>(bytes.data()),bytes.size(),hash);
    LH_EQ(facts.at("file_size").integer(),int64_t(bytes.size()));LH_EQ(facts.at("sha1").str(),std::string(hash));
}
LH_GAME_TEST(test_player_contains_no_guest_text) {
    auto file=lh::owned_path("SLUS_008.92");if(!lh::fs::is_regular_file(file)) lh::skip("owned game EXE is absent");
    auto bytes=lh::read_file(file);uint32_t size=0;for(unsigned i=0;i<4;++i) size|=uint32_t(uint8_t(bytes.at(28+i)))<<(8*i);
    auto r=player_check::check_image(lh::read_file(t.player().program()),bytes.substr(2048,size));LH_CHECK(r.at("passed").truth());
}
LH_TEST(test_legal_check_detects_windows_and_ignores_low_entropy) {
    lh::PyRandom random(14);auto text=random.randbytes(130);std::string message;try {player_check::check_image(std::string(13,'\0')+text.substr(17,64)+std::string(20,'\0'),text);} catch(const std::runtime_error &e) {message=e.what();}LH_CONTAINS(message,"offset 13");
    LH_EQ(player_check::check_image(std::string(200,'\0'),std::string(200,'\0')).at("unique_guest_windows").integer(),0u);
}
static const char *const stops[]={"fault","limit"};
LH_TEST_P(test_player_stop_log_and_status,stops) {
    auto image=disc(t.temp()/"game.bin");bool limit=std::string(p)=="limit";auto cmd=play(t,{"--disc",image,"--fast","--mute"});cmd.args(limit?std::initializer_list<lh::Arg>{"--max-instructions","16"}:std::initializer_list<lh::Arg>{"--stall","4"});
    auto r=t.run(cmd);LH_EQ(r.rc,limit?0:1);auto end=r.out.find('\n'),next=r.out.find('\n',end+1);auto first=r.out.substr(0,end),second=r.out.substr(end+1,next-end-1);
    LH_CHECK(first.rfind(limit?"Stop: instruction budget exhausted":"Stop: stall:",0)==0);LH_CHECK(std::regex_match(second,std::regex("Last PC: [0-9a-f]{8}")));
    auto log=lh::read_file(t.temp()/"data/lionheart/logs/lionheart.log");if(limit) LH_NOT_CONTAINS(log,"Stop:");else {LH_CONTAINS(log,first+"\n"+second+"\n");LH_CONTAINS(r.err,first+"\n"+second+"\n");}
}
