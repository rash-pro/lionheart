// Synthetic controller schedules and guest movement assertions.
#include "lh_test.h"
#include "../../../tools/devtools/pad_support.h"
LH_TEST(test_facing_changes_do_not_count_as_walking) {
    auto idle = pad::field_positions("PAD_FIELD vblank=1850 map=bghoke_2 x=-696320 y=3366912 z=0 facing=64",1849,1851);
    auto turned = pad::field_positions("PAD_FIELD vblank=1850 map=bghoke_2 x=-696320 y=3366912 z=0 facing=75",1849,1851);
    LH_CHECK(!pad::walking_changed(idle,turned));
}
LH_TEST(test_walking_requires_corresponding_samples) {
    LH_CHECK(!pad::walking_changed({},{}));
    LH_CHECK(!pad::walking_changed({{1850,{"bghoke_2",0,0,0}}},{}));
    LH_CHECK(!pad::walking_changed({{1850,{"bghoke_2",0,0,0}}},{{1851,{"bghoke_2",1,0,0}}}));
}
LH_TEST(test_translation_and_field_transitions_count_as_walking) {
    pad::Positions idle{{1850,{"bghoke_2",1,2,3}}};
    LH_CHECK(pad::walking_changed(idle,{{1850,{"bghoke_2",2,2,3}}}));
    LH_CHECK(pad::walking_changed(idle,{{1850,{"bghoke_1",1,2,3}}}));
    idle[1851]=idle[1850];
    LH_CHECK(pad::walking_changed(idle,{{1850,{"bghoke_1",1,2,3}}}));
}
LH_TEST(test_probe_schedule_preserves_the_route_and_centers_idle_axes) {
    const char *modes[]={"idle","digital","axis"};
    const char *expected[]={"0000 0 0 0 0", "0020 0 0 0 0", "0000 32767 0 0 0"};
    auto path=t.temp()/"input.txt";
    for(int i=0;i<3;++i) {
        pad::write_sequence(path.string(),2400,2600,"RIGHT",{{1700,1900,"DOWN"}},modes[i]);
        auto text=lh::read_file(path);
        LH_CONTAINS(text,"1700 0040 0 0 0 0\n"); LH_CONTAINS(text,"1900 0000 0 0 0 0\n");
        LH_CONTAINS(text,std::string("2400 ")+expected[i]+"\n"); LH_CONTAINS(text,"2600 0000 0 0 0 0\n");
    }
}
LH_TEST(test_every_observed_poll_must_match_even_when_a_vblank_is_skipped) {
    std::string reply("\xff\x73\x5a\xdf\xff\x80\x80\x80\x80",9);
    std::vector<std::pair<int,std::string>> polls;
    for(int f=1701;f<1900;++f) if(f!=1727) polls.emplace_back(f,reply);
    LH_CHECK(pad::protocol_matches(polls,reply,1700,1900));
    polls[20].second=std::string("\xff\x73\x5a\xdf\xff\x7f\x80\x80\x80",9);
    LH_CHECK(!pad::protocol_matches(polls,reply,1700,1900));
    LH_CHECK(!pad::protocol_matches({},reply,1700,1900));
    polls.resize(3); LH_CHECK(!pad::protocol_matches(polls,reply,1700,1900));
}
