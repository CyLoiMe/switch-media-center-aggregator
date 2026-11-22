#pragma once

#include <borealis/core/singleton.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/theme.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <memory>
#include <optional>

namespace media {
class Backend;
// opaque (scoped enums default to an int base, so this is a complete type) —
// avoids pulling api/backend.hpp into every TU that includes config.hpp.
enum class BackendType;
}  // namespace media

namespace plenx {
struct ThemePalette;
}

class AppVersion {
public:
    static std::string getVersion();
    static std::string getPlatform();
    static std::string getDeviceName();
    static std::string getPackageName();
    static std::string getCommit();
    static bool needUpdate(std::string latestVersion);
    static void checkUpdate(int delay = 2000, bool showUpToDateDialog = false);

    inline static std::shared_ptr<std::atomic_bool> updating = std::make_shared<std::atomic_bool>(true);
    inline static std::string git_repo = "CyLoiMe/switch-media-center-aggregator";

    /// Real path of the running NRO (argv[0] provided by hbloader or the
    /// forwarder), filled in main(). THIS is the file the auto-update must
    /// replace: the NRO may live at `sdmc:/switch/SMCA.nro` (forwarder)
    /// or `sdmc:/switch/SMCA/SMCA.nro`. Empty off-Switch or if unavailable.
    inline static std::string nro_path;
};

/// One connection: a server plus the profile that browses it.
///
/// The two `access_token` fields mean different things per backend:
///
///   - Plex: `AppUser::access_token` is the plex.tv token of THIS profile (used
///     for the plex.tv API — resources, home users — never for server requests);
///     `AppServer::access_token` is the PMS token.
///   - Jellyfin/Emby: every ACCOUNT gets its own AccessToken. Two accounts on one
///     server share a single `AppServer` entry (keyed by the server Id), so that
///     entry's `access_token` only ever holds whichever account authenticated
///     last — `AppUser::access_token` is the per-account token to use there.
///   - Stremio: the account authKey, stored on both.
///
/// `AppConfig::getToken()` resolves this (see utils/connection_token.hpp).
struct AppUser {
    std::string id;  // plex.tv uuid / Jellyfin user Id / Stremio userId
    std::string name;
    std::string access_token;
    std::string server_id;  // clientIdentifier / Jellyfin server Id / Stremio userId
    std::string thumb;      // avatar URL
    /// Jellyfin/Emby only: libraries this profile may browse while the connection
    /// is on its WAN address (tunnel/FRP), where every byte costs quota.
    /// `nullopt` = never configured, which keeps the historical behaviour
    /// (everything visible); an engaged list is authoritative, so an empty one
    /// hides every library on WAN. Stored per PROFILE, because two accounts on one
    /// server see different libraries.
    std::optional<std::vector<std::string>> wan_visible_libraries;
};

/// Hand-written instead of NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT: the
/// bundled nlohmann (3.12, from borealis) only defines its std::optional
/// converters when `JSON_USE_IMPLICIT_CONVERSIONS` is 0 (json.hpp:4835 / 5917
/// are `#ifndef`, and that macro defaults to 1 at json.hpp:2946). With the
/// default, `json::value(key, std::optional<T>)` has no viable overload — the
/// key-based one is SFINAE'd out, the json_pointer one cannot deduce — so the
/// macro cannot be used on a struct holding an optional. The pair below keeps
/// the macro's semantics (a missing key leaves the member's default in place)
/// and serialises the tri-state as "key absent = nullopt, key present = engaged".
///
/// Adding a member to AppUser therefore means adding it here too.
inline void to_json(nlohmann::json& nlohmann_json_j, const AppUser& nlohmann_json_t) {
    nlohmann_json_j = nlohmann::json{{"id", nlohmann_json_t.id},
        {"name", nlohmann_json_t.name},
        {"access_token", nlohmann_json_t.access_token},
        {"server_id", nlohmann_json_t.server_id},
        {"thumb", nlohmann_json_t.thumb}};
    // Written only when engaged: an absent key reads back as nullopt, which is
    // exactly "never configured" (an engaged EMPTY list must survive as []).
    if (nlohmann_json_t.wan_visible_libraries)
        nlohmann_json_j["wan_visible_libraries"] = *nlohmann_json_t.wan_visible_libraries;
}

inline void from_json(const nlohmann::json& nlohmann_json_j, AppUser& nlohmann_json_t) {
    nlohmann_json_t.id = nlohmann_json_j.value("id", std::string{});
    nlohmann_json_t.name = nlohmann_json_j.value("name", std::string{});
    nlohmann_json_t.access_token = nlohmann_json_j.value("access_token", std::string{});
    nlohmann_json_t.server_id = nlohmann_json_j.value("server_id", std::string{});
    nlohmann_json_t.thumb = nlohmann_json_j.value("thumb", std::string{});
    nlohmann_json_t.wan_visible_libraries.reset();
    if (const auto it = nlohmann_json_j.find("wan_visible_libraries");
        it != nlohmann_json_j.end() && !it->is_null())
        nlohmann_json_t.wan_visible_libraries = it->get<std::vector<std::string>>();
}

/// A known media server. `access_token` is the server access token (Plex: from
/// /api/v2/resources; Jellyfin/Emby: the AccessToken of the account that
/// authenticated last — see AppUser). urls.front() = last reachable connection.
/// `type` discriminates the backend implementation (defaults to "plex" for
/// configs written before multi-backend support).
struct AppServer {
    std::string name;
    std::string id;  // clientIdentifier (machine id) / Jellyfin server Id / Stremio account user id
    std::string access_token;
    std::vector<std::string> urls;
    /// Optional home-network address (Jellyfin/Emby). Probed FIRST: reaching it
    /// puts the app in LAN mode (no bandwidth quota); falling through to `urls`
    /// means the WAN address answered instead. Empty = single-address server.
    std::string local_url;
    std::string type = "plex";  // plex | jellyfin | emby | stremio
    // Stremio only: transport URLs (…/manifest.json) of the installed addons,
    // either entered manually or synced from the account's addon collection.
    std::vector<std::string> addons;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AppServer, id, name, access_token, urls, local_url, type, addons);

struct AppRemote {
    std::string name;
    std::string url;
    std::string user;
    std::string passwd;
    std::string user_agent;
};
inline void to_json(nlohmann::json& nlohmann_json_j, const AppRemote& nlohmann_json_t) {
    if (!nlohmann_json_t.user.empty()) nlohmann_json_j["user"] = nlohmann_json_t.user;
    if (!nlohmann_json_t.passwd.empty()) nlohmann_json_j["passwd"] = nlohmann_json_t.passwd;
    if (!nlohmann_json_t.user_agent.empty()) nlohmann_json_j["user_agent"] = nlohmann_json_t.user_agent;
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, name, url))
}
inline void from_json(const nlohmann::json& nlohmann_json_j, AppRemote& nlohmann_json_t) {
    if (nlohmann_json_j.contains("user")) nlohmann_json_j["user"].get_to(nlohmann_json_t.user);
    if (nlohmann_json_j.contains("passwd")) nlohmann_json_j["passwd"].get_to(nlohmann_json_t.passwd);
    if (nlohmann_json_j.contains("user_agent")) nlohmann_json_j["user_agent"].get_to(nlohmann_json_t.user_agent);
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, name, url))
}

class AppConfig : public brls::Singleton<AppConfig> {
    using UserIter = std::vector<AppUser>::iterator;

public:
    enum Item {
        OVERCLOCK,
        APP_THEME,
        APP_LANG,
        APP_UPDATE,
        APP_UI_SCALE,
        SCROLLBAR,  // show the scroll indicator (scrollbar); default true
        AUDIO_CHANNELS,
        KEYMAP,
        WINDOW_STATE,
        TRANSCODEC,
        /// "auto" | "direct" | "transcode" — Settings → Playback → Playback mode.
        /// Replaced FORCE_DIRECTPLAY + PLAYER_RESOLUTION_LIMIT, which gated each
        /// other (see AppConfig::init).
        PLAYBACK_MODE,
        /// Per-title quality overrides: `{"<server>/<titleId>": bps}`, written by the
        /// in-player quality menu (0 or an absent key = follow PLAYBACK_MODE). Keyed by
        /// title so a resume, or the next episode of the same show, plays the way it was
        /// chosen — but never leaks to another title, which is what a single global
        /// remembered cap did.
        PLAYBACK_QUALITY_OVERRIDES,
        PLAYER_MAX_RESOLUTION,
        OSD_ON_TOGGLE,
        TOUCH_GESTURE,
        CLIP_POINT,
        SYNC_SETTING,
        MPV_VO,
        PLAYER_LOW_QUALITY,
        PLAYER_INMEMORY_CACHE,
        PLAYER_SPEED,
        PLAYER_HWDEC,
        PLAYER_HWDEC_CUSTOM,
        PLAYER_ASPECT,
        PLAYER_SUBS_FALLBACK,
        PLAYER_SUBTITLE_LANG,  // preferred external-subtitle language: "auto" (= app locale), "off", or a 2-letter code
        /// Custom User-Agent for the mpv stream layer (empty = the app's own UA,
        /// or the user's `user-agent=` in {configDir}/mpv.conf when present).
        /// Needed by cloud-drive CDNs reached through an Emby/Jellyfin 302,
        /// which reject anything but a browser UA — see mpv_core.cpp init().
        PLAYER_USER_AGENT,
        PLAYER_TV_MODE,
        SHOW_FPS,
        SWAP_INTERVAL,
        APP_SWAP_ABXY,  // A-B 交换 和 X-Y 交换
        TEXTURE_CACHE_NUM,
        REQUEST_THREADS,
        REQUEST_TIMEOUT,
        HTTP_PROXY_STATUS,
        HTTP_PROXY,

        /// Which address wins when a connection declares both a LAN and a WAN one:
        /// "lan_first" (default — the LAN address is strictly preferred), "lan_only"
        /// (never touch the tunnel while a LAN address exists) or "race" (first
        /// answer wins; at home the tunnel can win and spend quota). See
        /// utils/dual_url.hpp.
        WAN_POLICY,

        /// How long the HOME address gets to answer before the WAN one is used
        /// (ms). The trade-off is the user's: a short budget keeps the app from
        /// feeling stuck, a longer one keeps a slow home Wi-Fi from being read as
        /// "away" and falling through to the metered address. See
        /// AppConfig::reachableUrl().
        WAN_PROBE_MS,

        /// Library sorts/filters, persisted locally (json object
        /// {itemId: "sortBy,sortOrder,filter"} — /DisplayPreferences does not
        /// exist in Plex, cf. PLEX_MIGRATION.md §2.5)
        LIBRARY_SORT,

        /// Per-server sidebar layout: order + hidden state of the reorderable
        /// tabs (libraries + Playlists + Watchlist), keyed by the active server
        /// id (getUser().server_id). Section keys collide across servers, so
        /// this MUST stay server-scoped. JSON:
        /// { "<serverId>": { "order": [ids...], "hidden": [ids...] } }
        SIDEBAR_LAYOUT,

        /// HOME tile install prompt (forwarder NSP) already shown at first
        /// launch in application mode (Switch).
        HINT_FORWARDER,

        KEY_REFRESH,        // 刷新快捷键
        KEY_LAST,           // 上一个Tab快捷键
        KEY_NEXT,           // 下一个Tab快捷键
        KEY_VOLUME_UP,      // 音量增大快捷键
        KEY_VOLUME_DOWN,    // 音量减小快捷键
        KEY_VIDEO_PROFILE,  // 视频详情快捷键
        KEY_FORWARD,        // 快进快捷键
        KEY_REWIND,         // 快退快捷键
        KEY_SETTING,        // 设置快捷键
        KEY_VIDEO_QUALITY,  // 视频清晰度菜单快捷键
        KEY_VIDEO_SPEED,    // 视频倍速菜单快捷键
        KEY_VIDEO_OSD,      // 切换OSD显示
        KEY_VIDEO_PAUSE,    // 视频播放暂停快捷键
    };

    AppConfig() = default;
    ~AppConfig();  // out-of-line: activeBackend is a unique_ptr to an incomplete type

    bool init();
    void initThemes();
    /// (Re)applies the accent surface for `type` onto BOTH borealis theme
    /// objects (dark + light). std::nullopt = the neutral pleNx DEFAULT theme
    /// used on pre-connection screens. Structural chrome is left untouched.
    /// Must run BEFORE the activity that will read the colors is (re)built.
    void applyTheme(std::optional<media::BackendType> type);
    void save();
    bool checkLogin();

    std::string configDir();
    void checkRestart(char* argv[]);

    template <typename T>
    T getItem(const Item item, T defaultValue) {
        auto& o = settingMap[item];
        try {
            if (!setting.contains(o.key)) return defaultValue;
            return this->setting.at(o.key).get<T>();
        } catch (const std::exception& e) {
            brls::Logger::error("Damaged config found: {}/{}", o.key, e.what());
            return defaultValue;
        }
    }

    template <typename T>
    void setItem(const Item item, T data) {
        auto& o = settingMap[item];
        this->setting[o.key] = data;
        this->save();
    }

    /// Quality override for one title, in bps (0 = none, follow PLAYBACK_MODE).
    /// `key` is "<server>/<titleId>" — see PlayerView::qualityKey.
    int64_t playbackQualityFor(const std::string& key);
    /// Stores (bps > 0) or clears (bps <= 0) the override for one title.
    void setPlaybackQualityFor(const std::string& key, int64_t bps);

    struct Option {
        std::string key;
        std::vector<std::string> options;
        std::vector<long> values;
    };

    int getOptionIndex(const Item item, int default_index = 0) const;
    int getValueIndex(const Item item, int default_index = 0) const;
    inline const Option& getOptions(const Item item) const { return settingMap[item]; }

    bool addServer(const AppServer& s);
    /// `lan` records which of the server's addresses answered: it drives the
    /// LAN/WAN badge and the WAN-only library filter (see utils/dual_url.hpp).
    void addUser(const AppUser& u, const std::string& url, bool lan = false);
    /// Registers a server/connection WITHOUT making it active — no change to the
    /// active url/token/profile or the backend/theme. Used to store the other
    /// servers of a Plex account at link time; addServer/addUser activate one.
    void upsertServer(const AppServer& s);
    void upsertUser(const AppUser& u);
    /// Replace a stored connection IN PLACE — the edit flow, never a
    /// delete-and-re-add. `oldServerId`/`oldUserId` identify the entry being
    /// edited; empty `oldUserId` means "not stored yet" and falls back to adding.
    ///
    /// A re-login can hand back a different identity (a restored Jellyfin server
    /// issues a new server Id, an account can be recreated with a new user Id).
    /// The connection then keeps what belongs to the PROFILE, not to the id: its
    /// WAN allow-list, its sidebar layout and its place in the list. The old
    /// server entry is dropped only when nothing else still references it.
    ///
    /// Makes the connection active when it was the active one (`url`/`lan` are
    /// then applied), and leaves the active connection alone otherwise.
    void updateConnection(const std::string& oldServerId, const std::string& oldUserId,
        const AppServer& server, const AppUser& user, const std::string& url, bool lan);
    bool removeServer(const std::string& id);
    bool removeUser(const std::string& id);
    const std::string& getDeviceId() { return this->device; }
    const std::string& getUserId() const { return this->user_id; }
    /// `AppServer::id` of the active connection, "" when nothing is connected.
    /// Resolved through the active profile like activeServerType(), because the
    /// URL does not identify a connection: one server has two addresses and two
    /// servers can share one. `userId` + this id is what "same connection" means
    /// (a download stamps both, see utils/download.hpp).
    std::string activeServerId() const;
    const std::string& getUserName() const { return this->user->name; }
    /// Active profile (name, avatar...) — valid after init()/checkLogin().
    const AppUser& getUser() const { return *this->user; }
    /// Token that authenticates requests against the ACTIVE connection: the
    /// server token (Plex PMS, Stremio authKey) or, for Jellyfin/Emby, the active
    /// profile's own AccessToken — those servers issue one token per account, and
    /// two accounts on one server share an AppServer entry. See
    /// utils/connection_token.hpp.
    std::string getToken() const;
    /// plex.tv token of the active profile (resources, home users).
    const std::string& getAccountToken() const { return this->user->access_token; }
    const std::string& getUrl() const { return this->server_url; }
    /// True when the active address is the connection's LAN one, i.e. requests do
    /// not travel through the tunnel (and cost no quota).
    bool isLanMode() const { return this->lan_mode; }
    /// LAN address of the active connection, or "" when it declares none.
    std::string getLocalUrl() const;
    /// Libraries the active connection may browse on its CURRENT address, or
    /// `nullopt` when nothing is filtered (LAN mode, or the profile never
    /// configured a WAN allow-list). Snapshot it on the UI thread before going
    /// async and filter with `wanfilter::allows()` (utils/wan_filter.hpp).
    std::optional<std::vector<std::string>> allowedLibraries() const;
    /// The active profile's STORED allow-list, ignoring the current LAN/WAN mode:
    /// this is the setting the visibility manager edits. `nullopt` = never
    /// configured, i.e. every library is visible on WAN.
    std::optional<std::vector<std::string>> getWanVisibleLibraries() const;
    /// Replace that list and persist it. Passing `nullopt` returns the profile to
    /// "everything visible", so a library added later needs no second visit.
    void setWanVisibleLibraries(std::optional<std::vector<std::string>> libs);
    /// Point the active connection at another of its addresses without
    /// re-authenticating (re-probe result, or a manual force). The backend reads
    /// getUrl() live, so the next request uses it.
    void setActiveUrl(const std::string& url, bool lan);
    /// Stremio only: addon transport URLs of the active server (manifest URLs).
    /// Empty for other backends / when not logged in.
    const std::vector<std::string>& getStremioAddons() const;
    /// Stremio only: replace the active server's addon list (after an account
    /// collection re-sync) and persist. No-op when unchanged or not logged in.
    void setStremioAddons(const std::vector<std::string>& addons);
    /// Active media backend (built lazily from the active server's type).
    /// The UI talks to this; it never formats a provider URL itself.
    media::Backend& backend();
    /// `AppServer::type` of the active connection ("plex" | "jellyfin" | "emby" |
    /// "stremio"), "" when nothing is connected (offline shell). Resolved through
    /// the active profile's `server_id`, because matching on `server_url` is
    /// ambiguous when two servers share one URL. `backend()` builds from it, and
    /// views gate the backends that enforce the WAN allow-list on it
    /// (`view/library_manager.cpp`).
    std::string activeServerType() const;
    const std::vector<AppRemote>& getRemotes() const { return this->remotes; }
    void addRemote(const AppRemote& r);
    void updateRemote(size_t index, const AppRemote& r);
    void removeRemote(size_t index);

    /// Local folders pinned to the Files root screen (issue #24). Stored as
    /// absolute browser paths (e.g. "sdmc:/A_Media/").
    const std::vector<std::string>& getPins() const { return this->pins; }
    bool isPinned(const std::string& path) const;
    void addPin(const std::string& path);
    void removePin(const std::string& path);
    const std::vector<AppServer>& getServers() const { return this->servers; }
    const std::vector<AppUser> getUsers(const std::string& id) const;
    /// All known connections (one AppUser = one server+profile pair).
    const std::vector<AppUser>& getUsers() const { return this->users; }
    /// Maps the AppServer::type discriminant to a BackendType (defaults to Plex).
    /// Public so the connection switcher can tint each tile by its backend brand.
    static media::BackendType backendTypeFromString(const std::string& type);
    /// First of the connection's addresses that answers, probed the way its
    /// backend expects; "" when none does. Blocking: call off the UI thread.
    /// `lan` (optional) reports whether the LAN address was the one that answered.
    static std::string reachableUrl(const AppServer& s, bool* lan = nullptr);

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AppConfig, user_id, device, users, servers, setting, remotes, pins);

    inline static bool SYNC = true;
    /// Live copy of the WAN_POLICY setting ("lan_first" | "lan_only" | "race"),
    /// read by reachableUrl() on every activation. Kept next to SYNC: a setting
    /// the network layer consults without going through an instance.
    inline static std::string wan_policy = "lan_first";
    /// Live copy of WAN_PROBE_MS: the budget (ms) the home address gets to
    /// answer before reachableUrl() falls through to the remote one. A setting
    /// the probe consults without going through an instance, like wan_policy.
    inline static long wan_probe_ms = 2000;

private:
    static std::unordered_map<Item, Option> settingMap;

    /// (Re)builds activeBackend from the active server's type on next backend().
    void resetBackend();

    /// Writes one palette variant onto the matching borealis Theme singleton.
    void applyThemeVariant(brls::ThemeVariant tv, const plenx::ThemePalette& p);

    UserIter user;
    // owning raw pointer (forward-declared type): deleted in ~AppConfig/resetBackend
    media::Backend* activeBackend = nullptr;
    std::string user_id;
    std::string server_url;
    std::string server_token;
    /// Runtime only (never persisted): did the active address come from the
    /// connection's LAN field?
    bool lan_mode = false;
    std::string device;
    std::string device_name;
    std::vector<AppUser> users;
    std::vector<AppServer> servers;
    std::vector<AppRemote> remotes;
    std::vector<std::string> pins;
    nlohmann::json setting = {};

    void addColor(const brls::ThemeVariant tv, const std::string& name, NVGcolor defaultColor);
};