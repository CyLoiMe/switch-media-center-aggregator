#include "tab/home_tab.hpp"
#include "view/recyling_video.hpp"
#include "view/loading_spinner.hpp"
#include "api/plex.hpp"
#include "api/backend.hpp"
#include "utils/keybind.hpp"
#include "utils/network_state.hpp"
#include "utils/offline_library.hpp"
#include "utils/offline_ui.hpp"
#include "utils/playback_events.hpp"

using namespace brls::literals;  // for _i18n

HomeTab::HomeTab() {
    brls::Logger::debug("Tab HomeTab: create");
    this->inflateFromXMLRes("xml/tabs/home.xml");
    // centered spinner overlay shown while the home hubs load (the rows are
    // built into boxHome only once the response arrives).
    this->spinner = new LoadingSpinner();
    this->addView(this->spinner);

    // A playback that ends while this tab is cached must refresh the Resume row now:
    // re-attaching a cached tab does not re-request anything, so the film just
    // watched would stay missing until a manual refresh or restart.
    this->playbackEndedSub = playbackEndedEvent().subscribe([this](const std::string& event, void*) {
        if (event != "PlaybackEnded") return;
        if (this->resumeRow) this->doResume(this->resumeRow);
    });
}

HomeTab::~HomeTab() {
    playbackEndedEvent().unsubscribe(this->playbackEndedSub);
    brls::Logger::debug("View HomeTab: delete");
}

brls::View* HomeTab::create() { return new HomeTab(); }

/// The home screen mirrors the rows configured server-side: "Continue
/// watching" then the hubs from /hubs, with their localized titles
/// (X-Plex-Language) — PLEX_MIGRATION.md §2.5.
void HomeTab::doRequest() {
    // anything the rows pointed at is about to be destroyed by clearViews()
    this->resumeRow = nullptr;
    // drop any previous offline empty overlay -> back to the scrollable list
    if (this->offlineEmpty) {
        this->removeView(this->offlineEmpty);
        this->offlineEmpty = nullptr;
        this->scroll->setVisibility(brls::Visibility::VISIBLE);
    }

    // offline: the server hubs are unavailable — show one poster row per
    // downloaded library instead (same row layout as online) (SPEC AC13)
    if (NetworkState::isOffline()) {
        this->boxHome->clearViews();
        auto& lib = OfflineLibrary::instance();
        bool any = false;
        for (auto& s : lib.sections()) {
            auto items = lib.sectionItems(s.key);
            if (items.empty()) continue;
            RecylingVideo* row = new RecylingVideo();
            row->setTitle(s.title);
            row->setFrameHeight(brls::getStyle()["app/card/poster/row"]);
            row->setItemWidth(brls::getStyle()["app/card/poster/width"]);
            row->setSidePadding(brls::getStyle()["main/content_padding_sides"]);
            row->setItems(items);
            this->boxHome->addView(row);
            any = true;
        }
        // nothing downloaded: offline empty state (icon + message + Retry),
        // centered by filling the tab (scroll hidden) instead of scrolling
        if (!any) {
            this->scroll->setVisibility(brls::Visibility::GONE);
            this->offlineEmpty = offline_ui::makeEmpty();
            this->addView(this->offlineEmpty);
        }
        return;
    }

    // clearViews destroys the focused card when refreshing after playback:
    // remember to give the focus back to the first rebuilt row, otherwise it
    // falls back to the sidebar and the user loses track of it
    this->restoreFocus = hasFocusWithin(this);
    this->spinner->setSpinning(true);
    this->boxHome->clearViews();

    // "Continue watching" row — added right away to guarantee its position
    // at the top, filled when the response arrives
    RecylingVideo* resume = new RecylingVideo();
    resume->setTitle("main/home/resume"_i18n);
    resume->setFrameHeight(brls::getStyle()["app/card/poster/row"]);
    resume->setItemWidth(brls::getStyle()["app/card/poster/width"]);
    resume->setSidePadding(brls::getStyle()["main/content_padding_sides"]);
    resume->setVisibility(brls::Visibility::GONE);
    this->boxHome->addView(resume);

    this->resumeRow = resume;
    this->doResume(resume);
    this->doHubs();
}

void HomeTab::doResume(RecylingVideo* row) {
    // The row must not travel into the answer. It is a child of boxHome, and
    // doRequest() deletes the rows it replaces (nulling resumeRow first) — so an
    // answer that arrived after a rebuild wrote into freed memory: on hardware that
    // was an Instruction Abort at 0, i.e. a virtual call through the dead object,
    // with "继续观看" on the stack. Carry a generation and re-read the member
    // instead: a non-null member is always a live row, and a newer request makes an
    // older answer a no-op.
    (void)row;
    const uint32_t gen = ++this->resumeGeneration;
    ASYNC_RETAIN
    AppConfig::instance().backend().getContinueWatching(20,
        [ASYNC_TOKEN, gen](const media::Container<media::Hub>& r) {
            ASYNC_RELEASE
            if (gen != this->resumeGeneration) return;  // a newer request owns the row now
            RecylingVideo* row = this->resumeRow;
            if (!row) return;                            // the rows were dropped meanwhile
            for (auto& hub : r.Items) {
                if (hub.items.empty()) continue;
                std::string title = hub.title.empty() ? "main/home/resume"_i18n : hub.title;
                row->setTitle(title);
                // truncated hub (more=1): "+" card to the full page
                if (hub.more && !hub.key.empty()) {
                    row->setItems(hub.items, title, hub.key);
                } else {
                    row->setItems(hub.items);
                }
                this->tryRestoreFocus();
                return;
            }
            row->setItems({});
        },
        [ASYNC_TOKEN, gen](const std::string& ex) {
            ASYNC_RELEASE
            if (gen != this->resumeGeneration) return;
            if (RecylingVideo* row = this->resumeRow) row->setItems({});
            brls::Logger::warning("home continueWatching: {}", ex);
        });
}

void HomeTab::doHubs() {
    ASYNC_RETAIN
    AppConfig::instance().backend().getHomeHubs(20, true,
        [ASYNC_TOKEN](const media::Container<media::Hub>& r) {
            ASYNC_RELEASE
            for (auto& hub : r.Items) {
                if (hub.items.empty()) continue;
                // in case the server returns them despite excludeContinueWatching
                if (hub.hubIdentifier == "home.continue" || hub.hubIdentifier == "home.ondeck") continue;

                // playlist hubs mix audio/photo/video: only video playlists
                // are playable in pleNx
                std::vector<plex::Item> items;
                items.reserve(hub.items.size());
                for (auto& item : hub.items) {
                    if (item.type == plex::mediaTypePlaylist && item.playlistType != "video") continue;
                    items.push_back(item);
                }
                if (items.empty()) continue;

                RecylingVideo* row = new RecylingVideo();
                row->setTitle(hub.title);
                float frameHeight = brls::getStyle()["app/card/poster/row"];
                row->setFrameHeight(frameHeight);
                // playlists AND music (artist/album/track): SQUARE covers (1:1)
                // — width = image height of the row (frame - 55 of labels,
                // video_card.xml metrics); everything else keeps 2:3 posters
                const std::string& t0 = items.front().type;
                bool square = t0 == plex::mediaTypePlaylist || t0 == plex::mediaTypeArtist ||
                              t0 == plex::mediaTypeAlbum || t0 == plex::mediaTypeTrack;
                row->setItemWidth(square ? frameHeight - 55 : brls::getStyle()["app/card/poster/width"]);
                row->setSidePadding(brls::getStyle()["main/content_padding_sides"]);
                // truncated hub (more=1): "+" card to the full page
                if (hub.more && !hub.key.empty()) {
                    row->setItems(items, hub.title, hub.key);
                } else {
                    row->setItems(items);
                }
                this->boxHome->addView(row);
            }
            this->spinner->setSpinning(false);
            this->tryRestoreFocus();
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->spinner->setSpinning(false);
            auto dialog = new brls::Dialog(ex);
            dialog->addButton("hints/retry"_i18n, [this]() { brls::sync([this]() { this->doRequest(); }); });
            dialog->addButton("hints/cancel"_i18n, []() {});
            dialog->open();
        });
}

void HomeTab::tryRestoreFocus() {
    if (!this->restoreFocus) return;
    this->restoreFocus = false;
    // deferred one frame: the rows added by this callback are not laid out
    // yet, and giveFocus before the first layout fails silently
    ASYNC_RETAIN
    brls::sync([ASYNC_TOKEN]() {
        ASYNC_RELEASE
        brls::View* target = this->boxHome->getDefaultFocus();
        if (target) brls::Application::giveFocus(target);
    });
}

void HomeTab::onCreate() {
    auto actionRefresh = [this](brls::View* view) {
        this->doRequest();
        return true;
    };

    this->registerAction("hints/refresh"_i18n, brls::BUTTON_BACK, actionRefresh);
    this->registerAction(KeyBind::getRefresh(), actionRefresh);

    this->doRequest();
}
