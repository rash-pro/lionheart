// Preserve object order so catalog deduplication retains the versioned order.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace player_check {
inline void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
inline std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "cannot read " + path);
    std::ostringstream out; out << in.rdbuf();
    require(!in.bad(), "cannot read " + path);
    return out.str();
}
struct Json {
    enum Kind { null, boolean, number, string, array, object } kind = null;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    Json() = default;
    Json(const char *s) : kind(string), text(s) {}
    Json(std::string s) : kind(string), text(std::move(s)) {}
    Json(bool b) : kind(boolean), text(b ? "true" : "false") {}
    Json(size_t n) : kind(number), text(std::to_string(n)) {}
    static Json list() { Json j; j.kind = array; return j; }
    static Json dict() { Json j; j.kind = object; return j; }
    const Json *find(const std::string &key) const {
        for (auto &p : members) if (p.first == key) return &p.second;
        return nullptr;
    }
    const Json &at(const std::string &key) const {
        auto p = find(key); require(kind == object && p, "missing JSON field: " + key); return *p;
    }
    Json &set(const std::string &key, Json value) {
        for (auto &p : members) if (p.first == key) { p.second = std::move(value); return *this; }
        members.emplace_back(key, std::move(value)); return *this;
    }
    std::string str() const { require(kind == string, "expected JSON string"); return text; }
    size_t integer() const {
        require(kind == number && !text.empty() && text.find_first_not_of("0123456789") == std::string::npos,
                "expected nonnegative JSON integer");
        size_t n = 0; for (char c : text) { require(n <= (SIZE_MAX - (c-'0')) / 10, "JSON integer overflow"); n=n*10+c-'0'; }
        return n;
    }
    bool truth() const { require(kind == boolean, "expected JSON boolean"); return text == "true"; }
    const std::vector<Json> &values() const { require(kind == array, "expected JSON array"); return items; }
    std::string dump() const {
        if (kind == null) return "null";
        if (kind == boolean || kind == number) return text;
        if (kind == string) {
            std::string out="\""; const char *hex="0123456789abcdef";
            for (unsigned char c : text) {
                if (c=='"' || c=='\\') { out+='\\'; out+=char(c); }
                else if (c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
                else out+=char(c);
            }
            return out+'"';
        }
        std::string out = kind == array ? "[" : "{"; bool first=true;
        if (kind == array) for (auto &v : items) { if (!first) out+=','; first=false; out+=v.dump(); }
        else for (auto &p : members) { if (!first) out+=','; first=false; out+=Json(p.first).dump()+':'+p.second.dump(); }
        return out+(kind==array ? ']' : '}');
    }
};
class JsonParser {
    const std::string &s; size_t p=0;
    void space() { while (p<s.size() && (s[p]==' ' || s[p]=='\n' || s[p]=='\r' || s[p]=='\t')) ++p; }
    char take() { require(p<s.size(), "truncated JSON"); return s[p++]; }
    unsigned hex4() {
        unsigned n=0; for (int i=0;i<4;++i) { char c=take(); unsigned v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
            require(v<16,"invalid JSON escape"); n=n*16+v; } return n;
    }
    std::string quoted() {
        require(take()=='"',"expected JSON string"); std::string out;
        for (;;) {
            unsigned char c=take(); if (c=='"') return out;
            require(c>=32,"control character in JSON string");
            if (c!='\\') { out+=char(c); continue; }
            char e=take();
            switch(e) {
            case '"': case '\\': case '/': out+=e; break;
            case 'b': out+='\b'; break; case 'f': out+='\f'; break; case 'n': out+='\n'; break;
            case 'r': out+='\r'; break; case 't': out+='\t'; break;
            case 'u': {
                unsigned u=hex4();
                if (u>=0xd800 && u<=0xdbff) { require(take()=='\\' && take()=='u',"invalid JSON surrogate"); unsigned lo=hex4();
                    require(lo>=0xdc00&&lo<=0xdfff,"invalid JSON surrogate"); u=0x10000+((u-0xd800)<<10)+(lo-0xdc00); }
                require(u<0xd800 || u>0xdfff,"invalid JSON surrogate");
                if (u<0x80) out+=char(u);
                else if(u<0x800) { out+=char(0xc0|(u>>6)); out+=char(0x80|(u&63)); }
                else if(u<0x10000) { out+=char(0xe0|(u>>12)); out+=char(0x80|((u>>6)&63)); out+=char(0x80|(u&63)); }
                else { out+=char(0xf0|(u>>18)); out+=char(0x80|((u>>12)&63)); out+=char(0x80|((u>>6)&63)); out+=char(0x80|(u&63)); }
                break;
            }
            default: require(false,"invalid JSON escape");
            }
        }
    }
    Json value(unsigned depth) {
        require(depth<256,"JSON nesting too deep"); space(); require(p<s.size(),"truncated JSON");
        if(s[p]=='"') return Json(quoted());
        if(s[p]=='[' || s[p]=='{') {
            bool object=s[p++]=='{'; Json j=object?Json::dict():Json::list(); space();
            char close=object?'}':']'; if(p<s.size()&&s[p]==close) { ++p; return j; }
            for (;;) {
                space(); std::string key; if(object) { key=quoted(); space(); require(take()==':',"expected JSON colon"); }
                Json v=value(depth+1); if(object) j.set(key,std::move(v)); else j.items.push_back(std::move(v));
                space(); char c=take(); if(c==close) return j; require(c==',',"expected JSON comma");
            }
        }
        for(auto token:{"null","true","false"}) { size_t n=std::char_traits<char>::length(token);
            if(s.compare(p,n,token)==0) { p+=n; if(token[0]=='n') return {}; return Json(token[0]=='t'); } }
        size_t start=p; if(s[p]=='-') ++p;
        require(p<s.size()&&std::isdigit(static_cast<unsigned char>(s[p])),"invalid JSON value");
        if(s[p]=='0') ++p; else while(p<s.size()&&std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
        if(p<s.size()&&s[p]=='.') { ++p; size_t b=p; while(p<s.size()&&std::isdigit(static_cast<unsigned char>(s[p]))) ++p; require(p>b,"invalid JSON number"); }
        if(p<s.size()&&(s[p]=='e'||s[p]=='E')) { ++p; if(p<s.size()&&(s[p]=='+'||s[p]=='-')) ++p; size_t b=p;
            while(p<s.size()&&std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
            require(p>b,"invalid JSON exponent"); }
        Json j; j.kind=Json::number; j.text=s.substr(start,p-start); return j;
    }
public:
    explicit JsonParser(const std::string &input) : s(input) {}
    Json parse() { Json j=value(0); space(); require(p==s.size(),"trailing JSON content"); return j; }
};
inline Json parse(const std::string &s) { return JsonParser(s).parse(); }
inline Json load_json(const std::string &p) { return parse(read_file(p)); }
} // namespace player_check
