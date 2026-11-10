// NumPy's integer-seeded SeedSequence, PCG64 and uint8 Lemire sampling.
// Keep the byte reservoir local to integers(), as NumPy does for each fill.
#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace lh {
class NumpyRandom {
    // Two limbs avoid a compiler-specific 128-bit integer type.
    struct Wide { uint64_t hi, lo; };
    static Wide add(Wide a, Wide b) { const uint64_t lo=a.lo+b.lo;return {a.hi+b.hi+uint64_t(lo<a.lo),lo}; }
    static Wide product(uint64_t a,uint64_t b) {
        const uint64_t a0=uint32_t(a),a1=a>>32,b0=uint32_t(b),b1=b>>32;
        const uint64_t p0=a0*b0,p1=a0*b1,p2=a1*b0,p3=a1*b1;
        const uint64_t middle=(p0>>32)+uint32_t(p1)+uint32_t(p2);
        return {p3+(p1>>32)+(p2>>32)+(middle>>32),(middle<<32)|uint32_t(p0)};
    }
    void step() {
        constexpr Wide multiplier{2549297995355413924ull,4865540595714422341ull};
        Wide p=product(state_.lo,multiplier.lo);
        p.hi+=state_.hi*multiplier.lo+state_.lo*multiplier.hi;
        state_=add(p,increment_);
    }
    uint64_t next64() {
        step();const uint64_t word=state_.hi^state_.lo;const unsigned rot=unsigned(state_.hi>>58);
        return (word>>rot)|(word<<((-rot)&63));
    }
    uint32_t next32() {
        if(buffered_) {buffered_=false;return high_;}
        uint64_t value=next64();high_=uint32_t(value>>32);buffered_=true;return uint32_t(value);
    }
    Wide state_{0,0},increment_{0,0};
    uint32_t high_=0;bool buffered_=false;
public:
    explicit NumpyRandom(uint64_t seed) {
        uint32_t hash=0x43b0d7e5u;
        auto mix_hash=[&](uint32_t value) {value^=hash;hash*=0x931e8875u;value*=hash;return value^(value>>16);};
        std::array<uint32_t,4> pool{};
        for(unsigned i=0;i<4;++i) pool[i]=mix_hash(i==0?uint32_t(seed):i==1?uint32_t(seed>>32):0u);
        for(unsigned src=0;src<4;++src) for(unsigned dst=0;dst<4;++dst) if(src!=dst) {
            uint32_t value=0xca01f9ddu*pool[dst]-0x4973f715u*mix_hash(pool[src]);pool[dst]=value^(value>>16);
        }
        hash=0x8b51f9ddu;std::array<uint32_t,8> words{};
        for(unsigned i=0;i<8;++i) {uint32_t value=pool[i%4]^hash;hash*=0x58f38dedu;value*=hash;words[i]=value^(value>>16);}
        auto word=[&](unsigned i) {return uint64_t(words[2*i])|(uint64_t(words[2*i+1])<<32);};
        const Wide initial{word(0),word(1)},sequence{word(2),word(3)};
        increment_={(sequence.hi<<1)|(sequence.lo>>63),(sequence.lo<<1)|1};
        step();state_=add(state_,initial);step();
    }
    // The half-open scalar bounds and flattened C-order output used by the
    // movie fixtures. Unsupported dtypes never silently choose another stream.
    std::string integers_u8(unsigned low,unsigned high,size_t count) {
        if(low>=high || high>256) throw std::invalid_argument("uint8 bounds must satisfy 0 <= low < high <= 256");
        std::string out(count,'\0');unsigned remaining=0;uint32_t buffer=0;
        auto byte=[&]() {if(!remaining) {buffer=next32();remaining=4;}const unsigned result=buffer&255;buffer>>=8;--remaining;return result;};
        const unsigned range=high-low;
        for(char &c:out) {
            if(range==1) {c=char(low);continue;}
            if(range==256) {c=char(byte());continue;}
            unsigned product_value;
            do {product_value=byte()*range;} while((product_value&255)<(256-range)%range);
            c=char(low+(product_value>>8));
        }
        return out;
    }
};
}
