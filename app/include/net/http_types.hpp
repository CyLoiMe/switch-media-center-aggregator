/*
    SMCA — the HTTP bits that can be tested without a socket.

    The LAN config portal (Settings → Quick config) speaks a deliberately small
    subset of HTTP/1.1: one request per connection, `Connection: close`, no
    chunked bodies, no pipelining, no keep-alive. That is all a phone browser
    needs for six JSON endpoints, and every feature we leave out is one less
    parser bug reachable from the network.

    Everything here is pure: no sockets, no borealis, no logging. tests/run.sh
    compiles this header on the host, which is the only way to prove a parser
    rejects the malformed input it is supposed to reject — the console build
    would just crash once, on hardware, in front of the user.
*/

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace portal {

/// Bounded on purpose: a head over 8 KB or a body over 64 KB is refused with
/// 413 instead of being buffered. The biggest thing a browser sends us is a
/// username and a password.
inline constexpr size_t kMaxHeadBytes = 8 * 1024;
inline constexpr size_t kMaxBodyBytes = 64 * 1024;
inline constexpr size_t kMaxHeaders = 64;

struct Request {
    std::string method;
    std::string path;   ///< always starts with '/', never contains '?'
    std::string query;  ///< everything after '?', empty when absent
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;  ///< names lower-cased
    /// Filled by the server, never by the parser: the client address, formatted
    /// for display ("192.168.3.42"). The portal shows it so the person at the
    /// console can tell whether the device claiming the session is theirs.
    std::string peer;

    /// Header value by name (case-insensitive), empty when absent.
    std::string header(const std::string& name) const {
        for (const auto& h : this->headers)
            if (iequals(h.first, name)) return h.second;
        return {};
    }

    /// One cookie by name. Used for the session cookie; the portal sets exactly
    /// one, so this stays a scan rather than a module.
    std::string cookie(const std::string& name) const {
        const std::string all = this->header("cookie");
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(';', pos);
            if (end == std::string::npos) end = all.size();
            std::string item = trim(all.substr(pos, end - pos));
            const size_t eq = item.find('=');
            if (eq != std::string::npos && item.compare(0, eq, name) == 0 && eq == name.size())
                return item.substr(eq + 1);
            pos = end + 1;
        }
        return {};
    }

    static bool iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const unsigned char x = static_cast<unsigned char>(a[i]);
            const unsigned char y = static_cast<unsigned char>(b[i]);
            if (std::tolower(x) != std::tolower(y)) return false;
        }
        return true;
    }

    static std::string trim(const std::string& s) {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }
};

enum class ParseError {
    Ok = 0,
    TooLarge,      ///< head exceeded kMaxHeadBytes
    Malformed,     ///< request line or a header that does not parse
    TooManyHeaders,
    Unsupported,   ///< method or version we do not serve
};

/// Parses one head block: request-line then header lines, separated by CRLF (LF
/// alone is tolerated — the tests use it). `head` must NOT include the blank line
/// that ends the block; the caller finds `\r\n\r\n` first, because it also needs
/// to know where the body starts.
///
/// Rejections that matter, each of which would otherwise be a puzzle later:
///   - a NUL byte anywhere (request smuggling / log injection),
///   - a header line with no ':' or with leading whitespace (obs-fold),
///   - a target that does not start with '/',
///   - anything but GET / POST / DELETE (OPTIONS is refused too: no CORS here).
inline ParseError parseHead(const std::string& head, Request& out) {
    if (head.size() > kMaxHeadBytes) return ParseError::TooLarge;
    if (head.find('\0') != std::string::npos) return ParseError::Malformed;

    size_t pos = 0;
    bool first = true;
    while (pos <= head.size()) {
        size_t end = head.find('\n', pos);
        if (end == std::string::npos) end = head.size();
        std::string line = head.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = end + 1;

        if (first) {
            first = false;
            if (line.empty()) return ParseError::Malformed;
            const size_t sp1 = line.find(' ');
            if (sp1 == std::string::npos) return ParseError::Malformed;
            const size_t sp2 = line.find(' ', sp1 + 1);
            if (sp2 == std::string::npos) return ParseError::Malformed;
            out.method = line.substr(0, sp1);
            const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            const std::string version = line.substr(sp2 + 1);

            if (out.method != "GET" && out.method != "POST" && out.method != "DELETE")
                return ParseError::Unsupported;
            // Exactly the two versions a browser uses; HTTP/2 and 3 arrive as a
            // different protocol on a different port, and "HTTP/9.9" is a probe.
            if (version != "HTTP/1.1" && version != "HTTP/1.0") return ParseError::Unsupported;
            if (target.empty() || target[0] != '/') return ParseError::Malformed;

            const size_t q = target.find('?');
            if (q == std::string::npos) {
                out.path = target;
            } else {
                out.path = target.substr(0, q);
                out.query = target.substr(q + 1);
            }
            if (end >= head.size()) break;
            continue;
        }

        if (line.empty()) break;
        if (line[0] == ' ' || line[0] == '\t') return ParseError::Malformed;  // obs-fold
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return ParseError::Malformed;
        if (out.headers.size() >= kMaxHeaders) return ParseError::TooManyHeaders;
        std::string name = line.substr(0, colon);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.headers.emplace_back(std::move(name), Request::trim(line.substr(colon + 1)));
        if (end >= head.size()) break;
    }

    if (out.method.empty()) return ParseError::Malformed;
    return ParseError::Ok;
}

/// `Content-Length` of a parsed request. Absent means 0 (a GET has no body).
/// Anything we cannot honour is reported instead of guessed:
///   >= 0  the length to read
///   -1    absent
///   -2    not a plain decimal number (negative, hex, spaces, overflow)
///   -3    larger than `max`
inline long contentLength(const Request& r, size_t max = kMaxBodyBytes) {
    const std::string v = r.header("content-length");
    if (v.empty()) {
        // A body we cannot measure is worse than no body: refuse the encoding
        // we do not implement instead of reading until the client stops.
        if (!r.header("transfer-encoding").empty()) return -2;
        return -1;
    }
    if (v.size() > 9) return -3;
    long long n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return -2;
        n = n * 10 + (c - '0');
    }
    if (n < 0) return -2;
    if (static_cast<unsigned long long>(n) > max) return -3;
    return static_cast<long>(n);
}

/// %XX decoding. A malformed escape is left as-is rather than dropping the value:
/// the caller validates what it gets (a PIN of digits, an id we look up).
inline std::string percentDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

/// One query parameter, percent-decoded. `+` becomes a space (form encoding),
/// which is what a browser sends for a form field.
inline std::string queryParam(const std::string& query, const std::string& name) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        const std::string_view item(query.data() + pos, end - pos);
        const size_t eq = item.find('=');
        std::string key(item.substr(0, eq == std::string_view::npos ? item.size() : eq));
        if (key == name) {
            if (eq == std::string_view::npos) return {};
            std::string raw(item.substr(eq + 1));
            for (char& c : raw)
                if (c == '+') c = ' ';
            return percentDecode(raw);
        }
        pos = end + 1;
    }
    return {};
}

/// Comparison whose duration does not depend on where the first difference is.
/// The PIN and the session id are the only things between the LAN and the config
/// file, and `==` on std::string returns at the first mismatch.
inline bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

inline const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 423: return "Locked";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 504: return "Gateway Timeout";
        default: return "Error";
    }
}

}  // namespace portal
