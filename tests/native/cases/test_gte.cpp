// GTE register/command vectors derived from psx-spx's hardware equations.
// No emulator implementation supplies expected results; external corpora stay optional.
#include "lh_test.h"
#include "equiv.h"
#include "py_random.h"
#include "lh_json.h"
#include <cstdlib>
#include "oracles/gte.h"
#include <set>

namespace {
using namespace lh::oracle::gte;
using lh::oracle::floor_shift;
constexpr int commands[]={1,6,12,16,17,18,19,20,22,27,28,30,32,40,41,42,45,46,48,61,62,63};
struct GTE {
    lh::Library lib;
    void *cpu;
    explicit GTE(lh::Case &t):lib(t.library("gte")),cpu(lib.fn<void*()>("gte_test_new")()) { LH_CHECK(cpu); }
    ~GTE() { lib.fn<void(void*)>("gte_test_free")(cpu); }
    void write(unsigned reg,int64_t value,bool control=false) { lib.fn<void(void*,unsigned,uint32_t,int)>("gte_write")(cpu,reg,u32(value),control); }
    uint32_t read(unsigned reg,bool control=false) { return lib.fn<uint32_t(void*,unsigned,int)>("gte_read")(cpu,reg,control); }
    void command(int cmd,int sf=1,int lm=0,int mx=0,int v=0,int cv=0) {
        lib.fn<void(void*,uint32_t)>("gte_cmd")(cpu,cmd|(sf<<19)|(lm<<10)|(mx<<17)|(v<<15)|(cv<<13));
    }
    void matrix(unsigned select,const Matrix &rows) {
        std::array<int64_t,10> flat{};
        for (unsigned i=0;i<9;++i) flat[i]=rows[i/3][i%3];
        for (unsigned i=0;i<5;++i) write(select*8+i,xy(flat[i*2],flat[i*2+1]),true);
    }
    void vector(unsigned select,const Triple &values) {
        if (select==3) for (unsigned i=0;i<3;++i) write(9+i,values[i]);
        else { write(select*2,xy(values[0],values[1])); write(select*2+1,values[2]); }
    }
    void triple(unsigned reg,const Triple &expected) { for (unsigned i=0;i<3;++i) LH_EQ(s32(read(reg+i)),expected[i]); }
    uint32_t flag() { return read(31,true); }
};
struct Row { std::string id; std::vector<int64_t> v; };
std::vector<Row> params(const std::vector<std::vector<int64_t>> &values) {
    std::vector<Row> out;
    for (auto v:values) { std::string id="host"; for (auto n:v) id+="-"+std::to_string(n); out.push_back({id,v}); }
    return out;
}
std::vector<Row> range(int n) { std::vector<std::vector<int64_t>> v; for (int i=0;i<n;++i) v.push_back({i}); return params(v); }
const char *const host[]={"host"};
std::vector<Row> widths() {
    std::vector<Row> out;
    for (auto n:{0u,0x12348001u,0x89ab7fffu,0xffffffffu}) for (int c=0;c<2;++c)
        out.push_back({"host-"+std::to_string(n)+"-"+(c?"True":"False"),{int64_t(n),c}});
    return out;
}
std::vector<Row> command_rows() { std::vector<std::vector<int64_t>> v; for (int c:commands) v.push_back({c}); return params(v); }
std::vector<Row> selections() {
    std::vector<std::vector<int64_t>> v;
    for(int mx=0;mx<4;++mx) for(int vs=0;vs<4;++vs) for(int cv=0;cv<4;++cv)
        for(int sf=0;sf<2;++sf) for(int lm=0;lm<2;++lm) v.push_back({mx,vs,cv,sf,lm});
    return params(v);
}
}
LH_TEST_P(test_register_widths,widths()) {
    GTE g(t); const auto value=uint32_t(p.v[0]); bool control=p.v[1];
    const std::set<int> sign=control?std::set<int>{4,12,20,26,27,29,30}:std::set<int>{1,3,5,8,9,10,11};
    const std::set<int> unsign=control?std::set<int>{}:std::set<int>{7,16,17,18,19};
    const std::set<int> excluded=control?std::set<int>{31}:std::set<int>{15,28,29,30,31};
    for(int reg=0;reg<32;++reg) if(!excluded.count(reg)) {
        g.write(reg,value,control);
        LH_EQ(g.read(reg,control),sign.count(reg)?u32(signed16(value)):unsign.count(reg)?value&65535:value);
    }
    LH_EQ(g.flag(),0u);
}
LH_TEST_P(test_xy_fifo_and_reserved_register,host) {
    (void)p;
    GTE g(t); for(int n:{11,22,33}) g.write(15,n);
    for(unsigned i=0;i<4;++i) LH_EQ(g.read(12+i),(std::array<unsigned,4>{11,22,33,33})[i]);
    g.write(14,44);
    for(unsigned i=0;i<4;++i) LH_EQ(g.read(12+i),(std::array<unsigned,4>{11,22,44,44})[i]);
    g.write(23,0xdeadbeef); LH_EQ(g.read(23),0xdeadbeefu); LH_EQ(g.read(22),0u);
}
LH_TEST_P(test_color_conversion_and_read_only,host) {
    (void)p;
    GTE g(t); g.write(28,0xffff8000u|(3<<10)|(2<<5)|1); g.triple(9,{128,256,384});
    LH_EQ(g.read(28),0xc41u); LH_EQ(g.read(29),0xc41u);
    g.vector(3,{-1,4096,127}); LH_EQ(g.read(28),31u<<5); LH_EQ(g.read(29),31u<<5);
    g.write(29,0x7fff); g.triple(9,{-1,4096,127}); LH_EQ(g.flag(),0u);
}
LH_TEST_P(test_leading_sign_bits,range(33)) {
    GTE g(t); int count=p.v[0]; uint32_t value=count==32?0:1u<<(31-count);
    for(auto n:{value,~value}) { g.write(30,n); LH_EQ(g.read(30),n); LH_EQ(g.read(31),unsigned(std::max(1,count)));
        g.write(31,99); LH_EQ(g.read(31),unsigned(std::max(1,count))); }
}
LH_TEST_P(test_flag_write_summary,range(32)) {
    GTE g(t); uint32_t bit=1u<<p.v[0]; g.write(31,bit,true); LH_EQ(g.flag(),flags(bit&0x7ffff000));
}
LH_TEST_P(test_command_resets_flags_and_counts_once,command_rows()) {
    GTE g(t); int cmd=p.v[0]; g.write(31,0xffffffff,true); g.command(cmd);
    LH_EQ(g.flag(),cmd==1||cmd==48?0x80020000u:0u); LH_EQ(g.lib.fn<uint64_t(void*)>("gte_test_count")(g.cpu),1u);
}
namespace {
std::vector<Row> edges() {
    std::vector<Row> out;
    for(auto v:std::vector<std::vector<int64_t>>{{0,0,0x1ffff,1},{1,0,0x1ffff,1},{0,1,0,0},{1,1,0x10000,0},
        {2,1,0x1ffff,1},{1,2,0x8000,0},{0x8000,0x8000,0x10000,0},{0xffff,0xffff,0xffff,0},
        {0xfe3f,0x7f20,0x1ffff,0},{0xf015,0x780b,0x1ffff,0}}) {
        out.push_back({"host-"+std::to_string(v[0])+"-"+std::to_string(v[1])+"-"+std::to_string(v[2])+"-"+(v[3]?"True":"False"),v});
    } return out;
}
}
LH_TEST_P(test_unr_documented_edges,edges()) {
    GTE g(t); g.write(7,p.v[1],true); g.write(26,p.v[0],true); g.write(27,1,true); g.command(1);
    LH_EQ(g.read(24),uint32_t(p.v[2])); LH_EQ(bool(g.flag()&(1<<17)),bool(p.v[3]));
}
LH_TEST_P(test_unr_all_denominators,host) {
    (void)p;
    GTE g(t); g.write(27,1,true);
    for(uint32_t z=1;z<65536;++z) {
        const uint32_t h=(z*12345)&65535; auto e=unr(h,z);
        lh::equiv::record(t,h,z,e.quotient,uint32_t(e.overflow));
        g.write(7,z,true); g.write(26,h,true); g.command(1);
        LH_CHECK_MSG(g.read(24)==e.quotient,"UNR h="+std::to_string(h)+" z="+std::to_string(z));
        LH_EQ(bool(g.flag()&(1<<17)),e.overflow);
    }
}
LH_TEST_P(test_rtps_saturation,host) {
    (void)p;
    GTE g(t); g.matrix(0,identity); g.vector(0,{2000,-2000,100}); g.write(26,100,true); g.write(27,256,true); g.command(1);
    g.triple(25,{2000,-2000,100}); g.triple(9,{2000,-2000,100}); LH_EQ(g.read(14),xy(1023,-1024));
    LH_EQ(g.read(19),100u); LH_EQ(g.read(8),4096u); LH_EQ(g.read(24),0x1000000u); LH_EQ(g.flag(),0x80006000u);
}
LH_TEST_P(test_rtps_ir3_flag_uses_unshifted_signed_depth,(params({{0,0,10,40960,32767},{1,1,-1,-1,0}}))) {
    GTE g(t); g.matrix(0,identity); g.vector(0,{0,0,p.v[2]}); g.command(1,p.v[0],p.v[1]);
    LH_EQ(s32(g.read(27)),p.v[3]); LH_EQ(g.read(11),uint32_t(p.v[4]));
    LH_CHECK(!(g.flag()&(1<<22))); LH_EQ(g.flag(),p.v[2]<0?0x80060000u:0u);
}
LH_TEST_P(test_rtps_uses_full_mac0_before_saturation,host) {
    (void)p;
    GTE g(t); g.matrix(0,identity); g.vector(0,{32767,-32768,1}); g.write(26,2,true); g.write(27,32767,true); g.command(1);
    LH_EQ(g.read(14),xy(1023,-1024)); LH_EQ(g.read(8),4096u); LH_EQ(g.read(24),u32(int64_t(0x1ffff)*32767));
    LH_EQ(g.flag(),flags((1<<17)|(1<<16)|(1<<15)|(1<<14)|(1<<13)|(1<<12)));
}
LH_TEST_P(test_rtpt_fifo_and_final_depth_cue,host) {
    (void)p;
    GTE g(t); g.matrix(0,identity); g.vector(0,{10,0,100}); g.vector(1,{20,0,200}); g.vector(2,{30,0,400});
    g.write(19,7); g.write(26,100,true); g.write(27,1,true); g.command(0x30);
    for(unsigned i=0;i<4;++i) LH_EQ(g.read(16+i),(std::array<unsigned,4>{7,100,200,400})[i]);
    for(unsigned i=0;i<3;++i) LH_EQ(g.read(12+i),(std::array<unsigned,3>{10,10,7})[i]);
    g.triple(25,{30,0,400}); LH_EQ(g.read(24),16384u); LH_EQ(g.read(8),4u); LH_EQ(g.flag(),0u);
}
LH_TEST_P(test_mvmva_every_selection,selections()) {
    GTE g(t); int mx=p.v[0],vs=p.v[1],cv=p.v[2],sf=p.v[3],lm=p.v[4];
    std::array<Matrix,4> matrices{};
    const std::array<Triple,4> vectors={{{-3,7,11},{13,-17,19},{-23,29,-31},{37,-41,43}}};
    const std::array<Triple,4> translations={{{2,-3,4},{-5,6,-7},{8,-9,10},{0,0,0}}};
    for(int k=0;k<3;++k) {
        for(int r=0;r<3;++r) for(int c=0;c<3;++c) matrices[k][r][c]=1+k*9+r*3+c;
        g.matrix(k,matrices[k]); for(int i=0;i<3;++i) g.write(5+k*8+i,translations[k][i],true);
    }
    for(int k=0;k<4;++k) g.vector(k,vectors[k]);
    g.write(6,5); g.write(8,7); matrices[3]={{{-80,80,7},{3,3,3},{5,5,5}}};
    auto e=mvmva(matrices[mx],vectors[vs],translations[cv],cv,sf,lm);
    lh::equiv::record(t,int32_t(mx),int32_t(vs),int32_t(cv),int32_t(sf),int32_t(lm),matrices[mx][0],matrices[mx][1],matrices[mx][2],vectors[vs],translations[cv],e.mac,e.ir,e.flag);
    g.command(0x12,sf,lm,mx,vs,cv); g.triple(25,e.mac); g.triple(9,e.ir); LH_EQ(g.flag(),e.flag);
}
LH_TEST_P(test_mvmva_far_color_discarded_stage_still_flags,host) {
    (void)p;
    GTE g(t); g.matrix(0,{{{4096,4096,4096},{4096,4096,4096},{4096,4096,4096}}}); g.vector(0,{10,20,30});
    g.write(21,40000,true); g.write(22,-40000,true); g.write(23,0,true); g.command(0x12,1,1,0,0,2);
    g.triple(25,{50,50,50}); g.triple(9,{50,50,50}); LH_EQ(g.flag(),0x81800000u);
}
LH_TEST_P(test_matrix_intermediate_44_bit_wrap_latches_both_directions,range(2)) {
    GTE g(t); int sf=p.v[0]; g.matrix(0,{{{1,1,0},{0,0,0},{0,0,0}}}); g.vector(0,{8192,-8192,0});
    g.write(5,0x7fffffff,true); g.command(0x12,sf); LH_EQ(s32(g.read(25)),sf?0x7fffffff:-4096);
    LH_EQ(s32(g.read(9)),sf?32767:-4096); LH_EQ(g.flag(),flags((1<<30)|(1<<27)|(sf?(1<<24):0)));
}
namespace {
struct ClipRow { std::string id; std::array<std::array<int,2>,3> points; int64_t expected; uint32_t error; };
const ClipRow clips[]={
 {"host-points0-100-0",{{{0,0},{10,0},{0,10}}},100,0},
 {"host-points1--100-0",{{{0,0},{0,10},{10,0}}},-100,0},
 {"host-points2-0-0",{{{0,0},{1,1},{2,2}}},0,0},
 {"host-points3-4294836225-65536",{{{-32768,-32768},{32767,-32768},{-32768,32767}}},4294836225ll,1<<16},
 {"host-points4--4294836225-32768",{{{-32768,-32768},{-32768,32767},{32767,-32768}}},-4294836225ll,1<<15}};
struct AvszRow { std::string id; int cmd; std::array<int,4> z; int scale; int64_t expected; unsigned otz,error; };
const AvszRow avsz[]={
 {"host-45-z0-1365-24570-5-0",45,{999,3,6,9},1365,24570,5,0},
 {"host-46-z1-1024-40960-10-0",46,{4,8,12,16},1024,40960,10,0},
 {"host-45-z2--1--18-0-262144",45,{0,3,6,9},-1,-18,0,1<<18},
 {"host-46-z3-32767-8589541380-65535-327680",46,{65535,65535,65535,65535},32767,8589541380ll,65535,(1<<18)|(1<<16)},
 {"host-46-z4--32768--8589803520-0-294912",46,{65535,65535,65535,65535},-32768,-8589803520ll,0,(1<<18)|(1<<15)}};
struct ColorRow { std::string id; int cmd; Triple mac; uint32_t rgb; };
const ColorRow colors[]={
 {"host-30-mac0-2855280656",30,{256,512,768},0xaa302010},
 {"host-27-mac1-2852521992",27,{128,128,96},0xaa060808},
 {"host-19-mac2-2858632228",19,{576,1088,1584},0xaa634424},
 {"host-28-mac3-2852521992",28,{128,128,96},0xaa060808},
 {"host-20-mac4-2858632228",20,{576,1088,1584},0xaa634424},
 {"host-41-mac5-2858632228",41,{576,1088,1584},0xaa634424},
 {"host-17-mac6-2860011560",17,{640,1280,1920},0xaa785028},
 {"host-16-mac7-2859491424",16,{1536,1536,1792},0xaa706060}};
struct LightingRow { std::string id; int cmd; std::array<uint32_t,3> colors; Triple last; };
const LightingRow lighting[]={
 {"host-32-colors0-last0",32,{0xaa302010,0xaa605040,0xaa908070},{1792,2048,2304}},
 {"host-63-colors1-last1",63,{0xaa060808,0xaa0c1420,0xaa122038},{896,512,288}},
 {"host-22-colors2-last2",22,{0xaa634424,0xaa664a30,0xaa69503c},{960,1280,1680}}};
}
LH_TEST_P(test_nclip_orientation_and_overflow,clips) {
    GTE g(t); for(unsigned i=0;i<3;++i) g.write(12+i,xy(p.points[i][0],p.points[i][1]));
    g.command(6); LH_EQ(g.read(24),u32(p.expected)); LH_EQ(g.flag(),flags(p.error));
}
LH_TEST_P(test_avsz_full_sum_and_signed_scaling,avsz) {
    GTE g(t); for(unsigned i=0;i<4;++i) g.write(16+i,p.z[i]);
    g.write(p.cmd==45?29:30,p.scale,true); g.command(p.cmd);
    LH_EQ(g.read(24),u32(p.expected)); LH_EQ(g.read(7),p.otz); LH_EQ(g.flag(),flags(p.error));
}
LH_TEST_P(test_op_and_square,(params({{0,0},{0,1},{1,0},{1,1}}))) {
    GTE g(t); int sf=p.v[0],lm=p.v[1]; g.matrix(0,{{{2,99,99},{99,3,99},{99,99,4}}});
    g.vector(3,{5,6,7}); g.command(12,sf,lm);
    Triple e=shifted({-3,6,-3},sf), ir=clamped(e,lm);
    lh::equiv::record(t,int32_t(sf),int32_t(lm),e,ir,flags(lm?((1<<24)|(1<<22)):0));
    g.triple(25,e); g.triple(9,ir); LH_EQ(g.flag(),flags(lm?((1<<24)|(1<<22)):0));
    g.vector(3,{-32768,-3,4}); g.command(40,sf,lm);
    e=shifted({1073741824,9,16},sf); ir=clamped(e,lm);
    lh::equiv::record(t,e,ir,uint32_t(0x81000000));
    g.triple(25,e); g.triple(9,ir); LH_EQ(g.flag(),0x81000000u);
}
LH_TEST_P(test_color_command_hand_vectors,colors) {
    GTE g(t); g.matrix(1,identity); g.matrix(2,identity); g.vector(0,{256,512,768}); g.vector(3,{256,512,768});
    g.write(6,0xaa204080); g.write(8,2048);
    for(unsigned i=0;i<3;++i) g.write(21+i,(i+1)*1024,true);
    g.write(21,11); g.write(22,22); g.command(p.cmd,1,1);
    g.triple(25,p.mac); g.triple(9,p.mac); LH_EQ(g.read(20),11u); LH_EQ(g.read(21),22u); LH_EQ(g.read(22),p.rgb); LH_EQ(g.flag(),0u);
}
LH_TEST_P(test_triple_lighting,lighting) {
    GTE g(t); g.matrix(1,identity); g.matrix(2,identity);
    for(int i=0;i<3;++i) { g.vector(i,{256+768*i,512+768*i,768+768*i}); g.write(21+i,(i+1)*1024,true); }
    g.write(6,0xaa204080); g.write(8,2048); g.command(p.cmd,1,1);
    for(unsigned i=0;i<3;++i) LH_EQ(g.read(20+i),p.colors[i]);
    g.triple(25,p.last); g.triple(9,p.last); LH_EQ(g.flag(),0u);
}
LH_TEST_P(test_dpct_reads_old_fifo_colors_and_rgbc_code,host) {
    (void)p;
    GTE g(t); std::array<uint32_t,3> rgb={0x11203040,0x22506070,0x338090a0};
    for(unsigned i=0;i<3;++i) g.write(20+i,rgb[i]);
    g.write(6,0xabffffff); g.command(42);
    for(unsigned i=0;i<3;++i) LH_EQ(g.read(20+i),0xab000000u|(rgb[i]&0xffffff));
    g.triple(25,{0xa00,0x900,0x800}); LH_EQ(g.flag(),0u);
}
LH_TEST_P(test_depth_difference_always_has_signed_clamp,range(2)) {
    GTE g(t); g.vector(3,{4096,4096,4096}); g.write(8,2048); g.command(17,1,p.v[0]);
    g.triple(25,{2048,2048,2048}); g.triple(9,{2048,2048,2048}); LH_EQ(g.flag(),0u);
}
LH_TEST_P(test_color_clamps_mac_not_ir,host) {
    (void)p;
    GTE g(t); g.vector(3,{32767,-32768,256}); g.write(8,32767); g.write(6,0xab000000); g.command(61,0,1);
    LH_EQ(g.read(22),0xabff00ffu); g.triple(9,{32767,0,32767}); LH_EQ(g.flag(),0x81f80000u);
}
namespace {
std::vector<Row> gpf_rows() {
    std::vector<std::vector<int64_t>> v;
    for(int cmd:{61,62}) for(int sf=0;sf<2;++sf) for(int lm=0;lm<2;++lm) v.push_back({cmd,sf,lm});
    return params(v);
}
}
LH_TEST_P(test_gpf_gpl_signed_input_and_base,gpf_rows()) {
    GTE g(t); int cmd=p.v[0],sf=p.v[1],lm=p.v[2];
    Triple input={64,-64,32},old={4096,8192,2048},e{},ir{};
    g.vector(3,input); g.write(8,-16);
    for(unsigned i=0;i<3;++i) g.write(25+i,old[i]);
    e=gpf_gpl(input,old,cmd,sf,-16); ir=clamped(e,lm);
    const auto rgb=color(e);
    lh::equiv::record(t,int32_t(cmd),int32_t(sf),int32_t(lm),input,old,int64_t(-16),e,ir,rgb);
    g.command(cmd,sf,lm); g.triple(25,e); g.triple(9,ir); LH_EQ(g.read(22),rgb);
}
LH_TEST_P(test_gpl_44_bit_overflow,host) {
    (void)p;
    GTE g(t); g.vector(3,{32767,0,0}); g.write(8,32767); g.write(25,0x7fffffff); g.command(62);
    LH_EQ(s32(g.read(25)),s32(u32(0x7fffffffll+(32767ll*32767/4096)))); LH_EQ(s32(g.read(9)),-32768);
    LH_EQ(g.flag(),flags((1<<30)|(1<<24)|(1<<21)));
}
LH_TEST_P(test_deterministic_extreme_command_sequences,host) {
    (void)p;
    GTE g(t); lh::PyRandom rng(0x475445);
    const std::array<uint32_t,7> extremes={0,1,0xffffffff,0x7fffffff,0x80000000,0x7fff7fff,0x80008000};
    for(int round=0;round<200;++round) {
        for(unsigned reg=0;reg<64;++reg) {
            const uint32_t value=rng.randrange(2)?extremes[rng.randrange(7)]:rng.getrandbits(32);
            lh::equiv::record(t,uint32_t(reg),value); g.write(reg&31,value,reg>=32);
        }
        for(int cmd:commands) {
            const int sf=rng.randrange(2), lm=rng.randrange(2), mx=rng.randrange(4), v=rng.randrange(4), cv=rng.randrange(4);
            lh::equiv::record(t,int32_t(cmd),int32_t(sf),int32_t(lm),int32_t(mx),int32_t(v),int32_t(cv));
            g.command(cmd,sf,lm,mx,v,cv); LH_EQ(g.flag(),flags(g.flag()&0x7ffff000));
            for(unsigned reg=9;reg<12;++reg) { auto n=s32(g.read(reg)); LH_CHECK(n>=-32768&&n<=32767); }
            LH_EQ(g.read(15),g.read(14)); LH_EQ(g.read(28),g.read(29)); LH_CHECK(g.read(29)<=32767);
        }
    }
}
namespace {
struct External { std::string id; std::vector<uint32_t> input,output; uint32_t opcode=0; bool absent=false; };
std::vector<External> external_vectors() {
    std::vector<External> out;
    const char *path=std::getenv("FF8_GTE_VECTORS");
    if(path&&*path) {
        const auto doc=lh::Json::parse(lh::read_file(path));
        for(const auto &row:doc["tests"].items()) {
            External e; e.id="host-"+row["name"].text(); e.opcode=uint32_t(row["opcode"].integer());
            for(const auto &n:row["input"].items()) e.input.push_back(uint32_t(n.integer()));
            for(const auto &n:row["output"].items()) e.output.push_back(uint32_t(n.integer()));
            out.push_back(std::move(e));
        }
    }
    if(out.empty()) out.push_back({"host-NOTSET",{},{},0,true});
    return out;
}
}
LH_TEST_P(test_public_vectors,external_vectors()) {
    if(p.absent) lh::skip("got empty parameter set for (case)");
    GTE g(t);
    for(unsigned reg=0;reg<p.input.size();++reg) g.write(reg&31,p.input[reg],reg>=32);
    if(p.opcode!=0xffffffff) g.lib.fn<void(void*,uint32_t)>("gte_cmd")(g.cpu,p.opcode);
    for(unsigned reg=0;reg<std::min<size_t>(64,p.output.size());++reg) {
        LH_CHECK_MSG(g.read(reg&31,reg>=32)==p.output[reg],"register "+std::to_string(reg)+" differs from external vector");
    }
}
