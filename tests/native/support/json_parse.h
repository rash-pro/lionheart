// Read structured build receipts without executing their command text in a shell.
#pragma once
#include "../../../tools/devtools/json.h"
#include <cctype>
namespace lh {
class JsonParser {
    const std::string &text_;size_t at_=0;
    void space() {while(at_<text_.size() && std::isspace(uint8_t(text_[at_]))) ++at_;}
    char next() {if(at_==text_.size()) throw std::runtime_error("truncated JSON receipt");return text_[at_++];}
    void expect(char c) {space();if(next()!=c) throw std::runtime_error("invalid JSON receipt");}
    unsigned hex() {unsigned value=0;for(int i=0;i<4;++i) {char c=next();int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(digit<0) throw std::runtime_error("invalid JSON Unicode escape");value=value*16+unsigned(digit);}return value;}
    static void utf8(std::string &s,unsigned c) {
        if(c<128) s+=char(c);else if(c<2048) {s+=char(192|(c>>6));s+=char(128|(c&63));}
        else if(c<65536) {s+=char(224|(c>>12));s+=char(128|((c>>6)&63));s+=char(128|(c&63));}
        else {s+=char(240|(c>>18));s+=char(128|((c>>12)&63));s+=char(128|((c>>6)&63));s+=char(128|(c&63));}
    }
    std::string string() {
        expect('"');std::string s;
        for(char c=next();c!='"';c=next()) {
            if(c!='\\') {if(uint8_t(c)<32) throw std::runtime_error("invalid JSON control byte");s+=c;continue;}
            c=next();if(c=='u') {unsigned code=hex();if(code>=0xd800 && code<=0xdbff) {if(next()!='\\' || next()!='u') throw std::runtime_error("missing JSON low surrogate");unsigned low=hex();if(low<0xdc00 || low>0xdfff) throw std::runtime_error("invalid JSON low surrogate");code=0x10000+((code-0xd800)<<10)+(low-0xdc00);}utf8(s,code);}
            else if(c=='n') s+='\n';else if(c=='r') s+='\r';else if(c=='t') s+='\t';else if(c=='b') s+='\b';else if(c=='f') s+='\f';
            else if(c=='"' || c=='\\' || c=='/') s+=c;else throw std::runtime_error("invalid JSON escape");
        }return s;
    }
    dt::Json value() {
        space();if(at_==text_.size()) throw std::runtime_error("missing JSON value");char c=text_[at_];
        if(c=='"') return string();
        if(c=='[' || c=='{') {
            ++at_;const char close=c=='['?']':'}';dt::Json result=c=='['?dt::Json::array():dt::Json::object();space();
            if(at_<text_.size() && text_[at_]==close) {++at_;return result;}
            for(;;) {
                if(c=='{') {auto key=string();expect(':');result.set(key,value());}else result.push(value());
                space();char end=next();if(end==close) return result;if(end!=',') throw std::runtime_error("invalid JSON separator");
            }
        }
        for(auto literal:{"true","false","null"}) if(text_.compare(at_,std::char_traits<char>::length(literal),literal)==0) {
            at_+=std::char_traits<char>::length(literal);return std::string(literal)=="null"?dt::Json():dt::Json(std::string(literal)=="true");
        }
        size_t length=0;double number=std::stod(text_.substr(at_),&length);at_+=length;return number;
    }
public:
    explicit JsonParser(const std::string &text):text_(text) {}
    dt::Json parse() {auto result=value();space();if(at_!=text_.size()) throw std::runtime_error("data after JSON receipt");return result;}
};
inline dt::Json parse_json(const std::string &text) {return JsonParser(text).parse();}
inline const dt::Json *json_find(const dt::Json &object,const std::string &key) {
    for(const auto &p:std::get<dt::Json::Object>(object.value)) if(p.first==key) return &p.second;
    return nullptr;
}
inline std::string json_string(const dt::Json &json) {return std::get<std::string>(json.value);}
}
