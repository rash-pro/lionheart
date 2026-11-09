#pragma once
#include "iro_extract.h"
#include <charconv>
#include <cmath>
#include <variant>
namespace dt {
struct Json {
    using Array=std::vector<Json>;
    using Object=std::vector<std::pair<std::string,Json>>;
    std::variant<std::nullptr_t,bool,int64_t,double,std::string,Array,Object> value;
    Json():value(nullptr) {} Json(bool v):value(v) {} Json(int v):value(int64_t(v)) {} Json(int64_t v):value(v) {}
    Json(double v):value(v) {} Json(const char *v):value(std::string(v)) {} Json(std::string v):value(std::move(v)) {}
    Json(Array v):value(std::move(v)) {} Json(Object v):value(std::move(v)) {}
    static Json object() { return Object{}; } static Json array() { return Array{}; }
    Json &set(std::string key,Json v) { auto &o=std::get<Object>(value); for(auto &p:o) if(p.first==key) { p.second=std::move(v); return *this; } o.emplace_back(std::move(key),std::move(v)); return *this; }
    const Json &at(const std::string &key) const { for(const auto &p:std::get<Object>(value)) if(p.first==key) return p.second; throw std::runtime_error("missing JSON key: "+key); }
    void push(Json v) { std::get<Array>(value).push_back(std::move(v)); }
    std::string dump(int indent=-1,int depth=0) const {
        if(std::holds_alternative<std::nullptr_t>(value)) return "null";
        if(auto p=std::get_if<bool>(&value)) return *p?"true":"false";
        if(auto p=std::get_if<int64_t>(&value)) return std::to_string(*p);
        if(auto p=std::get_if<double>(&value)) {
            if(std::isnan(*p)) return "NaN";
            if(std::isinf(*p)) return *p<0?"-Infinity":"Infinity";
            char b[512]; double a=std::abs(*p);
            auto fmt=(a==0 || (a>=1e-4&&a<1e16))?std::chars_format::fixed:std::chars_format::scientific;
            auto r=std::to_chars(b,b+sizeof(b),*p,fmt); std::string s(b,r.ptr);
            if(s.find_first_of(".e")==std::string::npos) s+=".0";
            return s;
        }
        if(auto p=std::get_if<std::string>(&value)) return iro::quoted(*p);
        bool object=std::holds_alternative<Object>(value);
        size_t n=object?std::get<Object>(value).size():std::get<Array>(value).size();
        std::string s=object?"{":"[";
        for(size_t i=0;i<n;++i) {
            if(i) s+=indent<0?", ":",";
            if(indent>=0) s+='\n'+std::string((depth+1)*indent,' ');
            const Json *v;
            if(object) { const auto &p=std::get<Object>(value)[i]; s+=iro::quoted(p.first)+": "; v=&p.second; }
            else v=&std::get<Array>(value)[i];
            s+=v->dump(indent,depth+1);
        }
        if(n&&indent>=0) s+='\n'+std::string(depth*indent,' ');
        return s+(object?"}":"]");
    }
};
inline std::string read(const std::filesystem::path &p) { std::ifstream in(p,std::ios::binary); if(!in) throw std::runtime_error("cannot read: "+p.string()); return {std::istreambuf_iterator<char>(in),{}}; }
inline void write(const std::filesystem::path &p,const std::string &s) { std::ofstream out(p,std::ios::binary); out.exceptions(std::ios::failbit|std::ios::badbit); out<<s; }
}
