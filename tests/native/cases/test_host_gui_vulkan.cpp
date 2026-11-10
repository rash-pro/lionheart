// Real offscreen Vulkan GUI pixels, atlas lifecycle and attachment formats.
#include "vulkan.h"
#include "lh_json.h"
#include <regex>
namespace {
LH_TEST(test_vulkan_gui_pixels_formats_font_rebuild_resize_and_reinitialize) {
    lh::vulkan_replay(t);
    if(!lh::executable_on_path("c++") || !lh::executable_on_path("cc"))
        lh::skip("A C/C++ toolchain are required for the Vulkan GUI contract");
    auto command=t.harness("gui").timeout(90);
    const lh::fs::path layers="/usr/share/vulkan/explicit_layer.d";
    if(lh::fs::exists(layers)) for(const auto &entry:lh::fs::directory_iterator(layers)) {
        const auto name=entry.path().filename().string();
        if(std::regex_match(name,std::regex(".*khronos.*validation.*\\.json")))
            command.env("VK_INSTANCE_LAYERS","VK_LAYER_KHRONOS_validation").env("VK_LAYER_ENABLES","VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
    }
    auto run=t.ok(command);
    const auto result=lh::Json::parse(run.out);
    LH_EQ(result.size(),size_t(7));
    for(auto key:{"passed","font_rebuild","resize","texture_text"}) LH_CHECK(result[key].boolean());
    LH_EQ(result["frames"].integer(),12);
    LH_EQ(result["reinitializations"].integer(),2);
    const auto &formats=result["formats"].items();
    LH_EQ(formats.size(),size_t(2));LH_EQ(formats[0].text(),"RGBA");LH_EQ(formats[1].text(),"BGRA");
    LH_NOT_CONTAINS(run.err,"Validation Error");LH_NOT_CONTAINS(run.err,"SYNC-HAZARD");
}
} // namespace
