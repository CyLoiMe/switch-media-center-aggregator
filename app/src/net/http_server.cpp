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
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <borealis.hpp>

namespace portal {

namespace {

constexpr int kRecvTimeoutSec = 5;
constexpr int kSendTimeoutSec = 5;
constexpr int kBacklog = 8;
/// How long the accept loop sleeps in poll(). It bounds how late a stopped server
/// lets go of its port, because that thread is the one that closes the socket.
constexpr int kAcceptPollMs = 100;
/// Idle wake-up of the service thread waiting for work.
constexpr int kWorkerWaitMs = 500;
/// Connections waiting for the service thread. A browser opens six at once; past
/// this, answering 503 is cheaper than growing a queue nobody drains.
constexpr size_t kMaxQueued = 8;

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
    Handler handler;

    /// Accepted sockets waiting for the service thread, with the peer address
    /// remembered next to each: the queue is drained by another thread and
    /// getpeername() after a close is useless.
    std::deque<int> queue;
    std::map<int, std::string> queuePeers;
    std::mutex queueMutex;
    std::condition_variable queueCv;

    std::thread acceptThread;
    std::thread workerThread;
};

namespace {

void serveOne(std::shared_ptr<HttpServer::State> st, int fd, const std::string& peer);

/// Closes every accepted-but-unserved socket. Called by whoever is last: the accept
/// loop on its way out (so a connection it queued after stop() is not leaked) and
/// stop() itself (for a server whose accept thread never started). Both take the
/// queue mutex, so a socket is only ever in one of the two.
void drainQueue(HttpServer::State& st) {
    std::lock_guard<std::mutex> lock(st.queueMutex);
    for (int fd : st.queue) ::close(fd);
    st.queue.clear();
    st.queuePeers.clear();
}

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

        bool full = false;
        {
            std::lock_guard<std::mutex> lock(st->queueMutex);
            if (st->queue.size() >= kMaxQueued) {
                full = true;
            } else {
                st->queue.push_back(cfd);
                st->queuePeers[cfd] = ip;
            }
        }
        if (full) {
            respondSimple(cfd, 503, "busy");
            ::close(cfd);
            continue;
        }
        st->queueCv.notify_one();
    }

    // This thread owns the listening socket: it is the only one that closes it.
    // Closing it from stop() while this loop sits in poll()/accept() on that very
    // fd frees the socket underneath a blocked call — the reason leaving the page
    // killed the app right after "Tab QuickConfig: delete".
    const int fd = st->listenFd;
    st->listenFd = -1;
    if (fd >= 0) ::close(fd);
    drainQueue(*st);
    brls::Logger::info("portal: listener closed");
}

void workerLoop(std::shared_ptr<HttpServer::State> st) {
    while (true) {
        int fd = -1;
        std::string peer;
        {
            std::unique_lock<std::mutex> lock(st->queueMutex);
            st->queueCv.wait_for(lock, std::chrono::milliseconds(kWorkerWaitMs),
                [&st] { return !st->queue.empty() || !st->running.load(); });
            if (st->queue.empty()) {
                if (!st->running.load()) return;
                continue;
            }
            fd = st->queue.front();
            st->queue.pop_front();
            auto it = st->queuePeers.find(fd);
            if (it != st->queuePeers.end()) {
                peer = it->second;
                st->queuePeers.erase(it);
            }
        }
        serveOne(st, fd, peer);
    }
}

void serveOne(std::shared_ptr<HttpServer::State> st, int fd, const std::string& peer) {
    // Every socket path is wrapped: an exception on this thread has no caller to
    // catch it, and an uncaught one is a process death ("the software was closed
    // because an error occurred") rather than a 500.
    int status = -1;
    try {
        status = [&]() -> int {
            setTimeouts(fd);

            std::string raw;
            raw.reserve(2048);
            size_t headEnd = std::string::npos;
            char chunk[2048];

            while (true) {
                const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
                if (n <= 0) return -1;  // closed, reset, or the receive budget expired
                raw.append(chunk, static_cast<size_t>(n));
                headEnd = raw.find("\r\n\r\n");
                if (headEnd != std::string::npos) break;
                if (raw.size() > kMaxHeadBytes + 4) {
                    respondSimple(fd, 413, "head too large");
                    return 413;
                }
            }

            Request req;
            const ParseError err = parseHead(raw.substr(0, headEnd), req);
            if (err != ParseError::Ok) {
                const int code = err == ParseError::TooLarge || err == ParseError::TooManyHeaders ? 413
                                 : err == ParseError::Unsupported                            ? 405
                                                                                             : 400;
                respondSimple(fd, code, statusText(code));
                return code;
            }
            req.peer = peer;

            const long cl = contentLength(req);
            if (cl == -2) {
                respondSimple(fd, 400, "bad content-length");
                return 400;
            }
            if (cl == -3) {
                respondSimple(fd, 413, "body too large");
                return 413;
            }
            if (cl > 0) {
                req.body = raw.substr(headEnd + 4);
                while (req.body.size() < static_cast<size_t>(cl)) {
                    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
                    if (n <= 0) return -1;
                    req.body.append(chunk, static_cast<size_t>(n));
                }
                req.body.resize(static_cast<size_t>(cl));
            }

            Response resp;
            if (!st->handler) {
                resp.status = 503;
            } else {
                resp = st->handler(req);
            }
            // One line per request: if a request kills the console, this names it.
            brls::Logger::info("portal: {} {} -> {}", req.method, req.path, resp.status);
            respond(fd, resp);
            return resp.status;
    }();
    } catch (const std::exception& ex) {
        // The comment above the lambda is not enough: without this the throw would
        // unwind this thread with no handler and kill the process.
        brls::Logger::error("portal: request from {} failed: {}", peer, ex.what());
        respondSimple(fd, 500, "error");
    } catch (...) {
        brls::Logger::error("portal: request from {} failed", peer);
        respondSimple(fd, 500, "error");
    }

    if (status < 0) brls::Logger::info("portal: connection from {} closed without a request", peer);
    ::close(fd);
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

    // Both threads are created HERE, on the UI thread, before the page is shown.
    // Nothing creates a thread from another thread later on — the rest of this app
    // also creates its workers once, at startup.
    st->running.store(true);
    try {
        st->acceptThread = std::thread(acceptLoop, st);
        st->workerThread = std::thread(workerLoop, st);
    } catch (const std::exception& ex) {
        // thread creation can fail (EAGAIN); a throw here would escape the page's
        // constructor, so it is a "cannot start", not a crash.
        brls::Logger::error("portal: cannot start service threads: {}", ex.what());
        st->running.store(false);
        if (st->workerThread.joinable()) st->workerThread.detach();
        if (st->acceptThread.joinable()) {
            st->acceptThread.detach();  // it owns listenFd and closes it on the way out
        } else if (st->listenFd >= 0) {
            ::close(st->listenFd);
            st->listenFd = -1;
        }
        return false;
    }
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
    st->queueCv.notify_all();
    brls::Logger::info("portal: stopping");

    // Nothing here waits, and nothing here closes a socket that another thread is
    // using. This runs from a page destructor, i.e. inside Application::frame():
    //   - joining the accept loop means blocking the frame for a poll tick;
    //   - closing the listening socket here is worse: the accept loop may be inside
    //     poll()/accept() on that fd, and freeing it from under a blocked call is a
    //     data abort, not an error code.
    // So each thread releases what it owns (the accept loop closes the listener on
    // its next tick) and both are detached. The shared_ptr keeps State alive until
    // the last one is gone.
    if (st->acceptThread.joinable()) st->acceptThread.detach();
    if (st->workerThread.joinable()) st->workerThread.detach();
    brls::Logger::info("portal: threads released");

    // Sockets the service thread will never reach: closing them from here is what
    // unblocks a browser still waiting for a response. The accept loop repeats this
    // on its way out, for whatever it queued in the meantime.
    drainQueue(*st);
    brls::Logger::info("portal: stopped");
}

bool HttpServer::running() const { return this->state && this->state->running.load(); }

int HttpServer::port() const { return this->state ? this->state->port : 0; }

}  // namespace portal
