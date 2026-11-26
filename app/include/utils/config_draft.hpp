/*
    SMCA — the draft a LAN client edits, before anything is written to config.json.

    The portal never writes as you type. A change lands in this object, the console
    page shows it as a pending item, and only Apply turns it into an AppConfig call
    (utils/config_draft.hpp forces that order: there is no setter here that saves).
    Two reasons, both practical:
      - the screen is in front of you, so "what am I about to change" is worth
        showing before it happens, not after;
      - Discard is then free (drop the object), which is the only honest undo for a
        config whose writes go through a whole-file rewrite.

    Pure: no AppConfig, no borealis, no I/O — tests/run.sh compiles it on the host.

    The rule that keeps this list short: the portal may only touch connections that
    are NOT the active one. Editing the active connection means resetBackend() plus
    rebuilding the shell, and the shell is the page hosting the portal.
*/

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace draft {

/// One connection as the page sees it. `id` empty = a connection to create;
/// `removal` marks a delete. Both carry `user_id` so the profile can be found
/// even when a re-login is about to change the server id.
struct Connection {
    std::string id;       ///< AppServer::id, empty for a new one
    std::string user_id;  ///< AppUser::id, empty for a new one
    std::string type = "jellyfin";  ///< jellyfin | emby (Plex is console-only)
    std::string name;     ///< display name; falls back to the host
    std::string url;      ///< remote address
    std::string local_url;///< optional LAN address
    std::string token;    ///< filled by a successful login, never by the page
    std::string user_name;
    bool can_download = true;
    bool removal = false;

    bool needsLogin() const { return this->token.empty(); }
};

enum class Kind {
    UserAgent,
    Proxy,
    Add,     ///< create a connection
    Edit,    ///< change a connection's address/name/account
    Remove,  ///< delete a connection
};

struct Change {
    Kind kind;
    std::string name;  ///< connection name for Add/Edit/Remove, empty for the two knobs
};

/// Trims, then requires `http://` or `https://`, then drops a trailing '/'.
/// An address typed on a phone usually arrives without a scheme, and http is the
/// only sensible default for a Jellyfin/Emby server on a LAN. Returns "" when the
/// value cannot be an address (embedded space, empty host, other scheme) — the
/// caller turns that into a field error instead of storing a URL that cannot work.
inline std::string normalizeUrl(const std::string& raw) {
    size_t first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    size_t last = raw.find_last_not_of(" \t\r\n");
    std::string s = raw.substr(first, last - first + 1);

    if (s.compare(0, 7, "http://") != 0 && s.compare(0, 8, "https://") != 0) {
        // reject an outright different scheme rather than gluing http:// onto it
        const size_t scheme = s.find("://");
        if (scheme != std::string::npos) return {};
        s = "http://" + s;
    }
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    if (s.find_first_of(" \t\r\n") != std::string::npos) return {};

    const size_t scheme = s.find("://");
    if (scheme == std::string::npos) return {};
    const size_t hostStart = scheme + 3;
    if (hostStart >= s.size()) return {};
    // a bare "http://" or "http:///" has no host
    const size_t hostEnd = s.find_first_of(":/?#", hostStart);
    const std::string host = s.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
    if (host.empty()) return {};
    return s;
}

/// Same rule the Settings tab applies to the proxy field: a value without "://"
/// is an http proxy. Empty is valid (no proxy) and means "clear it".
inline std::string normalizeProxy(const std::string& raw) {
    const size_t first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = raw.find_last_not_of(" \t\r\n");
    std::string s = raw.substr(first, last - first + 1);
    if (s.find("://") == std::string::npos) s = "http://" + s;
    return s;
}

/// Trim + reject control characters. A newline in a header value is a request
/// splitter, and a control byte in a UA is never intentional.
inline std::string normalizeUserAgent(const std::string& raw) {
    const size_t first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = raw.find_last_not_of(" \t\r\n");
    const std::string s = raw.substr(first, last - first + 1);
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) return {};
    }
    if (s.size() > 512) return {};
    return s;
}

class Draft {
public:
    bool uaSet = false;
    bool uaCleared = false;
    std::string ua;
    bool proxySet = false;
    bool proxyCleared = false;
    std::string proxy;
    std::vector<Connection> connections;

    bool empty() const { return this->changes().empty(); }

    void clear() {
        this->uaSet = this->uaCleared = false;
        this->ua.clear();
        this->proxySet = this->proxyCleared = false;
        this->proxy.clear();
        this->connections.clear();
    }

    /// Create-or-replace by (id) when editing, or append when creating. Callers
    /// are the page's own handlers, so there is no "duplicate id" case to guard:
    /// an edit carries the id it was loaded with.
    void upsert(Connection c) {
        for (auto& existing : this->connections) {
            if (!c.id.empty() && existing.id == c.id) {
                existing = std::move(c);
                return;
            }
        }
        this->connections.push_back(std::move(c));
    }

    /// Marks an existing connection for deletion; a connection that only existed
    /// in the draft is simply dropped (nothing was ever applied).
    bool remove(const std::string& id) {
        for (auto it = this->connections.begin(); it != this->connections.end(); ++it) {
            if (it->id != id) continue;
            if (it->user_id.empty()) {
                this->connections.erase(it);
            } else {
                it->removal = true;
            }
            return true;
        }
        return false;
    }

    const Connection* find(const std::string& id) const {
        for (const auto& c : this->connections)
            if (c.id == id) return &c;
        return nullptr;
    }

    /// What Apply would do, in the order it does it. The UI localises each entry
    /// from (kind, name); nothing here needs to know any text.
    std::vector<Change> changes() const {
        std::vector<Change> out;
        if (this->uaSet || this->uaCleared) out.push_back({Kind::UserAgent, {}});
        if (this->proxySet || this->proxyCleared) out.push_back({Kind::Proxy, {}});
        for (const auto& c : this->connections) {
            if (c.removal)
                out.push_back({Kind::Remove, c.name});
            else if (c.id.empty())
                out.push_back({Kind::Add, c.name});
            else
                out.push_back({Kind::Edit, c.name});
        }
        return out;
    }

    /// True when every pending connection has a token, i.e. Apply cannot fail on
    /// an unauthenticated entry. A removal needs none.
    bool ready() const {
        for (const auto& c : this->connections)
            if (!c.removal && c.token.empty()) return false;
        return true;
    }
};

}  // namespace draft
