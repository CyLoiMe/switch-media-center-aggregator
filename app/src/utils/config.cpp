#ifdef __SWITCH__
#include <switch.h>
#include "utils/overclock.hpp"
#endif

#include <borealis.hpp>
#include <borealis/core/cache_helper.hpp>
#include <algorithm>
#include <borealis/views/edit_text_dialog.hpp>
#include "api/plex/auth.hpp"
#include "api/backend.hpp"
#include "api/plex/backend.hpp"
#include "api/jellyfin/auth.hpp"
#include "api/jellyfin/backend.hpp"
#include "api/stremio/backend.hpp"
#include "api/http.hpp"
#include "utils/atomic_file.hpp"
#include "utils/config.hpp"
#include "utils/connection_edit.hpp"
#include "utils/connection_token.hpp"
#include "utils/dual_url.hpp"
#include "utils/theme_palette.hpp"
#include "utils/keybind.hpp"
#include "utils/misc.hpp"
#include "utils/mpv_conf.hpp"
#include "utils/ums.hpp"
#include "utils/thread.hpp"
#include "view/mpv_core.hpp"
#include "view/video_view.hpp"

std::unordered_map<AppConfig::Item, AppConfig::Option> AppConfig::settingMap = {
    {APP_THEME, {"app_theme", {"auto", "light", "dark"}}},
    {APP_LANG, {"app_lang", {brls::LOCALE_AUTO, brls::LOCALE_EN_US, brls::LOCALE_ZH_HANS, brls::LOCALE_ZH_HANT,
                                brls::LOCALE_JA, brls::LOCALE_Ko, brls::LOCALE_RU, brls::LOCALE_DE, brls::LOCALE_FR,
                                brls::LOCALE_ES, brls::LOCALE_PT, "cs", "uk", "tr", "vi"}}},
    {APP_UPDATE, {"app_update"}},
    {APP_UI_SCALE, {"app_ui_scale", {"544p", "720p", "900p", "1080p"}}},
    {SCROLLBAR, {"scrollbar"}},
    {AUDIO_CHANNELS, {"audio-channels", {"auto-safe", "stereo", "mono"}}},
    {KEYMAP, {"keymap", {"xbox", "ps", "keyboard"}}},
    {WINDOW_STATE, {"window_state"}},
    // "h264" | "hevc" — the transcode targets the console can actually decode
    {TRANSCODEC, {"transcodec", {"h264", "hevc"}}},
    {PLAYBACK_MODE, {"playback_mode", {"auto", "direct", "transcode"}}},
    {PLAYBACK_QUALITY_OVERRIDES, {"playback_quality_overrides"}},
    {PLAYER_MAX_RESOLUTION,
        {
            "player_max_resolution",
            {"Auto", "4K", "1440p", "1080p", "720p", "480p"},
            {0, 2160, 1440, 1080, 720, 480},
        }},
    {OSD_ON_TOGGLE, {"osd_on_toggle"}},
    {TOUCH_GESTURE, {"touch_gesture"}},
    {CLIP_POINT, {"clip_point"}},
    {SYNC_SETTING, {"sync_setting"}},
    {OVERCLOCK, {"overclock"}},
    {MPV_VO, {"mpv_vo", {"gpu", "gpu-next", "mediacodec_embed"}}},
    {PLAYER_LOW_QUALITY, {"player_low_quality"}},
    {PLAYER_SUBS_FALLBACK, {"player_subs_fallback"}},
    // options/labels are built at runtime from media::subtitleLangCatalog() in the
    // settings tab (value stored as-is: "auto" | "off" | 2-letter code)
    {PLAYER_SUBTITLE_LANG, {"player_subtitle_lang"}},
    // free-form string, no option list: the settings tab uses an InputCell
    {PLAYER_USER_AGENT, {"player_user_agent"}},
    {PLAYER_INMEMORY_CACHE,
        {
            "player_inmemory_cache",
            {"0MB", "10MB", "20MB", "50MB", "100MB", "200MB", "500MB"},
            {0, 10, 20, 50, 100, 200, 500},
        }},
    {PLAYER_SPEED,
        {
            "player_speed",
            {"4x", "3x", "2x"},
            {400, 300, 200},
        }},
    {PLAYER_HWDEC, {"player_hwdec"}},
    {PLAYER_HWDEC_CUSTOM, {"player_hwdec_custom"}},
    {PLAYER_ASPECT, {"player_aspect", {"auto", "stretch", "crop", "4:3", "16:9"}}},
    {PLAYER_TV_MODE, {"player_tv_mode"}},
    {SHOW_FPS, {"show_fps"}},
    {SWAP_INTERVAL, {"swap_interval"}},
    {APP_SWAP_ABXY, {"app_swap_abxy"}},
    {TEXTURE_CACHE_NUM, {"texture_cache_num"}},
    {REQUEST_THREADS, {"request_threads", {"1", "2", "4", "8"}, {1, 2, 4, 8}}},
    {REQUEST_TIMEOUT,
        {"request_timeout", {"3000", "5000", "10000", "20000", "30000"}, {3000, 5000, 10000, 20000, 30000}}},
    {HTTP_PROXY_STATUS, {"http_proxy_status"}},
    {HTTP_PROXY, {"http_proxy"}},
    {WAN_POLICY, {"wan_policy", {"lan_first", "lan_only", "race"}}},
    {WAN_PROBE_MS, {"wan_probe_ms", {"1.5 s", "2 s", "3.5 s", "5 s"}, {1500, 2000, 3500, 5000}}},

    {LIBRARY_SORT, {"library_sort"}},
    {SIDEBAR_LAYOUT, {"sidebar_layout"}},

    {HINT_FORWARDER, {"hint_forwarder"}},

    {KEY_REFRESH, {"key_refresh"}},
    {KEY_LAST, {"key_last"}},
    {KEY_NEXT, {"key_next"}},
    {KEY_VOLUME_UP, {"key_volume_up"}},
    {KEY_VOLUME_DOWN, {"key_volume_down"}},
    {KEY_VIDEO_PROFILE, {"key_video_profile"}},
    {KEY_FORWARD, {"key_forward"}},
    {KEY_REWIND, {"key_rewind"}},
    {KEY_SETTING, {"key_setting"}},
    {KEY_VIDEO_QUALITY, {"key_video_quality"}},
    {KEY_VIDEO_SPEED, {"key_video_speed"}},
    {KEY_VIDEO_OSD, {"key_video_osd"}},
    {KEY_VIDEO_PAUSE, {"key_video_pause"}},
};

static std::string generateDeviceId() {
#ifdef __SWITCH__
    AccountUid uid;
    accountInitialize(AccountServiceType_Administrator);
    if (R_FAILED(accountGetPreselectedUser(&uid))) {
        if (R_FAILED(accountTrySelectUserWithoutInteraction(&uid, false))) {
            accountGetLastOpenedUser(&uid);
        }
    }
    accountExit();
    if (accountUidIsValid(&uid)) {
        uint8_t digest[32];
        sha256CalculateHash(digest, &uid, sizeof(uid));
        return misc::hexEncode(digest, sizeof(digest));
    }
#endif
    return misc::randHex(16);
}

/// Per-platform data folder for a given application name.
/// Factored out so the migration can compute the old name's path.
static std::string dataDir(const std::string& name) {
    return fmt::format("sdmc:/switch/{}", name);
}

bool AppConfig::init() {
    const std::string path = this->configDir() + "/config.json";
    // get_to may have half-applied a rejected config: restore the defaults
    const nlohmann::json defaults = *this;
    try {
        std::string raw;
        if (AtomicFile::read(path, raw)) {
            nlohmann::json::parse(raw).get_to(*this);
            brls::Logger::info("Load config from: {}", path);
        }
    } catch (const std::exception& ex) {
        brls::Logger::error("AppConfig::load {}: {}", path, ex.what());
        defaults.get_to(*this);
        try {
            brls::Logger::warning("AppConfig: unreadable config moved to {}", AtomicFile::quarantine(path));
        } catch (const std::exception& e) {
            brls::Logger::warning("AppConfig: cannot move {} aside: {}", path, e.what());
        }
        brls::sync([]() { brls::Application::notify(brls::getStr("main/setting/others/config_reset")); });
    }

    std::string uiScale = this->getItem(APP_UI_SCALE, std::string(""));
    if (uiScale == "544p") {
        brls::Application::ORIGINAL_WINDOW_WIDTH = 960;
        brls::Application::ORIGINAL_WINDOW_HEIGHT = 544;
    } else if (uiScale == "720p") {
        brls::Application::ORIGINAL_WINDOW_WIDTH = 1280;
        brls::Application::ORIGINAL_WINDOW_HEIGHT = 720;
    } else if (uiScale == "900p") {
        brls::Application::ORIGINAL_WINDOW_WIDTH = 1600;
        brls::Application::ORIGINAL_WINDOW_HEIGHT = 900;
    } else if (uiScale == "1080p") {
        brls::Application::ORIGINAL_WINDOW_WIDTH = 1920;
        brls::Application::ORIGINAL_WINDOW_HEIGHT = 1080;
    }

    AppConfig::SYNC = this->getItem(SYNC_SETTING, true);

    HTTP::TIMEOUT = this->getItem(REQUEST_TIMEOUT, 3000L);
    HTTP::PROXY_STATUS = this->getItem(HTTP_PROXY_STATUS, false);
    HTTP::PROXY = this->getItem(HTTP_PROXY, std::string("http://192.168.1.1:1080"));

    // Which address wins when a connection has both a LAN and a WAN one. Defaults
    // to the strictly-priority behaviour: a reachable LAN address is never
    // preempted by a faster tunnel answer (utils/dual_url.hpp).
    AppConfig::wan_policy = this->getItem(WAN_POLICY, std::string("lan_first"));
    // How long the home address may take before the remote one is used. Longer
    // than the 1.2 s the protocol would need on a good day, because being wrong
    // here costs tunnel quota (see reachableUrl).
    AppConfig::wan_probe_ms = this->getItem(WAN_PROBE_MS, 2000L);

    MPVCore::OSD_ON_TOGGLE = this->getItem(OSD_ON_TOGGLE, true);
    MPVCore::TOUCH_GESTURE = this->getItem(TOUCH_GESTURE, true);
    MPVCore::CLIP_POINT = this->getItem(CLIP_POINT, true);
    // 初始化内存缓存大小
    MPVCore::INMEMORY_CACHE = this->getItem(PLAYER_INMEMORY_CACHE, 10);
    // 是否使用低质量解码
    MPVCore::LOW_QUALITY = this->getItem(PLAYER_LOW_QUALITY, true);
    MPVCore::SUBS_FALLBACK = this->getItem(PLAYER_SUBS_FALLBACK, true);
    // ONE User-Agent for the whole app: the settings tab / config.json value,
    // else a `user-agent=` line in mpv.conf, else the built-in one. HTTP (API,
    // PlaybackInfo, artwork, downloads) and mpv's stream layer both read it back
    // through HTTP::default_user_agent(), so they can never disagree — cloud-drive
    // front-ends such as TgtoDrive sign the URL they hand out with the UA of the
    // request that asked for it, and a playback UA that differs is a CDN 403.
    {
        std::string ua = this->getItem(PLAYER_USER_AGENT, std::string{});
        if (ua.empty()) ua = mpvconf::value(this->configDir(), "user-agent").value_or("");
        HTTP::set_default_user_agent(ua);
        if (!ua.empty()) brls::Logger::info("User-Agent: {}", ua);
    }

    // 初始化是否使用硬件加速
    MPVCore::VO = this->getItem(MPV_VO, MPVCore::VO);
    MPVCore::HARDWARE_DEC = this->getItem(PLAYER_HWDEC, true);

    // ---- playback mode (Settings → Playback) --------------------------------
    // One choice replaces the two switches that used to gate each other ("force
    // direct play" and "also limit direct play"). They are read once and dropped:
    // forcing direct meant mode=direct, and turning the limit off meant "no
    // resolution threshold" (0), which under Auto is the same as always-direct.
    {
        const bool legacy = this->setting.contains("force_directplay") ||
                            this->setting.contains("player_resolution_limit");
        std::string mode = this->getItem(PLAYBACK_MODE, std::string{});
        if (mode != "direct" && mode != "transcode")
            mode = (legacy && this->setting.value("force_directplay", false)) ? "direct" : "auto";

        MPVCore::PLAYBACK_MODE = mode == "direct"      ? MPVCore::PlaybackMode::Direct
                                 : mode == "transcode" ? MPVCore::PlaybackMode::Transcode
                                                       : MPVCore::PlaybackMode::Auto;
        // sources above 1080p overwhelm the Switch: transcode them down by default.
        // Under Transcode this is the output height, under Auto the threshold.
        MPVCore::MAX_RESOLUTION = this->getItem(PLAYER_MAX_RESOLUTION, 1080);

        if (legacy) {
            if (!this->setting.value("player_resolution_limit", true)) MPVCore::MAX_RESOLUTION = 0;
            this->setting.erase("force_directplay");
            this->setting.erase("player_resolution_limit");
            this->setting.erase("player_video_quality");  // a remembered cap is exactly what mode replaces
            this->setItem(PLAYBACK_MODE, mode);
            this->setItem(PLAYER_MAX_RESOLUTION, MPVCore::MAX_RESOLUTION);
        }
    }
    // "h264" | "hevc": av1 was offered but dropped — the console has no AV1 decoder,
    // so a stored "av1" is read back as h264.
    MPVCore::VIDEO_CODEC = this->getItem(TRANSCODEC, MPVCore::VIDEO_CODEC);
    if (MPVCore::VIDEO_CODEC != "hevc") MPVCore::VIDEO_CODEC = "h264";
    this->setItem(TRANSCODEC, MPVCore::VIDEO_CODEC);
    MPVCore::AUDIO_CHANNELS = this->getItem(AUDIO_CHANNELS, MPVCore::AUDIO_CHANNELS);
    // 初始化自定义的硬件加速方案
    MPVCore::PLAYER_HWDEC_METHOD = this->getItem(PLAYER_HWDEC_CUSTOM, MPVCore::PLAYER_HWDEC_METHOD);
    // 初始化默认的倍速设定
    MPVCore::VIDEO_SPEED = this->getItem(PLAYER_SPEED, MPVCore::VIDEO_SPEED);
    // 初始化视频比例
    MPVCore::VIDEO_ASPECT = this->getItem(PLAYER_ASPECT, MPVCore::VIDEO_ASPECT);
    // TV-style OSD by default: the progress bar is focusable and left/right
    // seek from it — the natural behaviour on a controller-driven device
    MPVCore::OSD_TV_MODE = this->getItem(PLAYER_TV_MODE, true);

    ThreadPool::max_thread_num = this->getItem(REQUEST_THREADS, ThreadPool::max_thread_num);

    // 初始化 deviceId
    if (this->device.empty()) this->device = generateDeviceId();

    // 初始化i18n
    brls::Platform::APP_LOCALE_DEFAULT = this->getItem(APP_LANG, brls::LOCALE_AUTO);

    brls::Application::setFPSStatus(this->getItem(SHOW_FPS, false));
    VideoContext::swapInterval = this->getItem(SWAP_INTERVAL, 1);

    // 初始化 KeyBind
    KeyBind::setLast(this->getItem(KEY_LAST, std::string{"pgup"}));
    KeyBind::setNext(this->getItem(KEY_NEXT, std::string{"pgdn"}));
    KeyBind::setVolumeUp(this->getItem(KEY_VOLUME_UP, std::string{"0"}));
    KeyBind::setVolumeDown(this->getItem(KEY_VOLUME_DOWN, std::string{"9"}));
    KeyBind::setVideoProfile(this->getItem(KEY_VIDEO_PROFILE, std::string{"f1"}));
    KeyBind::setVideoQuality(this->getItem(KEY_VIDEO_QUALITY, std::string{"f2"}));
    KeyBind::setVideoSpeed(this->getItem(KEY_VIDEO_SPEED, std::string{"f3"}));
    KeyBind::setSetting(this->getItem(KEY_SETTING, std::string{"f4"}));
    KeyBind::setRefresh(this->getItem(KEY_REFRESH, std::string{"f5"}));
    KeyBind::setForward(this->getItem(KEY_FORWARD, std::string{"]"}));
    KeyBind::setRewind(this->getItem(KEY_REWIND, std::string{"["}));
    KeyBind::setVideoOsd(this->getItem(KEY_VIDEO_OSD, std::string{"o"}));
    KeyBind::setVideoPause(this->getItem(KEY_VIDEO_PAUSE, std::string{"space"}));

    // 初始化一些在创建窗口之后才能初始化的内容
    brls::Application::getWindowCreationDoneEvent()->subscribe([this]() {
        if (this->getItem(APP_SWAP_ABXY, false))
        {
            // 系统设置里可能已经交换过按键，所以读取现值再取反，而不是直接设成 true
            brls::Application::setSwapInputKeys(!brls::Application::isSwapInputKeys());
        }

        // 初始化主题
        std::string appTheme = this->getItem(APP_THEME, std::string("auto"));
        if (appTheme == "light") {
            brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::LIGHT);
        } else if (appTheme == "dark") {
            brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
        }

        // 初始化纹理缓存数量
        brls::TextureCache::instance().cache.setCapacity(getItem(TEXTURE_CACHE_NUM, 200));
    });

#ifdef __SWITCH__
    /// Set Overclock
    if (getItem(AppConfig::OVERCLOCK, false)) {
        SwitchSys::setClock(true);
    };
#endif
    Ums::instance().init();

    // init custom font path
    brls::FontLoader::USER_FONT_PATH = configDir() + "/font.ttf";
    brls::FontLoader::USER_ICON_PATH = configDir() + "/icon.ttf";
    if (access(brls::FontLoader::USER_ICON_PATH.c_str(), F_OK) == -1) {
        // 自定义字体不存在，使用内置字体
        std::string icon = getItem(KEYMAP, std::string("xbox"));
        if (icon == "xbox") {
            brls::FontLoader::USER_ICON_PATH = BRLS_ASSET("font/keymap_xbox.ttf");
        } else if (icon == "ps") {
            brls::FontLoader::USER_ICON_PATH = BRLS_ASSET("font/keymap_ps.ttf");
        } else if (brls::Application::isSwapInputKeys()) {
            brls::FontLoader::USER_ICON_PATH = BRLS_ASSET("font/keymap_keyboard_swap.ttf");
        } else {
            brls::FontLoader::USER_ICON_PATH = BRLS_ASSET("font/keymap_keyboard.ttf");
        }
    }

    brls::FontLoader::USER_EMOJI_PATH = configDir() + "/emoji.ttf";
    if (access(brls::FontLoader::USER_EMOJI_PATH.c_str(), F_OK) == -1) {
        // 自定义emoji不存在，使用内置emoji
        brls::FontLoader::USER_EMOJI_PATH = BRLS_ASSET("font/emoji.ttf");
    }

    brls::Logger::info("init {} v{}-{} device {} from {}", AppVersion::getPlatform(), AppVersion::getVersion(),
        AppVersion::getCommit(), this->device, path);
    return true;
}

void AppConfig::save() {
    try {
        std::string dir = this->configDir();
        fs::create_directories(dir);
        AtomicFile::write(dir + "/config.json", nlohmann::json(*this).dump(2));
    } catch (const std::exception& ex) {
        brls::Logger::warning("AppConfig save: {}", ex.what());
    }
}

AppConfig::~AppConfig() { delete this->activeBackend; }

void AppConfig::resetBackend() {
    delete this->activeBackend;
    this->activeBackend = nullptr;
}

std::string AppConfig::activeServerType() const {
    // The active profile names its server; that is unambiguous where the URL is
    // not (two servers behind one reverse proxy) and still works in the offline
    // shell, where server_url is empty.
    auto profile = std::find_if(this->users.begin(), this->users.end(),
        [this](const AppUser& u) { return u.id == this->user_id; });
    if (profile != this->users.end()) {
        auto server = std::find_if(this->servers.begin(), this->servers.end(),
            [&profile](const AppServer& s) { return s.id == profile->server_id; });
        if (server != this->servers.end()) return server->type;
    }
    // no profile (first launch) or an orphaned one: fall back to the active url
    for (auto& s : this->servers)
        if (!this->server_url.empty() && !s.urls.empty() && s.urls.front() == this->server_url) return s.type;
    return "plex";
}

std::string AppConfig::activeServerId() const {
    // Same resolution as activeServerType(), deliberately: the profile names its
    // server, and `user_id` survives where the `user` iterator does not (an
    // insert into `users` invalidates it).
    auto profile = std::find_if(this->users.begin(), this->users.end(),
        [this](const AppUser& u) { return u.id == this->user_id; });
    return profile != this->users.end() ? profile->server_id : std::string{};
}

int64_t AppConfig::playbackQualityFor(const std::string& key) {
    if (key.empty()) return 0;
    nlohmann::json all = this->getItem(PLAYBACK_QUALITY_OVERRIDES, nlohmann::json::object());
    if (!all.is_object()) return 0;
    auto it = all.find(key);
    if (it == all.end() || !it->is_number()) return 0;
    return it->get<int64_t>();
}

void AppConfig::setPlaybackQualityFor(const std::string& key, int64_t bps) {
    if (key.empty()) return;
    nlohmann::json all = this->getItem(PLAYBACK_QUALITY_OVERRIDES, nlohmann::json::object());
    if (!all.is_object()) all = nlohmann::json::object();
    if (bps > 0)
        all[key] = bps;
    else if (all.contains(key))
        all.erase(key);  // "Auto" in the menu: back to Settings → Playback
    else
        return;  // nothing stored, nothing to write
    this->setItem(PLAYBACK_QUALITY_OVERRIDES, all);
}

std::string AppConfig::getToken() const {
    // Jellyfin/Emby issue one AccessToken per account, and two accounts on one
    // server share an AppServer entry — so that entry's token only ever holds the
    // last account that authenticated. Read the active profile's own token there.
    // Resolved by id (not through the `user` iterator, which an insert into
    // `users` can invalidate).
    const std::string type = this->activeServerType();
    auto activeUser = std::find_if(this->users.begin(), this->users.end(),
        [this](const AppUser& u) { return u.id == this->user_id; });
    const std::string& userToken = activeUser != this->users.end() ? activeUser->access_token : this->server_token;
    return connection::activeToken(type, this->server_token, userToken);
}

media::Backend& AppConfig::backend() {
    if (!this->activeBackend) {
        std::string type = this->activeServerType();
        switch (backendTypeFromString(type)) {
            case media::BackendType::Jellyfin:
                this->activeBackend = new jellyfin::JellyfinBackend(media::BackendType::Jellyfin);
                break;
            case media::BackendType::Emby:
                this->activeBackend = new jellyfin::JellyfinBackend(media::BackendType::Emby);
                break;
            case media::BackendType::Stremio:
                this->activeBackend = new stremio::StremioBackend();
                break;
            case media::BackendType::Plex:
                this->activeBackend = new plex::PlexBackend();
                break;
        }
    }
    return *this->activeBackend;
}

media::BackendType AppConfig::backendTypeFromString(const std::string& type) {
    if (type == "jellyfin") return media::BackendType::Jellyfin;
    if (type == "emby") return media::BackendType::Emby;
    if (type == "stremio") return media::BackendType::Stremio;
    return media::BackendType::Plex;
}

std::string AppConfig::reachableUrl(const AppServer& s, bool* lan) {
    // The candidates are raced in parallel so an unreachable LAN address does
    // not block a reachable remote/relay one while roaming (GH #36). Stremio has
    // no single reachable "server" (it aggregates remote addons + an optional
    // account); skip the probe, accept the stored one.
    if (lan) *lan = false;
    switch (backendTypeFromString(s.type)) {
        case media::BackendType::Stremio:
            return s.urls.empty() ? std::string() : s.urls.front();
        case media::BackendType::Jellyfin:
        case media::BackendType::Emby: {
            // A LAN candidate fails within the budget the user picked in
            // Settings → Network (WAN_PROBE_MS): the default is generous on
            // purpose, because a slow home Wi-Fi read as "away" falls through to
            // the metered address and spends quota. A remote candidate keeps the
            // larger budget — it may have to cross a tunnel where 2 s is not
            // enough for TLS.
            auto probe = [&s](const std::string& url) {
                const bool home = dualurl::isLan(url, s.local_url);
                return jellyfin::probeConnection(
                    url, home ? AppConfig::wan_probe_ms : 5000, home ? AppConfig::wan_probe_ms : 2000);
            };
            const dualurl::Policy policy = dualurl::policyOf(AppConfig::wan_policy);
            const std::vector<std::string> candidates = dualurl::candidates(policy, s.local_url, s.urls);
            // LanFirst/LanOnly keep strict priority: the tunnel only wins once the
            // LAN probe has failed. Race is the opt-in "fastest answer wins".
            std::string chosen = (policy == dualurl::Policy::Race)
                                     ? plex::raceFirst(candidates, probe)
                                     : plex::raceConnections(candidates, probe);
            if (lan) *lan = dualurl::isLan(chosen, s.local_url);
            return chosen;
        }
        case media::BackendType::Plex:
            break;
    }
    return plex::raceConnections(s.urls, s.access_token);
}

std::string AppConfig::getLocalUrl() const {
    auto profile = std::find_if(this->users.begin(), this->users.end(),
        [this](const AppUser& u) { return u.id == this->user_id; });
    if (profile == this->users.end()) return {};
    auto server = std::find_if(this->servers.begin(), this->servers.end(),
        [&profile](const AppServer& s) { return s.id == profile->server_id; });
    return server == this->servers.end() ? std::string() : server->local_url;
}

std::optional<std::vector<std::string>> AppConfig::allowedLibraries() const {
    // Nothing to filter on the home network.
    if (this->lan_mode) return std::nullopt;
    return this->getWanVisibleLibraries();
}

std::optional<std::vector<std::string>> AppConfig::getWanVisibleLibraries() const {
    auto profile = std::find_if(this->users.begin(), this->users.end(),
        [this](const AppUser& u) { return u.id == this->user_id; });
    if (profile == this->users.end()) return std::nullopt;
    // Never configured: keep the historical "everything visible" behaviour.
    return profile->wan_visible_libraries;
}

void AppConfig::setWanVisibleLibraries(std::optional<std::vector<std::string>> libs) {
    auto profile = std::find_if(this->users.begin(), this->users.end(),
        [this](const AppUser& u) { return u.id == this->user_id; });
    if (profile == this->users.end()) return;
    profile->wan_visible_libraries = std::move(libs);
    this->save();
}

void AppConfig::setActiveUrl(const std::string& url, bool lan) {
    if (url.empty()) return;
    this->server_url = url;
    this->lan_mode = lan;
    // the backend caches nothing connection-specific except its class, but reset
    // anyway so a forced switch cannot leave a half-initialised backend behind
    this->resetBackend();
}

const std::vector<std::string>& AppConfig::getStremioAddons() const {
    static const std::vector<std::string> empty;
    if (this->user == this->users.end()) return empty;
    for (auto& s : this->servers)
        if (s.id == this->user->server_id) return s.addons;
    return empty;
}

void AppConfig::setStremioAddons(const std::vector<std::string>& addons) {
    if (this->user == this->users.end()) return;
    for (auto& s : this->servers) {
        if (s.id != this->user->server_id) continue;
        if (s.addons == addons) return;  // no disk write when nothing changed
        s.addons = addons;
        this->save();
        return;
    }
}

bool AppConfig::checkLogin() {
    auto is_user = [this](const AppUser& u) { return u.id == this->user_id; };
    this->user = std::find_if(this->users.begin(), this->users.end(), is_user);
    if (this->user == this->users.end()) return false;

    auto is_server = [this](const AppServer& s) { return s.id == this->user->server_id; };
    auto it = std::find_if(this->servers.begin(), this->servers.end(), is_server);
    if (it == this->servers.end() || (it->urls.empty() && it->local_url.empty())) return false;

    // Reconnect to a remembered endpoint. No dependency on plex.tv here: a
    // reachable stored URL is enough.
    bool lan = false;
    std::string url = reachableUrl(*it, &lan);
    if (url.empty()) {
        brls::Logger::warning("AppConfig checkLogin: aucun endpoint joignable pour {}", it->name);
        return false;
    }
    this->server_url = url;
    this->lan_mode = lan;
    this->server_token = it->access_token;
    this->resetBackend();
    this->applyTheme(backendTypeFromString(it->type));
    // Remember the reachable WAN endpoint at the front of the candidate list. A
    // LAN answer must NOT overwrite that list: local_url holds it, and copying it
    // into urls would forget the remote addresses.
    if (!lan && !it->urls.empty() && url != it->urls.front()) {
        AppServer front = *it;
        front.urls = {url};
        this->addServer(front);
    }
    return true;
}

std::string AppConfig::configDir() { return dataDir(AppVersion::getPackageName()); }

void AppConfig::checkRestart(char* argv[]) {
    // No restart path on Switch: the forwarder relaunches the NRO.
    (void)argv;
}

int AppConfig::getOptionIndex(const Item item, int default_index) const {
    auto it = settingMap.find(item);
    if (setting.contains(it->second.key)) {
        try {
            std::string value = this->setting.at(it->second.key);
            for (size_t i = 0; i < it->second.options.size(); ++i)
                if (it->second.options[i] == value) return i;
        } catch (const std::exception& e) {
            brls::Logger::error("Damaged config found: {}/{}", it->second.key, e.what());
        }
    }
    return default_index;
}

int AppConfig::getValueIndex(const Item item, int default_index) const {
    auto it = settingMap.find(item);
    if (setting.contains(it->second.key)) {
        try {
            long value = this->setting.at(it->second.key);
            for (size_t i = 0; i < it->second.values.size(); ++i)
                if (it->second.values[i] == value) return i;
        } catch (const std::exception& e) {
            brls::Logger::error("Damaged config found: {}/{}", it->second.key, e.what());
        }
    }
    return default_index;
}

bool AppConfig::addServer(const AppServer& s) {
    if (s.urls.size() > 0) {
        this->server_url = s.urls.front();
    }

    for (auto& o : this->servers) {
        if (s.id == o.id) {
            if (!s.name.empty()) o.name = s.name;
            if (!s.access_token.empty()) o.access_token = s.access_token;
            // The addresses belong to the SERVER, not to the account: a second
            // account signed in on the same server may be the first to know the
            // home address, so a non-empty one is adopted. A blank field must not
            // clear it — clearing is the edit form's job, and updateConnection()
            // writes the address unconditionally.
            if (!s.local_url.empty()) o.local_url = s.local_url;
            this->server_token = o.access_token;
            // remove old url
            for (auto it = o.urls.begin(); it != o.urls.end(); ++it) {
                if (it->compare(this->server_url) == 0) {
                    it = o.urls.erase(it);
                    break;
                }
            }
            o.urls.insert(o.urls.begin(), this->server_url);
            this->save();
            return true;
        }
    }
    this->server_token = s.access_token;
    this->servers.push_back(s);
    this->save();
    return false;
}

void AppConfig::addUser(const AppUser& u, const std::string& url, bool lan) {
    auto is_user = [u](const AppUser& o) { return o.id == u.id; };
    auto it = std::find_if(this->users.begin(), this->users.end(), is_user);
    if (it != this->users.end()) {
        it->name = u.name;
        it->access_token = u.access_token;
        it->server_id = u.server_id;
        it->thumb = u.thumb;
    } else {
        it = this->users.insert(it, u);
    }
    this->server_url = url;
    this->user_id = u.id;
    this->user = it;
    this->lan_mode = lan;
    // keeps the active server token in sync with the active user
    std::string activeType = "plex";
    for (auto& s : this->servers) {
        if (s.id == u.server_id) {
            this->server_token = s.access_token;
            activeType = s.type;
        }
    }
    this->resetBackend();
    this->applyTheme(backendTypeFromString(activeType));
    this->save();
}

void AppConfig::upsertServer(const AppServer& s) {
    // Like addServer but never touches the active server_url/server_token: this
    // registers a server we are NOT switching to. On an existing entry, refresh
    // name/token and merge any new candidate urls, keeping the current ordering
    // so a previously resolved (reachable) front url survives.
    for (auto& o : this->servers) {
        if (s.id == o.id) {
            if (!s.name.empty()) o.name = s.name;
            if (!s.access_token.empty()) o.access_token = s.access_token;
            for (auto& u : s.urls) {
                if (std::find(o.urls.begin(), o.urls.end(), u) == o.urls.end()) o.urls.push_back(u);
            }
            this->save();
            return;
        }
    }
    this->servers.push_back(s);
    this->save();
}

void AppConfig::upsertUser(const AppUser& u) {
    // Like addUser but never sets the active profile: registers a connection
    // tile without switching to it.
    auto is_user = [u](const AppUser& o) { return o.id == u.id; };
    auto it = std::find_if(this->users.begin(), this->users.end(), is_user);
    if (it != this->users.end()) {
        it->name = u.name;
        it->access_token = u.access_token;
        it->server_id = u.server_id;
        it->thumb = u.thumb;
    } else {
        // push_back can reallocate and invalidate the active-profile iterator
        this->users.push_back(u);
        this->user = std::find_if(this->users.begin(), this->users.end(),
            [this](const AppUser& o) { return o.id == this->user_id; });
    }
    this->save();
}

namespace {

/// Move the per-PROFILE settings of a connection from `oldServerId` to
/// `newServerId`. They are the things a re-login must never cost the user: the
/// sidebar layout is stored per server id. Nothing is created when there was
/// nothing to move, so a first-time save stays clean. The old key is dropped: the
/// entry is gone after this edit, and leaving it would resurrect a stale layout
/// under a stale id.
void migrateSidebarLayout(AppConfig& conf, const std::string& oldServerId, const std::string& newServerId) {
    if (!connection_edit::dropOldEntry(oldServerId, newServerId, false)) return;

    // SIDEBAR_LAYOUT is one JSON object keyed by server id.
    auto all = conf.getItem(AppConfig::SIDEBAR_LAYOUT, nlohmann::json::object());
    if (all.is_object() && all.contains(oldServerId) && !all.contains(newServerId)) {
        all[newServerId] = all.at(oldServerId);
        all.erase(oldServerId);
        conf.setItem(AppConfig::SIDEBAR_LAYOUT, all);
    }
}

/// `AppUser::wan_visible_libraries` in the shape the pure rule expects (engaged
/// flag + list), so the production path and tests/test_connection_edit.cpp run
/// the same decision.
struct AllowList {
    bool hasLibraries;
    std::vector<std::string> libraries;
    explicit AllowList(bool has = false, const std::vector<std::string>& libs = {})
        : hasLibraries(has), libraries(libs) {}
};
AllowList allowListOf(const AppUser& u) {
    if (!u.wan_visible_libraries) return AllowList();
    return AllowList(true, *u.wan_visible_libraries);
}

}  // namespace

void AppConfig::updateConnection(const std::string& oldServerId, const std::string& oldUserId,
    const AppServer& server, const AppUser& user, const std::string& url, bool lan) {
    // Was this the connection being browsed? Everything active (url, token,
    // backend, theme) then has to follow the edited entry.
    const bool wasActive = !oldUserId.empty() && oldUserId == this->user_id;
    // The profile's own settings survive a re-login that changed its ids.
    migrateSidebarLayout(*this, oldServerId, server.id);

    // --- the server entry -------------------------------------------------------
    // The edited server: an existing entry (the same id, possibly refreshed) or a
    // brand new one. Either way the addresses the user just entered are the truth.
    auto target = std::find_if(this->servers.begin(), this->servers.end(),
        [&server](const AppServer& s) { return s.id == server.id; });
    if (target != this->servers.end()) {
        target->name = server.name;
        target->access_token = server.access_token;
        target->local_url = server.local_url;
        target->type = server.type;
        target->urls = server.urls;
    } else {
        this->servers.push_back(server);
    }

    // A re-login that changed the server Id (restored instance, new front) leaves
    // the old entry behind. Drop it only when no OTHER profile still points at it:
    // the one being edited is rewritten below, so it must not count as a user (its
    // iterator still holds the old id at this point).
    {
        std::vector<AppUser> others;
        for (auto& u : this->users)
            if (u.id != oldUserId) others.push_back(u);
        const bool referenced = connection_edit::serverStillReferenced(others, oldServerId);
        if (connection_edit::dropOldEntry(oldServerId, server.id, referenced))
            this->servers.erase(std::remove_if(this->servers.begin(), this->servers.end(),
                                    [&oldServerId](const AppServer& s) { return s.id == oldServerId; }),
                this->servers.end());
    }

    // --- the profile entry ------------------------------------------------------
    // Locate by the OLD id: the edited entry is the one we must rewrite.
    auto profile = std::find_if(this->users.begin(), this->users.end(),
        [&oldUserId](const AppUser& u) { return !oldUserId.empty() && u.id == oldUserId; });
    if (profile != this->users.end()) {
        // The form carries no allow-list, but losing the stored one here would
        // silently reopen every hidden library on the tunnel. A list the caller
        // did set (a future settings screen) wins: nullopt means "not
        // configured", not "clear it".
        AppUser merged = user;
        AllowList edited = allowListOf(merged);
        connection_edit::keepProfileSettings(edited, allowListOf(*profile));
        if (edited.hasLibraries)
            merged.wan_visible_libraries = edited.libraries;
        *profile = merged;
    } else {
        this->users.push_back(user);
        this->user = std::find_if(this->users.begin(), this->users.end(),
            [this](const AppUser& u) { return u.id == this->user_id; });
    }

    if (wasActive) {
        this->user_id = user.id;
        this->server_url = url;
        this->server_token = server.access_token;
        this->lan_mode = lan;
        this->user = std::find_if(this->users.begin(), this->users.end(),
            [this](const AppUser& u) { return u.id == this->user_id; });
        this->resetBackend();
        this->applyTheme(backendTypeFromString(server.type));
    }
    this->save();
}

bool AppConfig::removeServer(const std::string& id) {
    for (auto it = this->servers.begin(); it != this->servers.end(); ++it) {
        if (it->id == id) {
            this->servers.erase(it);
            this->save();
            return this->servers.empty();
        }
    }
    return false;
}

void AppConfig::addRemote(const AppRemote& r) {
    this->remotes.push_back(r);
    this->save();
}

void AppConfig::updateRemote(size_t index, const AppRemote& r) {
    if (index >= this->remotes.size()) return;
    this->remotes[index] = r;
    this->save();
}

void AppConfig::removeRemote(size_t index) {
    if (index >= this->remotes.size()) return;
    this->remotes.erase(this->remotes.begin() + index);
    this->save();
}

bool AppConfig::isPinned(const std::string& path) const {
    return std::find(this->pins.begin(), this->pins.end(), path) != this->pins.end();
}

void AppConfig::addPin(const std::string& path) {
    if (path.empty() || this->isPinned(path)) return;
    this->pins.push_back(path);
    this->save();
}

void AppConfig::removePin(const std::string& path) {
    auto it = std::find(this->pins.begin(), this->pins.end(), path);
    if (it == this->pins.end()) return;
    this->pins.erase(it);
    this->save();
}

bool AppConfig::removeUser(const std::string& id) {
    auto it = std::find_if(this->users.begin(), this->users.end(),
        [&id](const AppUser& u) { return u.id == id; });
    if (it == this->users.end()) return false;

    // vector::erase invalidates every iterator from the erased slot onwards, and
    // `user` is dereferenced by getUser() — re-seat it or the next MainActivity
    // reads freed storage.
    const bool wasActive = (id == this->user_id);
    this->users.erase(it);
    if (wasActive) {
        // the connection being browsed is gone: drop the active state instead of
        // leaving server_url/token pointing at it, and let the UI fall back to
        // the logged-out root (ServerList).
        this->user_id.clear();
        this->server_url.clear();
        this->server_token.clear();
        this->resetBackend();
        this->user = this->users.end();
    } else {
        this->user = std::find_if(this->users.begin(), this->users.end(),
            [this](const AppUser& u) { return u.id == this->user_id; });
    }
    this->save();
    return true;
}

const std::vector<AppUser> AppConfig::getUsers(const std::string& id) const {
    std::vector<AppUser> users;
    for (auto& u : this->users) {
        if (u.server_id == id) {
            users.push_back(u);
        }
    }
    return users;
}

void AppConfig::addColor(const brls::ThemeVariant tv, const std::string& name, NVGcolor defaultColor) {
    auto& theme = (tv == brls::ThemeVariant::LIGHT) ? brls::Theme::getLightTheme() : brls::Theme::getDarkTheme();

    if (!setting.contains(name)) {
        theme.addColor(name, defaultColor);
    } else {
        unsigned int r = 0, g = 0, b = 0;
        std::string s = setting.at(name).get<std::string>();

        std::stringstream sr{s.substr(1, 2)};
        sr >> std::hex >> r;
        std::stringstream sg{s.substr(3, 2)};
        sg >> std::hex >> g;
        std::stringstream sb{s.substr(5, 2)};
        sb >> std::hex >> b;
        theme.addColor(name, nvgRGB(r, g, b));
    }
}

void AppConfig::applyTheme(std::optional<media::BackendType> type) {
    const plenx::ThemeColors& tc = type ? plenx::backendPalette(*type) : plenx::defaultPalette();
    this->applyThemeVariant(brls::ThemeVariant::DARK, tc.dark);
    this->applyThemeVariant(brls::ThemeVariant::LIGHT, tc.light);
}

void AppConfig::applyThemeVariant(brls::ThemeVariant tv, const plenx::ThemePalette& p) {
    auto& theme = (tv == brls::ThemeVariant::LIGHT) ? brls::Theme::getLightTheme() : brls::Theme::getDarkTheme();
    const NVGcolor accent = nvgRGB(p.accent.r, p.accent.g, p.accent.b);
    const NVGcolor glow = nvgRGB(p.accentGlowTop.r, p.accentGlowTop.g, p.accentGlowTop.b);
    const NVGcolor onAccent = nvgRGB(p.onAccentText.r, p.onAccentText.g, p.onAccentText.b);
    const NVGcolor listValue = nvgRGB(p.listValue.r, p.listValue.g, p.listValue.b);

    // VARIANT tokens (the per-backend accent surface).
    theme.addColor("brls/accent", accent);
    theme.addColor("brls/highlight/color1", accent);
    theme.addColor("brls/highlight/color2", glow);
    theme.addColor("brls/sidebar/active_item", accent);
    theme.addColor("brls/button/primary_enabled_background", accent);
    theme.addColor("brls/button/primary_enabled_text", onAccent);
    theme.addColor("brls/button/highlight_enabled_text", accent);
    theme.addColor("brls/button/highlight_disabled_text", accent);
    theme.addColor("brls/list/listItem_value_color", listValue);
    theme.addColor("brls/slider/line_filled", accent);

    // app tokens routed through AppConfig::addColor() so a user color override in
    // the settings JSON still wins (addColor reads `setting` before this default).
    this->addColor(tv, "color/app", accent);
    this->addColor(tv, "color/focus/bg", nvgRGBA(p.accent.r, p.accent.g, p.accent.b, 115));
}

void AppConfig::initThemes() {
    // "Dark theater" identity (UI_REDESIGN.md §3): neutral dark chrome. Only the
    // STRUCTURAL tokens are set here (constant across themes). The accent surface
    // (accent, highlight, sidebar active, primary button, slider, color/app...)
    // is VARIANT: set by applyTheme() per connected backend — see the
    // applyTheme(std::nullopt) call at the end of this function and the hooks in
    // checkLogin()/addUser()/ServerList. Theme::addColor overrides the borealis
    // values (theme.cpp), no submodule patch needed.
    auto& dark = brls::Theme::getDarkTheme();
    dark.addColor("brls/background", nvgRGB(13, 14, 17));
    dark.addColor("brls/sidebar/background", nvgRGB(16, 18, 22));
    dark.addColor("brls/highlight/background", nvgRGB(30, 33, 39));
    // press pulse: near-transparent light gray (orange suggested a
    // selection, not a press)
    dark.addColor("brls/click_pulse", nvgRGBA(255, 255, 255, 24));
    // brls:Header draws a 1px line under each section title:
    // an artifact in our design (typography is enough)
    dark.addColor("brls/header/border", nvgRGBA(0, 0, 0, 0));
    // even at rectangle_width 0, the nanovg antialiasing fringe of the
    // decorative rectangle leaves a ~1px line: transparency neutralizes it
    dark.addColor("brls/header/rectangle", nvgRGBA(0, 0, 0, 0));
    // pill toast (brls::Application::notify): translucent dark surface
    // slightly above the #0D0E11 background, white text
    dark.addColor("brls/notification/background", nvgRGBA(24, 26, 31, 235));
    dark.addColor("brls/notification/text", nvgRGB(255, 255, 255));

    auto& light = brls::Theme::getLightTheme();
    light.addColor("brls/click_pulse", nvgRGBA(0, 0, 0, 20));
    light.addColor("brls/header/border", nvgRGBA(0, 0, 0, 0));
    light.addColor("brls/header/rectangle", nvgRGBA(0, 0, 0, 0));
    // pill toast: conventional dark pill, readable on a light background
    light.addColor("brls/notification/background", nvgRGBA(45, 45, 45, 230));
    light.addColor("brls/notification/text", nvgRGB(255, 255, 255));

    // color/app (the app accent token) and color/focus/bg (translucent focus
    // background for player OSD controls) are VARIANT: set by applyThemeVariant().
    // dark scrim behind elements placed over an image (bars, badges)
    this->addColor(brls::ThemeVariant::LIGHT, "color/scrim", nvgRGBA(0, 0, 0, 160));
    this->addColor(brls::ThemeVariant::DARK, "color/scrim", nvgRGBA(0, 0, 0, 160));
    // metadata pills (detail pages)
    this->addColor(brls::ThemeVariant::LIGHT, "color/pill", nvgRGBA(0, 0, 0, 18));
    this->addColor(brls::ThemeVariant::DARK, "color/pill", nvgRGBA(255, 255, 255, 22));
    // surfaces placed over the background (content cards, PIN code panel...)
    this->addColor(brls::ThemeVariant::LIGHT, "color/surface", nvgRGB(255, 255, 255));
    this->addColor(brls::ThemeVariant::DARK, "color/surface", nvgRGB(22, 24, 29));
    // banner fade towards the background (transparent -> background color)
    this->addColor(brls::ThemeVariant::LIGHT, "color/fade_0", nvgRGBA(235, 235, 235, 0));
    this->addColor(brls::ThemeVariant::LIGHT, "color/fade_1", nvgRGB(235, 235, 235));
    this->addColor(brls::ThemeVariant::DARK, "color/fade_0", nvgRGBA(13, 14, 17, 0));
    this->addColor(brls::ThemeVariant::DARK, "color/fade_1", nvgRGB(13, 14, 17));
    // 用于骨架屏背景色
    this->addColor(brls::ThemeVariant::LIGHT, "color/grey_1", nvgRGB(245, 246, 247));
    this->addColor(brls::ThemeVariant::DARK, "color/grey_1", nvgRGB(26, 28, 33));
    this->addColor(brls::ThemeVariant::LIGHT, "color/grey_2", nvgRGB(245, 245, 245));
    this->addColor(brls::ThemeVariant::DARK, "color/grey_2", nvgRGB(51, 53, 55));
    this->addColor(brls::ThemeVariant::LIGHT, "color/grey_3", nvgRGBA(200, 200, 200, 16));
    this->addColor(brls::ThemeVariant::DARK, "color/grey_3", nvgRGBA(160, 160, 160, 160));
    this->addColor(brls::ThemeVariant::LIGHT, "color/danger", nvgRGB(198, 28, 28));
    this->addColor(brls::ThemeVariant::DARK, "color/danger", nvgRGBA(198, 28, 28, 180));
    this->addColor(brls::ThemeVariant::LIGHT, "color/white", nvgRGB(255, 255, 255));
    this->addColor(brls::ThemeVariant::DARK, "color/white", nvgRGBA(255, 255, 255, 180));
    // 分割线颜色
    this->addColor(brls::ThemeVariant::LIGHT, "color/line", nvgRGB(208, 208, 208));
    this->addColor(brls::ThemeVariant::DARK, "color/line", nvgRGB(100, 100, 100));
    // 深浅配色通用的灰色字体颜色
    this->addColor(brls::ThemeVariant::LIGHT, "font/grey", nvgRGB(148, 153, 160));
    this->addColor(brls::ThemeVariant::DARK, "font/grey", nvgRGB(148, 153, 160));

    // establish the neutral pleNx DEFAULT accent for all pre-connection screens;
    // checkLogin()/addUser() re-apply the connected backend's palette afterwards.
    this->applyTheme(std::nullopt);

    if (brls::Application::ORIGINAL_WINDOW_HEIGHT == 544) {
        brls::getStyle().addMetric("app/album/height", 215);
        brls::getStyle().addMetric("app/books/height", 270);
        brls::getStyle().addMetric("app/video/height", 290);
        // row = width x image ratio (poster 2:3 = 1.5, wide 16:9 = 0.5625)
        // + 55 of label area (margin 10 + title 25 + subtitle 20), so the
        // image fill keeps exactly the media's ratio
        brls::getStyle().addMetric("app/card/poster/width", 150);
        brls::getStyle().addMetric("app/card/poster/row", 280);
        brls::getStyle().addMetric("app/card/wide/width", 280);
        brls::getStyle().addMetric("app/card/wide/row", 213);
        brls::getStyle().addMetric("app/grid/6", 5);
        brls::getStyle().addMetric("app/grid/5", 4);
        brls::getStyle().addMetric("app/grid/4", 3);
        brls::getStyle().addMetric("app/grid/3", 2);
        brls::getStyle().addMetric("app/grid/2", 1);
        brls::getStyle().addMetric("brls/tab_frame/content_padding_sides", 30);
        brls::getStyle().addMetric("main/content_padding_sides", 15);
        brls::getStyle().addMetric("main/content_padding_top_bottom", 20);
    } else {
        // Grids lightened by one column compared to Switchfin: bigger
        // posters, readable from the couch (UI_REDESIGN.md §4).
        switch (brls::Application::ORIGINAL_WINDOW_HEIGHT) {
        case 1080:
            brls::getStyle().addMetric("app/album/height", 250);
            brls::getStyle().addMetric("app/books/height", 320);
            brls::getStyle().addMetric("app/video/height", 340);
            brls::getStyle().addMetric("app/card/poster/width", 225);
            brls::getStyle().addMetric("app/card/poster/row", 393);
            brls::getStyle().addMetric("app/card/wide/width", 410);
            brls::getStyle().addMetric("app/card/wide/row", 286);
            brls::getStyle().addMetric("app/grid/6", 7);
            brls::getStyle().addMetric("app/grid/5", 6);
            brls::getStyle().addMetric("app/grid/4", 5);
            brls::getStyle().addMetric("app/grid/3", 4);
            brls::getStyle().addMetric("app/grid/2", 3);
            break;
        case 900:
            brls::getStyle().addMetric("app/album/height", 240);
            brls::getStyle().addMetric("app/books/height", 305);
            brls::getStyle().addMetric("app/video/height", 325);
            brls::getStyle().addMetric("app/card/poster/width", 205);
            brls::getStyle().addMetric("app/card/poster/row", 363);
            brls::getStyle().addMetric("app/card/wide/width", 375);
            brls::getStyle().addMetric("app/card/wide/row", 266);
            brls::getStyle().addMetric("app/grid/6", 6);
            brls::getStyle().addMetric("app/grid/5", 5);
            brls::getStyle().addMetric("app/grid/4", 4);
            brls::getStyle().addMetric("app/grid/3", 3);
            brls::getStyle().addMetric("app/grid/2", 2);
            break;
        default:
            brls::getStyle().addMetric("app/album/height", 225);
            brls::getStyle().addMetric("app/books/height", 280);
            brls::getStyle().addMetric("app/video/height", 300);
            // row = width x image ratio + 55 of labels
            brls::getStyle().addMetric("app/card/poster/width", 185);
            brls::getStyle().addMetric("app/card/poster/row", 333);
            brls::getStyle().addMetric("app/card/wide/width", 340);
            brls::getStyle().addMetric("app/card/wide/row", 246);
            brls::getStyle().addMetric("app/grid/6", 5);
            brls::getStyle().addMetric("app/grid/5", 4);
            brls::getStyle().addMetric("app/grid/4", 4);
            brls::getStyle().addMetric("app/grid/3", 3);
            brls::getStyle().addMetric("app/grid/2", 2);
        }
        brls::getStyle().addMetric("main/content_padding_sides", 40);
        brls::getStyle().addMetric("main/content_padding_top_bottom", 30);
    }

    // UI redesign (UI_REDESIGN.md §3.2-3.3): bare and larger section titles
    // (the decorative bar of brls::Header disappears), rounded focus halo
    // matching the cards.
    brls::getStyle().addMetric("brls/header/rectangle_width", 0);
    brls::getStyle().addMetric("brls/header/rectangle_margin", 0);
    brls::getStyle().addMetric("brls/header/font_size", 24);
    brls::getStyle().addMetric("brls/highlight/stroke_width", 4);
    // 14 (not 10): the halo extends ~5 px beyond the frame, so its arc must
    // be wider than the posters' cornerRadius 10 to hug it.
    brls::getStyle().addMetric("brls/highlight/corner_radius", 14);
}