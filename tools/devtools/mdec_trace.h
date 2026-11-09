#pragma once
#include "gte_trace.h"
#include <cstring>
namespace trace {
inline std::string instrument_hw(std::string source) {
    const std::pair<const char *,const char *> edits[]={
        {"#include \"cycle_math.h\"","void mdec_trace_observe(cpu_t *, unsigned, uint32_t, unsigned, uint32_t);"},
        {"    if (source >= 11) return;","    mdec_trace_observe(c, 2, source, 0, 0);"},
        {"    record(c, address, width, value, 0);","    mdec_trace_observe(c, 0, address, width, value);"},
        {"    value &= mask;","    mdec_trace_observe(c, 1, address, width, value);"}};
    for(const auto &p:edits) {auto pos=source.find(p.first);iro::require(pos!=std::string::npos&&source.find(p.first,pos+1)==std::string::npos,("hardware instrumentation boundary changed: "+std::string(p.first)).c_str());source.insert(pos+strlen(p.first),"\n"+std::string(p.second));}
    return source;
}
}
