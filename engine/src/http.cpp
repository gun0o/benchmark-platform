#include "bench/http.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace bench {
namespace {

// A socket that closes itself, so every early return below cannot leak a descriptor.
class Fd {
  public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = other.release();
        }
        return *this;
    }
    int get() const { return fd_; }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

  private:
    int fd_;
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Connect with a deadline: a non-blocking connect() plus poll(), because a plain blocking
// connect to a host that drops packets takes the kernel's 2-minute SYN retry budget.
std::string connect_with_timeout(int fd, const sockaddr* addr, socklen_t len, int timeout_ms) {
    if (::connect(fd, addr, len) == 0)
        return {};
    if (errno != EINPROGRESS)
        return std::format("connect: {}", std::strerror(errno));
    pollfd p{fd, POLLOUT, 0};
    const int rc = ::poll(&p, 1, timeout_ms);
    if (rc == 0)
        return std::format("connect: timed out after {} ms", timeout_ms);
    if (rc < 0)
        return std::format("poll: {}", std::strerror(errno));
    int err = 0;
    socklen_t err_len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0)
        return std::format("getsockopt: {}", std::strerror(errno));
    if (err != 0)
        return std::format("connect: {}", std::strerror(err));
    return {};
}

std::string send_all(int fd, const std::string& data, int timeout_ms) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        pollfd p{fd, POLLOUT, 0};
        const int rc = ::poll(&p, 1, timeout_ms);
        if (rc == 0)
            return std::format("send: timed out after {} ms", timeout_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return std::format("poll: {}", std::strerror(errno));
        }
        // MSG_NOSIGNAL: a peer that closed early must give us EPIPE, not SIGPIPE, or the
        // benchmark process dies because the API restarted.
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return std::format("send: {}", std::strerror(errno));
        }
        sent += static_cast<std::size_t>(n);
    }
    return {};
}

// Read until the peer closes (the request asks for Connection: close) or the deadline
// passes. Returns the error string, empty on success.
std::string recv_all(int fd, std::string& out, int timeout_ms) {
    char buf[16384];
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        const int rc = ::poll(&p, 1, timeout_ms);
        if (rc == 0)
            return std::format("read: timed out after {} ms", timeout_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return std::format("poll: {}", std::strerror(errno));
        }
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0)
            return {}; // orderly close: the whole response is in `out`
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return std::format("read: {}", std::strerror(errno));
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
}

} // namespace

Url parse_url(const std::string& url) {
    constexpr std::string_view kHttp = "http://";
    if (url.starts_with("https://"))
        throw std::runtime_error("https is not supported by the engine's HTTP client: " + url);
    if (!url.starts_with(kHttp))
        throw std::runtime_error("not an http:// URL: " + url);

    Url out;
    const std::string rest = url.substr(kHttp.size());
    const auto slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (slash != std::string::npos)
        out.path = rest.substr(slash);
    if (authority.empty())
        throw std::runtime_error("URL has no host: " + url);

    const auto colon = authority.rfind(':');
    // rfind, then check it is not inside an IPv6 literal such as [::1]:8080.
    if (colon != std::string::npos && authority.find(']', colon) == std::string::npos) {
        const std::string port_str = authority.substr(colon + 1);
        unsigned int port = 0;
        const auto* end = port_str.data() + port_str.size();
        const auto [p, ec] = std::from_chars(port_str.data(), end, port);
        if (ec != std::errc{} || p != end || port == 0 || port > 65535)
            throw std::runtime_error("bad port in URL: " + url);
        out.port = static_cast<std::uint16_t>(port);
        authority = authority.substr(0, colon);
    }
    if (authority.size() >= 2 && authority.front() == '[' && authority.back() == ']')
        authority = authority.substr(1, authority.size() - 2); // IPv6 literal
    if (authority.empty())
        throw std::runtime_error("URL has no host: " + url);
    out.host = authority;
    return out;
}

std::string decode_chunked(const std::string& body) {
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        const auto eol = body.find("\r\n", pos);
        if (eol == std::string::npos)
            break;
        std::size_t len = 0;
        const char* begin = body.data() + pos;
        const char* end = body.data() + eol;
        const auto [p, ec] = std::from_chars(begin, end, len, 16);
        (void)p;
        if (ec != std::errc{})
            break;
        pos = eol + 2;
        if (len == 0)
            break;
        out.append(body, pos, std::min(len, body.size() - pos));
        pos += len + 2; // skip the chunk and its trailing CRLF
    }
    return out;
}

HttpResponse post_json(const Url& url, const std::string& body, int timeout_ms) {
    HttpResponse res;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* info = nullptr;
    const std::string port_str = std::to_string(url.port);
    const int gai = ::getaddrinfo(url.host.c_str(), port_str.c_str(), &hints, &info);
    if (gai != 0) {
        res.error = std::format("resolve {}: {}", url.host, ::gai_strerror(gai));
        return res;
    }
    struct InfoGuard {
        addrinfo* p;
        ~InfoGuard() { ::freeaddrinfo(p); }
    } guard{info};

    std::string last_error = "no address to connect to";
    for (addrinfo* ai = info; ai != nullptr; ai = ai->ai_next) {
        Fd fd{::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol)};
        if (fd.get() < 0) {
            last_error = std::format("socket: {}", std::strerror(errno));
            continue;
        }
        last_error = connect_with_timeout(fd.get(), ai->ai_addr, ai->ai_addrlen, timeout_ms);
        if (!last_error.empty())
            continue;

        const std::string host_header =
            url.host.find(':') != std::string::npos // IPv6 literal needs its brackets back
                ? std::format("[{}]:{}", url.host, url.port)
                : std::format("{}:{}", url.host, url.port);
        const std::string request =
            std::format("POST {} HTTP/1.1\r\n"
                        "Host: {}\r\n"
                        "User-Agent: bench/{}\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: {}\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        url.path, host_header, BENCH_VERSION, body.size()) +
            body;

        last_error = send_all(fd.get(), request, timeout_ms);
        if (!last_error.empty())
            continue;

        std::string raw;
        last_error = recv_all(fd.get(), raw, timeout_ms);
        if (!last_error.empty())
            continue;
        if (raw.empty()) {
            last_error = "empty response";
            continue;
        }

        const auto line_end = raw.find("\r\n");
        if (line_end == std::string::npos || !raw.starts_with("HTTP/1.")) {
            last_error = "malformed response: no status line";
            continue;
        }
        const auto sp = raw.find(' ');
        int status = 0;
        if (sp != std::string::npos)
            std::from_chars(raw.data() + sp + 1, raw.data() + line_end, status);
        if (status == 0) {
            last_error = "malformed response: no status code";
            continue;
        }

        const auto headers_end = raw.find("\r\n\r\n");
        const std::string headers =
            lower(raw.substr(line_end + 2, headers_end == std::string::npos
                                               ? std::string::npos
                                               : headers_end - line_end - 2));
        std::string payload =
            headers_end == std::string::npos ? std::string{} : raw.substr(headers_end + 4);
        if (headers.find("transfer-encoding: chunked") != std::string::npos)
            payload = decode_chunked(payload);

        res.status = status;
        res.body = std::move(payload);
        res.error.clear();
        return res;
    }
    res.error = last_error;
    return res;
}

} // namespace bench
