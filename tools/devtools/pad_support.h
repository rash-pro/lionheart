#pragma once
#include <array>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <string>
#include <tuple>
#include <vector>

namespace pad {
using Position = std::tuple<std::string, int, int, int>;
using Positions = std::map<int, Position>;
inline Positions field_positions(const std::string &text, int first, int last) {
    const std::regex pattern("PAD_FIELD vblank=([0-9]+) map=([a-z0-9_]+) x=(-?[0-9]+) y=(-?[0-9]+) z=(-?[0-9]+) facing=([0-9]+)");
    Positions result;
    for (auto i = std::sregex_iterator(text.begin(), text.end(), pattern); i != std::sregex_iterator(); ++i) {
        const auto &m = *i; int frame = std::stoi(m[1]);
        if (first <= frame && frame <= last) result[frame] = {m[2], std::stoi(m[3]), std::stoi(m[4]), std::stoi(m[5])};
    }
    return result;
}
inline bool walking_changed(const Positions &idle, const Positions &moved) {
    for (const auto &row : idle) {
        auto i = moved.find(row.first);
        if (i != moved.end() && i->second != row.second) return true;
    }
    return false;
}
inline bool protocol_matches(const std::vector<std::pair<int, std::string>> &polls, const std::string &expected, int first, int last) {
    if (static_cast<int>(polls.size()) < (last-first)/2) return false;
    for (const auto &p : polls) if (p.second != expected) return false;
    return true;
}
using Route = std::vector<std::tuple<int, int, std::string>>;
struct Scene { std::string card; int first, last; std::string direction; Route route; };
inline const std::map<std::string, Scene> scenes = {
    {"field", {"disc1-start-maxed.mcr",1700,1900,"DOWN",{}}},
    {"infirmary", {"disc1-start-maxed.mcr",2400,2600,"RIGHT",{{1700,1900,"DOWN"}}}},
    {"garden-hall", {"disc1-start-maxed.mcr",3000,3200,"DOWN",{{1700,1900,"DOWN"},{2400,2600,"RIGHT"}}}},
    {"world", {"disc1-early.mcr",1700,1900,"RIGHT",{}}}
};
inline void write_sequence(const std::string &path, int first, int last, const std::string &direction, const Route &route, const std::string &mode) {
    const std::map<std::string, std::array<int, 3>> directions = {
        {"UP", {0x10,1,-32768}}, {"RIGHT", {0x20,0,32767}}, {"DOWN", {0x40,1,32767}}, {"LEFT", {0x80,0,-32768}}};
    using State = std::array<int, 5>;
    const State neutral{};
    std::map<int, State> events{{1, neutral}};
    const int starts[] = {300,600,900,1140,1300,1460};
    for (int i=0; i<6; ++i) { events[starts[i]] = {0x4000,0,0,0,0}; events[starts[i]+(i<3?2:8)] = neutral; }
    for (const auto &r : route) { events[std::get<0>(r)] = {directions.at(std::get<2>(r))[0],0,0,0,0}; events[std::get<1>(r)] = neutral; }
    auto d = directions.at(direction); State state{};
    if (mode == "digital") state[0] = d[0];
    else if (mode == "axis") state[d[1]+1] = d[2];
    events[first] = state; events[last] = neutral;
    std::ofstream out(path); out.exceptions(std::ios::failbit | std::ios::badbit);
    for (const auto &e : events) {
        out << e.first << ' ' << std::hex << std::setw(4) << std::setfill('0') << e.second[0] << std::dec;
        for (int i=1; i<5; ++i) out << ' ' << e.second[i];
        out << '\n';
    }
}
}
