// M1.4: the engine's minimal HTTP/1.1 client.
//
// The round-trip tests run a one-connection server on a loopback port in a thread, so they
// need no API, no network and no fixture files.
#include "bench/http.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

// A loopback server that accepts one connection, reads the request until the body is
// complete, and writes back whatever `response` says.
class OneShotServer {
  public:
    explicit OneShotServer(std::string response, bool reply = true, int delay_ms = 0)
        : response_(std::move(response)), reply_(reply), delay_ms_(delay_ms) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(listen_fd_, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // kernel picks a free port
        EXPECT_EQ(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        port_ = ::ntohs(addr.sin_port);
        EXPECT_EQ(::listen(listen_fd_, 1), 0);
        thread_ = std::thread([this] { serve(); });
    }

    ~OneShotServer() {
        if (thread_.joinable())
            thread_.join();
        if (listen_fd_ >= 0)
            ::close(listen_fd_);
    }

    std::uint16_t port() const { return port_; }
    const std::string& request() const { return request_; }

  private:
    void serve() {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0)
            return;
        // Read headers, then exactly Content-Length bytes of body.
        char buf[8192];
        std::size_t want_body = 0;
        bool have_headers = false;
        for (;;) {
            if (have_headers && request_.size() >= headers_end_ + 4 + want_body)
                break;
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            request_.append(buf, static_cast<std::size_t>(n));
            if (!have_headers) {
                const auto end = request_.find("\r\n\r\n");
                if (end != std::string::npos) {
                    have_headers = true;
                    headers_end_ = end;
                    const auto cl = request_.find("Content-Length: ");
                    if (cl != std::string::npos)
                        want_body = std::stoul(request_.substr(cl + 16));
                }
            }
        }
        if (delay_ms_ > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        if (reply_)
            ::send(fd, response_.data(), response_.size(), MSG_NOSIGNAL);
        ::close(fd);
    }

    std::string response_;
    bool reply_;
    int delay_ms_;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::string request_;
    std::size_t headers_end_ = 0;
    std::thread thread_;
};

std::string ok_response(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

} // namespace

TEST(ParseUrl, HostPortPath) {
    const auto u = bench::parse_url("http://localhost:8080/v1/runs");
    EXPECT_EQ(u.host, "localhost");
    EXPECT_EQ(u.port, 8080);
    EXPECT_EQ(u.path, "/v1/runs");
}

TEST(ParseUrl, DefaultsPortAndPath) {
    const auto u = bench::parse_url("http://api.example.com");
    EXPECT_EQ(u.host, "api.example.com");
    EXPECT_EQ(u.port, 80);
    EXPECT_EQ(u.path, "/");
}

TEST(ParseUrl, IPv6Literal) {
    const auto u = bench::parse_url("http://[::1]:8080/v1/runs");
    EXPECT_EQ(u.host, "::1");
    EXPECT_EQ(u.port, 8080);
    EXPECT_EQ(u.path, "/v1/runs");
}

TEST(ParseUrl, RejectsWhatItCannotDo) {
    // https is refused loudly rather than silently downgraded to http.
    EXPECT_THROW(bench::parse_url("https://example.com"), std::runtime_error);
    EXPECT_THROW(bench::parse_url("localhost:8080"), std::runtime_error);
    EXPECT_THROW(bench::parse_url("http://"), std::runtime_error);
    EXPECT_THROW(bench::parse_url("http://host:0/"), std::runtime_error);
    EXPECT_THROW(bench::parse_url("http://host:99999/"), std::runtime_error);
    EXPECT_THROW(bench::parse_url("http://host:abc/"), std::runtime_error);
}

TEST(DecodeChunked, RebuildsTheBody) {
    EXPECT_EQ(bench::decode_chunked("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"), "Wikipedia");
    EXPECT_EQ(bench::decode_chunked("0\r\n\r\n"), "");
}

TEST(PostJson, SendsAWellFormedRequestAndReadsTheReply) {
    const std::string body = R"({"schema_version":1,"results":[]})";
    OneShotServer server{ok_response(R"({"run_id":"x","inserted":0})")};
    bench::Url url{"127.0.0.1", server.port(), "/v1/runs"};

    const auto res = bench::post_json(url, body, 2000);
    ASSERT_TRUE(res.error.empty()) << res.error;
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.body, R"({"run_id":"x","inserted":0})");

    const std::string& req = server.request();
    EXPECT_TRUE(req.starts_with("POST /v1/runs HTTP/1.1\r\n")) << req;
    EXPECT_NE(req.find("Host: 127.0.0.1:" + std::to_string(server.port()) + "\r\n"),
              std::string::npos)
        << req;
    EXPECT_NE(req.find("Content-Type: application/json\r\n"), std::string::npos) << req;
    EXPECT_NE(req.find("Content-Length: " + std::to_string(body.size()) + "\r\n"),
              std::string::npos)
        << req;
    // The body must arrive byte-for-byte: a truncated envelope would be a 400 the
    // benchmark could not explain.
    EXPECT_TRUE(req.ends_with(body)) << req;
}

TEST(PostJson, ReportsNon2xxWithTheServersBody) {
    const std::string body = R"({"error":{"code":"schema_violation"}})";
    OneShotServer server{"HTTP/1.1 400 Bad Request\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body};
    bench::Url url{"127.0.0.1", server.port(), "/v1/runs"};

    const auto res = bench::post_json(url, "{}", 2000);
    ASSERT_TRUE(res.error.empty()) << res.error;
    EXPECT_EQ(res.status, 400);
    EXPECT_FALSE(res.ok());
    EXPECT_EQ(res.body, body); // the caller prints exactly what the API said
}

TEST(PostJson, UndoesChunkedTransferEncoding) {
    OneShotServer server{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                         "Connection: close\r\n\r\n7\r\n{\"a\":1}\r\n0\r\n\r\n"};
    bench::Url url{"127.0.0.1", server.port(), "/v1/runs"};

    const auto res = bench::post_json(url, "{}", 2000);
    ASSERT_TRUE(res.error.empty()) << res.error;
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.body, R"({"a":1})");
}

// The pitfall this milestone names: a hung API must not hang the benchmark.
TEST(PostJson, TimesOutOnASilentServer) {
    OneShotServer server{"", /*reply=*/false, /*delay_ms=*/3000};
    bench::Url url{"127.0.0.1", server.port(), "/v1/runs"};

    const auto start = std::chrono::steady_clock::now();
    const auto res = bench::post_json(url, "{}", 300);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              start)
            .count();
    EXPECT_FALSE(res.error.empty());
    EXPECT_NE(res.error.find("timed out"), std::string::npos) << res.error;
    EXPECT_LT(elapsed, 2000) << "the client waited " << elapsed << " ms for a 300 ms timeout";
}

TEST(PostJson, ReportsAConnectionRefusal) {
    // Port 1 on loopback: nothing listens there, and the refusal is immediate.
    bench::Url url{"127.0.0.1", 1, "/v1/runs"};
    const auto res = bench::post_json(url, "{}", 1000);
    EXPECT_FALSE(res.error.empty());
    EXPECT_EQ(res.status, 0);
}
