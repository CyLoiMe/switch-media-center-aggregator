/*
    SMCA — a very small HTTP/1.1 server, enough for the LAN config portal.

    Deliberately not cpp-httplib: that is ~11k lines, a thread pool, chunked
    encoding, keep-alive and its own exception model, all of which would have to
    be trusted on libnx where a bug is a black screen rather than a stack trace.
    Here the surface is one request per connection, `Connection: close`, a 5 s
    receive budget and a body we can size in advance (net/http_types.hpp refuses
    anything else).

    Lifetime, because that is what usually goes wrong:
      - the server owns a shared State; every thread holds a shared_ptr to it, so
        `stop()` can return immediately and the state dies with the last thread;
      - `stop()` only flips the running flag and detaches both threads. It does not
        join (that would block a frame for a poll tick) and it does not close the
        listening socket — the accept loop owns that fd and closes it when it wakes
        (kAcceptPollMs). Closing a socket from another thread while it sits in
        poll()/accept() is a data abort, not an error code: that is what killed the
        app every time the portal page was left;
      - the handler must therefore not depend on the server object still existing.
        The portal passes a lambda over its singleton, which lives for the process.
*/

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "net/http_types.hpp"

namespace portal {

struct Response {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    /// Extra headers, "Name: value" pairs. Cache-Control and nosniff are added
    /// for every response; these are for Set-Cookie only.
    std::vector<std::pair<std::string, std::string>> headers;
};

class HttpServer {
public:
    /// Runs on a request thread: never block on the UI thread for long (the
    /// portal marshals with a timeout), never throw across the boundary.
    using Handler = std::function<Response(const Request&)>;

    HttpServer() = default;
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// Binds the first free port in [preferred, preferred+2] — 8080 is a common
    /// choice and may already be taken by something else on the console's LAN.
    /// `actualPort` receives the bound one. False when no port could be bound.
    bool start(int preferredPort, Handler handler, int* actualPort);

    /// Idempotent, and safe to call from the UI thread.
    void stop();

    bool running() const;
    int port() const;

    /// Public only so the file-scope accept/request loops in the .cpp can name it;
    /// nothing outside net/http_server.cpp touches it.
    struct State;

private:
    std::shared_ptr<State> state;
};

}  // namespace portal
