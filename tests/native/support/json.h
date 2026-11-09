// Read optional vector documents without requiring an external interpreter.
#pragma once
#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace lh {
struct Json {
    enum Kind { null, boolean, number, string, array, object } kind=null;
    std::string text;
    std::vector<Json> items;
    std::map<std::string,Json> members;
    const Json &at(const std::string &key) const { return members.at(key); }
    int64_t integer() const {
        if(kind!=number) throw std::runtime_error("expected JSON integer");
        size_t end=0; auto n=std::stoll(text,&end);
        if(end!=text.size()) throw std::runtime_error("expected JSON integer");
        return n;
    }
};
class JsonReader {
    const std::string &source;
    size_t pos=0;
    [[noreturn]] void error() const { throw std::runtime_error("invalid JSON at byte "+std::to_string(pos)); }
    void space() { while(pos<source.size()&&std::isspace(static_cast<unsigned char>(source[pos]))) ++pos; }
    bool take(char c) { space(); if(pos<source.size()&&source[pos]==c) { ++pos; return true; } return false; }
    char get() { if(pos==source.size()) error(); return source[pos++]; }
    unsigned hex() {
        unsigned n=0;
        for(int i=0;i<4;++i) {
            char c=get(); unsigned digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
            if(digit==16) error();
            n=n*16+digit;
        } return n;
    }
    static void utf8(std::string &s,unsigned n) {
        if(n<128) s+=char(n);
        else if(n<2048) { s+=char(0xc0|(n>>6)); s+=char(0x80|(n&63)); }
        else if(n<65536) { s+=char(0xe0|(n>>12)); s+=char(0x80|((n>>6)&63)); s+=char(0x80|(n&63)); }
        else { s+=char(0xf0|(n>>18)); s+=char(0x80|((n>>12)&63)); s+=char(0x80|((n>>6)&63)); s+=char(0x80|(n&63)); }
    }
    std::string quoted() {
        if(!take('"')) error();
        std::string s;
        for(;;) {
            unsigned char c=get();
            if(c=='"') return s;
            if(c<32) error();
            if(c!='\\') { s+=char(c); continue; }
            switch(get()) {
            case '"': s+='"'; break; case '\\': s+='\\'; break; case '/': s+='/'; break;
            case 'b': s+='\b'; break; case 'f': s+='\f'; break; case 'n': s+='\n'; break;
            case 'r': s+='\r'; break; case 't': s+='\t'; break;
            case 'u': {
                unsigned n=hex();
                if(n>=0xd800&&n<=0xdbff) {
                    if(get()!='\\'||get()!='u') error();
                    unsigned low=hex(); if(low<0xdc00||low>0xdfff) error();
                    n=0x10000+((n-0xd800)<<10)+(low-0xdc00);
                } else if(n>=0xdc00&&n<=0xdfff) error();
                utf8(s,n); break;
            }
            default: error();
            }
        }
    }
    Json value(unsigned depth) {
        if(depth>128) error();
        space(); if(pos==source.size()) error();
        Json out;
        if(source[pos]=='"') { out.kind=Json::string; out.text=quoted(); }
        else if(take('[')) {
            out.kind=Json::array;
            if(!take(']')) { do { out.items.push_back(value(depth+1)); } while(take(',')); if(!take(']')) error(); }
        } else if(take('{')) {
            out.kind=Json::object;
            if(!take('}')) {
                do { auto key=quoted(); if(!take(':')) error(); out.members[key]=value(depth+1); } while(take(','));
                if(!take('}')) error();
            }
        } else {
            const size_t start=pos;
            while(pos<source.size()&&source[pos]!=','&&source[pos]!=']'&&source[pos]!='}'&&!std::isspace(static_cast<unsigned char>(source[pos]))) ++pos;
            out.text=source.substr(start,pos-start);
            if(out.text=="null") out.kind=Json::null;
            else if(out.text=="true"||out.text=="false") out.kind=Json::boolean;
            else {
                out.kind=Json::number;
                // Vector numbers are integers; reject malformed and fractional input.
                if(out.text.empty()) error();
                size_t i=out.text[0]=='-'?1:0;
                if(i==out.text.size()||(out.text[i]=='0'&&i+1<out.text.size())) error();
                for(;i<out.text.size();++i) if(out.text[i]<'0'||out.text[i]>'9') error();
                if(out.text.empty()) error();
            }
        } return out;
    }
public:
    explicit JsonReader(const std::string &s):source(s) {}
    Json read() { auto out=value(0); space(); if(pos!=source.size()) error(); return out; }
};
inline Json read_json(const std::string &source) { return JsonReader(source).read(); }
}
