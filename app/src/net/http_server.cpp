#include "net/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <borealis.hpp>

namespace portal {

namespace {

/// Enough for a phone page load (HTML + a few fetches) and small enough that a
/// hostile client cannot exhaust the console's thread table.
constexpr int kMaxConnections = 4;
constexpr int kRecvTimeoutSec = 5;
constexpr int kSendTimeoutSec = 5;
constexpr int kBacklog = 8;
constexpr int kAcceptPollMs = 500;

void setTimeouts(int fd) {
    timeval tv{};
    tv.tv_sec = kRecvTimeoutSec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = kSendTimeoutSec;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool sendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        // MSG_NOSIGNAL where the platform has it: a client that hangs up between
        // the response and the close must not take the process with it.
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, data + sent, len - sent, 0);
#endif
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

/// One response, already framed. `Connection: close` is not negotiable: without
/// keep-alive there is no pipelining state, no second request to parse and no
/// idle socket to time out.
std::string frame(const Response& r) {
    std::string out;
    out.reserve(r.body.size() + 256);
    out += "HTTP/1.1 ";
    out += std::to_string(r.status);
    out += ' ';
    out += statusText(r.status);
    out += "\r\nContent-Type: ";
    out += r.contentType;
    out += "\r\nContent-Length: ";
    out += std::to_string(r.body.size());
    out += "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
    for (const auto& h : r.headers) {
        out += h.first;
        out += ": ";
        out += h.second;
        out += "\r\n";
    }
    out += "\r\n";
    out += r.body;
    return out;
}

void respond(int fd, const Response& r) {
    const std::string out = frame(r);
    sendAll(fd, out.data(), out.size());
}

void respondSimple(int fd, int status, const char* text) {
    Response r;
    r.status = status;
    r.contentType = "text/plain; charset=utf-8";
    r.body = text;
    respond(fd, r);
}

}  // namespace

struct HttpServer::State {
    int listenFd = -1;
    int port = 0;
    std::atomic<bool> running{false};
    std::atomic<int> active{0};
    Handler handler;
    std::thread acceptThread;
};

namespace {

void serveOne(std::shared_ptr<HttpServer::State> st, int fd, const std::string& peer);

void acceptLoop(std::shared_ptr<HttpServer::State> st) {
    while (st->running.load()) {
        pollfd p{};
        p.fd = st->listenFd;
        p.events = POLLIN;
        const int rc = ::poll(&p, 1, kAcceptPollMs);
        if (rc <= 0) continue;                 // timeout or EINTR: re-check running
        if (!st->running.load()) break;

        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int cfd = ::accept(st->listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (cfd < 0) continue;

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

        if (st->active.load() >= kMaxConnections) {
            // Not a rate limit, an admission one: four in-flight requests are
            // already more than one phone shows, and refusing is better than
            // spawning threads until the console runs out.
            respondSimple(cfd, 503, "busy");
            ::close(cfd);
            continue;
        }
        st->active.fetch_add(1);
        // Detached on purpose: stop() must never join a handler that may be
        // waiting on the UI thread (see the header).
        std::thread(serveOne, st, cfd, std::string(ip)).detach();
    }
}

void serveOne(std::shared_ptr<HttpServer::State> st, int fd, const std::string& peer) {
    setTimeouts(fd);

    std::string raw;
    raw.reserve(2048);
    size_t headEnd = std::string::npos;
    char chunk[2048];

    while (true) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {  // closed, reset, or the receive budget expired
            ::close(fd);
            st->active.fetch_sub(1);
            return;
        }
        raw.append(chunk, static_cast<size_t>(n));
        headEnd = raw.find("\r\n\r\n");
        if (headEnd != std::string::npos) break;
        if (raw.size() > kMaxHeadBytes + 4) {
            respondSimple(fd, 413, "head too large");
            ::close(fd);
            st->active.fetch_sub(1);
            return;
        }
    }

    Request req;
    const ParseError err = parseHead(raw.substr(0, headEnd), req);
    if (err != ParseError::Ok) {
        const int status = err == ParseError::TooLarge || err == ParseError::TooManyHeaders ? 413
                           : err == ParseError::Unsupported                              ? 405
                                                                                         : 400;
        respondSimple(fd, status, statusText(status));
        ::close(fd);
        st->active.fetch_sub(1);
        return;
    }
    req.peer = peer;

    const long cl = contentLength(req);
    if (cl == -2) {
        respondSimple(fd, 400, "bad content-length");
        ::close(fd);
        st->active.fetch_sub(1);
        return;
    }
    if (cl == -3) {
        respondSimple(fd, 413, "body too large");
        ::close(fd);
        st->active.fetch_sub(1);
        return;
    }
    if (cl > 0) {
        req.body = raw.substr(headEnd + 4);
        // some of the body may already be in `raw`; ask only for the rest
        while (req.body.size() < static_cast<size_t>(cl)) {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                ::close(fd);
                st->active.fetch_sub(1);
                return;
            }
            req.body.append(chunk, static_cast<size_t>(n));
        }
        req.body.resize(static_cast<size_t>(cl));
    }

    Response resp;
    try {
        if (!st->handler) {
            resp.status = 503;
        } else {
            resp = st->handler(req);
        }
    } catch (const std::exception& ex) {
        // A handler that throws must not leave the client hanging: answer 500
        // and keep the message out of the body (it can name internal state).
        brls::Logger::warning("portal: handler threw: {}", ex.what());
        resp = Response{};
        resp.status = 500;
        resp.body = "{}";
    }

    respond(fd, resp);
    ::close(fd);
    st->active.fetch_sub(1);
}

}  // namespace

HttpServer::~HttpServer() { this->stop(); }

bool HttpServer::start(int preferredPort, Handler handler, int* actualPort) {
    this->stop();

    auto st = std::make_shared<State>();
    st->handler = std::move(handler);

    // 8080 is a popular default; the console shares its LAN with a NAS full of
    // containers, so try the next two before giving up.
    for (int attempt = 0; attempt < 3 && st->listenFd < 0; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(preferredPort + attempt));
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
            ::listen(fd, kBacklog) == 0) {
            st->listenFd = fd;
            st->port = preferredPort + attempt;
        } else {
            ::close(fd);
        }
    }

    if (st->listenFd < 0) {
        brls::Logger::warning("portal: no free port in {}-{}", preferredPort, preferredPort + 2);
        return false;
    }

    st->running.store(true);
    st->acceptThread = std::thread(acceptLoop, st);
    this->state = st;
    if (actualPort) *actualPort = st->port;
    brls::Logger::info("portal: listening on port {}", st->port);
    return true;
}

void HttpServer::stop() {
    auto st = this->state;
    if (!st) return;
    this->state.reset();

    st->running.store(false);
    if (st->listenFd >= 0) {
        ::shutdown(st->listenFd, SHUT_RDWR);
        ::close(st->listenFd);
        st->listenFd = -1;
    }
    if (st->acceptThread.joinable()) st->acceptThread.join();

    // Bounded and SHORT: stop() runs from the page's destructor, i.e. at the end of
    // a frame, so a long wait here is a visibly frozen console. A login in flight
    // can outlive this; it only ever touches globals (the portal singleton and
    // AppConfig), never the page that asked for it.
    for (int i = 0; i < 10 && st->active.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    brls::Logger::info("portal: stopped ({} request thread(s) still finishing)", st->active.load());
}

bool HttpServer::running() const { return this->state && this->state->running.load(); }

int HttpServer::port() const { return this->state ? this->state->port : 0; }

}  // namespace portal
