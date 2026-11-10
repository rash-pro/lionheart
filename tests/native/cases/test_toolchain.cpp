#include "lh_test.h"
#include "network.h"
#include <atomic>
#include <future>


namespace {
void put(std::string &s,uint32_t v,int n) {for(int k=0;k<n;++k) s+=char(v>>(8*k));}
std::string fixture_exe;
std::string fixture_name;
std::string zip(std::string name="",unsigned mode=0100755) {
    if(name.empty()) name=fixture_name;
    const std::string body=lh::read_file(fixture_exe);
    uint32_t crc=~0u;
    for(unsigned char c:body) {crc^=c;for(int k=0;k<8;++k) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1)));}
    crc=~crc;
    std::vector<std::string> names={fixture_name};
    if(name!=fixture_name) names.push_back(name);
    std::string s,central;
    for(const auto &entry:names) {
        unsigned offset=s.size();
        put(s,0x04034b50,4);put(s,20,2);put(s,0,2);put(s,0,2);put(s,0,4);put(s,crc,4);
        put(s,body.size(),4);put(s,body.size(),4);put(s,entry.size(),2);put(s,0,2);s+=entry;s+=body;
        put(central,0x02014b50,4);put(central,0x0314,2);put(central,20,2);put(central,0,2);put(central,0,2);put(central,0,4);put(central,crc,4);
        put(central,body.size(),4);put(central,body.size(),4);put(central,entry.size(),2);put(central,0,2);put(central,0,2);put(central,0,2);put(central,0,2);
        put(central,mode<<16,4);put(central,offset,4);central+=entry;
    }
    unsigned at=s.size();s+=central;
    put(s,0x06054b50,4);put(s,0,4);put(s,names.size(),2);put(s,names.size(),2);put(s,central.size(),4);put(s,at,4);put(s,0,2);return s;
}
class Server {
    lh::Socket socket_;std::thread worker_;std::atomic<bool> stop_{false};
public:
    std::atomic<unsigned> requests{0},ranges{0};std::string url;bool truncate=false;
    Server(std::string bytes,bool cut=false):socket_(::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0)),truncate(cut) {
        sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        lh::socket_check(!bind(socket_.get(),reinterpret_cast<sockaddr *>(&a),sizeof(a)),"bind");
        socklen_t n=sizeof(a);getsockname(socket_.get(),reinterpret_cast<sockaddr *>(&a),&n);
        url="http://127.0.0.1:"+std::to_string(ntohs(a.sin_port));listen(socket_.get(),16);
        worker_=std::thread([this,bytes] {
            while(!stop_) {
                pollfd p{socket_.get(),POLLIN,0};if(poll(&p,1,100)<=0) continue;
                lh::Socket peer(accept(socket_.get(),nullptr,nullptr));if(peer.get()<0) continue;
                auto req=peer.receive(2000);auto num=++requests;
                if(req.find("/bad ")!=std::string::npos) {peer.send("HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\n\r\n");continue;}
                auto range=req.find("Range: bytes=");size_t offset=0;
                if(range!=std::string::npos) {offset=std::stoull(req.substr(range+13));++ranges;}
                std::string header=offset?"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "+std::to_string(offset)+"-"+std::to_string(bytes.size()-1)+"/"+std::to_string(bytes.size())+"\r\n":"HTTP/1.1 200 OK\r\n";
                header+="Content-Length: "+std::to_string(bytes.size()-offset)+"\r\nConnection: close\r\n\r\n";
                try {peer.send(header); peer.send(bytes.substr(offset,truncate && num==1?bytes.size()/2:std::string::npos)); if(truncate && num==1) lh::sleep_ms(100);} catch(...) {}
            }
        });
    }
    ~Server(){stop_=true;worker_.join();}
};
}
LH_TEST(offline_and_receipt) {
    fixture_exe=t.harness("install").program();
    fixture_name=lh::fs::path(fixture_exe).extension()==".exe"?"payload/zig.exe":"payload/zig";
    auto d=t.temp(), archive=d/"input.zip";lh::write_file(archive,zip());
    auto cmd=[&](std::string mode){return t.harness("install").arg(d/"data").arg(archive).arg(mode).arg("offline");};
    LH_CONTAINS(t.ok(cmd("ok")).out,"after=2 result=1");
    lh::write_file(archive,"invalid");
    // Receipt is pinned to the archive identity, so restore the same fixture.
    lh::write_file(archive,zip());LH_CONTAINS(t.ok(cmd("ok")).out,"before=2");
    lh::write_file(d/("data/toolchain/test/"+lh::fs::path(fixture_name).filename().string()),"corrupt");
    LH_CONTAINS(t.ok(cmd("ok")).out,"before=1");
}
LH_TEST(reject_archive_and_cancel) {
    fixture_exe=t.harness("install").program();
    fixture_name=lh::fs::path(fixture_exe).extension()==".exe"?"payload/zig.exe":"payload/zig";
    auto d=t.temp(),a=d/"input.zip";
    for(const auto &mode:{"checksum","size","cancel-extract","kill"}) {
        lh::write_file(a,zip());auto cmd=t.harness("install").arg(d/"data").arg(a).arg(mode).arg("offline");
        LH_CHECK(t.run(cmd).rc!=0);
        LH_CONTAINS(t.ok(t.harness("install").arg(d/"data").arg(a).arg("ok").arg("offline")).out,"after=2");
        lh::fs::remove_all(d/"data");
    }
    for(const auto &name:{"../zig","/zig","payload/../zig"}) {
        lh::write_file(a,zip(name));LH_CHECK(t.run(t.harness("install").arg(d/"data").arg(a).arg("ok").arg("offline")).rc!=0);
    }
    lh::write_file(a,zip(fixture_name,0120777));LH_CHECK(t.run(t.harness("install").arg(d/"data").arg(a).arg("ok").arg("offline")).rc!=0);
}
LH_TEST(http_resume_fallback_and_no_requests) {
    fixture_exe=t.harness("install").program();
    fixture_name=lh::fs::path(fixture_exe).extension()==".exe"?"payload/zig.exe":"payload/zig";
    auto d=t.temp(),a=d/"input.zip";auto bytes=zip();lh::write_file(a,bytes);Server s(bytes,true);
    auto cmd=[&](std::string mode){return t.harness("install").arg(d/"data").arg(a).arg(mode).arg(s.url+"/ok");};
    LH_CHECK(t.run(cmd("ok")).rc!=0);
    LH_CONTAINS(t.ok(cmd("ok")).out,"after=2");LH_EQ(s.ranges.load(),1u);
    unsigned before=s.requests; t.ok(cmd("ok"));LH_EQ(s.requests.load(),before);
    lh::fs::remove_all(d/"data");
    LH_CHECK(t.run(cmd("cancel-download")).rc!=0);t.ok(cmd("ok"));
    lh::fs::remove_all(d/"data");
    t.ok(t.harness("install").arg(d/"data").arg(a).arg("ok").arg(s.url+"/bad").arg(s.url+"/ok"));
}
LH_TEST(concurrent_installers) {
    fixture_exe=t.harness("install").program();
    fixture_name=lh::fs::path(fixture_exe).extension()==".exe"?"payload/zig.exe":"payload/zig";
    auto d=t.temp(),a=d/"input.zip";lh::write_file(a,zip());
    auto cmd=[&](const char *mode){return t.harness("install").arg(d/"data").arg(a).arg(mode).arg("offline");};
    auto first=std::async(std::launch::async,[&]{return t.run(cmd("hold"));});
    for(int wait=0;wait<500 && !lh::fs::exists(d/"data/holding");++wait) lh::sleep_ms(10);
    LH_CHECK(lh::fs::exists(d/"data/holding"));
    auto busy=t.run(cmd("ok"));LH_CHECK(busy.rc!=0);LH_CONTAINS(busy.out,"Another installer");
    LH_EQ(first.get().rc,0);LH_CONTAINS(t.ok(cmd("ok")).out,"after=2");
}

LH_TEST(no_download_address_preserves_offline_install) {
    fixture_exe=t.harness("install").program();
    fixture_name=lh::fs::path(fixture_exe).extension()==".exe"?"payload/zig.exe":"payload/zig";
    auto d=t.temp(),a=d/"input.zip";auto bytes=zip();lh::write_file(a,bytes);Server s(bytes);
    auto missing=t.run(t.harness("install").arg(d/"data").arg(a).arg("no-url").arg(s.url));
    LH_CHECK(missing.rc!=0);LH_CONTAINS(missing.out,"No download URL configured for this host");
    LH_EQ(s.requests.load(),0u);
    LH_CONTAINS(t.ok(t.harness("install").arg(d/"data").arg(a).arg("no-url").arg("offline")).out,"after=2");
}
