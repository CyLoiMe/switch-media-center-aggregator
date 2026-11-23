/*
    SMCA — sign-in form for a Jellyfin or Emby server.

    A clickable cell per field opens the IME for THAT field only (no chained
    IME prompts — the chaining used to flicker open then close at once). "Connect"
    validates the URL, then runs jellyfin::getServerName
    + jellyfin::login off the UI thread; on success the server/user are stored
    and MainActivity is entered, on failure the message is shown in the status
    label.

    The same form edits an existing connection (see jellyfin_add.hpp): a blank
    password with an unchanged username only rewrites the addresses and reuses
    the stored token — editing must never demand a re-login, and a re-login can
    come back with new ids. A real re-login goes through
    AppConfig::updateConnection(), which migrates what belongs to the profile.

    Presented as a content view of the ServerList AppletFrame (View::present
    from ServerTypeChoose), so it keeps the footer; Back is governed by the
    frame's hints/back action (no own BUTTON_B handler — that would override it
    and popActivity the whole ServerList).
*/

#include "tab/jellyfin_add.hpp"
#include "activity/main_activity.hpp"
#include "api/jellyfin/auth.hpp"
#include "api/jellyfin/types.hpp"
#include "utils/config.hpp"
#include "utils/net_error.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/audio_player.hpp"

using namespace brls::literals;

static std::string trim(const std::string& in) {
    auto a = in.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = in.find_last_not_of(" \t\r\n");
    return in.substr(a, b - a + 1);
}

JellyfinAdd::JellyfinAdd(const std::string& type) : type(type) {
    this->inflateFromXMLRes("xml/view/jellyfin_add.xml");
    brls::Logger::debug("JellyfinAdd: create ({})", type);

    this->header->setText(type == "emby" ? "main/server/jellyfin/title_emby"_i18n
                                          : "main/server/jellyfin/title_jellyfin"_i18n);

    this->cellUrl->init("main/server/jellyfin/url"_i18n, "", [](std::string) {}, "http://host:8096", "", 256);
    this->cellLocal->init(
        "main/network/local_addr"_i18n, "", [](std::string) {}, "http://192.168.1.100:8096", "", 256);
    this->cellUser->init("main/server/jellyfin/username"_i18n, "", [](std::string) {}, "", "", 64);
    this->cellPasswd->init("main/server/jellyfin/password"_i18n, "", [](std::string) {}, "", "", 128);
    this->cellPasswd->setType(brls::InputCellType::PASSWORD);

    this->btnConnect->registerClickAction([this](...) {
        this->submit();
        return true;
    });
    // No BUTTON_B handler here: Back is owned by the ServerList AppletFrame's
    // hints/back action, which dismiss()es this content view back to the type
    // chooser. A local handler would be found first (it sits below the frame)
    // and popActivity the whole ServerList instead.
}

JellyfinAdd::JellyfinAdd(const std::string& type, const AppUser& user, const AppServer& server,
    std::function<void()> onSaved)
    : type(type), editing(true), oldServerId(server.id), oldUserId(user.id), oldUserName(user.name),
      oldServer(server), oldUser(user), onSaved(std::move(onSaved)) {
    this->inflateFromXMLRes("xml/view/jellyfin_add.xml");
    brls::Logger::debug("JellyfinAdd: edit ({})", user.id);

    this->header->setText("main/server/jellyfin/edit_title"_i18n);
    this->btnConnect->setText("hints/ok"_i18n);

    // Pre-filled, so the edit is a correction rather than a re-entry. The
    // password is not stored and stays blank; blank + unchanged username is what
    // tells submit() an address-only save is intended.
    const std::string remote = server.urls.empty() ? std::string() : server.urls.front();
    this->cellUrl->init("main/server/jellyfin/url"_i18n, remote, [](std::string) {}, "http://host:8096", "", 256);
    this->cellLocal->init("main/network/local_addr"_i18n, server.local_url, [](std::string) {},
        "http://192.168.1.100:8096", "", 256);
    this->cellUser->init("main/server/jellyfin/username"_i18n, user.name, [](std::string) {}, "", "", 64);
    this->cellPasswd->init("main/server/jellyfin/password"_i18n, "", [](std::string) {}, "", "", 128);
    this->cellPasswd->setType(brls::InputCellType::PASSWORD);

    this->btnConnect->registerClickAction([this](...) {
        this->submit();
        return true;
    });
}

JellyfinAdd::~JellyfinAdd() { brls::Logger::debug("JellyfinAdd: delete"); }

brls::View* JellyfinAdd::getDefaultFocus() {
    // BoundView<Button> and BoundView<InputCell> are unrelated types: the
    // conditional operator only tries to convert each member to the OTHER
    // member's type, so it never finds brls::View* and the build fails. Cast
    // both to the common base (each cast is the member's own operator T*()
    // followed by a derived-to-base pointer conversion).
    return this->editing ? static_cast<brls::View*>(this->btnConnect) : static_cast<brls::View*>(this->cellUrl);
}

void JellyfinAdd::submit() {
    std::string url = trim(this->cellUrl->getValue());
    // strip any trailing slashes so baseUrl + "/path" never doubles up
    while (!url.empty() && url.back() == '/') url.pop_back();

    if (url.empty() || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) {
        this->labelStatus->setText("main/setting/server/invalid"_i18n);
        return;
    }

    if (net::unresolvableHost(url)) {
        this->labelStatus->setText(net::dnsHelp(url));
        return;
    }

    std::string user = this->cellUser->getValue();
    std::string pass = this->cellPasswd->getValue();

    // Optional home address: same normalisation, ignored when left empty.
    std::string local = trim(this->cellLocal->getValue());
    while (!local.empty() && local.back() == '/') local.pop_back();
    if (!local.empty() && local.rfind("http://", 0) != 0 && local.rfind("https://", 0) != 0) {
        this->labelStatus->setText("main/setting/server/invalid"_i18n);
        return;
    }

    // Address-only edit: nothing about the account changes, so the stored token
    // is still the answer. Any other edit is a re-login and needs the password —
    // say so here rather than surface the server's generic 401.
    const bool addressOnly = this->editing && pass.empty() && user == this->oldUserName;
    if (this->editing && !addressOnly && pass.empty()) {
        this->labelStatus->setText("main/server/jellyfin/password_required"_i18n);
        return;
    }

    this->labelStatus->setText("main/plex/connecting"_i18n);
    this->btnConnect->setVisibility(brls::Visibility::GONE);
    this->spinner->setVisibility(brls::Visibility::VISIBLE);
    brls::Application::blockInputs();

    std::string type = this->type;
    // Snapshots for the worker: the stored identity an address-only edit reuses,
    // and the mode the user was in (a LAN console must not be demoted to WAN just
    // because the probe is slow).
    const std::string keptToken = this->oldUser.access_token;
    const std::string keptUserId = this->oldUser.id;
    const std::string keptUserName = this->oldUser.name;
    const std::string keptServerId = this->oldServer.id;
    const std::string keptServerName = this->oldServer.name;
    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, type, url, local, user, pass, addressOnly, keptToken, keptUserId, keptUserName,
                    keptServerId, keptServerName]() {
        std::string error;
        bool ok = false;
        // Address-only edits reuse the stored connection verbatim; re-logins fill
        // these from the server.
        jellyfin::LoginResult r;
        if (addressOnly) {
            r.token = keptToken;
            r.userId = keptUserId;
            r.userName = keptUserName;
            r.serverId = keptServerId;
            r.serverName = keptServerName;
            try {
                // Cheap sanity check that the address answers as Jellyfin/Emby.
                // The token is per ACCOUNT, so it cannot be validated for free;
                // a typo is caught here, a revoked token surfaces on first use.
                ok = jellyfin::probeConnection(url, 5000, 2000);
                if (!ok) error = url;
            } catch (const std::exception& ex) {
                error = ex.what();
            }
        } else {
            try {
                std::string name = jellyfin::getServerName(url);
                r = jellyfin::login(url, user, pass);
                r.serverName = name;
                ok = true;
            } catch (const HTTP::Error& ex) {
                error = net::errorText(ex, url);
            } catch (const std::exception& ex) {
                error = ex.what();
            }
        }

        // Pick the address to activate: the LAN one when it answers (no tunnel
        // quota), otherwise the remote one. Probed off the UI thread, so the
        // badge is right as soon as the app opens. Same budget as the runtime
        // probe (Settings → Network): signing in must not judge the home address
        // more harshly than every later activation does.
        bool lan = false;
        std::string active = url;
        const long budget = AppConfig::wan_probe_ms;
        if (ok && !local.empty() && jellyfin::probeConnection(local, budget, budget)) {
            active = local;
            lan = true;
        }

        brls::sync([ASYNC_TOKEN, url, local, active, lan, r, ok, error]() {
            ASYNC_RELEASE
            brls::Application::unblockInputs();
            this->spinner->setVisibility(brls::Visibility::GONE);
            this->btnConnect->setVisibility(brls::Visibility::VISIBLE);
            if (!ok) {
                this->labelStatus->setText(error);
                return;
            }
            if (this->editing)
                this->finishEdit(url, local, active, lan, r);
            else
                this->finish(url, local, active, lan, r);
        });
    });
}

void JellyfinAdd::finish(const std::string& remote, const std::string& local,
    const std::string& active, bool lan, const jellyfin::LoginResult& r) {
    AppServer s;
    s.name = r.serverName.empty() ? remote : r.serverName;
    s.id = r.serverId.empty() ? remote : r.serverId;
    s.access_token = r.token;
    s.urls.push_back(remote);
    s.local_url = local;
    s.type = this->type;
    AppConfig::instance().addServer(s);

    AppUser u;
    u.id = r.userId;
    u.name = r.userName;
    u.can_download = r.canDownload;
    u.access_token = r.token;
    u.server_id = s.id;
    u.thumb = jellyfin::userImageUrl(active, r.userId, r.userImageTag, this->type == "emby");
    AppConfig::instance().addUser(u, active, lan);

    // The queue that was playing belongs to the connection being left.
    AudioPlayer::releaseForSwitch();
    brls::Application::clear();
    brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::NONE);
}

void JellyfinAdd::finishEdit(const std::string& remote, const std::string& local,
    const std::string& active, bool lan, const jellyfin::LoginResult& r) {
    AppServer s;
    s.name = r.serverName.empty() ? remote : r.serverName;
    // An address-only edit keeps the stored identity: the probe cannot invent one,
    // and inventing it from the URL would silently fork the server entry.
    s.id = r.serverId.empty() ? remote : r.serverId;
    s.access_token = r.token.empty() ? this->oldServer.access_token : r.token;
    // Remote is the canonical candidate: it is the one the tunnel resolves to.
    s.urls = {remote};
    s.local_url = local;
    s.type = this->type;

    AppUser u = this->oldUser;
    if (!r.userId.empty()) u.id = r.userId;
    if (!r.userName.empty()) u.name = r.userName;
    u.can_download = r.canDownload;
    if (!r.token.empty()) u.access_token = r.token;
    u.server_id = s.id;
    // The avatar URL embeds the user Id: only rebuild it when the login gave one.
    if (!r.userImageTag.empty())
        u.thumb = jellyfin::userImageUrl(active, r.userId, r.userImageTag, this->type == "emby");

    const bool wasActive = this->oldUserId == AppConfig::instance().getUserId();
    AppConfig::instance().updateConnection(this->oldServerId, this->oldUserId, s, u, active, lan);

    if (wasActive) {
        // Same connection, possibly new ids: rebuild the shell so every cached id
        // (sidebar tabs, artwork, the backend's userId()) follows the edit. The
        // music queue holds those same ids, so it goes with them.
        AudioPlayer::releaseForSwitch();
        brls::Application::clear();
        brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::NONE);
        return;
    }
    // Editing a connection that is not the one being browsed: nothing to rebuild,
    // just refresh the tiles. This form is a detail view here, so its dismissal is
    // ui::popDetail() — View::dismiss() only pops an AppletFrame content view.
    std::function<void()> cb = this->onSaved;
    if (!ui::popDetail(this)) this->dismiss();
    if (cb) brls::sync([cb]() { cb(); });
}
