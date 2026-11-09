// Host settings parser: real files, defaults and CLI precedence.
#include "host_fixtures.h"
static lh::Command command(lh::Case &t, std::initializer_list<lh::Arg> args={}, bool automatic=false) {
    // The parser resolves adjacent settings from its executable, not its cwd.
    auto binary=t.temp()/"options";
    lh::fs::copy_file(t.harness("options").program(),binary,lh::fs::copy_options::overwrite_existing);
    auto cmd=t.target(binary).arg("unused.exe").args(args).env("HOME",t.temp().string()).env("XDG_CONFIG_HOME",(t.temp()/"xdg").string());
    bool config=false; for (const auto &a:args) if(a.text()=="--config") config=true;
    if(!automatic && !config) cmd.args({"--config","/dev/null"});
    return cmd;
}
static void prefix(const lh::Result &r,const std::string &text) { LH_EQ(r.out.rfind(text,0),0u); }
static std::string last(const lh::Result &r) { return lh::lines(r.out).back(); }
LH_TEST(test_defaults_and_classification) {
    auto r=t.run(command(t)); LH_EQ(r.rc,0);
    prefix(r,"0 1 0 0 0\npad L\nrumble P\nfullscreen P\nscale-mode P\nfilter P\n");
    std::map<std::string,std::string> classes;
    auto rows=lh::lines(r.out);
    for(size_t i=1;i<rows.size();++i) { std::istringstream in(rows[i]); std::string k,v; in>>k>>v; classes[k]=v; }
    for(auto key:{"renderer","pgxp","movies","state-bridge","achievements","load-state","save-at"}) LH_CHECK(classes.count(key));
    for(auto &kv:classes) LH_EQ(kv.second,(kv.first=="pad" || kv.first=="savestate-cards" ? "L":"P"));
}
LH_TEST(test_config_and_cli_precedence) {
    auto path=t.temp()/"custom.ini";
    lh::write_file(path," # settings\n pad = dualshock ; comment\nrumble=0\nfullscreen=1\nscale-mode=integer\nfilter=linear\nfuture-key=yes\n");
    auto r=t.run(command(t,{"--pad","digital","--config",path,"--windowed","--filter","nearest"}));
    LH_EQ(r.rc,0); prefix(r,"0 0 0 1 0\n"); LH_CONTAINS(r.err,"unknown key 'future-key' (ignored)");
}
LH_TEST(test_default_paths) {
    auto xdg=t.temp()/"xdg/ff8-recomp/ff8.ini"; lh::fs::create_directories(xdg.parent_path()); lh::write_file(xdg,"filter=linear\n");
    prefix(t.run(command(t,{},true)),"0 1 0 0 1\n");
    auto local=t.temp()/"ff8.ini"; lh::write_file(local,"pad=dualshock\n"); prefix(t.run(command(t,{},true)),"1 1 0 0 0\n"); lh::fs::remove(local);
    auto home=t.temp()/".config/ff8-recomp/ff8.ini"; lh::fs::create_directories(home.parent_path()); lh::write_file(home,"scale-mode=stretch\n");
    prefix(t.run(command(t,{},true).env("XDG_CONFIG_HOME","")),"0 1 0 2 0\n");
}
struct Text { const char *id,*text; };
struct Args { const char *id; std::vector<std::string> args; int expected; };
static const Text invalid_config[] = {
{"pad=analog\\n","pad=analog\n"},
{"rumble=2\\n","rumble=2\n"},
{"fullscreen=yes\\n","fullscreen=yes\n"},
{"filter=linear junk\\n","filter=linear junk\n"},
{"scale-mode=\\n","scale-mode=\n"},
{"[section]\\n","[section]\n"},
{"=value\\n","=value\n"},
{"text7","filter=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
};
static const Args invalid_cli[] = {
{"args0",{"--pad"},2},
{"args1",{"--pad","ANALOG"},2},
{"args2",{"--rumble","-1"},2},
{"args3",{"--config"},2},
{"args4",{"--config","missing.ini"},2},
{"args5",{"--filter","bilinear"},2},
{"args6",{"--scale-mode","crop"},2},
};
static const Args fullscreen_cli_values[] = {
{"args0-0",{"--fullscreen","0"},0},
{"args1-1",{"--fullscreen","1"},1},
{"args2-1",{"--fullscreen"},1},
{"args3-0",{"--fullscreen","1","--fullscreen","0"},0},
};
static const Args integrated_invalid_settings[] = {
{"args0",{"--internal-scale","9"},2},
{"args1",{"--msaa","3"},2},
{"args2",{"--save-at","-1"},2},
{"args3",{"--state-bridge","65536"},2},
{"args4",{"--state-bridge","fd:-1"},2},
{"args5",{"--pgxp","bad"},2},
};
static lh::Result run_args(lh::Case &t,const Args &p) {
    auto cmd=command(t,{},true); for(auto &a:p.args) cmd.arg(a);
    if(std::find(p.args.begin(),p.args.end(),"--config")==p.args.end()) cmd.args({"--config","/dev/null"});
    return t.run(cmd);
}
LH_TEST_P(test_invalid_config,invalid_config) {
    auto path=t.temp()/"bad.ini"; lh::write_file(path,p.text); auto r=t.run(command(t,{"--config",path})); LH_EQ(r.rc,2); LH_CONTAINS(r.err,"Config ");
}
LH_TEST_P(test_invalid_cli,invalid_cli) { LH_EQ(run_args(t,p).rc,2); }
LH_TEST_P(test_fullscreen_cli_values,fullscreen_cli_values) {
    auto r=run_args(t,p); LH_EQ(r.rc,0); std::istringstream in(lh::lines(r.out)[0]); int a,b,c; in>>a>>b>>c; LH_EQ(c,p.expected);
}
LH_TEST_P(test_integrated_invalid_settings,integrated_invalid_settings) { LH_EQ(run_args(t,p).rc,2); }
LH_TEST(test_explicit_empty_config_ignores_adjacent_file) {
    lh::write_file(t.temp()/"ff8.ini","pad=invalid-from-stray-config\n"); auto r=t.run(command(t,{"--config","/dev/null"})); LH_EQ(r.rc,0); prefix(r,"0 1 0 0 0\n");
}
LH_TEST(test_integrated_config_and_cli_precedence) {
    auto path=t.temp()/"pc.ini";
    lh::write_file(path,"renderer=vulkan\ninternal-scale=4\nmsaa=4\npgxp=perspective\nmovies=/tmp/user movies\nstate-bridge=8765\nsave-at=1900\n");
    auto r=t.run(command(t,{"--config",path,"--internal-scale","2","--pgxp","off"}).env("FF8_TEST_OPTIONS","1")); LH_EQ(r.rc,0); LH_EQ(last(r),"1 2 4 0 /tmp/user movies 8765 1900");
}
LH_TEST(test_card_policy_default_config_and_cli) {
    auto run=[&](std::initializer_list<lh::Arg> args){return t.run(command(t,args).env("FF8_TEST_CARDS","1"));};
    LH_EQ(last(run({})),"cards=live"); auto path=t.temp()/"cards.ini"; lh::write_file(path,"savestate-cards=snapshot\n");
    LH_EQ(last(run({"--config",path})),"cards=snapshot");
    auto r=run({"--savestate-cards","live","--config",path}); LH_EQ(r.rc,0); LH_EQ(last(r),"cards=live");
    r=run({"--savestate-cards","snapshot"}); LH_EQ(r.rc,0); LH_EQ(last(r),"cards=snapshot");
    LH_EQ(run({"--savestate-cards","rollback"}).rc,2); lh::write_file(path,"savestate-cards=rollback\n"); LH_EQ(run({"--config",path}).rc,2);
}
