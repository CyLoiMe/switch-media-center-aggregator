/*
    Jellyfin/Emby PlaybackInfo: the play URL the SERVER hands out.

    Most items play fine from the URL we assemble ourselves
    (/Videos/{id}/stream?static=true&mediaSourceId=…). Remote sources (`.strm`
    pointing at a cloud drive — 115, 123, 夸克…) can be different: front-ends
    such as TgtoDrive answer PlaybackInfo with `DirectStreamUrl` rewritten to the
    cloud CDN's signed link and **reject** our own URL (403 "缺少 Emby UserId"),
    so the URL from the response is the only one that plays.

    Pure logic (JSON + strings), so tests/test_jellyfin_playback.cpp covers it.
*/

#pragma once

#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

#include "api/media/types.hpp"
#include "utils/hostname.hpp"

namespace jellyfin {

namespace detail {

/// Link-local / loopback / RFC1918 literals: the address a server reports for
/// itself, which a console on another network cannot reach.
inline bool isPrivateIpv4(const std::string& host) {
    int octet[4] = {0, 0, 0, 0};
    int dots = 0, digits = 0, value = 0;
    for (char c : host) {
        if (c == '.') {
            if (digits == 0 || dots >= 3) return false;
            octet[dots++] = value;
            value = digits = 0;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            if (++digits > 3) return false;
            value = value * 10 + (c - '0');
        } else {
            return false;  // not a numeric IPv4 literal
        }
    }
    if (dots != 3 || digits == 0) return false;
    octet[3] = value;
    return octet[0] == 10 || octet[0] == 127 || (octet[0] == 192 && octet[1] == 168) ||
           (octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31) || (octet[0] == 169 && octet[1] == 254);
}

/// Host the console cannot reach: a private address, loopback or an mDNS name.
inline bool isInternalHost(const std::string& url) {
    std::string host = net::hostFromUrl(url);
    for (char& c : host) c = (char)std::tolower(static_cast<unsigned char>(c));
    if (host.empty() || host == "localhost" || host == "::1") return true;
    return net::isMdnsHost(url) || isPrivateIpv4(host);
}

}  // namespace detail

/// What a PlaybackInfo response offers for one source.
struct SourceUrls {
    /// `DirectStreamUrl`, ready to play (token appended when the server left it out).
    /// Empty when the server gives none the console can use.
    std::string direct;
    /// The MediaSource's own absolute URL — the `.strm` target. This is the file the
    /// server would otherwise fetch through itself; playing or downloading it
    /// directly is what avoids Emby's 20 s proxy timeout.
    std::string path;
};

/// Reads both URLs out of a PlaybackInfo response. `sourceId` selects the matching
/// MediaSources entry (falls back to the first); `serverUrl` + `token` are only used
/// to recognise the server's own host, whose URL needs the token appended when the
/// server left it out.
inline SourceUrls sourceUrls(const nlohmann::json& resp, const std::string& sourceId,
    const std::string& serverUrl, const std::string& token) {
    SourceUrls out;
    auto sources = resp.find("MediaSources");
    if (sources == resp.end() || !sources->is_array()) return out;

    const nlohmann::json* picked = nullptr;
    for (const auto& source : *sources) {
        if (!source.is_object()) continue;
        if (!picked) picked = &source;  // first one, when sourceId is unknown here
        if (!sourceId.empty() && media::jstr(source, "Id") == sourceId) {
            picked = &source;
            break;
        }
    }
    if (picked == nullptr) return out;

    // The source's own URL first: it is worth having even when DirectStreamUrl is
    // unusable, because it is the only thing that can bypass the server's proxy.
    // No token is appended — this is not the server, and the front-ends behind it
    // reject unknown query parameters.
    const std::string path = media::jstr(*picked, "Path");
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) {
        // same reachability rule as below: a private address only when the server is
        // private too (the console is on that network)
        if (!detail::isInternalHost(path) || detail::isInternalHost(serverUrl)) out.path = path;
    }

    std::string url = media::jstr(*picked, "DirectStreamUrl");
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return out;

    const bool ownHost = net::hostFromUrl(url) == net::hostFromUrl(serverUrl);
    // A private address is unreachable only when the server is NOT on that network.
    // Rejecting it unconditionally broke the ordinary home setup: the server sits at
    // 192.168.x.y and the cloud gateway next to it (the 115/123 front-end that
    // rewrites DirectStreamUrl) at another private address the console can reach
    // perfectly well. Dropping that URL sent playback back to /Videos/{id}/stream,
    // i.e. the server fetching the cloud file through itself — Emby gives up after
    // its 20 s HttpClient timeout and answers HTTP 500.
    // Rejected from `direct` only: `path` (above) stays the way out of that proxy.
    if (!ownHost && detail::isInternalHost(url) && !detail::isInternalHost(serverUrl)) return out;

    if (ownHost && !token.empty() && url.find("api_key=") == std::string::npos)
        url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("api_key=") + token;
    out.direct = url;
    return out;
}

/// `DirectStreamUrl` alone: what most callers want, and what the tests exercise.
inline std::string directStreamUrl(const nlohmann::json& resp, const std::string& sourceId,
    const std::string& serverUrl, const std::string& token) {
    return sourceUrls(resp, sourceId, serverUrl, token).direct;
}

}  // namespace jellyfin
