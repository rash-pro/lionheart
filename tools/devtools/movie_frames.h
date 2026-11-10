// Per-tile movie checks retain narrow misplaced bands that a frame mean hides.
#pragma once
#include "json.h"
#include "../../tests/native/oracles/image.h"
#include <iomanip>
namespace movie_frames {
using lh::oracle::PngImage;
inline std::string pack_rgb15(std::string pixels) {
    for(char &c:pixels) {unsigned n=std::min((unsigned(uint8_t(c))+4)/8,31u);c=char((n<<3)|(n>>2));}return pixels;
}
inline std::vector<double> tile_errors(const PngImage &displayed,const PngImage &reference) {
    if(displayed.width!=320 || displayed.height!=224 || reference.width!=320 || reference.height!=224)
        throw std::runtime_error("movie dimensions must be 320x224 RGB");
    std::vector<double> errors(14*20);
    for(unsigned y=0;y<224;++y) for(unsigned x=0;x<320;++x) for(unsigned c=0;c<3;++c) {
        size_t i=(size_t(y)*320+x)*3;
        errors[(y/16)*20+x/16]+=std::abs(int(uint8_t(displayed.rgb.at(i+c)))-int(uint8_t(reference.rgb.at(i+c))));
    }
    for(double &error:errors) error/=16*16*3;
    return errors;
}
inline bool rgb15(const PngImage &image) {
    for(uint8_t c:image.rgb) {bool found=false;for(unsigned n=0;n<32;++n) if(c==((n<<3)|(n>>2))) found=true;if(!found) return false;}return true;
}
inline double mean(const std::vector<double> &values) {return std::accumulate(values.begin(),values.end(),0.0)/values.size();}
inline dt::Json compare_frames(const std::vector<PngImage> &reference,const std::filesystem::path &directory,int first,int last,double tolerance=3.0,int radius=4) {
    dt::Json failures=dt::Json::array(),anchors=dt::Json::object();
    double maximum=0,total=0;int checked=0,packed_vblanks=0,cached_best=-1;bool packed=false;
    std::string previous;std::vector<double> errors;
    for(int number=first;number<=last;++number) {
        std::ostringstream name;name<<"frame-"<<std::setfill('0')<<std::setw(6)<<number<<".png";auto path=directory/name.str();
        if(!std::filesystem::exists(path)) throw std::filesystem::filesystem_error("movie frame is missing",path,std::make_error_code(std::errc::no_such_file_or_directory));
        auto displayed=lh::oracle::read_png(dt::read(path));
        if(displayed.width!=320 || displayed.height!=224) throw std::runtime_error("movie dimensions must be 320x224 RGB");
        int expected=(number-first)/4;
        int low=std::max(0,expected-radius),high=std::min(int(reference.size()),expected+radius+1);
        if(low>=high) throw std::runtime_error("capture extends past the reference movie");
        if(previous.empty() || displayed.rgb!=previous || cached_best<low || cached_best>=high) {
            const bool can_pack=rgb15(displayed);double minimum=std::numeric_limits<double>::infinity();
            for(int frame=low;frame<high;++frame) {
                const auto &ref=reference[frame];auto packed_ref=can_pack?pack_rgb15(ref.rgb):std::string();double score=0,packed_score=0;
                for(unsigned y=0;y<224;y+=8) for(unsigned x=0;x<320;x+=8) for(unsigned c=0;c<3;++c) {
                    const size_t i=(size_t(y)*320+x)*3+c;
                    score+=std::abs(int(uint8_t(ref.rgb.at(i)))-int(uint8_t(displayed.rgb.at(i))));
                    if(can_pack) packed_score+=std::abs(int(uint8_t(packed_ref.at(i)))-int(uint8_t(displayed.rgb.at(i))));
                }
                if(can_pack) score=std::min(score,packed_score);
                if(score<minimum) {minimum=score;cached_best=frame;}
            }
            errors=tile_errors(displayed,reference[cached_best]);packed=false;
            if(can_pack) {
                auto packed_errors=tile_errors(displayed,{320,224,pack_rgb15(reference[cached_best].rgb)});
                if(mean(packed_errors)<mean(errors)) {errors=std::move(packed_errors);packed=true;}
            }
            previous=displayed.rgb;
        }
        const double worst=*std::max_element(errors.begin(),errors.end());maximum=std::max(maximum,worst);total+=mean(errors);++checked;packed_vblanks+=packed;
        const std::string key=std::to_string(cached_best);auto &items=std::get<dt::Json::Object>(anchors.value);
        if(std::none_of(items.begin(),items.end(),[&](const auto &item){return item.first==key;})) anchors.set(key,number);
        dt::Json bad=dt::Json::array();for(size_t i=0;i<errors.size();++i) if(errors[i]>tolerance) bad.push(dt::Json::Array{int(i/20),int(i%20)});
        if(!std::get<dt::Json::Array>(bad.value).empty()) failures.push(dt::Json::Object{{"png",path.string()},{"reference_frame",cached_best},{"max_tile_error",worst},{"bad_tiles_yx",bad}});
    }
    return dt::Json::Object{{"checked_vblanks",checked},{"first_png",first},{"last_png",last},{"reference_frames",int(reference.size())},
        {"tolerance_rgb_levels",tolerance},{"max_tile_error",maximum},{"mean_pixel_error",total/checked},{"rgb15_reference_vblanks",packed_vblanks},
        {"bad_displayed_frames",int(std::get<dt::Json::Array>(failures.value).size())},{"failures",failures},{"first_png_by_reference_frame",anchors}};
}
inline bool matched_anchor(double left,double right,double first,double second) {
    return std::isfinite(left) && std::isfinite(right) && std::max(std::abs(left),std::abs(right))>.85 && std::abs(first-second)<=4.0/44100;
}
}
