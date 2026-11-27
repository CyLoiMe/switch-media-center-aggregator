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
/// How long the accept loop sleeps in poll(). It bounds two things: how late a
/// stopped server lets go of its port, and how long `stop()`'s join of that thread
/// can take — 20 ms is under a frame and a half, so leaving the page does not show.
constexpr int kAcceptPollMs = 20;
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

    /// The socket the service thread is serving right now (-1 when idle). stop()
    /// shuts it down to wake a recv() that is waiting on a client, so the join of
    /// that thread cannot wait for the 5 s receive timeout.
    std::atomic<int> activeFd{-1};

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

/// Releases a std::thread this libc will not join. `~std::thread` terminates the
/// process while the object is still joinable, so the only safe move is to leak the
/// handle — and only in a case that should not happen: `pthread_join` exists here
/// (utils/thread.cpp relies on it), `pthread_detach` does not.
void releaseUnjoinable(std::thread& t) {
    if (t.joinable()) new std::thread(std::move(t));
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
            // Stop means stop: what is still queued is closed by the drain instead
            // of served. A half-closed server answering a client would hold up the
            // join that the page destructor is doing.
            if (!st->running.load()) return;
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
    // Published so stop() can shut this socket down and break a blocking recv().
    st->activeFd.store(fd);
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
    st->activeFd.store(-1);  // before the close: a stale fd must not be shut down
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
        st->queueCv.notify_all();
        try {
            // The accept loop closes listenFd on its way out, so it goes first.
            if (st->acceptThread.joinable()) st->acceptThread.join();
            if (st->workerThread.joinable()) st->workerThread.join();
        } catch (const std::system_error&) {
            releaseUnjoinable(st->acceptThread);
            releaseUnjoinable(st->workerThread);
        }
        if (st->listenFd >= 0) {  // no accept thread existed: the fd is still ours
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

    // Join both threads — never detach. This runs from the page destructor, i.e.
    // inside Application::frame(), and on this libc `pthread_detach` returns ENOSYS
    // ("stopping the portal failed: Function not implemented"); the joinable
    // std::thread left behind then terminates the process when the state dies.
    //
    // The joins are short by construction:
    //   - the accept loop owns the listening socket, wakes within kAcceptPollMs,
    //     closes it and exits (closing it from THIS thread used to free the socket
    //     under a blocked poll() — a data abort);
    //   - the service thread wakes on the notify, sees running == false and exits,
    //     or gives up its UI wait within one 20 ms slice because the portal is
    //     marked stopping (Portal::close sets that before calling this).
    // Joining is also what guarantees the listener is really closed by the time the
    // page is gone: a portal that outlives its page would break the one promise this
    // class makes.
    try {
        if (st->acceptThread.joinable()) st->acceptThread.join();
        brls::Logger::info("portal: accept thread joined");
        // A request that is sitting in recv() waiting for a slow client would hold
        // the join for up to the 5 s receive timeout. shutdown() wakes it and does
        // not free the socket (close would); a failure here is just a slower join.
        const int live = st->activeFd.load();
        if (live >= 0) ::shutdown(live, SHUT_RDWR);
        if (st->workerThread.joinable()) st->workerThread.join();
        brls::Logger::info("portal: service thread joined");
    } catch (const std::system_error& ex) {
        brls::Logger::error("portal: cannot join a server thread: {}", ex.what());
        releaseUnjoinable(st->acceptThread);
        releaseUnjoinable(st->workerThread);
    }

    drainQueue(*st);
    brls::Logger::info("portal: stopped");
}

bool HttpServer::running() const { return this->state && this->state->running.load(); }

int HttpServer::port() const { return this->state ? this->state->port : 0; }

}  // namespace portal
