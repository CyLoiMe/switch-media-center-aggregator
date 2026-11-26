#include "net/portal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <thread>

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include "api/http.hpp"
#include "api/jellyfin/auth.hpp"
#include "api/jellyfin/types.hpp"
#include "utils/config.hpp"
#include "utils/net_error.hpp"
#include "view/mpv_core.hpp"

namespace portal {

namespace {

/// Wrong PINs tolerated before the portal pauses itself. Five tries at a six-digit
/// code is 1 in 200 000, and clearing the pause needs a press on the console —
/// i.e. the one thing a script cannot do.
constexpr int kMaxPinFailures = 5;
constexpr int kIdleSeconds = 300;
/// How long a request thread waits for the UI thread. Long enough for a config
/// write, short enough that a hung UI does not hold the socket open.
constexpr int kUiTimeoutMs = 5000;

/// Seed material that cannot fail.
///
/// libstdc++ implements `std::random_device` on top of a real entropy device
/// (`/dev/urandom`), and on a console there may be none: the CONSTRUCTOR then
/// throws. This used to run inside the first call to rng() — i.e. inside the A
/// handler that mints a PIN — so the exception escaped a borealis action listener
/// and terminated the app: "the software was closed because an error occurred" on
/// the first A press, before a single digit existed.
///
/// The clocks, a stack address and a counter always exist, so they are the base;
/// random_device is mixed in only when it actually works.
uint64_t seedEntropy() {
    static std::atomic<uint64_t> counter{0};
    int probe = 0;
    uint64_t seed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    seed ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) << 21;
    seed ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&probe)) << 7;
    seed ^= counter.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ull;
    try {
        std::random_device rd;
        seed ^= static_cast<uint64_t>(rd()) << 32;
        seed ^= static_cast<uint64_t>(rd());
    } catch (...) {
        // No entropy device here: the mix above is the whole seed. A code that
        // lives five minutes and dies after five wrong attempts does not need
        // more than that.
    }
    // splitmix64 finaliser: the low bits of a nanosecond clock are predictable,
    // the bits after this are not usefully guessable from outside.
    seed += 0x9E3779B97F4A7C15ull;
    seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
    seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
    return seed ^ (seed >> 31);
}

std::mt19937_64& rng() {
    // Constructed once, on the first PIN: seedEntropy() must not throw (it does
    // not), because a throw here is a throw inside an action listener.
    static std::mt19937_64 engine(seedEntropy());
    return engine;
}

std::string randomDigits(int n) {
    std::uniform_int_distribution<int> d(0, 9);
    std::string out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) out.push_back(static_cast<char>('0' + d(rng())));
    return out;
}

std::string randomHex(int n) {
    static const char* digits = "0123456789abcdef";
    std::uniform_int_distribution<int> d(0, 15);
    std::string out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) out.push_back(digits[d(rng())]);
    return out;
}

Response jsonResponse(int status, const nlohmann::json& j) {
    Response r;
    r.status = status;
    r.contentType = "application/json";
    r.body = j.dump();
    return r;
}

Response errorJson(int status, const std::string& code) {
    return jsonResponse(status, nlohmann::json{{"error", code}});
}

nlohmann::json parseBody(const std::string& body) {
    if (body.empty()) return nlohmann::json::object();
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    return j.is_discarded() ? nlohmann::json::object() : j;
}

// ---- UI marshalling ------------------------------------------------------------
//
// The condition variable and the "done" flag live on the heap: a `brls::sync` task
// that runs after the waiter gave up must not write into a dead stack frame.

struct UiTask {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

/// Runs `fn` on the UI thread and waits for it. False on timeout. `fn` must capture
/// by value (or shared_ptr) — it may run after this function returns.
bool runOnUi(std::function<void()> fn, int timeoutMs = kUiTimeoutMs) {
    auto task = std::make_shared<UiTask>();
    brls::sync([task, fn = std::move(fn)]() {
        try {
            fn();
        } catch (const std::exception& ex) {
            brls::Logger::warning("portal: UI command failed: {}", ex.what());
        }
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->done = true;
        }
        task->cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(task->mutex);
    return task->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&task] { return task->done; });
}

// ---- i18n ----------------------------------------------------------------------
//
// The page follows the console's language, so the strings are read from the same
// romfs i18n table the rest of the app uses. A missing key yields the key itself
// (borealis logs it), which is visible in a browser and impossible to ship by
// accident once all 14 files carry the block.

const char* const kWebKeys[] = {
    "title", "pin_title", "pin_hint", "pin_submit", "pin_wrong", "pin_paused", "pin_idle",
    "claimed_other", "ua_label", "ua_hint", "proxy_label", "conn_title", "conn_name", "conn_addr",
    "conn_lan", "conn_type", "conn_user", "conn_password", "conn_add", "conn_delete", "conn_save",
    "conn_cancel", "conn_active", "conn_active_hint", "conn_addr_locked", "pending_title",
    "pending_none", "apply", "discard", "saved", "failed", "disconnected", "delete_confirm",
};

nlohmann::json stringsJson() {
    nlohmann::json out = nlohmann::json::object();
    for (const char* key : kWebKeys) out[key] = brls::getStr(std::string("web/") + key);
    return out;
}

/// One file out of romfs, or "" when it is not there.
///
/// `romfs::get()` is the compiled-in resource map borealis itself uses for i18n —
/// it needs no devoptab. The stream path is kept as a fallback for a build where
/// the map does not carry the web assets. `get()` throws on a missing path
/// (libromfs maps it to std::invalid_argument), so both branches are guarded.
std::string readAsset(const std::string& path) {
    try {
        return romfs::get(path).string();
    } catch (...) {
    }
    std::ifstream in("romfs:/" + path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

nlohmann::json connectionJson(const draft::Connection& c) {
    return nlohmann::json{
        {"id", c.key()},
        {"name", c.name},
        {"url", c.url},
        {"local_url", c.local_url},
        {"type", c.type},
        {"user_name", c.user_name},
    };
}

/// Builds the draft row for an applied connection: the page edits what is in
/// config.json, so the first touch of a row starts from the stored values (and the
/// stored token — the page never sees one).
bool seedFromConfig(const AppConfig& cfg, const std::string& profileId, draft::Connection& out) {
    for (const auto& u : cfg.getUsers()) {
        if (u.id != profileId) continue;
        out.from_id = u.server_id;
        out.from_user_id = u.id;
        out.id = u.server_id;
        out.user_id = u.id;
        out.token = u.access_token;
        out.user_name = u.name;
        out.can_download = u.can_download;
        for (const auto& s : cfg.getServers()) {
            if (s.id != u.server_id) continue;
            out.name = s.name;
            out.type = s.type;
            out.local_url = s.local_url;
            if (!s.urls.empty()) out.url = s.urls.front();
        }
        if (out.name.empty()) out.name = u.name;
        return true;
    }
    return false;
}

const char* kindName(draft::Kind k) {
    switch (k) {
        case draft::Kind::UserAgent: return "ua";
        case draft::Kind::Proxy: return "proxy";
        case draft::Kind::Add: return "add";
        case draft::Kind::Edit: return "edit";
        case draft::Kind::Remove: return "remove";
    }
    return "?";
}

}  // namespace

Portal& Portal::instance() {
    static Portal portal;
    return portal;
}

Portal::Portal() = default;

bool Portal::open() {
    if (this->server_.running()) return true;

    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->phase_ = Phase::Idle;
        this->pin_.clear();
        this->session_.clear();
        this->clientIp_.clear();
        this->failures_ = 0;
    }
    this->draft_.clear();  // UI thread: open() comes from the page's constructor
    this->webStrings = stringsJson();
    this->localeText = brls::Application::getLocale();
    this->pageHtml = readAsset("web/index.html");
    this->pageJs = readAsset("web/app.js");
    brls::Logger::info("portal: page {} bytes, script {} bytes", this->pageHtml.size(), this->pageJs.size());
    // One line that proves the strings loaded at all: if the page shows keys
    // instead of words, this is where to look.
    brls::Logger::info("portal: i18n web/title=\"{}\"", brls::getStr("web/title"));

    int port = 0;
    if (!this->server_.start(8080, [this](const Request& req) { return this->handle(req); }, &port))
        return false;
    return true;
}

void Portal::close() {
    this->server_.stop();
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->phase_ = Phase::Idle;
        this->pin_.clear();
        this->session_.clear();
        this->clientIp_.clear();
        this->failures_ = 0;
    }
    this->draft_.clear();
}

bool Portal::running() const { return this->server_.running(); }

int Portal::port() const { return this->server_.port(); }

std::string Portal::url() const {
    if (!this->server_.running()) return {};
    // A console without a network connection answers 0.0.0.0: show nothing rather
    // than an address that cannot be typed anywhere useful.
    const std::string ip = brls::Application::getPlatform()->getIpAddress();
    if (ip.empty() || ip == "0.0.0.0") return {};
    return "http://" + ip + ":" + std::to_string(this->server_.port());
}

Phase Portal::phase() {
    std::lock_guard<std::mutex> lock(this->mutex);
    if (this->phase_ == Phase::Claimed &&
        std::chrono::steady_clock::now() - this->lastSeen_ > std::chrono::seconds(kIdleSeconds)) {
        this->phase_ = Phase::Idle;
        this->session_.clear();
        this->pin_.clear();
        this->clientIp_.clear();
        this->failures_ = 0;
        brls::Logger::info("portal: session idle for {} s, dropped", kIdleSeconds);
    }
    return this->phase_;
}

std::string Portal::pin() {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->pin_;
}

std::string Portal::clientIp() {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->clientIp_;
}

int Portal::failures() {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->failures_;
}

int Portal::attemptsLeft() {
    std::lock_guard<std::mutex> lock(this->mutex);
    return kMaxPinFailures - this->failures_;
}

void Portal::arm() {
    // A new PIN invalidates the old one, and any session it produced: re-arming is
    // exactly "I am starting over".
    brls::Logger::info("portal: arming");
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->pin_ = randomDigits(6);
        this->session_.clear();
        this->clientIp_.clear();
        this->failures_ = 0;
        this->phase_ = Phase::Armed;
    }
    this->draft_.clear();
    brls::Logger::info("portal: armed with a new PIN");
}

void Portal::disconnect() {
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->phase_ = Phase::Idle;
        this->session_.clear();
        this->pin_.clear();
        this->clientIp_.clear();
        this->failures_ = 0;
    }
    this->draft_.clear();
    brls::Logger::info("portal: device disconnected from the console");
}

std::vector<draft::Change> Portal::changes() const { return this->draft_.changes(); }

bool Portal::apply() { return this->applyNow(); }

void Portal::discard() { this->draft_.clear(); }

bool Portal::authed(const Request& req) {
    std::lock_guard<std::mutex> lock(this->mutex);
    if (this->phase_ != Phase::Claimed || this->session_.empty()) return false;
    if (!constantTimeEquals(req.cookie("smca"), this->session_)) return false;
    this->lastSeen_ = std::chrono::steady_clock::now();
    return true;
}

bool Portal::applyNow() {
    if (this->draft_.empty()) return true;
    auto& cfg = AppConfig::instance();

    if (this->draft_.uaSet || this->draft_.uaCleared) {
        const std::string ua = this->draft_.uaCleared ? std::string() : this->draft_.ua;
        cfg.setItem(AppConfig::PLAYER_USER_AGENT, ua);
        // Same two steps as Settings → Network → User agent. Without the restart a
        // running mpv keeps the old UA, which is how a cloud front-end starts
        // answering 403 to a URL it signed for the previous one.
        HTTP::set_default_user_agent(ua);
        if (MPVCore::created()) MPVCore::instance().restart();
    }

    if (this->draft_.proxySet || this->draft_.proxyCleared) {
        const std::string proxy = this->draft_.proxyCleared ? std::string() : this->draft_.proxy;
        const bool on = !proxy.empty();
        cfg.setItem(AppConfig::HTTP_PROXY, proxy);
        cfg.setItem(AppConfig::HTTP_PROXY_STATUS, on);
        HTTP::PROXY = proxy;
        HTTP::PROXY_STATUS = on;
    }

    for (const auto& c : this->draft_.connections) {
        if (c.removal) {
            if (!c.from_user_id.empty()) cfg.removeUser(c.from_user_id);
            bool referenced = false;
            for (const auto& u : cfg.getUsers())
                if (u.server_id == c.from_id) referenced = true;
            if (!referenced && !c.from_id.empty()) cfg.removeServer(c.from_id);
            continue;
        }

        AppServer server;
        server.id = c.id;
        server.name = c.name;
        server.type = c.type;
        server.access_token = c.token;
        server.urls.clear();
        if (!c.url.empty()) server.urls.push_back(c.url);
        server.local_url = c.local_url;

        AppUser profile;
        profile.id = c.user_id;
        profile.name = c.user_name;
        profile.access_token = c.token;
        profile.server_id = server.id;
        profile.can_download = c.can_download;
        profile.thumb = jellyfin::userImageUrl(c.url, c.user_id, c.image_tag, c.type == "emby");

        if (c.from_id.empty()) {
            // Register without switching: upsert* never touches server_url/user_id,
            // which is the difference that lets a portal add an account while the
            // console keeps browsing a different one.
            cfg.upsertServer(server);
            cfg.upsertUser(profile);
        } else {
            // A re-login can change both ids; updateConnection() knows to migrate
            // the profile's own settings and to drop the stale server entry.
            cfg.updateConnection(c.from_id, c.from_user_id, server, profile, c.url, false);
        }
    }

    this->draft_.clear();
    brls::Logger::info("portal: draft applied");
    return true;
}

// ---- routing -------------------------------------------------------------------

Response Portal::handle(const Request& req) {
    if (!this->server_.running()) return errorJson(503, "closed");

    // Static page. Cheap, unauthenticated by design: it contains no data.
    if (req.method == "GET") {
        if (req.path == "/" || req.path == "/index.html") {
            if (this->pageHtml.empty()) return errorJson(500, "page missing");
            Response r;
            r.status = 200;
            r.contentType = "text/html; charset=utf-8";
            r.body = this->pageHtml;
            return r;
        }
        if (req.path == "/app.js") {
            if (this->pageJs.empty()) return errorJson(500, "script missing");
            Response r;
            r.status = 200;
            r.contentType = "text/javascript; charset=utf-8";
            r.body = this->pageJs;
            return r;
        }
    }

    // Everything the login screen needs, and nothing else: no connection names, no
    // addresses, no draft. The PIN screen has to work before a session exists.
    if (req.method == "GET" && req.path == "/api/state") {
        if (!this->authed(req)) {
            const Phase p = this->phase();
            return jsonResponse(200,
                nlohmann::json{{"phase", p == Phase::Armed ? "armed" : p == Phase::Paused ? "paused" : "idle"},
                    {"locale", this->localeText},
                    {"strings", this->webStrings},
                    {"need", "pin"}});
        }
        auto out = std::make_shared<std::string>();
        if (!runOnUi([this, out]() {
                nlohmann::json j;
                j["phase"] = "claimed";
                j["locale"] = this->localeText;
                j["url"] = this->url();
                j["version"] = AppVersion::getVersion();
                j["strings"] = this->webStrings;

                // getItem() is non-const (it may create the entry), and it is the
                // only way to read a stored string, so this is a mutable reference.
                auto& cfg = AppConfig::instance();
                // What is in force right now, so a field can be edited rather than
                // guessed. The UA is the stored one (an empty value means "the
                // built-in", which mpv.conf may still override — the page shows it
                // as empty, which is exactly what Apply would also store).
                j["current"] = nlohmann::json{
                    {"ua", cfg.getItem(AppConfig::PLAYER_USER_AGENT, std::string())},
                    {"proxy", HTTP::PROXY_STATUS ? HTTP::PROXY : std::string()},
                };
                j["active"] = nlohmann::json{{"id", cfg.getUserId()}};
                nlohmann::json list = nlohmann::json::array();
                for (const auto& u : cfg.getUsers()) {
                    const AppServer* srv = nullptr;
                    for (const auto& s : cfg.getServers())
                        if (s.id == u.server_id) srv = &s;
                    // `id` is the PROFILE: one server can carry two accounts, and the
                    // page edits accounts, not servers.
                    nlohmann::json row{{"id", u.id},
                        {"server_id", u.server_id},
                        {"name", (srv && !srv->name.empty()) ? srv->name : u.name},
                        {"user_name", u.name},
                        {"active", u.id == cfg.getUserId()}};
                    if (srv) {
                        row["url"] = srv->urls.empty() ? std::string() : srv->urls.front();
                        row["local_url"] = srv->local_url;
                        row["type"] = srv->type;
                    }
                    list.push_back(std::move(row));
                }
                j["connections"] = std::move(list);

                nlohmann::json d{
                    {"ua_set", this->draft_.uaSet},
                    {"ua", this->draft_.ua},
                    {"proxy_set", this->draft_.proxySet},
                    {"proxy", this->draft_.proxy},
                    {"ready", this->draft_.ready()},
                };
                nlohmann::json changes = nlohmann::json::array();
                for (const auto& c : this->draft_.changes())
                    changes.push_back(nlohmann::json{{"kind", kindName(c.kind)}, {"name", c.name}});
                d["changes"] = std::move(changes);
                j["draft"] = std::move(d);
                *out = j.dump();
            }))
            return errorJson(504, "ui timeout");
        nlohmann::json built = nlohmann::json::parse(*out, nullptr, false);
        if (built.is_discarded()) return errorJson(500, "state");
        return jsonResponse(200, built);
    }

    // --- claim the session ----------------------------------------------------
    if (req.method == "POST" && req.path == "/api/session") {
        const nlohmann::json body = parseBody(req.body);
        const std::string given = body.value("pin", std::string());

        std::string cookie;
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            if (this->phase_ == Phase::Paused) return errorJson(423, "paused");
            if (this->phase_ == Phase::Claimed)
                return jsonResponse(409, nlohmann::json{{"error", "claimed"}, {"ip", this->clientIp_}});
            if (this->phase_ != Phase::Armed) return errorJson(409, "idle");

            if (!constantTimeEquals(given, this->pin_)) {
                ++this->failures_;
                if (this->failures_ >= kMaxPinFailures) {
                    // Pausing, not rotating: the console has to be touched, so a
                    // script that keeps guessing only ever sees 423.
                    this->phase_ = Phase::Paused;
                    this->pin_.clear();
                    brls::Logger::warning("portal: {} wrong PINs, paused", this->failures_);
                }
                return jsonResponse(401,
                    nlohmann::json{{"error", "pin"}, {"left", kMaxPinFailures - this->failures_}});
            }

            this->session_ = randomHex(32);
            this->clientIp_ = req.peer;
            this->lastSeen_ = std::chrono::steady_clock::now();
            this->phase_ = Phase::Claimed;
            this->failures_ = 0;
            cookie = "smca=" + this->session_ + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=3600";
        }
        Response r = jsonResponse(200, nlohmann::json{{"ok", true}});
        r.headers.emplace_back("Set-Cookie", cookie);
        brls::Logger::info("portal: session claimed from {}", req.peer);
        return r;
    }

    if (!this->authed(req)) return errorJson(401, "auth");

    // --- user agent / proxy ---------------------------------------------------
    if (req.method == "POST" && req.path == "/api/draft/ua") {
        const nlohmann::json body = parseBody(req.body);
        const std::string raw = body.value("value", std::string());
        const std::string ua = draft::normalizeUserAgent(raw);
        if (ua.empty() && !raw.empty()) return errorJson(400, "ua");
        auto out = std::make_shared<int>(0);
        if (!runOnUi([this, ua, out]() {
                this->draft_.uaSet = true;
                this->draft_.uaCleared = ua.empty();
                this->draft_.ua = ua;
                *out = 1;
            }))
            return errorJson(504, "ui timeout");
        return jsonResponse(200, nlohmann::json{{"ok", true}});
    }

    if (req.method == "POST" && req.path == "/api/draft/proxy") {
        const nlohmann::json body = parseBody(req.body);
        const std::string raw = body.value("value", std::string());
        const std::string proxy = draft::normalizeProxy(raw);
        if (!runOnUi([this, proxy]() {
                this->draft_.proxySet = true;
                this->draft_.proxyCleared = proxy.empty();
                this->draft_.proxy = proxy;
            }))
            return errorJson(504, "ui timeout");
        return jsonResponse(200, nlohmann::json{{"ok", true}});
    }

    // --- add a connection (login happens here, on this thread) ----------------
    if (req.method == "POST" && req.path == "/api/draft/login") {
        const nlohmann::json body = parseBody(req.body);
        const std::string url = draft::normalizeUrl(body.value("url", std::string()));
        if (url.empty()) return errorJson(400, "url");
        const std::string user = body.value("user", std::string());
        const std::string pass = body.value("password", std::string());
        if (user.empty()) return errorJson(400, "user");
        std::string type = body.value("type", std::string("jellyfin"));
        if (type != "emby") type = "jellyfin";

        try {
            // Blocking, on purpose: this is a request thread, and the login is the
            // only slow thing the portal does. The UI thread must not wait for it.
            const jellyfin::LoginResult r = jellyfin::login(url, user, pass);
            draft::Connection c;
            c.from_id = body.value("from_id", std::string());
            c.from_user_id = body.value("from_user_id", std::string());
            c.type = type;
            c.url = url;
            c.local_url = draft::normalizeUrl(body.value("local_url", std::string()));
            c.id = r.serverId.empty() ? url : r.serverId;
            c.user_id = r.userId;
            c.token = r.token;
            c.user_name = r.userName;
            c.image_tag = r.userImageTag;
            c.can_download = r.canDownload;
            c.name = body.value("name", std::string());
            if (c.name.empty()) c.name = r.serverName.empty() ? url : r.serverName;

            const draft::Connection stored = c;
            if (!runOnUi([this, stored]() { this->draft_.upsert(stored); }))
                return errorJson(504, "ui timeout");
            return jsonResponse(200, connectionJson(c));
        } catch (const HTTP::Error& ex) {
            return errorJson(502, net::errorText(ex, url));
        } catch (const std::exception& ex) {
            return errorJson(502, ex.what());
        }
    }

    // --- edit / delete a connection already in the draft ----------------------
    if (req.method == "POST" && req.path == "/api/draft/connection") {
        const nlohmann::json body = parseBody(req.body);
        const std::string key = body.value("id", std::string());
        const std::string name = body.value("name", std::string());
        const std::string local = draft::normalizeUrl(body.value("local_url", std::string()));
        std::string type = body.value("type", std::string());
        if (type != "emby") type = "jellyfin";

        auto out = std::make_shared<std::string>();
        if (!runOnUi([this, out, key, name, local, type]() {
                draft::Connection* c = this->draft_.find(key);
                if (!c) {
                    // First touch of an applied row: start from what is stored. If
                    // the new values are the stored ones, queue nothing — otherwise
                    // merely opening the editor would leave a pending "edit".
                    draft::Connection seeded;
                    if (!seedFromConfig(AppConfig::instance(), key, seeded)) return;
                    const bool unchanged = (name.empty() || name == seeded.name) &&
                                           local == seeded.local_url && type == seeded.type;
                    if (unchanged) {
                        *out = connectionJson(seeded).dump();
                        return;
                    }
                    this->draft_.upsert(std::move(seeded));
                    c = this->draft_.find(key);
                    if (!c) return;
                }
                if (!name.empty()) c->name = name;
                c->local_url = local;
                c->type = type;
                *out = connectionJson(*c).dump();
            }))
            return errorJson(504, "ui timeout");
        if (out->empty()) return errorJson(404, "no such row");
        nlohmann::json built = nlohmann::json::parse(*out, nullptr, false);
        if (built.is_discarded()) return errorJson(500, "row");
        return jsonResponse(200, built);
    }

    if (req.method == "DELETE" && req.path.rfind("/api/draft/connection/", 0) == 0) {
        const std::string key = req.path.substr(std::string("/api/draft/connection/").size());
        auto found = std::make_shared<bool>(false);
        if (!runOnUi([this, key, found]() {
                if (!this->draft_.find(key)) {
                    // Deleting a row that was never drafted: seed it from config so
                    // the removal has ids to work with.
                    draft::Connection seeded;
                    if (!seedFromConfig(AppConfig::instance(), key, seeded)) return;
                    this->draft_.upsert(std::move(seeded));
                }
                *found = this->draft_.remove(key);
            }))
            return errorJson(504, "ui timeout");
        if (!*found) return errorJson(404, "no such row");
        return jsonResponse(200, nlohmann::json{{"ok", true}});
    }

    if (req.method == "POST" && req.path == "/api/draft/discard") {
        if (!runOnUi([this]() { this->draft_.clear(); })) return errorJson(504, "ui timeout");
        return jsonResponse(200, nlohmann::json{{"ok", true}});
    }

    if (req.method == "POST" && req.path == "/api/apply") {
        auto ok = std::make_shared<bool>(false);
        if (!runOnUi([this, ok]() { *ok = this->applyNow(); })) return errorJson(504, "ui timeout");
        if (!*ok) return errorJson(409, "not ready");
        return jsonResponse(200, nlohmann::json{{"ok", true}});
    }

    return errorJson(404, "not found");
}

}  // namespace portal
