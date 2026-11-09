// Disposable driver for check_ra_fixture.py; delete both with the Python suite.
#include "ra_service.h"
#include "synthetic_disc.h"
#include <fstream>
#include <iostream>
namespace lh {
void write_file(const fs::path &p, const std::string &bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), std::streamsize(bytes.size()));
    if (!out)
        throw Failure("cannot write synthetic-disc equivalence input");
}
} // namespace lh
int main(int argc, char **argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--quote") {
            std::cout << lh::Json::quote(argv[2]) << std::endl;
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--disc") {
            synthetic::disc(argv[2], synthetic::Disc{std::stoi(argv[3]), 0, {}, {}, true});
            return 0;
        }
        if (argc != 3)
            return 2;
        lh::RaService service;
        service.configure([&](auto &c) {
            std::string scenario(argv[1]);
            c.now = 1700000000;
            c.origin = "http://127.0.0.1:12345";
            c.server = argv[2];
            if (scenario == "unknown")
                c.unknown = true;
            else if (scenario == "set-failure")
                c.set_failure = true;
            else if (scenario == "award-offline")
                c.award_mode = "offline";
            else if (scenario == "delay")
                c.delay_ms = 400;
            else if (scenario == "renewed")
                c.token = "renewed-test-token";
            else
                c.mode = scenario;
        });
        std::cout << service.port() << std::endl;
        for (std::string line; std::getline(std::cin, line);) {
            if (line == "log") {
                lh::Json::Array requests, awards;
                for (const auto &r : service.requests()) {
                    lh::Json::Object p;
                    for (const auto &v : r.params)
                        p.emplace(v.first, v.second);
                    requests.emplace_back(lh::Json::Array{r.path, p, r.agent});
                }
                for (const auto &r : service.awards()) {
                    lh::Json::Object p;
                    for (const auto &v : r)
                        p.emplace(v.first, v.second);
                    awards.emplace_back(p);
                }
                std::cout << lh::Json(lh::Json::Object{{"requests", requests},
                                                       {"awards", awards},
                                                       {"badge_requests", int(service.badge_requests())}})
                                 .dump()
                          << std::endl;
            } else if (line == "quit")
                break;
            else
                return 2;
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
