// Player session behavior and bounded portable hashing, without private media.
#include "host_fixtures.h"
#include <csignal>
static lh::Command session(lh::Case &t) {
    return t.harness("session").env("XDG_DATA_HOME",(t.temp()/"data").string());
}
static lh::fs::path log_path(lh::Case &t) { return t.temp()/"data/lionheart/logs/lionheart.log"; }
LH_TEST(test_session_tee_rotation_and_fatal_box) {
    auto identity=t.ok(session(t).arg("--version")).out;
    auto expected=std::string("fatal startup: supply disc 1\n")+std::string(10000,'x');
    for(int n=0;n<7;++n) {
        auto cmd=lh::dummy(session(t)).env("HOME",t.temp().string()); if(n<6) cmd.arg("--headless");
        auto r=t.run(cmd); LH_EQ(r.rc,2); LH_TEXT_EQ(r.err,expected);
        LH_EQ(r.out.find("messagebox title=Lionheart text=fatal startup")!=std::string::npos,n==6);
    }
    auto files=lh::files_starting(log_path(t).parent_path(),""); LH_EQ(files.size(),5u);
    std::vector<std::string> names;
    for(auto &file:files) { names.push_back(file.filename().string()); LH_TEXT_EQ(lh::read_file(file),identity+expected); }
    LH_CHECK((names==std::vector<std::string>{"lionheart.1.log","lionheart.2.log","lionheart.3.log","lionheart.4.log","lionheart.log"}));
}
LH_TEST(test_gui_diagnostic_errors_never_open_native_fatal_dialog) {
    auto r=t.run(session(t).args({"--gui-test","unused-script.txt"})); LH_EQ(r.rc,2); LH_CONTAINS(r.err,"fatal startup"); LH_NOT_CONTAINS(r.out,"messagebox");
}
LH_TEST(test_streaming_sha1_padding_and_chunks) {
    for(int size:{0,1,55,56,63,64,65,65535,65536,1048583}) {
        std::string data; for(int i=0;i<size;++i) data+=char(i%251);
        auto path=t.temp()/"input"; lh::write_file(path,data);
        auto r=t.ok(t.harness("sha").stdin_file(path));
        auto expected=t.ok(t.host("sha1sum").arg(path)).out.substr(0,40);
        LH_EQ(r.out,expected);
    }
}
LH_TEST(test_log_directory_failure_has_fatal_diagnostic) {
    auto blocked=t.temp()/"not-a-directory"; lh::write_file(blocked,"owned test file");
    auto r=t.run(lh::dummy(session(t)).env("XDG_DATA_HOME",blocked.string()));
    LH_EQ(r.rc,2); LH_CONTAINS(r.err,"Cannot create Lionheart log directory: cannot use "+blocked.string()+": "); LH_CONTAINS(r.out,"messagebox title=Lionheart"); LH_EQ(lh::read_file(blocked),"owned test file");
}
struct Signal { const char *id; int number; };
static const Signal signals[]={{"11",SIGSEGV},{"6",SIGABRT},{"8",SIGFPE},{"4",SIGILL},{"7",SIGBUS}};
LH_TEST_P(test_player_session_fatal_signal,signals) {
    auto r=t.run(session(t).args({"--crash",p.number}).timeout(10)); LH_EQ(r.rc,-p.number);
    char expected[80]; std::snprintf(expected,sizeof(expected),"Fatal signal 0x%08x\n",p.number);
    LH_EQ(lh::read_file(log_path(t)),t.ok(session(t).arg("--version")).out+expected);
}
LH_TEST(test_player_session_abnormal_stop_has_no_dialog) {
    auto r=t.run(session(t).arg("--abnormal").timeout(10)); LH_EQ(r.rc,1); LH_EQ(r.out,"");
    LH_EQ(r.err,"Stop: test fault\nLast PC: 80010000\n"); LH_EQ(lh::read_file(log_path(t)),t.ok(session(t).arg("--version")).out+r.err);
}
