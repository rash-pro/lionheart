#include "lh_test.h"
#ifdef _WIN32
#include <winsock2.h>
#include <thread>
LH_TEST(test_winhttp_dll_loopback) {
    WSADATA data;
    LH_EQ(WSAStartup(MAKEWORD(2,2), &data), 0);
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    LH_CHECK(server != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    LH_EQ(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    LH_EQ(listen(server, 1), 0);
    int size = sizeof(address);
    LH_EQ(getsockname(server, reinterpret_cast<sockaddr *>(&address), &size), 0);
    auto get = t.library("http").fn<int(const char *)>("lh_http_get");
    std::thread peer([&] {
        fd_set ready; FD_ZERO(&ready); FD_SET(server, &ready);
        timeval deadline{5,0};
        if (select(0, &ready, nullptr, nullptr, &deadline) > 0) {
            SOCKET client = accept(server, nullptr, nullptr);
            if (client != INVALID_SOCKET) {
                DWORD timeout = 3000;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
                char buffer[4096]; recv(client, buffer, sizeof(buffer), 0);
                const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
                send(client, response.data(), int(response.size()), 0);
                closesocket(client);
            }
        }
    });
    int result = get(("http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/test").c_str());
    peer.join(); closesocket(server); WSACleanup();
    LH_EQ(result, 1);
}
LH_TEST(test_windows_unicode_file_roundtrip) {
    auto path = t.temp() / lh::fs::path(L"save-\u00e9-\u65e5.dat");
    lh::write_file(path, std::string("a\0b\r\n",5));
    LH_BYTES_EQ(lh::read_file(path), std::string("a\0b\r\n",5));
    auto renamed = path.parent_path() / L"renamed.dat";
    lh::fs::rename(path, renamed);
    LH_CHECK(lh::fs::remove(renamed));
}
// The host reaches every file through extended paths, which the C runtime's own
// stat rejects on Windows itself while emulators accept them.
LH_TEST(test_windows_stat_describes_files_and_directories) {
    auto kind = t.library("io").fn<int(const char *, long long *)>("lh_stat_kind");
    auto make = t.library("io").fn<int(const char *)>("lh_make_directory");
    long long size = -1;
    auto directory = t.temp() / lh::fs::path(L"carpeta-\u00e9"), file = directory / "card.mcr";
    LH_EQ(kind(directory.u8string().c_str(), &size), 0);
    LH_EQ(make(directory.u8string().c_str()), 0);
    LH_EQ(kind(directory.u8string().c_str(), &size), 1);
    lh::write_file(file, std::string(1234, 'x'));
    LH_EQ(kind(file.u8string().c_str(), &size), 2);
    LH_EQ(size, 1234);
    auto forward = directory.u8string() + "/card.mcr";
    LH_EQ(kind(forward.c_str(), &size), 2);
    LH_EQ(kind(t.temp().root_path().u8string().c_str(), &size), 1);
}
// The player builds its folders one level at a time from a path that mixes both
// separators: the system's data folder, a forward slash, its own names.
LH_TEST(test_windows_player_folders_are_created_level_by_level) {
    auto tree = t.library("io").fn<const char *(const char *)>("lh_make_tree");
    auto kind = t.library("io").fn<int(const char *, long long *)>("lh_stat_kind");
    long long size = -1;
    auto base = t.temp().string();
    LH_EQ(std::string(tree((base + "/Lionheart/logs").c_str())), "");
    LH_EQ(kind((base + "\\Lionheart\\logs").c_str(), &size), 1);
    LH_EQ(std::string(tree((base + "/Lionheart/logs").c_str())), "");
    auto forward = base; for (char &c : forward) if (c == '\\') c = '/';
    LH_EQ(std::string(tree((forward + "/Second/saves").c_str())), "");
    lh::write_file(t.temp() / "file", "x");
    LH_CONTAINS(std::string(tree((base + "/file/logs").c_str())), "cannot use ");
}
// The folder the player would use on this machine must be one it can prepare a
// subfolder path for; this is where a system-specific rule would show.
LH_TEST(test_windows_data_folder_is_reachable) {
    auto folder = std::string(t.library("io").fn<const char *()>("lh_data_folder")());
    auto kind = t.library("io").fn<int(const char *, long long *)>("lh_stat_kind");
    long long size = -1;
    LH_CHECK(!folder.empty());
    auto parent = folder.substr(0, folder.find_last_of("/\\"));
    if (kind(parent.c_str(), &size) != 1) LH_FAIL("the parent of " + folder + " is not a directory the player can read");
}
#endif
