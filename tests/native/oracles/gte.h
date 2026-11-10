#pragma once
#include "numeric.h"
#include <array>
#include <algorithm>

namespace lh::oracle::gte {
inline uint32_t u32(int64_t n) { return uint32_t(n); }
inline int64_t s32(uint32_t n) { return n < 0x80000000u ? int64_t(n) : int64_t(n)-0x100000000ll; }
inline int64_t signed16(uint32_t n) { n &= 65535; return n < 32768 ? int64_t(n) : int64_t(n)-65536; }
inline uint32_t xy(int64_t x,int64_t y) { return (u32(x)&65535)|((u32(y)&65535)<<16); }
inline uint32_t flags(uint32_t bits) { return bits | (bits & 0x7f87e000 ? 0x80000000u : 0); }
using Triple = std::array<int64_t,3>;
using Matrix = std::array<Triple,3>;
inline const Matrix identity = {{{4096,0,0},{0,4096,0},{0,0,4096}}};
struct Division { uint32_t quotient; bool overflow; };
inline Division unr(uint32_t h,uint32_t z) {
    if (h >= z*2) return {0x1ffff,true};
    unsigned length=0;
    for (auto n=z;n;n>>=1) ++length;
    const unsigned shift=16-length;
    const int64_t d=int64_t(z)<<shift;
    const auto seed=std::max<int64_t>(0,(0x40000/(((d-0x7fc0)>>7)+256)+1)/2-257)+257;
    const auto correction=(0x2000080-d*seed)/256;
    const auto reciprocal=(128+correction*seed)/256;
    return {uint32_t(std::min<int64_t>(0x1ffff,((int64_t(h)<<shift)*reciprocal+32768)/65536)),false};
}
struct Selection { Triple mac{},ir{}; uint32_t flag=0; };
inline Selection mvmva(const Matrix &matrix, const Triple &vector, const Triple &translation,
                       int cv,int sf,int lm) {
    Selection out;
    uint32_t errors=0;
    for (unsigned i=0;i<3;++i) {
        Triple products{};
        for (unsigned j=0;j<3;++j) products[j]=matrix[i][j]*vector[j];
        int64_t n;
        if (cv==2) {
            const auto discarded=floor_shift(translation[i]*4096+products[0],sf*12);
            if (discarded < -32768 || discarded > 32767) errors|=1u<<(24-i);
            n=floor_shift(products[1]+products[2],sf*12);
        } else n=floor_shift(translation[i]*4096+products[0]+products[1]+products[2],sf*12);
        out.mac[i]=n; out.ir[i]=clip(n,lm?0:-32768,32767);
        if (out.ir[i]!=n) errors|=1u<<(24-i);
    }
    out.flag=flags(errors);
    return out;
}
inline Triple shifted(Triple values,int sf) {
    for(auto &n:values) n=floor_shift(n,sf*12);
    return values;
}
inline Triple clamped(Triple values,int lm) {
    for(auto &n:values) n=clip(n,lm?0:-32768,32767);
    return values;
}
inline Triple gpf_gpl(const Triple &input,const Triple &old,int cmd,int sf,int64_t ir0) {
    Triple out{};
    for(unsigned i=0;i<3;++i)
        out[i]=floor_shift(input[i]*ir0+(cmd==62?old[i]*(int64_t(1)<<(sf*12)):0),sf*12);
    return out;
}
inline uint32_t color(const Triple &mac) {
    uint32_t rgb=0;
    for(unsigned i=0;i<3;++i) rgb+=uint32_t(clip(floor_shift(mac[i],4),0,255))<<(8*i);
    return rgb;
}

}
