// The ctypes ABI and command-line presentation fixture shared by host cases.
#pragma once
#include "vulkan.h"
#include "rendering.h"
#include "oracles/image.h"
#include <regex>
namespace lh {
using Rect=oracle::Rect;
struct View { Rect frame,content; };
struct Timing { uint32_t horizontal,vertical,mode; };
constexpr Timing ntsc{600u|(3160u<<12),16u|(256u<<10),1};
inline bool same(Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.w==b.w && a.h==b.h;}
inline View viewport(Case &t,int width,int height,Timing timing=ntsc,int nw=320,int nh=240,int mode=0) {
    return t.library("viewport").fn<View(Timing,int,int,int,int,int)>("host_viewport")(timing,nw,nh,width,height,mode);
}
inline oracle::PngImage present(Case &t,const fs::path &tool,const std::string &rgb,int width,int height,
    const std::string &preset,Timing timing=ntsc,int nw=320,int nh=240,int scale=1,
    const std::string &filter="nearest",const std::string &scale_mode="aspect") {
    const auto src=t.temp()/(preset+"-in.rgb"),dst=t.temp()/(preset+"-out.rgb");write_file(src,rgb);
    LH_EQ(rgb.size(),size_t(nw)*nh*scale*scale*3);
    auto run=t.ok(t.host(tool.string()).args({src,nw*scale,nh*scale,"--scale",scale,"--native",nw,nh,
        "--timing",timing.horizontal,timing.vertical,timing.mode,"--size",std::to_string(width)+"x"+std::to_string(height),
        "--crt",preset,"--filter",filter,"--scale-mode",scale_mode,"--out",dst})
        .env("SDL_VIDEODRIVER","offscreen").env("SDL_AUDIODRIVER","dummy").timeout(300));
    LH_EQ(json_integer(run.out,"width"),width);LH_EQ(json_integer(run.out,"height"),height);
    return {unsigned(width),unsigned(height),read_file(dst)};
}
inline void shader_contract(Case &t,const std::string &name) {
    const auto shaders=t.root()/"backends/host/shaders",prebuilt=shaders/"prebuilt"/(name+".inc");
    const auto text=read_file(prebuilt);std::regex re("0x[0-9a-fA-F]+");std::vector<uint32_t> words;
    for(auto i=std::sregex_iterator(text.begin(),text.end(),re);i!=std::sregex_iterator();++i) words.push_back(uint32_t(std::stoul(i->str(),nullptr,16)));
    LH_CHECK(words.size()>5);LH_EQ(words[0],0x07230203u);
    std::string bytes;for(auto word:words) little(bytes,word);auto spv=t.temp()/"shader.spv";write_file(spv,bytes);
    if(executable_on_path("spirv-val")) t.ok(t.host("spirv-val").arg(spv));
    if(!executable_on_path("glslc")) skip("glslc is not installed; prebuilt SPIR-V only validated");
    const auto rebuilt=t.temp()/(name+".inc");
    t.ok(t.host("glslc").args({"-O","-mfmt=c","-o",rebuilt,shaders/name}).cwd(shaders));
    if(t.ok(t.host("glslc").arg("--version")).out!=read_file(shaders/"prebuilt/GLSLC_VERSION"))
        skip("prebuilt SPIR-V comes from another glslc build; source compiled and prebuilt validated");
    LH_CHECK_MSG(read_file(rebuilt)==text,"run backends/host/shaders/rebuild.sh after editing "+name);
}
}
