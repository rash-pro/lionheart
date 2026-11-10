#pragma once
#include "metadata.h"
namespace metadata {
// Only block mappings/sequences and single-line flow values occur in the source
// configs. Reject anchors, tags and multiline scalars rather than guessing.
struct Yaml {
  struct Line {
    int indent;
    std::string value;
  };
  std::vector<Line> lines;
  size_t at = 0;
  static std::string space(std::string s) {
    auto p = s.find_first_not_of(" \r\t");
    return p == s.npos ? "" : s.substr(p, s.find_last_not_of(" \r\t") - p + 1);
  }
  static std::vector<std::string> split(const std::string &s, char delimiter) {
    std::vector<std::string> out;
    size_t start = 0;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
      char c = s[i];
      if (quote) {
        if (c == '\\' && quote == '"')
          ++i;
        else if (c == quote) {
          if (quote == '\'' && i + 1 < s.size() && s[i + 1] == '\'')
            ++i;
          else
            quote = 0;
        }
      } else if (c == '"' || c == '\'')
        quote = c;
      else if (c == '[' || c == '{')
        ++depth;
      else if (c == ']' || c == '}')
        --depth;
      else if (c == delimiter && depth == 0) {
        out.push_back(space(s.substr(start, i - start)));
        start = i + 1;
      }
    }
    require(!quote && depth == 0, "unsupported or unbalanced YAML flow value");
    out.push_back(space(s.substr(start)));
    return out;
  }
  static J scalar(std::string s) {
    s = space(s);
    if (s.empty() || s == "null" || s == "~")
      return nullptr;
    if (s[0] == '[') {
      require(s.back() == ']', "unclosed YAML sequence");
      J a = J::array();
      auto body = space(s.substr(1, s.size() - 2));
      if (!body.empty())
        for (auto &p : split(body, ','))
          a.push_back(scalar(p));
      return a;
    }
    if (s[0] == '{') {
      require(s.back() == '}', "unclosed YAML mapping");
      J o = J::object();
      auto body = space(s.substr(1, s.size() - 2));
      if (!body.empty())
        for (auto &p : split(body, ',')) {
          auto kv = split(p, ':');
          require(kv.size() == 2, "bad YAML mapping");
          o[text(scalar(kv[0]))] = scalar(kv[1]);
        }
      return o;
    }
    if (s[0] == '"')
      return J::parse(s);
    if (s[0] == '\'') {
      require(s.size() > 1 && s.back() == '\'', "unclosed YAML string");
      s = s.substr(1, s.size() - 2);
      size_t p = 0;
      while ((p = s.find("''", p)) != s.npos)
        s.replace(p, 2, "'");
      return s;
    }
    require(std::string("&*!|>").find(s[0]) == std::string::npos,
            "unsupported YAML feature");
    if (s == "true" || s == "True")
      return true;
    if (s == "false" || s == "False")
      return false;
    if (std::regex_match(s, std::regex("[-+]?(0x[0-9a-fA-F]+|[0-9]+)"))) {
      size_t n;
      auto value = std::stoll(s, &n, 0);
      if (n == s.size())
        return value;
      return std::stoll(s, nullptr, 10);
    }
    return s;
  }
  explicit Yaml(const std::string &s) {
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
      char quote = 0;
      for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote) {
          if (c == '\\' && quote == '"')
            ++i;
          else if (c == quote)
            quote = 0;
        } else if (c == '"' || c == '\'')
          quote = c;
        else if (c == '#' &&
                 (i == 0 || std::isspace((unsigned char)line[i - 1]))) {
          line.resize(i);
          break;
        }
      }
      auto p = line.find_first_not_of(' ');
      if (p == line.npos || space(line).empty())
        continue;
      require(line[p] != '\t', "tabs in YAML indentation");
      lines.push_back({int(p), space(line.substr(p))});
    }
  }
  static bool sequence(const std::string &s) {
    return s == "-" || s.rfind("- ", 0) == 0;
  }
  void member(J &o, std::string s, int indent) {
    auto kv = split(s, ':');
    require(kv.size() >= 2, "expected YAML mapping member");
    auto key = space(kv[0]);
    require(!key.empty(), "empty YAML key");
    if (key[0] == '"' || key[0] == '\'')
      key = text(scalar(key));
    auto colon = s.find(':');
    auto value = space(s.substr(colon + 1));
    require(!o.contains(key), "duplicate YAML key: " + key);
    if (!value.empty())
      o[key] = scalar(value);
    else if (at < lines.size() &&
             (lines[at].indent > indent ||
              (lines[at].indent == indent && sequence(lines[at].value))))
      o[key] = block(lines[at].indent);
    else
      o[key] = nullptr;
  }
  J block(int indent) {
    bool seq = sequence(lines.at(at).value);
    J out = seq ? J::array() : J::object();
    while (at < lines.size() && lines[at].indent == indent &&
           sequence(lines[at].value) == seq) {
      auto s = lines[at++].value;
      if (!seq)
        member(out, s, indent);
      else {
        s = space(s.substr(1));
        if (s.empty()) {
          require(at < lines.size() && lines[at].indent > indent,
                  "empty YAML sequence item");
          out.push_back(block(lines[at].indent));
        } else if (s[0] != '[' && s[0] != '{' && s.find(": ") != s.npos) {
          J o = J::object();
          member(o, s, indent + 2);
          while (at < lines.size() && lines[at].indent == indent + 2 &&
                 !sequence(lines[at].value)) {
            auto v = lines[at++].value;
            member(o, v, indent + 2);
          }
          out.push_back(o);
        } else
          out.push_back(scalar(s));
      }
    }
    return out;
  }
  J parse() {
    require(!lines.empty(), "empty YAML document");
    auto out = block(lines[0].indent);
    require(at == lines.size(), "unsupported YAML indentation");
    return out;
  }
};
} // namespace metadata
