// Comparison tools preserve the input card even with a conflicting output.
#include "lh_test.h"
#include "../../../tools/devtools/compare.h"
static const char *links[]={"False","True"};
LH_TEST_P(test_output_collision_preserves_input_card,links) {
    bool symlink=std::string(p)=="True"; auto card=t.temp()/"card.mcr";
    const std::string original="owned synthetic card";
    if(symlink) { lh::write_file(t.temp()/"source.mcr",original); lh::fs::create_symlink(t.temp()/"source.mcr",card); }
    else lh::write_file(card,original);
    bool caught=false;
    try { comparison::run(t.temp()/"unused-player",t.temp()/"unused-exe",{},t.temp()/"unused-script",1,t.temp(),card); }
    catch(const std::runtime_error &e) { caught=true; LH_CONTAINS(e.what(),"input card"); }
    LH_CHECK(caught); LH_BYTES_EQ(lh::read_file(card),original); LH_EQ(lh::fs::is_symlink(card),symlink);
}
