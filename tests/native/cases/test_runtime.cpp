#include "recompiler_fixture.h"
using namespace lh::recompiler;
namespace {
std::array<uint32_t,135> snapshot(const Harness &h, void *c) {
    std::array<uint32_t,135> out{}; h.test_snapshot(c,out.data()); return out;
}
uint32_t bios(const Harness &h, void *c, unsigned table, unsigned fn, std::initializer_list<uint32_t> args = {}) {
    h.test_register(c,9,fn);
    unsigned reg=4; for (auto value:args) h.test_register(c,reg++,value);
    h.test_bios(c,table);
    auto state=snapshot(h,c); LH_CHECK_MSG(!state[37],h.test_reason(c)); return state[2];
}
thread_local std::vector<std::string> logs;
void logger(void *,const char *message) { logs.emplace_back(message); }
struct Mirror { std::string id, variant; uint32_t value; };
std::vector<Mirror> mirrors() {
    std::vector<Mirror> out;
    for (auto variant:kVariants) for(uint32_t value:{0u,0x200000u,0x400000u,0x600000u,0x80000000u,0x80200000u,0xA0000000u,0xA0600000u})
        out.push_back({std::string(variant)+"-"+std::to_string(value),variant,value});
    return out;
}
struct Leading { std::string id, variant; uint32_t value, expected; };
std::vector<Leading> leading() {
    std::vector<Leading> out;
    for(auto variant:kVariants) for(auto row:std::vector<std::pair<uint32_t,uint32_t>>{{0,32},{0xFFFFFFFF,32},{0x80000000,1},{0x40000000,1},{0xFFFF0000,16}})
        out.push_back({std::string(variant)+"-"+std::to_string(row.first)+"-"+std::to_string(row.second),variant,row.first,row.second});
    return out;
}
#define CPU(variant) auto lib=compiled_library(t,variant); auto h=functions_of(lib); Cpu owner(h); void *c=owner.get()
}
LH_TEST_P(test_ram_mirrors, mirrors()) {
    CPU(p.variant);
    h.test_write(c,0x104,0x89ABCDEF,4);
    LH_EQ(h.test_read(c,p.value+0x104,4),0x89ABCDEFu);
    h.test_write(c,p.value+0x106,0x1234,2);
    LH_EQ(h.test_read(c,0x104,4),0x1234CDEFu);
}
LH_TEST_P(test_scratchpad_and_rom,kVariants) {
    CPU(p); logs.clear(); h.test_logger(c,logger);
    h.test_write(c,0x1F8003FC,0x12345678,4);
    LH_EQ(h.test_read(c,0x9F8003FC,4),0x12345678u);
    LH_EQ(h.test_read(c,0x1FC00100,4),0u); LH_EQ(h.test_read(c,0xBFC00000,4),0u);
    LH_EQ(logs.size(),2u); for(auto &line:logs) LH_CONTAINS(line,"UNMAPPED read");
}
LH_TEST_P(test_mmio_logging_and_defaults,kVariants) {
    CPU(p); logs.clear(); h.test_logger(c,logger);
    h.test_write(c,0x1F801074,0x321,2);
    LH_EQ(h.test_read(c,0xBF801074,2),0x321u);
    LH_EQ(h.test_read(c,0x1F801814,4)&0x14000000,0x14000000u);
    LH_EQ(h.test_read(c,0x1F801800,1),0x18u);
    LH_EQ(logs.size(),4u); LH_CONTAINS(logs[0],"MMIO W address=1f801074 width=2 value=00000321");
}
LH_TEST_P(test_stall_ignores_unchanged_mmio,kVariants) {
    CPU(p); for(int i=0;i<6;++i) { h.test_read(c,0x1F801814,4); h.test_poll(c,BASE,5); }
    LH_CONTAINS(h.test_reason(c),"stall");
}
LH_TEST_P(test_stall_resets_on_mmio_change,kVariants) {
    CPU(p); for(unsigned i=0;i<10;++i) { h.test_write(c,0x1F801074,i,2); h.test_poll(c,BASE,3); }
    LH_CHECK(!snapshot(h,c)[37]);
}
LH_TEST_P(test_stall_with_different_read_and_write_values,kVariants) {
    CPU(p); for(int i=0;i<10;++i) { h.test_write(c,0x1F801814,0,4); h.test_read(c,0x1F801814,4); h.test_poll(c,BASE,5); }
    LH_CONTAINS(h.test_reason(c),"stall");
}
LH_TEST_P(test_bios_memory_strings_and_random,kVariants) {
    CPU(p); h.test_write(c,DATA,0x636261,4);
    LH_EQ(bios(h,c,0,0x1B,{DATA}),3u);
    LH_EQ(bios(h,c,0,0x2A,{DATA+16,DATA,4}),DATA+16);
    LH_EQ(h.test_read(c,DATA+16,4),0x636261u);
    LH_EQ(bios(h,c,0,0x2B,{DATA+20,0xAA,4}),DATA+20);
    LH_EQ(h.test_read(c,DATA+20,4),0xAAAAAAAAu);
    bios(h,c,0,0x2C,{DATA+1,DATA,3}); LH_EQ(h.test_read(c,DATA,4),0x63626161u);
    bios(h,c,0,0x30,{1}); LH_EQ(bios(h,c,0,0x2F),16838u); LH_EQ(bios(h,c,0,0x2F),5758u);
}
LH_TEST_P(test_bios_stubs_and_unknown_call,kVariants) {
    CPU(p); auto one=bios(h,c,1,8,{0xF0000001,2,0x1000,0}); auto two=bios(h,c,1,8,{0xF0000001,2,0x1000,0});
    LH_CHECK(one!=two); LH_EQ(one&0xFFFF0000,0xF1000000u);
    LH_EQ(bios(h,c,1,0x0C,{one}),1u); LH_EQ(bios(h,c,1,0x0E,{BASE,DATA,0})&0xFF000000,0xFF000000u);
    h.test_register(c,9,0x42); h.test_bios(c,0); LH_CONTAINS(h.test_reason(c),"unimplemented BIOS A0:42");
}
LH_TEST_P(test_critical_sections,kVariants) {
    CPU(p); h.test_register(c,2,123);
    for(auto row:std::vector<std::pair<unsigned,unsigned>>{{2,123},{1,1},{1,0},{2,0}}) {
        h.test_register(c,4,row.first); h.test_syscall(c); auto state=snapshot(h,c);
        LH_EQ(bool(state[51]&0x401),row.first==2); LH_EQ(state[2],row.second);
    }
}
LH_TEST_P(test_gte_transfer_registers,kVariants) {
    CPU(p); h.test_gte_write(c,1,0x12348001,0); LH_EQ(h.test_gte_read(c,1,0),0xFFFF8001u);
    h.test_gte_write(c,7,0x12348001,0); LH_EQ(h.test_gte_read(c,7,0),0x8001u);
    for(unsigned value:{1,2,3}) h.test_gte_write(c,15,value,0);
    unsigned index=0; for(unsigned reg:{12,13,14,15}) { const unsigned expected[]={1,2,3,3}; LH_EQ(h.test_gte_read(c,reg,0),expected[index++]); }
    h.test_gte_write(c,28,0x7FFF,0); LH_EQ(h.test_gte_read(c,29,0),0x7FFFu); LH_EQ(h.test_gte_read(c,9,0),0xF80u);
    h.test_gte_write(c,9,0xFFFF,0); LH_EQ(h.test_gte_read(c,29,0),0x7FE0u);
    h.test_gte_write(c,26,0x8000,1); LH_EQ(h.test_gte_read(c,26,1),0xFFFF8000u);
    h.test_gte_write(c,31,0xFFFFFFFF,1); LH_EQ(h.test_gte_read(c,31,1),0xFFFFF000u);
}
LH_TEST_P(test_gte_count_leading_bits,leading()) {
    CPU(p.variant); h.test_gte_write(c,30,p.value,0); LH_EQ(h.test_gte_read(c,31,0),p.expected);
    h.test_gte_write(c,31,123,0); LH_EQ(h.test_gte_read(c,31,0),p.expected);
}
LH_TEST_P(test_pending_load_survives_faulting_write,kVariants) {
    CPU(p); h.test_register(c,20,DATA); h.test_write(c,DATA,123,4);
    h.test_write(c,BASE,imm(35,20,8,0),4); h.test_write(c,BASE+4,imm(35,20,8,1),4);
    h.test_step(c,BASE); h.test_step(c,BASE+4); auto state=snapshot(h,c);
    LH_EQ(state[8],123u); LH_CHECK(state[37]);
}
LH_TEST(test_discovery_does_not_translate_zero_buffers) {
    Image image(pack(RET)+std::string(100000,'\0'),BASE,BASE);
    Discovery found(image); LH_EQ(found.entries().size(),1u); LH_EQ(found.instructions(BASE).size(),2u);
}
LH_TEST(test_exe_validation) {
    auto path=t.temp()/"bad.exe";
    auto validate=[&] {
        auto session=lhgt_open(); LH_CHECK(session);
        int rc=lhgt_load_exe(session,t.root().c_str(),path.c_str());
        std::string error=lhgt_error(session); lhgt_close(session);
        LH_CHECK(rc!=0); return error;
    };
    lh::write_file(path,"PS-X EXE"); validate();
    std::string data(0x808,'\0'); data.replace(0,8,"PS-X EXE");
    auto header=pack({BASE,0,BASE,16}); data.replace(0x10,header.size(),header);
    lh::write_file(path,data); LH_CONTAINS(validate(),"truncated");
}
