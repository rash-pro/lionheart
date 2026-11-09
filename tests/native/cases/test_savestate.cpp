// Fresh-session, bounded versioned states; synthetic data only.
#include "host_fixtures.h"
static std::string sha(lh::Case &t,const std::string &bytes,bool hex=false) {
    auto file=t.temp()/"hash-input"; lh::write_file(file,bytes);
    auto digest=t.ok(t.host("sha1sum").arg(file)).out.substr(0,40);
    if(hex) return digest;
    std::string out; for(size_t i=0;i<40;i+=2) out+=char(std::stoul(digest.substr(i,2),nullptr,16)); return out;
}
static void put(std::string &s,size_t pos,uint32_t value) { for(int i=0;i<4;++i) s.at(pos+i)=char(value>>(i*8)); }
static uint32_t get(const std::string &s,size_t pos) {
    uint32_t v=0; for(int i=0;i<4;++i) v|=uint32_t(static_cast<unsigned char>(s.at(pos+i)))<<(i*8); return v;
}
static size_t section(const std::string &s,unsigned target) { size_t pos=148; while(get(s,pos)!=target) pos+=32+get(s,pos+8); return pos; }
static void checksum(lh::Case &t,std::string &s) { s.replace(16,20,sha(t,s.substr(36))); }
static const char *const branches[]={"taken","untaken"};
LH_TEST_P(test_pipeline_devices_callback_and_private_cards,branches) {
    auto state=t.temp()/"state",card=t.temp()/"card.mcr";
    t.ok(t.harness("state").args({p,state,card}).timeout(30)); LH_BYTES_EQ(lh::read_file(state).substr(0,8),std::string("FF8SAVE\0",8));
    t.ok(t.harness("state").args({"read",state}).timeout(30));
}
static const char *const kinds[]={"truncate","checksum","build","version","section_version","section_size","trailing","duplicate","oversized"};
LH_TEST_P(test_rejects_without_replacing_guest,kinds) {
    auto path=t.temp()/"state"; t.ok(t.harness("state").args({"roundtrip",path}).timeout(30)); auto data=lh::read_file(path); std::string kind=p;
    if(kind=="section_version") {
        auto original=data;
        for(unsigned target=1;target<=10;++target) {
            data=original; put(data,section(data,target)+4,99); checksum(t,data); lh::write_file(path,data);
            auto r=t.run(t.harness("state").args({"read",path}).timeout(30)); LH_EQ(r.rc,2); LH_CONTAINS(r.out,"section "+std::to_string(target)+" version 99");
        } return;
    }
    if(kind=="oversized") data=std::string(16*1024*1024+1,0);
    else if(kind=="truncate") data.resize(data.size()-11);
    else if(kind=="checksum") data[data.size()-7]^=char(0x80);
    else if(kind=="version") put(data,8,99);
    else {
        if(kind=="build") data[36]^=1;
        else if(kind=="section_size") put(data,156,0xffffffff);
        else if(kind=="duplicate") put(data,148,2);
        else if(kind=="trailing") { data+=char(0); put(data,12,data.size()); }
        checksum(t,data);
    }
    lh::write_file(path,data); auto r=t.run(t.harness("state").args({"read",path}).timeout(30)); LH_EQ(r.rc,2); LH_CONTAINS(r.out,"rejected:");
    if(kind=="build") LH_CONTAINS(r.out,"different build");
}
static const int old_versions[]={1,2};
LH_TEST_P(test_old_spu_section_is_rejected_without_replacing_guest,old_versions) {
    auto path=t.temp()/"state"; t.ok(t.harness("state").args({"roundtrip",path}).timeout(30)); auto data=lh::read_file(path); auto pos=section(data,6);
    LH_EQ(get(data,pos+4),3u); put(data,pos+4,p); checksum(t,data); lh::write_file(path,data);
    auto r=t.run(t.harness("state").args({"read",path}).timeout(30)); LH_EQ(r.rc,2); LH_CONTAINS(r.out,"unsupported save state section 6 version "+std::to_string(p));
}
LH_TEST(test_host_hotkeys_replace_only_validated_sessions) {
    t.ok(lh::dummy(t.harness("host")).args({t.temp()/"states",t.temp()/"original.mcr"}).timeout(60));
}
LH_TEST(test_disc_identity_and_cache) {
    std::string image(3000*2352,0);
    auto sector=[&](int lba,const std::string &data) { image.at(lba*2352+15)=2; image.replace(lba*2352+24,data.size(),data); };
    auto record=[](int lba,int size,const std::string &name) { std::string out(34+name.size(),0); out[0]=char(out.size()); put(out,2,lba); put(out,10,size); out[32]=char(name.size()); out.replace(33,name.size(),name); return out; };
    std::string pvd(2048,0); pvd.replace(1,5,"CD001"); put(pvd,158,20); put(pvd,166,2048); sector(16,pvd);
    sector(20,record(21,40,"SYSTEM.CNF;1")+record(700,4096,"SLUS_008.92;1")); sector(21,"BOOT = cdrom:\\SLUS_008.92;1\r\n");
    std::string exe(4096,0); exe.replace(0,8,"PS-X EXE"); put(exe,28,2048); for(int i=2048;i<4096;++i) exe[i]=char(i%256);
    sector(700,exe.substr(0,2048)); sector(701,exe.substr(2048)); auto path=t.temp()/"disc.bin"; lh::write_file(path,image);
    std::string expected("FF8DISC\1",8); expected+=std::string(12,0); put(expected,8,image.size());
    expected+=sha(t,image.substr(0,1024*1024))+sha(t,image.substr(image.size()-1024*1024))+sha(t,"SLUS_008.92"+exe)+std::string(16,0);
    auto r=t.ok(t.harness("disc").arg(path).timeout(30)); auto rows=lh::lines(r.out); LH_CHECK(rows.size()>=2); LH_EQ(rows[0],sha(t,expected,true)); LH_EQ(rows[1],sha(t,image,true));
}
