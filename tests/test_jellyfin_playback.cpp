// Standalone logic test — picking the play URL out of a PlaybackInfo response
// (api/jellyfin/playback_info.hpp).
//
// Remote sources (`.strm` -> cloud drive) may only be playable through the
// DirectStreamUrl the server hands out; the URL we build ourselves gets a 403
// from TgtoDrive-style front-ends. This covers the selection rules, and the
// cases where the server's URL must be ignored.
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include -Ilibrary/borealis/library/include/borealis/extern \
//       tests/test_jellyfin_playback.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>
#include <api/jellyfin/playback_info.hpp>

using jellyfin::directStreamUrl;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

static const std::string server = "https://kan.example.com";
static const std::string token = "abc123";

/// A one-source response carrying `url`.
static json response(const std::string& url, const std::string& id = "x") {
    json resp;
    resp["MediaSources"] = json::array({json{{"Id", id}, {"DirectStreamUrl", url}}});
    return resp;
}

int main() {
    // --- the real shape: TgtoDrive answering for a .strm on 115 ---------------
    {
        json resp = json::parse(R"({
          "MediaSources": [{
            "Id": "mediasource_1223",
            "Protocol": "Http",
            "IsRemote": true,
            "Path": "http://192.168.1.20:12366/play115/<pickcode>/123/file.mp4",
            "Container": "mp4",
            "SupportsDirectPlay": true,
            "AddApiKeyToDirectStreamUrl": false,
            "DirectStreamUrl": "https://cdn.example.net/abc123/file.mp4?t=1794748531&k=09b8&v=1"
          }],
          "PlaySessionId": "130302a87b"
        })");
        CHECK(directStreamUrl(resp, "mediasource_1223", server, token) ==
              "https://cdn.example.net/abc123/file.mp4?t=1794748531&k=09b8&v=1");
    }

    // --- source selection: the requested source wins, otherwise the first ----
    {
        json resp;
        resp["MediaSources"] = json::array({
            json{{"Id", "a"}, {"DirectStreamUrl", "https://cdn.example.com/a.mp4"}},
            json{{"Id", "b"}, {"DirectStreamUrl", "https://cdn.example.com/b.mp4"}},
        });
        CHECK(directStreamUrl(resp, "b", server, token) == "https://cdn.example.com/b.mp4");
        CHECK(directStreamUrl(resp, "zzz", server, token) == "https://cdn.example.com/a.mp4");  // first
        CHECK(directStreamUrl(resp, "", server, token) == "https://cdn.example.com/a.mp4");
    }

    // --- the server's own host: its URL needs the token when it left it out --
    {
        CHECK(directStreamUrl(response("https://kan.example.com/videos/1/stream?static=true"), "x", server, token) ==
              "https://kan.example.com/videos/1/stream?static=true&api_key=abc123");
        CHECK(directStreamUrl(response("https://kan.example.com/videos/1/stream?api_key=abc123"), "x", server, token) ==
              "https://kan.example.com/videos/1/stream?api_key=abc123");  // not doubled
    }

    // --- rejected: URLs the console cannot use ------------------------------
    {
        CHECK(directStreamUrl(response("/videos/1/stream"), "x", server, token).empty());  // relative
        // server-internal addresses are unreachable from a console elsewhere
        CHECK(directStreamUrl(response("http://192.168.1.20:12366/play115/a.mp4"), "x", server, token).empty());
        CHECK(directStreamUrl(response("http://10.0.0.5:8096/v.mp4"), "x", server, token).empty());
        CHECK(directStreamUrl(response("http://172.20.1.9/v.mp4"), "x", server, token).empty());
        CHECK(directStreamUrl(response("http://169.254.1.1/v.mp4"), "x", server, token).empty());
        CHECK(directStreamUrl(response("http://127.0.0.1/v.mp4"), "x", server, token).empty());
        CHECK(directStreamUrl(response("http://localhost:8096/v.mp4"), "x", server, token).empty());
        CHECK(directStreamUrl(response("http://nas.local/v.mp4"), "x", server, token).empty());
        // a public literal IP is fine
        CHECK(directStreamUrl(response("http://1.2.3.4/v.mp4"), "x", server, token) == "http://1.2.3.4/v.mp4");
    }

    // --- the media source's own URL, alongside DirectStreamUrl -----------
    {
        // A cloud item: Path is the .strm target. It is what playback and download
        // use when the server would otherwise proxy the file through itself.
        json cloud = json::parse(R"({
          "MediaSources": [{
            "Id": "mediasource_390",
            "Path": "http://192.168.3.244:12366/play115/a.mp4",
            "DirectStreamUrl": "http://192.168.3.244:12366/play115/a.mp4?sig=1"
          }]
        })");
        // same LAN as the gateway: both URLs usable, no token appended to either
        CHECK(sourceUrls(cloud, "mediasource_390", "http://192.168.3.10:8096", token).path ==
              "http://192.168.3.244:12366/play115/a.mp4");
        CHECK(sourceUrls(cloud, "mediasource_390", "http://192.168.3.10:8096", token).direct ==
              "http://192.168.3.244:12366/play115/a.mp4?sig=1");
        // public server: a private Path is unreachable, so it is not offered
        CHECK(sourceUrls(cloud, "mediasource_390", server, token).path.empty());
        // a local file has an ordinary path, never an http URL
        json local = json::parse(R"({"MediaSources":[{"Id":"x","Path":"/media/films/a.mkv"}]})");
        CHECK(sourceUrls(local, "x", server, token).path.empty());
        CHECK(sourceUrls(local, "x", server, token).direct.empty());
    }

    // --- malformed responses -----------------------------------------------
    {
        CHECK(directStreamUrl(json::parse("{}"), "x", server, token).empty());  // no MediaSources
        CHECK(directStreamUrl(json::parse(R"({"MediaSources":[]})"), "x", server, token).empty());
        CHECK(directStreamUrl(json::parse(R"({"MediaSources":"nope"})"), "x", server, token).empty());
        CHECK(directStreamUrl(json::parse(R"({"MediaSources":[{"Id":"x"}]})"), "x", server, token).empty());
        json numeric = json::parse(R"({"MediaSources":[{"Id":"x","DirectStreamUrl":42}]})");
        CHECK(directStreamUrl(numeric, "x", server, token).empty());
    }

    // --- the server's own host is used even when it is a private address ----
    {
        // console and server on the same LAN: host == server host, so it is kept
        CHECK(directStreamUrl(response("http://192.168.1.10:8096/videos/1/stream"), "x", "http://192.168.1.10:8096",
                  token) == "http://192.168.1.10:8096/videos/1/stream?api_key=abc123");
    }

    // --- both private: server and cloud gateway on the same LAN -------------
    {
        // The gateway that rewrites DirectStreamUrl sits at another private address,
        // next to the server (115/123 front-ends do). The console can reach it, and
        // rejecting it used to send playback to the server's proxy, which 500s after
        // its own 20 s timeout — see docs/playback.md.
        CHECK(directStreamUrl(response("http://192.168.3.244:12366/play115/a.mp4"), "x", "http://192.168.3.10:8096",
                  token) == "http://192.168.3.244:12366/play115/a.mp4");
        CHECK(directStreamUrl(response("http://10.0.0.7:12366/x.mp4"), "x", "http://10.0.0.5:8096", token) ==
              "http://10.0.0.7:12366/x.mp4");
        // a public server still refuses a private URL: the console is elsewhere
        CHECK(directStreamUrl(response("http://192.168.3.244:12366/play115/a.mp4"), "x", server, token).empty());
    }

    if (failures == 0)
        printf("test_jellyfin_playback: all checks passed\n");
    else
        printf("test_jellyfin_playback: %d FAILED\n", failures);
    return failures ? 1 : 0;
}
