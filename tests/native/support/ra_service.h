// Synthetic rc_client service, independent of the HTTP implementation it tests.
#pragma once
#include "lh_json.h"
#include "network.h"
#include <atomic>
#include <ctime>
#include <mutex>

namespace lh {
inline std::string badge_png() {
    // PNG of the mock's 64 RGBA rows: (70 + 2*y, 135, 195, 255).
    const std::string hex = "89504e470d0a1a0a0000000d4948445200000040000000400806000000aa6971de000000"
                            "e649444154789ce5c4d14618000040d1fb7549924c9224499224499224496a9224333349"
                            "922449924c92acfeabee7fdc87731838f8f82a63d0ca18b23286ad8c112b63d4caf86165"
                            "8c5919e356c684953169654c5919d356c68c95316b65cc5919f356c68295b168652c5919"
                            "cb56c68a95b16a65ac5919eb56c68695b169656c5919db56c68e95b16b65ec5919fb56c6"
                            "4f2be3d0ca38b2328ead8c132be3d4caf86565fcb632fe58197fad8c332be3dccab8b032"
                            "2ead8c2b2be3dacab8b1326ead8c3b2be3deca78b0321ead8c272be3d9caf86765bc5819"
                            "af56c69b95f16e65fcb7323eadec1b4212e29581d28fb60000000049454e44ae426082";
    std::string out;
    for (size_t i = 0; i < hex.size(); i += 2)
        out += char(std::stoul(hex.substr(i, 2), nullptr, 16));
    return out;
}
class RaService {
  public:
    using Params = std::map<std::string, std::string>;
    struct Request {
        std::string path;
        Params params;
        std::string agent;
    };
    struct Config {
        std::string mode = "ok", award_mode = "ok", token = "test-token";
        bool unknown = false, set_failure = false;
        unsigned delay_ms = 0;
        time_t now = 0;
        std::string origin, server = "LionheartFixture/1";
    };
    RaService() : listener_(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        socket_check(listener_.get() >= 0, "service socket");
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socket_check(bind(listener_.get(), reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0,
                     "bind loopback fixture");
        socklen_t n = sizeof(a);
        socket_check(getsockname(listener_.get(), reinterpret_cast<sockaddr *>(&a), &n) == 0, "fixture port");
        port_ = ntohs(a.sin_port);
        url_ = "http://127.0.0.1:" + std::to_string(port_);
        socket_check(listen(listener_.get(), 16) == 0, "listen fixture");
        thread_ = std::thread([this] { serve(); });
    }
    ~RaService() {
        stop_ = true;
        shutdown(listener_.get(), SHUT_RDWR);
        thread_.join();
        for (auto &worker : workers_)
            worker.join();
    }
    RaService(const RaService &) = delete;
    RaService &operator=(const RaService &) = delete;
    unsigned port() const { return port_; }
    const std::string &url() const { return url_; }
    template <class F> void configure(F f) {
        std::lock_guard<std::mutex> lock(mutex_);
        f(config_);
    }
    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        check();
        return requests_;
    }
    std::vector<Params> awards() const {
        std::lock_guard<std::mutex> lock(mutex_);
        check();
        return awards_;
    }
    unsigned badge_requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        check();
        return badges_;
    }
    static Params parse_form(const std::string &body) {
        Params out;
        for (size_t at = 0; at < body.size();) {
            auto end = body.find('&', at);
            if (end == std::string::npos)
                end = body.size();
            auto eq = body.find('=', at);
            if (eq < end) {
                auto key = unquote(body.substr(at, eq - at));
                auto value = unquote(body.substr(eq + 1, end - eq - 1));
                if (!value.empty())
                    out.emplace(key, value);
            }
            at = end + 1;
        }
        return out;
    }

  private:
    Socket listener_;
    unsigned port_ = 0;
    std::string url_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    Config config_;
    std::vector<Request> requests_;
    std::vector<Params> awards_;
    unsigned badges_ = 0;
    std::string error_;
    void check() const {
        if (!error_.empty())
            throw Failure("loopback fixture: " + error_);
    }
    static std::string unquote(const std::string &s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '+')
                out += ' ';
            else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                     std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                out += char(std::stoul(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else
                out += s[i];
        }
        return out;
    }
    static std::string get(const Params &p, const std::string &key) {
        auto i = p.find(key);
        return i == p.end() ? "" : i->second;
    }
    std::string achievement_set(const Config &c) const {
        const auto u = c.origin.empty() ? url_ : c.origin;
        std::string entries;
        const char *titles[] = {"First signal", "Measured signal", "Saved signal"};
        const char *defs[] = {"0xH000100=7", "M:0xH000100>=10", "0xH000101=42"};
        const int points[] = {5, 10, 2};
        for (int i = 0; i < 3; ++i) {
            if (i)
                entries += ", ";
            auto id = std::to_string(101 + i);
            entries += "{\"ID\": " + id + ", \"Title\": " + Json::quote(titles[i]) +
                       ", \"Description\": \"Synthetic memory contract.\", "
                       "\"Flags\": 3, \"Points\": " +
                       std::to_string(points[i]) + ", \"MemAddr\": " + Json::quote(defs[i]) +
                       ", \"Author\": \"Fixture\", \"BadgeName\": \"" + id +
                       "\", \"Created\": 1, \"Modified\": 1, \"BadgeURL\": \"" + u + "/Badge/" + id +
                       ".png\"}";
        }
        return "{\"Success\": true, \"GameId\": 1234, \"Title\": \"Synthetic "
               "adventure\", \"ConsoleId\": 12, \"ImageIconUrl\": \"" +
               u +
               "/Images/1.png\", \"RichPresenceGameId\": 1234, "
               "\"RichPresencePatch\": \"\", \"Sets\": [{\"AchievementSetId\": 1, "
               "\"GameId\": 1234, \"Title\": null, \"Type\": \"core\", "
               "\"ImageIconUrl\": \"" +
               u + "/Images/1.png\", \"Achievements\": [" + entries +
               "], \"Leaderboards\": [{\"ID\": 501, \"Title\": \"Hidden board\", "
               "\"Description\": \"Never submitted\", \"Mem\": "
               "\"STA:0xH000100=1::CAN:0xH000100=2::SUB:0xH000100=7::VAL:"
               "0xH000100\", \"Format\": \"SCORE\"}]}]}";
    }
    std::pair<int, std::string> response(const Params &p, const Config &c) {
        const std::string maintenance = "{\"Success\": false, \"Error\": \"Maintenance\"}";
        if (c.mode == "offline")
            return {503, maintenance};
        if (c.mode == "blocked" || c.mode == "blocked-json")
            return {c.mode == "blocked" ? 403 : 200,
                    "{\"Success\": false, \"Error\": \"Client blocked\", \"Code\": "
                    "\"access_denied\"}"};
        if (c.mode == "expired")
            return {200, "{\"Success\": false, \"Error\": \"Token expired\", "
                         "\"Code\": \"expired_token\"}"};
        const auto op = get(p, "r");
        if (op == "awardachievement" && c.award_mode == "offline")
            return {503, maintenance};
        if (op == "login2") {
            if (get(p, "p") != "test-password" && get(p, "t") != c.token)
                return {200, "{\"Success\": false, \"Error\": \"Invalid credentials\", "
                             "\"Code\": \"invalid_credentials\"}"};
            return {200, "{\"Success\": true, \"User\": \"TestUser\", \"Token\": " + Json::quote(c.token) +
                             ", \"Score\": 0, \"SoftcoreScore\": 2, \"Messages\": 0, "
                             "\"Permissions\": 1, \"AccountType\": \"Registered\"}"};
        }
        if (op == "achievementsets") {
            if (c.set_failure)
                return {503, maintenance};
            if (c.unknown)
                return {200, "{\"Success\": false, \"Error\": \"Unknown game\", "
                             "\"Code\": \"not_found\", \"Status\": 404}"};
            return {200, achievement_set(c)};
        }
        if (op == "startsession")
            return {200, "{\"Success\": true, \"Unlocks\": [], \"HardcoreUnlocks\": "
                         "[], \"ServerNow\": " +
                             std::to_string(c.now ? c.now : std::time(nullptr)) + "}"};
        if (op == "awardachievement") {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                awards_.push_back(p);
            }
            return {200, "{\"Success\": true, \"AchievementID\": " + std::to_string(std::stoi(p.at("a"))) +
                             ", \"Score\": 0, \"SoftcoreScore\": 5, "
                             "\"AchievementsRemaining\": 2}"};
        }
        if (op == "gameid")
            return {200, "{\"Success\": true, \"GameID\": " + std::string(c.unknown ? "0" : "1234") + "}"};
        if (op == "ping")
            return {200, "{\"Success\": true}"};
        return {400, "{\"Success\": false, \"Error\": \"Unexpected mock operation\"}"};
    }
    void serve() {
        while (!stop_) {
            sockaddr_in peer{};
            socklen_t n = sizeof(peer);
            Socket s(accept4(listener_.get(), reinterpret_cast<sockaddr *>(&peer), &n, SOCK_CLOEXEC));
            if (s.get() < 0) {
                if (stop_)
                    break;
                if (errno == EINTR)
                    continue;
                record_error("accept failed");
                break;
            }
            if (peer.sin_family != AF_INET || ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
                record_error("non-loopback peer");
                continue;
            }
            workers_.emplace_back([this, s = std::move(s)]() mutable {
                try {
                    handle(s);
                } catch (const std::exception &e) {
                    record_error(e.what());
                }
            });
        }
    }
    void record_error(const std::string &s) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = s;
    }
    void handle(Socket &s) {
        timeval timeout{2, 0};
        setsockopt(s.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::string data;
        size_t end;
        while ((end = data.find("\r\n\r\n")) == std::string::npos) {
            char b[4096];
            auto n = recv(s.get(), b, sizeof(b), 0);
            socket_check(n > 0, "read HTTP headers");
            data.append(b, size_t(n));
            if (data.size() > 65536)
                throw Failure("oversized fixture request");
        }
        std::istringstream in(data.substr(0, end));
        std::string method, path, version, line;
        in >> method >> path >> version;
        std::getline(in, line);
        Params headers;
        while (std::getline(in, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            auto key = line.substr(0, colon);
            for (char &c : key)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            auto value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ')
                value.erase(0, 1);
            if (!value.empty() && value.back() == '\r')
                value.pop_back();
            headers[key] = value;
        }
        const auto length = headers.count("content-length") ? std::stoul(headers.at("content-length")) : 0;
        if (length > 65536)
            throw Failure("oversized fixture body");
        while (data.size() - end - 4 < length) {
            char b[4096];
            auto n = recv(s.get(), b, sizeof(b), 0);
            socket_check(n > 0, "read HTTP body");
            data.append(b, size_t(n));
        }
        Config c;
        int status = 200;
        std::string body, mime;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            c = config_;
        }
        if (method == "GET") {
            std::lock_guard<std::mutex> lock(mutex_);
            ++badges_;
            body = badge_png();
            mime = "image/png";
        } else if (method == "POST") {
            auto params = parse_form(data.substr(end + 4, length));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back({path, params, get(headers, "user-agent")});
            }
            sleep_ms(c.delay_ms);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                c = config_;
            }
            auto result = response(params, c);
            status = result.first;
            body = result.second;
            mime = "application/json";
        } else
            throw Failure("unsupported fixture HTTP method: " + method);
        time_t now = c.now ? c.now : std::time(nullptr);
        tm utc{};
        gmtime_r(&now, &utc);
        char date[64];
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc);
        const char *reason = status == 200   ? "OK"
                             : status == 400 ? "Bad Request"
                             : status == 403 ? "Forbidden"
                                             : "Service Unavailable";
        auto bytes = "HTTP/1.0 " + std::to_string(status) + " " + reason + "\r\nServer: " + c.server +
                     "\r\nDate: " + date + "\r\nContent-Type: " + mime +
                     "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        size_t at = 0;
        while (at < bytes.size()) {
            auto n = ::send(s.get(), bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EPIPE || errno == ECONNRESET))
                return;
            socket_check(n > 0, "write fixture response");
            at += size_t(n);
        }
    }
};
} // namespace lh
