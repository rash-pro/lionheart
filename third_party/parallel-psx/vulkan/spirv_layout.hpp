// Original host-port SPIR-V reflection for the bundled, immutable PSX shaders.
// No compiler, cross-compiler, or external reflection library is linked.
#pragma once
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <string>
#include <functional>
namespace Vulkan {
static bool get_stock_sampler(StockSampler &, const std::string &);
static void reflect_layout(const uint32_t *code, size_t words, ResourceLayout &layout) {
    struct Id {
        unsigned op=0, type=0, storage=0, set=0, binding=0, location=~0u;
        unsigned element=0, count=0, dim=0, sampled=0, bytes=0;
        std::vector<unsigned> members, offsets;
        std::string name;
        bool buffer_block=false;
    };
    if (words < 5 || code[0] != 0x07230203 || code[3] > 1000000)
        throw std::runtime_error("Invalid bundled SPIR-V");
    std::vector<Id> ids(code[3]);
    for (size_t at=5; at<words;) {
        unsigned n=code[at]>>16, op=code[at]&65535;
        if (!n || at+n>words) throw std::runtime_error("Truncated SPIR-V");
        const uint32_t *p=code+at;
        auto &id=ids.at((op==5 || (op>=20 && op<=32) || op==71 || op==72) ? p[1] : 0);
        if (op==5) id.name=std::string(reinterpret_cast<const char *>(p+2), strnlen(reinterpret_cast<const char *>(p+2), (n-2)*4));
        if (op>=20 && op<=32) {
            id.op=op;
            if (op==21 || op==22) id.bytes=p[2]/8;
            if (op==23 || op==24 || op==28) { id.element=p[2]; id.count=p[3]; }
            if (op==25) { id.element=p[2]; id.dim=p[3]; id.sampled=p[7]; }
            if (op==27) id.element=p[2];
            if (op==30) id.members.assign(p+2,p+n);
            if (op==32) { id.storage=p[2]; id.element=p[3]; }
        }
        if (op==43) ids.at(p[2]).count=p[3]; // integer array lengths
        if (op==59) { auto &v=ids.at(p[2]); v.op=op; v.type=p[1]; v.storage=p[3]; }
        if (op==71) {
            if (p[2]==3) id.buffer_block=true; // SPIR-V 1.0 SSBO BufferBlock
            if (p[2]==1 && p[3]<VULKAN_NUM_SPEC_CONSTANTS) layout.spec_constant_mask|=1u<<p[3];
            if (p[2]==30) id.location=p[3];
            if (p[2]==33) id.binding=p[3];
            if (p[2]==34) id.set=p[3];
        }
        if (op==72 && p[3]==35) {
            if (id.offsets.size()<=p[2]) id.offsets.resize(p[2]+1);
            id.offsets[p[2]]=p[4];
        }
        at+=n;
    }
    std::function<unsigned(unsigned)> bytes=[&](unsigned n) -> unsigned {
        const auto &t=ids.at(n);
        if (t.op==21 || t.op==22) return t.bytes;
        if (t.op==23 || t.op==24) return bytes(t.element)*t.count;
        if (t.op==28) return bytes(t.element)*ids.at(t.count).count;
        if (t.op==30) {
            unsigned size=0;
            for (unsigned i=0;i<t.members.size();++i)
                size=std::max(size,(i<t.offsets.size()?t.offsets[i]:size)+bytes(t.members[i]));
            return size;
        }
        return 0;
    };
    for (const auto &v:ids) if (v.op==59) {
        const auto &t=ids.at(ids.at(v.type).element);
        if (v.storage==1 || v.storage==3) {
            if (v.location<32) (v.storage==1?layout.input_mask:layout.output_mask)|=1u<<v.location;
            continue;
        }
        if (v.storage==9) { layout.push_constant_size=bytes(ids.at(v.type).element); continue; }
        if (v.set>=VULKAN_NUM_DESCRIPTOR_SETS || v.binding>=VULKAN_NUM_BINDINGS)
            throw std::runtime_error("SPIR-V binding exceeds host layout");
        auto &set=layout.sets[v.set]; unsigned bit=1u<<v.binding;
        if (v.storage==2) {
            if (t.buffer_block) set.storage_buffer_mask|=bit;
            else set.uniform_buffer_mask|=bit;
            continue;
        }
        if (v.storage==12) { set.storage_buffer_mask|=bit; continue; }
        if (v.storage!=0) continue;
        const auto &image=t.op==27 ? ids.at(t.element) : t;
        if (t.op==26) set.sampler_mask|=bit;
        else if (image.dim==6) set.input_attachment_mask|=bit;
        else if (image.dim==5) set.sampled_buffer_mask|=bit;
        else if (image.sampled==2) set.storage_image_mask|=bit;
        else if (t.op==27) set.sampled_image_mask|=bit;
        else set.separate_image_mask|=bit;
        if (image.op==25 && ids.at(image.element).op==22) set.fp_mask|=bit;
        StockSampler sampler;
        if ((t.op==26 || t.op==27) && get_stock_sampler(sampler,v.name))
            set_immutable_sampler(set,v.binding,sampler);
    }
}
}
