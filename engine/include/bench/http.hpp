// A minimal blocking HTTP/1.1 client over POSIX sockets, enough to POST a run envelope.
//
// Why not libcurl: the engine is a benchmark binary that must be trivial to build on a
// machine under test. One POST of one JSON document needs a socket, a request line and a
// response parser; a runtime dependency on libcurl would be a larger promise than this
// makes use of. Everything here times out, because a hung API must never hang a benchmark.
#pragma once

#include <cstdint>
#include <string>

namespace bench {

// A parsed http:// URL. https is not supported and is rejected with a message rather than
// silently treated as http.
struct Url {
    std::string host;
    std::uint16_t port = 80;
    std::string path = "/";
};

// Throws std::runtime_error on anything that is not an http:// URL this client can use.
Url parse_url(const std::string& url);

struct HttpResponse {
    int status = 0;        // HTTP status code, 0 when the request never completed
    std::string body;      // decoded body (chunked transfer-encoding is undone)
    std::string error;     // non-empty when the request failed before a status line
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// POST `body` as application/json. Connect, send and receive are each bounded by
// timeout_ms; the call returns rather than blocking forever on a stalled peer.
HttpResponse post_json(const Url& url, const std::string& body, int timeout_ms);

// Decode a chunked-transfer-encoding body. Exposed for testing.
std::string decode_chunked(const std::string& body);

} // namespace bench
