// Standalone logic test — the HTTP subset the LAN portal parses.
//
// Everything here is reachable from the network before any authentication, so a
// rejection that does not happen is a bug, not an inconvenience. See
// net/http_types.hpp.

#include <cstdio>
#include <string>

#include <net/http_types.hpp>

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__);  \
            ++failures;                                      \
        }                                                    \
    } while (0)

using portal::ParseError;
using portal::Request;

static Request parse(const std::string& head, ParseError* err = nullptr) {
    Request r;
    const ParseError e = portal::parseHead(head, r);
    if (err) *err = e;
    return r;
}

int main() {
    // --- request line -------------------------------------------------------
    {
        Request r = parse("GET /api/state HTTP/1.1\r\nHost: 192.168.3.7:8080\r\n");
        CHECK(r.method == "GET");
        CHECK(r.path == "/api/state");
        CHECK(r.query.empty());
        CHECK(r.header("host") == "192.168.3.7:8080");
        CHECK(r.header("HOST") == "192.168.3.7:8080");  // names are lower-cased on the way in
    }
    {
        Request r = parse("POST /api/session?x=1&y=2 HTTP/1.1\r\n");
        CHECK(r.path == "/api/session");
        CHECK(r.query == "x=1&y=2");
    }
    {
        // LF alone is tolerated: the tests and a few tiny clients send it
        Request r = parse("DELETE /api/draft/connection/abc\n");
        CHECK(r.method == "DELETE");
        CHECK(r.path == "/api/draft/connection/abc");
    }

    // --- methods and versions we refuse -------------------------------------
    {
        ParseError e;
        parse("PUT /api/state HTTP/1.1\r\n", &e);
        CHECK(e == ParseError::Unsupported);
        parse("OPTIONS /api/state HTTP/1.1\r\n", &e);
        CHECK(e == ParseError::Unsupported);
        parse("GET /api/state HTTP/9.9\r\n", &e);
        CHECK(e == ParseError::Unsupported);
    }

    // --- malformed heads ----------------------------------------------------
    {
        ParseError e;
        parse("GET\r\n", &e);
        CHECK(e == ParseError::Malformed);          // no target, no version
        parse("GET /x\r\n", &e);
        CHECK(e == ParseError::Malformed);          // no version
        parse("GET x HTTP/1.1\r\n", &e);
        CHECK(e == ParseError::Malformed);          // target must start with '/'
        parse("GET /x HTTP/1.1\r\nNoColonHere\r\n", &e);
        CHECK(e == ParseError::Malformed);
        parse("GET /x HTTP/1.1\r\nFolded: a\r\n  b\r\n", &e);
        CHECK(e == ParseError::Malformed);          // obs-fold
        parse(std::string("GET /x HTTP/1.1\r\nA: b\0c\r\n", 25), &e);
        CHECK(e == ParseError::Malformed);          // NUL byte
    }
    {
        // one header too many
        std::string head = "GET /x HTTP/1.1\r\n";
        for (size_t i = 0; i <= portal::kMaxHeaders; ++i)
            head += "H" + std::to_string(i) + ": v\r\n";
        ParseError e;
        parse(head, &e);
        CHECK(e == ParseError::TooManyHeaders);
    }
    {
        // an oversized head is refused before it is tokenised
        std::string head = "GET /x HTTP/1.1\r\n";
        head.append(portal::kMaxHeadBytes, 'A');
        ParseError e;
        parse(head, &e);
        CHECK(e == ParseError::TooLarge);
    }

    // --- Content-Length -----------------------------------------------------
    {
        Request r = parse("POST /api/session HTTP/1.1\r\nContent-Length: 12\r\n");
        CHECK(portal::contentLength(r) == 12);
        Request none = parse("GET /api/state HTTP/1.1\r\n");
        CHECK(portal::contentLength(none) == -1);
        Request bad = parse("POST / HTTP/1.1\r\nContent-Length: -3\r\n");
        CHECK(portal::contentLength(bad) == -2);
        Request hex = parse("POST / HTTP/1.1\r\nContent-Length: 0x10\r\n");
        CHECK(portal::contentLength(hex) == -2);
        Request big = parse("POST / HTTP/1.1\r\nContent-Length: 999999999\r\n");
        CHECK(portal::contentLength(big) == -3);
        // a body we cannot measure is refused rather than read until close
        Request chunked = parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n");
        CHECK(portal::contentLength(chunked) == -2);
    }

    // --- cookies ------------------------------------------------------------
    {
        Request r = parse("GET / HTTP/1.1\r\nCookie: a=1; smca=deadbeef; b=2\r\n");
        CHECK(r.cookie("smca") == "deadbeef");
        CHECK(r.cookie("a") == "1");
        CHECK(r.cookie("b") == "2");
        CHECK(r.cookie("missing").empty());
        // a name that is a prefix of another must not match
        Request p = parse("GET / HTTP/1.1\r\nCookie: smca2=nope\r\n");
        CHECK(p.cookie("smca").empty());
    }

    // --- percent-decoding and query -----------------------------------------
    {
        CHECK(portal::percentDecode("a%20b") == "a b");
        CHECK(portal::percentDecode("%E4%B8%AD") == "\xE4\xB8\xAD");
        CHECK(portal::percentDecode("100%") == "100%");      // trailing '%' left alone
        CHECK(portal::percentDecode("%zz") == "%zz");        // malformed escape survives
        CHECK(portal::queryParam("pin=123456&x=1", "pin") == "123456");
        CHECK(portal::queryParam("x=a+b", "x") == "a b");
        CHECK(portal::queryParam("x=%E4%B8%AD", "x") == "\xE4\xB8\xAD");
        CHECK(portal::queryParam("pin=", "pin").empty());
        CHECK(portal::queryParam("", "pin").empty());
        CHECK(portal::queryParam("pin=1", "nope").empty());
    }

    // --- constant-time compare ----------------------------------------------
    {
        CHECK(portal::constantTimeEquals("abc", "abc"));
        CHECK(!portal::constantTimeEquals("abc", "abd"));
        CHECK(!portal::constantTimeEquals("abc", "ab"));
        CHECK(portal::constantTimeEquals("", ""));
    }

    // --- status text --------------------------------------------------------
    {
        CHECK(std::string(portal::statusText(200)) == "OK");
        CHECK(std::string(portal::statusText(401)) == "Unauthorized");
        CHECK(std::string(portal::statusText(999)).size() > 0);
    }

    if (failures == 0) printf("test_http_parse: ok\n");
    return failures == 0 ? 0 : 1;
}
