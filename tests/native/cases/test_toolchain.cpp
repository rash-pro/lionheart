#include "lh_test.h"
#include "network.h"
#include "tar_xz.h"
#include <algorithm>
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

// The official Linux archive is a GNU tar inside one xz stream; these build
// such archives entry by entry, with stored LZMA2 chunks.
namespace {
struct TarXz {
    lh::Case &t;
    lh::fs::path dir, archive;
    std::string exe;
    explicit TarXz(lh::Case &c) : t(c), dir(c.temp()), archive(dir / "input.tar.xz") {
        fixture_exe = t.harness("install").program();
        exe = lh::read_file(fixture_exe);
    }
    std::string zig() const { return lh::fs::path(fixture_exe).extension() == ".exe" ? "zig.exe" : "zig"; }
    lh::Result install(const std::string &bytes, const char *mode = "ok") {
        lh::write_file(archive, bytes);
        lh::fs::remove_all(dir / "data");
        return t.run(t.harness("install").arg(dir / "data").arg(archive).arg(mode).arg("offline"));
    }
    // The fixture as upstream lays it out: a top folder, the compiler, more
    // files beside it that are not kept.
    std::vector<std::string> members() const {
        return {tar_xz::member({"payload/", '5', ""}), tar_xz::member({"payload/lib/", '5', ""}),
                tar_xz::member({"payload/lib/not-kept.h", '0', "int x;\n"}), tar_xz::member({"payload/" + zig(), '0', exe})};
    }
    std::string installed() const {
        std::vector<std::string> names;
        std::string text;
        for (const auto &e : lh::fs::recursive_directory_iterator(dir / "data/toolchain/test"))
            if (e.is_regular_file()) names.push_back(lh::fs::relative(e.path(), dir / "data/toolchain/test").generic_string());
        std::sort(names.begin(), names.end());
        for (const auto &name : names) text += name + "\n";
        return text;
    }
};
}

LH_TEST(tar_xz_installs_only_the_listed_files) {
    TarXz a(t);
    auto members = a.members();
    // A name longer than a header holds, which GNU tar writes as an entry of its own.
    members.insert(members.begin() + 2, tar_xz::member({"payload/lib/" + std::string(120, 'n') + ".h", '0', "long\n"}));
    LH_CONTAINS(a.install(tar_xz::xz(tar_xz::tar(members))).out, "after=2 result=1");
    LH_TEXT_EQ(a.installed(), "receipt\n" + a.zig() + "\n");
    LH_BYTES_EQ(lh::read_file(a.dir / ("data/toolchain/test/" + a.zig())), a.exe);
}

LH_TEST(tar_xz_names_from_long_name_entries_pax_headers_and_ustar_prefixes) {
    TarXz a(t);
    // Each names the compiler in a different place than the header's name field.
    const std::string body = tar_xz::padded(a.exe);
    const std::string gnu = tar_xz::header("././@LongLink", 'L', 8 + a.zig().size() + 1) + tar_xz::padded("payload/" + a.zig() + '\0') +
                            tar_xz::header("payload/elsewhere", '0', a.exe.size()) + body;
    const std::string records = tar_xz::pax_record("mtime", "1.5") + tar_xz::pax_record("path", "payload/" + a.zig());
    const std::string pax = tar_xz::header("PaxHeaders/x", 'x', records.size(), "", true) + tar_xz::padded(records) +
                            tar_xz::header("payload/elsewhere", '0', a.exe.size(), "", true) + body;
    const std::string global = tar_xz::pax_record("comment", "made by hand");
    const std::string prefixed = tar_xz::header("pax_global_header", 'g', global.size(), "", true) + tar_xz::padded(global) +
                                 tar_xz::member({"payload/" + a.zig(), '0', a.exe, true});
    for (const auto &entry : {gnu, pax, prefixed}) {
        LH_CONTAINS(a.install(tar_xz::xz(tar_xz::tar({entry}))).out, "after=2 result=1");
        LH_TEXT_EQ(a.installed(), "receipt\n" + a.zig() + "\n");
    }
}

LH_TEST(tar_xz_refuses_entries_that_leave_the_destination) {
    TarXz a(t);
    // Refused anywhere in the archive, kept or not, as the ZIP reader refuses them.
    std::vector<std::string> bad = {
        tar_xz::member({"../zig", '0', "x"}), tar_xz::member({"/zig", '0', "x"}),
        tar_xz::member({"payload/../zig", '0', "x"}), tar_xz::member({"payload/lib/../../zig", '0', "x"}),
        tar_xz::member({"./payload/zig", '0', "x"}), tar_xz::member({"payload\\..\\zig", '0', "x"}),
        tar_xz::header("payload/link", '2', 0) , tar_xz::header("payload/hard", '1', 0),
        tar_xz::header("payload/char", '3', 0), tar_xz::header("payload/block", '4', 0),
        tar_xz::header("payload/fifo", '6', 0), tar_xz::header("payload/long-link", 'K', 0),
        tar_xz::member({"payload/" + std::string(100, 'a') + "/../../../zig", '0', "x"})};
    const std::string escaping = tar_xz::pax_record("path", "../" + a.zig());
    bad.push_back(tar_xz::header("PaxHeaders/x", 'x', escaping.size(), "", true) + tar_xz::padded(escaping) +
                  tar_xz::header("payload/fine", '0', 1, "", true) + tar_xz::padded("x"));
    // A global header may not name or size entries.
    const std::string global = tar_xz::pax_record("path", "payload/" + a.zig());
    bad.push_back(tar_xz::header("pax_global_header", 'g', global.size(), "", true) + tar_xz::padded(global));
    for (const auto &entry : bad) {
        auto members = a.members();
        members.insert(members.begin() + 1, entry);
        const auto r = a.install(tar_xz::xz(tar_xz::tar(members)));
        LH_CHECK_MSG(r.rc != 0, r.out);
        LH_CONTAINS(r.out, "reason=Invalid archive or missing retained file");
        LH_CHECK(!lh::fs::exists(a.dir / "data/toolchain/test"));
    }
}

LH_TEST(tar_xz_refuses_damaged_and_incomplete_archives) {
    TarXz a(t);
    const std::string whole = tar_xz::xz(tar_xz::tar(a.members()));
    LH_CONTAINS(a.install(whole).out, "after=2");
    // The archive's checksum is not the pinned one.
    LH_CHECK(a.install(whole, "checksum").rc != 0);
    // Cut inside the stream header, the block, the check, the index and the footer.
    for (size_t at : {size_t(6), size_t(20), whole.size() / 2, whole.size() - 40, whole.size() - 20, whole.size() - 1}) {
        const auto r = a.install(whole.substr(0, at));
        LH_CHECK_MSG(r.rc != 0, "cut at " + std::to_string(at));
        LH_CONTAINS(r.out, "reason=Invalid archive or missing retained file");
    }
    // One byte changed inside the block: the xz check catches it after the
    // pinned checksum, here the damaged archive's own, has let it through.
    std::string damaged = whole;
    damaged[whole.size() / 2] ^= 0x10;
    LH_CONTAINS(a.install(damaged).out, "reason=Invalid archive or missing retained file");
    // Bytes after the stream, a missing end, a bad header checksum.
    LH_CHECK(a.install(whole + std::string(4, '\0')).rc != 0);
    std::string no_end;
    for (const auto &m : a.members()) no_end += m;
    LH_CHECK(a.install(tar_xz::xz(no_end)).rc != 0);
    auto members = a.members();
    members.back()[0] ^= 1;
    LH_CHECK(a.install(tar_xz::xz(tar_xz::tar(members))).rc != 0);
    // Without the compiler the archive is not the one asked for.
    members = a.members();
    members.pop_back();
    LH_CHECK(a.install(tar_xz::xz(tar_xz::tar(members))).rc != 0);
}

LH_TEST(tar_xz_cancelled_while_unpacking_installs_on_retry) {
    TarXz a(t);
    const std::string whole = tar_xz::xz(tar_xz::tar(a.members()));
    LH_CHECK(a.install(whole, "cancel-extract").rc != 0);
    LH_CONTAINS(t.ok(t.harness("install").arg(a.dir / "data").arg(a.archive).arg("ok").arg("offline")).out, "after=2");
}
