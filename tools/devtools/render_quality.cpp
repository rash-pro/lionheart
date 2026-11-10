#include "render_quality.h"
#include <iostream>
int main(int argc, char **argv) {
    try {
        std::string off, on, scene, output;
        int budget=8;
        for (int i=1; i<argc; ++i) {
            std::string key=argv[i];
            if(key=="--help" || key=="-h") {
                std::cout<<"usage: render-quality --off DIR --on DIR --scene {battle,world,dorm} --output FILE [--max-crack-pixels N]\n\nCount new clear-color holes inside opaque 3D regions of 4x replay images.\n";
                return 0;
            }
            std::string value;
            const auto equal=key.find('=');
            if(equal!=std::string::npos) {value=key.substr(equal+1);key.resize(equal);}
            else {
                if (i+1==argc) throw std::runtime_error("missing value for " + key);
                value=argv[++i];
            }
            if (key=="--off") off=value; else if (key=="--on") on=value;
            else if (key=="--scene") scene=value; else if (key=="--output") output=value;
            else if (key=="--max-crack-pixels") { size_t used=0; budget=std::stoi(value,&used); if (used!=value.size()) throw std::runtime_error("invalid budget"); }
            else throw std::runtime_error("unknown option: " + key);
        }
        if (off.empty() || on.empty() || output.empty() || (scene!="battle" && scene!="world" && scene!="dorm"))
            throw std::runtime_error("required: --off DIR --on DIR --scene battle|world|dorm --output FILE");
        std::vector<std::filesystem::path> paths;
        for (const auto &entry : std::filesystem::directory_iterator(off)) if (entry.path().extension()==".png") paths.push_back(entry.path());
        std::sort(paths.begin(),paths.end());
        dt::Json frames=dt::Json::array();
        int64_t maximum=0, total=0, raw=0;
        for (const auto &path : paths) {
            const std::string stem=path.stem().string(); size_t used=0;
            const std::string suffix=stem.substr(stem.find_last_of('-')+1);
            int frame=std::stoi(suffix,&used);
            if (used!=suffix.size()) throw std::runtime_error("invalid frame number");
            if (frame<11 || frame>20) continue;
            auto c=quality::compare(lh::oracle::read_png(dt::read(path)),lh::oracle::read_png(dt::read(std::filesystem::path(on)/path.filename())),scene);
            dt::Json row=dt::Json::Object{{"frame",frame},{"raw_pixels",c.raw},{"interior_pixels",c.interior},{"roi_pixels",c.roi}};
            frames.push(row); maximum=std::max(maximum,c.interior); total+=c.interior; raw+=c.raw;
        }
        if (std::get<dt::Json::Array>(frames.value).size()!=10) throw std::runtime_error("ten consecutive completed replay frames required");
        dt::Json result=dt::Json::Object{{"scene",scene},{"off",off},{"on",on},{"frames",frames},{"max_interior",maximum},{"total_interior",total},{"total_raw",raw},{"budget",budget},{"pass",maximum<=budget}};
        dt::write(output,result.dump(2)+"\n");
        std::cout<<dt::Json(dt::Json::Object{{"scene",scene},{"max_interior",maximum},{"total_interior",total},{"total_raw",raw},{"pass",maximum<=budget}}).dump()<<'\n';
        return maximum<=budget ? 0 : 1;
    } catch (const std::exception &e) { std::cerr<<"render-quality: "<<e.what()<<'\n'; return 2; }
}
