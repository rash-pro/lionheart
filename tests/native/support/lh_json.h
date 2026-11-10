// Independent JSON values for protocol assertions; no host parser is reused.
#pragma once
#include "lh_test.h"
#include <cctype>
#include <iomanip>
#include <variant>

namespace lh {
class Json {
  public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : value_(v) {}
    Json(int v) : value_(int64_t(v)) {}
    Json(int64_t v) : value_(v) {}
    Json(double v) : value_(v) {}
    Json(const char *v) : value_(std::string(v)) {}
    Json(std::string v) : value_(std::move(v)) {}
    Json(Object v) : value_(std::move(v)) {}
    Json(Array v) : value_(std::move(v)) {}
    const Json &operator[](const std::string &key) const {
        const auto &o = std::get<Object>(value_);
        auto i = o.find(key);
        if (i == o.end())
            throw Failure("missing JSON key: " + key + " in " + dump());
        return i->second;
    }
    const Json &operator[](size_t i) const { return std::get<Array>(value_).at(i); }
    bool contains(const std::string &key) const { return std::get<Object>(value_).count(key); }
    const std::string &text() const { return std::get<std::string>(value_); }
    const Array &items() const { return std::get<Array>(value_); }
    size_t size() const { return std::holds_alternative<Object>(value_) ? std::get<Object>(value_).size() : std::get<Array>(value_).size(); }
    bool boolean() const { return std::get<bool>(value_); }
    int64_t integer() const { return std::get<int64_t>(value_); }
    bool operator==(const Json &other) const { return value_ == other.value_; }
    bool operator!=(const Json &other) const { return !(*this == other); }
    static std::string quote(const std::string &s) {
        std::ostringstream o;
        o << '"';
        const auto escape = [&](unsigned v) {
            o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << v;
        };
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = s[i];
            switch (c) {
            case '"':
            case '\\':
                o << '\\' << char(c);
                continue;
            case '\b':
                o << "\\b";
                continue;
            case '\f':
                o << "\\f";
                continue;
            case '\n':
                o << "\\n";
                continue;
            case '\r':
                o << "\\r";
                continue;
            case '\t':
                o << "\\t";
                continue;
            default:
                break;
            }
            if (c < 32 || c == 127)
                escape(c);
            else if (c < 128)
                o << char(c);
            else {
                unsigned n = c >= 0xf0 && c <= 0xf4   ? 3
                             : c >= 0xe0 && c <= 0xef ? 2
                             : c >= 0xc2 && c <= 0xdf ? 1
                                                      : 0;
                if (!n || i + n >= s.size())
                    throw Failure("invalid UTF-8 in JSON input");
                unsigned v = c & ((1u << (6 - n)) - 1);
                for (unsigned k = 0; k < n; ++k) {
                    unsigned char d = s[++i];
                    if ((d & 0xc0) != 0x80)
                        throw Failure("invalid UTF-8 continuation in JSON input");
                    v = (v << 6) | (d & 63);
                }
                if (v < (n == 1   ? 0x80u
                         : n == 2 ? 0x800u
                                  : 0x10000u) ||
                    v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
                    throw Failure("invalid UTF-8 code point in JSON input");
                // Match json.dumps's ASCII output, including surrogate pairs.
                if (v < 0x10000)
                    escape(v);
                else {
                    v -= 0x10000;
                    escape(0xd800 + (v >> 10));
                    escape(0xdc00 + (v & 1023));
                }
            }
        }
        return o.str() + '"';
    }
    std::string dump() const {
        switch (value_.index()) {
        case 0:
            return "null";
        case 1:
            return std::get<bool>(value_) ? "true" : "false";
        case 2:
            return std::to_string(std::get<int64_t>(value_));
        case 3:
            return quote(text());
        case 4: {
            std::string s = "[";
            for (const auto &v : std::get<Array>(value_)) {
                if (s.size() > 1)
                    s += ',';
                s += v.dump();
            }
            return s + ']';
        }
        case 6: {
            std::ostringstream out;
            out << std::setprecision(17) << std::get<double>(value_);
            return out.str();
        }
        default: {
            std::string s = "{";
            for (const auto &v : std::get<Object>(value_)) {
                if (s.size() > 1)
                    s += ',';
                s += quote(v.first) + ':' + v.second.dump();
            }
            return s + '}';
        }
        }
    }
    static Json parse(const std::string &s) {
        size_t i = 0;
        auto out = read(s, i);
        space(s, i);
        if (i != s.size())
            throw Failure("trailing JSON bytes: " + s.substr(i));
        return out;
    }

  private:
    std::variant<std::monostate, bool, int64_t, std::string, Array, Object, double> value_;
    static void space(const std::string &s, size_t &i) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }
    static void expect(const std::string &s, size_t &i, char c) {
        if (i >= s.size() || s[i++] != c)
            throw Failure("invalid JSON at byte " + std::to_string(i) + ": " + s);
    }
    static unsigned hex4(const std::string &s, size_t &i) {
        unsigned v = 0;
        for (int n = 0; n < 4; ++n) {
            if (i >= s.size() || !std::isxdigit(static_cast<unsigned char>(s[i])))
                throw Failure("invalid JSON Unicode escape");
            char c = s[i++];
            v = v * 16 + (c <= '9' ? c - '0' : std::tolower(c) - 'a' + 10);
        }
        return v;
    }
    static void utf8(std::string &o, unsigned v) {
        if (v < 0x80)
            o += char(v);
        else if (v < 0x800) {
            o += char(0xc0 | (v >> 6));
            o += char(0x80 | (v & 63));
        } else if (v < 0x10000) {
            o += char(0xe0 | (v >> 12));
            o += char(0x80 | ((v >> 6) & 63));
            o += char(0x80 | (v & 63));
        } else {
            o += char(0xf0 | (v >> 18));
            o += char(0x80 | ((v >> 12) & 63));
            o += char(0x80 | ((v >> 6) & 63));
            o += char(0x80 | (v & 63));
        }
    }
    static std::string string(const std::string &s, size_t &i) {
        expect(s, i, '"');
        std::string out;
        while (i < s.size() && s[i] != '"') {
            unsigned char c = s[i++];
            if (c == '\\') {
                if (i == s.size())
                    throw Failure("truncated JSON escape");
                c = s[i++];
                switch (c) {
                case '"':
                case '\\':
                case '/':
                    out += char(c);
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    unsigned v = hex4(s, i);
                    if (v >= 0xd800 && v <= 0xdbff) {
                        expect(s, i, '\\');
                        expect(s, i, 'u');
                        unsigned lo = hex4(s, i);
                        if (lo < 0xdc00 || lo > 0xdfff)
                            throw Failure("invalid JSON surrogate");
                        v = 0x10000 + ((v - 0xd800) << 10) + lo - 0xdc00;
                    } else if (v >= 0xdc00 && v <= 0xdfff)
                        throw Failure("invalid JSON surrogate");
                    utf8(out, v);
                    break;
                }
                default:
                    throw Failure("invalid JSON escape");
                }
            } else {
                if (c < 32)
                    throw Failure("JSON control character");
                // Validate raw UTF-8 too: truncating rich presence must not split a
                // code point.
                if (c >= 128) {
                    unsigned n = c >= 0xf0 && c <= 0xf4   ? 3
                                 : c >= 0xe0 && c <= 0xef ? 2
                                 : c >= 0xc2 && c <= 0xdf ? 1
                                                          : 0;
                    if (!n || i + n > s.size())
                        throw Failure("invalid JSON UTF-8");
                    unsigned v = c & ((1u << (6 - n)) - 1);
                    out += char(c);
                    for (unsigned k = 0; k < n; ++k) {
                        unsigned char d = s[i++];
                        if ((d & 0xc0) != 0x80)
                            throw Failure("invalid JSON UTF-8 continuation");
                        v = (v << 6) | (d & 63);
                        out += char(d);
                    }
                    if (v < (n == 1   ? 0x80u
                             : n == 2 ? 0x800u
                                      : 0x10000u) ||
                        v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
                        throw Failure("invalid JSON UTF-8 code point");
                } else
                    out += char(c);
            }
        }
        expect(s, i, '"');
        return out;
    }
    static Json read(const std::string &s, size_t &i) {
        space(s, i);
        if (i >= s.size())
            throw Failure("truncated JSON");
        if (s[i] == '"')
            return string(s, i);
        if (s[i] == '{' || s[i] == '[') {
            bool object = s[i++] == '{';
            Object o;
            Array a;
            space(s, i);
            if (i < s.size() && s[i] != (object ? '}' : ']'))
                for (;;) {
                    space(s, i);
                    std::string k;
                    if (object) {
                        k = string(s, i);
                        space(s, i);
                        expect(s, i, ':');
                    }
                    Json v = read(s, i);
                    if (object) {
                        if (!o.emplace(k, v).second)
                            throw Failure("duplicate JSON key");
                    } else
                        a.push_back(v);
                    space(s, i);
                    if (i >= s.size() || s[i] != ',')
                        break;
                    ++i;
                }
            expect(s, i, object ? '}' : ']');
            return object ? Json(o) : Json(a);
        }
        for (auto word : {"true", "false", "null"}) {
            const std::string w(word);
            if (s.compare(i, w.size(), w) == 0) {
                i += w.size();
                return w == "null" ? Json() : Json(w == "true");
            }
        }
        size_t end = i;
        if (s[end] == '-')
            ++end;
        const size_t digits = end;
        while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end])))
            ++end;
        if (end == digits)
            throw Failure("invalid JSON value: " + s.substr(i));
        bool fractional = false;
        if (end < s.size() && s[end] == '.') {
            fractional = true;
            const size_t first = ++end;
            while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
            if (end == first) throw Failure("missing JSON fractional digits");
        }
        if (end < s.size() && (s[end] == 'e' || s[end] == 'E')) {
            fractional = true;
            ++end;
            if (end < s.size() && (s[end] == '+' || s[end] == '-')) ++end;
            const size_t first = end;
            while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
            if (end == first) throw Failure("missing JSON exponent digits");
        }
        if (fractional) {
            auto n = std::stod(s.substr(i, end - i));
            i = end;
            return Json(n);
        }
        auto n = std::stoll(s.substr(i, end - i));
        i = end;
        return Json(int64_t(n));
    }
};
inline std::ostream &operator<<(std::ostream &o, const Json &v) { return o << v.dump(); }
} // namespace lh
