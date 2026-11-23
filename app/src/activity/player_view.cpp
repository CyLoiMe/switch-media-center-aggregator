/*
    SMCA — Plex video player.
    Verified pipeline: PLEX_MIGRATION.md §2.7.
    Units: mpv positions in seconds, Plex API in milliseconds.
*/

#include <cstdlib>

#include "activity/player_view.hpp"
#include "api/plex.hpp"
#include "api/backend.hpp"
#include "api/media/forbidden.hpp"
#include "api/media/resolution.hpp"
#include "utils/dialog.hpp"
#include "utils/misc.hpp"
#include "utils/wan_filter.hpp"
#include "utils/wan_guard.hpp"
#include "utils/playback_events.hpp"
#include "view/mpv_core.hpp"
#include "view/player_setting.hpp"
#include "view/video_view.hpp"
#include "view/video_profile.hpp"
#include "view/audio_player.hpp"

using namespace brls::literals;

/// "Watched" threshold: default value of the server preference
/// LibraryVideoPlayedThreshold
static const double SCROBBLE_THRESHOLD = 0.90;

PlayerView::PlayerView(const plex::Item& item, const int64_t seekMs, int versionIndex)
    : itemId(item.ratingKey), item(item), preferredVersion(versionIndex) {
    // take sole ownership of MPVCore: if music was playing, the audio controller
    // must stop owning the shared event bus (else it reports this video's
    // progress against the audio track and auto-advances over it). SPEC.md §11.
    AudioPlayer::instance().release();
    float width = brls::Application::contentWidth;
    float height = brls::Application::contentHeight;
    view = new VideoView();
    view->setDimensions(width, height);
    view->setWidthPercentage(100);
    view->setHeightPercentage(100);
    view->setId("video");
    this->setDimensions(width, height);
    this->addView(view);
    view->registerVideoQuality([this](...) { return this->toggleQuality(); });
    // direct-access OSD pickers; &stream lets them switch transcode-side
    // tracks as well as embedded ones
    view->registerVideoSubtitle([this](...) {
        PlayerSetting::showSubtitleMenu(&this->stream);
        return true;
    });
    view->registerVideoAudio([this](...) {
        PlayerSetting::showAudioMenu(&this->stream);
        return true;
    });
    // playback failed -> try the other delivery path once before the error
    // dialog (a hardware decoder can reject a transcoded stream, a server
    // can refuse the raw part)
    view->registerError([this](...) {
        return this->tryDirectPlayFallback() || this->tryPlainUrlFallback() ||
               this->tryPlaybackInfoFallback() || this->explainRefusal() || this->tryTranscodeFallback();
    });

    // Repeat::One: reload from 0 rather than mpv loop-file, which loops back to
    // the start= of a resumed load
    view->registerReplay([this](...) {
        MPVCore::instance().reset();
        this->scrobbled = false;
        this->playMedia(0);
        return true;
    });

    // stable session identifier (24 characters)
    this->sessionId = misc::randHex(12);

    auto& mpv = MPVCore::instance();

    brls::Application::pushActivity(new brls::Activity(this), brls::TransitionAnimation::NONE);

    playSubscribeID = view->getPlayEvent()->subscribe([this](int index) { this->playIndex(index); });

    settingSubscribeID = view->getSettingEvent()->subscribe([this]() {
        brls::View* setting = new PlayerSetting(this->view);
        brls::Application::pushActivity(new brls::Activity(setting));
    });

    eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
        auto& mpv = MPVCore::instance();
        switch (event) {
        case MpvEventEnum::MPV_RESUME:
            this->reportTimeline("playing", int64_t(mpv.video_progress) * 1000);
            view->getProfile()->init(this->playMethod);
            break;
        case MpvEventEnum::MPV_PAUSE:
            this->reportTimeline("paused", int64_t(mpv.video_progress) * 1000);
            break;
        case MpvEventEnum::LOADING_END:
            this->reportTimeline("playing", int64_t(mpv.playback_time) * 1000);
            break;
        case MpvEventEnum::MPV_STOP:
            this->mpvLoaded = false;
            this->reportStop();
            break;
        case MpvEventEnum::MPV_LOADED: {
            const char* flag = MPVCore::SUBS_FALLBACK ? "select" : "auto";
            // External (sidecar) subtitles embedded in the Media streams at detail
            // time (Plex/Jellyfin direct play).
            for (auto& part : this->stream.parts) {
                for (auto& s : part.streams) {
                    if (s.streamType != media::streamTypeSubtitle || s.key.empty()) continue;
                    std::string url = AppConfig::instance().backend().subtitleSidecarUrl(s.key);
                    mpv.command("sub-add", url.c_str(), flag, s.displayTitle.c_str());
                }
            }
            // External subtitles resolved lazily by the backend (Stremio addons):
            // mpv dropped the previous load's tracks, so (re)add them here. If the
            // fetch is still in flight, its callback adds them once it lands.
            this->mpvLoaded = true;
            this->addExternalSubtitles();
            break;
        }
        case MpvEventEnum::UPDATE_PROGRESS:
            // report cadence: every 10 s
            if (mpv.video_progress % 10 == 0) {
                this->reportTimeline("playing", int64_t(mpv.video_progress) * 1000);
                this->maybeScrobble(int64_t(mpv.video_progress) * 1000);
            }
            break;
        default:;
        }
    });
    customEventSubscribeID = mpv.getCustomEvent()->subscribe([this](const std::string& event, void* data) {
        if (event == QUALITY_CHANGE) {
            // Quality/audio/subtitle change: the HLS transcode carries a single
            // audio track and no selectable subtitle, so switching means asking
            // the server for a fresh transcode and reloading it. Mirror
            // playIndex and reset() mpv first: reloading in place kept the old
            // (hardware) decoder pinned, so the switch stalled and then
            // failed with a playback error. Read the position before reset()
            // zeroes it so the new transcode resumes where we were.
            int64_t pos = int64_t(MPVCore::instance().playback_time) * 1000;
            MPVCore::instance().reset();
            this->playMedia(pos);
        } else if (event == "PreviousTrack") {
            this->view->playNext(-1);
        } else if (event == "NextTrack") {
            this->view->playNext(1);
        }
    });

    // The quality choice belongs to the TITLE, not to the entry point: what was
    // picked while watching this show is what a resume — or the next episode — and a
    // fresh open from the library all use. Episodes share their show's key, and the
    // key carries the connection because two servers can hand out the same id.
    const bool seriesScope = !item.grandparentRatingKey.empty();
    const std::string& scope = seriesScope ? item.grandparentRatingKey : item.ratingKey;
    const std::string& scopeTitle = seriesScope ? item.grandparentTitle : item.title;
    const std::string server = AppConfig::instance().activeServerId();
    if (!scope.empty()) this->qualityKey = server.empty() ? scope : server + "/" + scope;
    this->qualityTitle = scopeTitle;
    this->qualityCap = AppConfig::instance().playbackQualityFor(this->qualityKey, this->qualityTitle);

    this->playMedia(seekMs > 0 ? seekMs : item.viewOffset);

    // Report stop when application exit
    this->exitSubscribeID = brls::Application::getExitEvent()->subscribe([this]() {
        if (!MPVCore::instance().isStopped()) this->reportStop();
    });
}

PlayerView::~PlayerView() {
    auto& mpv = MPVCore::instance();
    mpv.getEvent()->unsubscribe(eventSubscribeID);
    mpv.getCustomEvent()->unsubscribe(customEventSubscribeID);
    view->getPlayEvent()->unsubscribe(playSubscribeID);
    view->getSettingEvent()->unsubscribe(settingSubscribeID);

    brls::sync([&mpv]() { mpv.getCustomEvent()->fire(VIDEO_CLOSE, nullptr); });

    PlayerSetting::selectedSubtitle = 0;
    PlayerSetting::selectedAudio = 0;

    if (!mpv.isStopped()) this->reportStop();
    // Free the server-side transcode session on exit (else it lingers orphaned).
    this->stopTranscode();
    // The playback is over: whoever lists "continue watching" must refetch. The home
    // Resume row is cached with its tab and would otherwise still show the answer
    // from before this film was started.
    playbackEndedEvent().fire("PlaybackEnded", nullptr);
    brls::Application::getExitEvent()->unsubscribe(this->exitSubscribeID);
    brls::Logger::debug("trying delete PlayerView...");
}

void PlayerView::setSeries(const std::string& showRatingKey) {
    ASYNC_RETAIN
    // all episodes of the show
    AppConfig::instance().backend().getAllEpisodes(showRatingKey, true,
        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            int index = -1;
            std::vector<std::string> values;
            for (size_t i = 0; i < r.Items.size(); i++) {
                auto& it = r.Items.at(i);
                if (it.ratingKey == this->itemId) index = i;
                values.push_back(fmt::format("S{}E{} - {}", it.parentIndex, it.index, it.title));
            }
            view->setList(values, index);
            this->episodes = std::move(r.Items);
        },
        [ASYNC_TOKEN](const std::string& error) {
            ASYNC_RELEASE
            Dialog::show(error);
        });
}

void PlayerView::setTitie(const std::string& title) { this->view->setTitie(title); }

void PlayerView::setChapters(const std::vector<plex::Chapter>& chaps, int64_t durationMs) {
    std::vector<float> clips;
    if (durationMs > 0) {
        for (auto& c : chaps) {
            clips.push_back(float(c.startTimeOffset) / float(durationMs));
        }
    }
    this->view->setClipPoint(clips);
}

bool PlayerView::playIndex(int index) {
    if (index < 0 || index >= (int)this->episodes.size()) {
        return VideoView::close();
    }
    MPVCore::instance().reset();

    auto next = this->episodes.at(index);
    this->itemId = next.ratingKey;
    this->item = next;
    this->scrobbled = false;
    this->preferredVersion = -1;  // binge: auto-pick the best source for the new episode
    this->playMedia(0);
    view->setTitie(next.grandparentTitle.empty()
                       ? fmt::format("S{}E{} — {}", next.parentIndex, next.index, next.title)
                       : fmt::format("{} · S{}E{} — {}", next.grandparentTitle, next.parentIndex, next.index,
                             next.title));
    return true;
}

void PlayerView::playMedia(const int64_t seekMs) {
    // Release any transcode session we were running before (re)loading. Covers
    // quality/track switches, episode navigation, and transcode->direct play.
    // Without it each reload orphaned a server-side session (verified on dev:
    // they stack up at ~0% progress and never free), starving new transcodes.
    this->stopTranscode();
    // deliberate (re)start: allow the playback fallbacks to trigger again
    this->directPlayFallback = false;
    this->transcodeFallback = false;
    this->playbackInfoFallback = false;

    // Fast path: the caller already resolved the exact source (Stremio source
    // picker and movie version selector pass the fully-resolved item + chosen
    // index). Re-fetching would re-resolve streams and could return a different
    // order/set, silently playing a different release than the one selected — so
    // play the chosen one directly.
    {
        auto accessible = [](const plex::Media& m) {
            for (auto& p : m.parts)
                if (p.accessible && p.exists && !p.key.empty()) return true;
            return false;
        };
        if (this->preferredVersion >= 0 && this->preferredVersion < (int)this->item.media.size() &&
            accessible(this->item.media[this->preferredVersion])) {
            this->stream = this->item.media[this->preferredVersion];
            this->setChapters(this->item.chapters, this->item.duration);
            this->startPlayback(seekMs);
            return;
        }
    }

    ASYNC_RETAIN
    // fresh metadata: Media/Part/Stream + chapters
    AppConfig::instance().backend().getItemDetail(
        this->itemId, true,
        [ASYNC_TOKEN, seekMs](const media::Item& item) {
            ASYNC_RELEASE
            this->item = item;

            // caller-chosen source (Stremio picker) if it still resolves to an
            // accessible file; otherwise the first accessible version.
            const plex::Media* chosen = nullptr;
            auto accessible = [](const plex::Media& m) {
                for (auto& p : m.parts)
                    if (p.accessible && p.exists && !p.key.empty()) return true;
                return false;
            };
            if (this->preferredVersion >= 0 && this->preferredVersion < (int)this->item.media.size() &&
                accessible(this->item.media[this->preferredVersion])) {
                chosen = &this->item.media[this->preferredVersion];
            }
            for (auto& m : this->item.media) {
                if (chosen) break;
                if (accessible(m)) chosen = &m;
            }
            if (!chosen) {
                Dialog::show("main/player/error"_i18n, []() { VideoView::close(); });
                return;
            }
            this->stream = *chosen;
            this->setChapters(this->item.chapters, this->item.duration);
            this->startPlayback(seekMs);
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            Dialog::show(ex, []() { VideoView::close(); });
        });
}

void PlayerView::startPlayback(const int64_t seekMs, bool forceDirect, int64_t forceBitrate,
    bool forcePlaybackInfo) {
    // We are about to (re)load: mpv will drop any sub-add'ed tracks. Clear the
    // loaded flag so a subtitle fetch landing mid-load waits for MPV_LOADED to
    // re-add. (External subtitles are resolved AFTER the playback task is queued
    // — see the note at the end of this function.)
    this->mpvLoaded = false;

    this->lastSeekMs = seekMs;

    media::PlaybackOptions opts;
    opts.seekMs = seekMs;
    // A remote source (.strm on a cloud drive) IS the original file: the CDN that
    // serves it has no encoder, and asking the server to transcode means pulling
    // the whole file through the server. So neither a quality tier nor the playback
    // mode may route it into a transcode — `forceBitrate` is the one exception,
    // because it only comes from tryTranscodeFallback() (a rescue, not a preference).
    const bool remote = this->stream.remote;
    const bool directMode = MPVCore::PLAYBACK_MODE == MPVCore::PlaybackMode::Direct;
    const bool transcodeMode = MPVCore::PLAYBACK_MODE == MPVCore::PlaybackMode::Transcode;
    // Both of those leave nothing to choose: every tier of the quality menu means
    // "ask the server to re-encode", which a cloud file cannot do and Direct mode
    // refuses. The OSD button goes; the bound key still lands in toggleQuality(),
    // which explains instead of opening a dead menu.
    if (remote || directMode) this->view->hideVideoQuality();

    const int maxHeight = MPVCore::MAX_RESOLUTION;
    // Policy order, first match wins:
    //   1. an explicit rescue bitrate (a failed attempt being retried),
    //   2. a remote source: direct only (see above),
    //   3. the tier picked for THIS title (remembered; see qualityCap),
    //   4. Settings → Playback: always transcode / auto (transcode only above the
    //      resolution limit; 0 = no limit). Direct mode leaves the cap at 0.
    if (forceBitrate > 0) {
        opts.bitrateCap = forceBitrate;
    } else if (remote || directMode) {
        opts.bitrateCap = 0;
    } else if (this->qualityCap > 0) {
        // The tier remembered for this title (the show, for an episode): honoured as
        // stored, because an explicit choice must not be second-guessed — a stale one
        // (the file was replaced) can only cost one re-encode, and the menu's "Auto"
        // clears it.
        opts.bitrateCap = this->qualityCap;
    } else if (this->fallbackBitrate > 0) {
        // a transcode already rescued this playback: stay on it until the user
        // picks something (toggleQuality clears this) — see tryTranscodeFallback
        opts.bitrateCap = this->fallbackBitrate;
    } else if (transcodeMode) {
        opts.bitrateCap = media::resolutionBitrate(maxHeight > 0 ? maxHeight : 1080);
    } else {
        opts.bitrateCap = media::limitedBitrate(0, maxHeight > 0,
            AppConfig::instance().backend().caps().transcode, this->stream.width, this->stream.height, maxHeight);
    }
    // forceDirect: the transcode->direct-play fallback re-resolves with direct
    // play forced (resolvePlayback returns the direct source when set).
    opts.forceDirectPlay = directMode || forceDirect;
    opts.forcePlaybackInfo = forcePlaybackInfo;
    opts.maxHeight = maxHeight;
    opts.plainPartUrl = this->plainPartUrl;
    opts.audioStreamId = PlayerSetting::selectedAudio;
    opts.subtitleStreamId = PlayerSetting::selectedSubtitle;
    opts.burnSubtitles = PlayerSetting::selectedSubtitle > 0;
    // the target the server encodes to when it transcodes (Settings → Playback)
    opts.videoCodec = MPVCore::VIDEO_CODEC;
    opts.sessionId = this->sessionId;

    // copies for the worker thread (avoids racing on this->item during a switch)
    media::Item item = this->item;
    media::Media version = this->stream;

    // Traffic guard input, snapshotted on the UI thread: the WAN allow-list in
    // force right now (nullopt = nothing filtered). See utils/wan_guard.hpp.
    auto allow = AppConfig::instance().allowedLibraries();

    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, item, version, opts, allow]() {
        try {
            // resolvePlayback runs the transcode decision synchronously and
            // throws on failure; the direct-play fallback is internal. The Plex
            // universal-transcoder request (incl. the 1080p height cap) is
            // built here — see PlexBackend::resolvePlayback.
            media::PlaybackSource src = AppConfig::instance().backend().resolvePlayback(item, version, opts);
            // One line per playback attempt, enough to tell the three paths apart in
            // a user's log without touching the URL (which carries tokens):
            // directplay with remote=true is the CDN case, "transcode" a server encode.
            brls::Logger::info("playback {}: method={} remote={} cap={} maxHeight={}", item.ratingKey,
                src.playMethod, version.remote, opts.bitrateCap, opts.maxHeight);

            // Traffic guard: on the metered address, an item whose library is not
            // allowed there — or that cannot prove it is — must never reach mpv.
            // Cross-library surfaces (favorites, playlist, deep link) are the ways
            // around the list filtering, and a few seconds of playback is enough
            // to burn the day's quota.
            bool blocked = false;
            if (allow) {
                std::string libraryId = item.librarySectionID;
                if (libraryId.empty() && !item.ratingKey.empty())
                    libraryId = AppConfig::instance().backend().sectionOfSync(item.ratingKey).key;
                blocked = wanguard::blocksTransfer(true, !libraryId.empty(),
                    wanfilter::allows(&*allow, libraryId),
                    wanguard::isTunnelUrl(AppConfig::instance().getUrl(), src.url));
            }

            brls::sync([ASYNC_TOKEN, src, blocked, startedItem = item.ratingKey, startMs = opts.seekMs,
                           capBps = opts.bitrateCap]() {
                ASYNC_RELEASE
                if (blocked) {
                    Dialog::show("main/player/wan_block"_i18n, []() { VideoView::close(); });
                    return;
                }
                // A backend may report "nothing playable" with an empty url
                // (e.g. Stremio with no direct/debrid stream) instead of throwing
                // across the async/TU boundary; surface it as a player error.
                if (src.url.empty()) {
                    Dialog::show("main/player/error"_i18n, []() { VideoView::close(); });
                    return;
                }
                this->playMethod = src.playMethod;
                // Tell the OSD which path this playback took: the one question a log
                // used to answer ("why is this one slower?"). Quality switches reload
                // through here too, so the badge follows them.
                this->view->setStreamBadge(
                    !src.isTranscode ? "main/player/direct"_i18n
                    : capBps > 0     ? fmt::format(fmt::runtime("main/player/transcode_mbps"_i18n), capBps / 1000000)
                                     : "main/player/transcode"_i18n);
                // Register the session with the server (Jellyfin/Emby
                // /Sessions/Playing): without it the device never appears as "now
                // playing", and the progress reports have no session behind them.
                AppConfig::instance().backend().reportStart(startedItem, startMs, this->sessionId);
                // Remember the backend transcode session so we can tear it down
                // server-side on reload/exit; left running, the server keeps
                // encoding a stream nobody is watching. Set by the backend
                // (Plex); empty for direct play or backends without a server
                // transcode session, leaving stopTranscode a safe no-op.
                this->transcodeSession = src.transcodeSession;
                MPVCore::instance().setUrl(src.url, src.mpvExtra);
            });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            brls::sync([ASYNC_TOKEN, msg]() {
                ASYNC_RELEASE
                Dialog::show(msg, []() { VideoView::close(); });
            });
        }
    });

    // Resolve external subtitles AFTER queuing the playback task above. brls::async
    // is a single FIFO worker thread (not a pool): the Stremio subtitle fan-out
    // (ensureLoaded + one getSync per subtitles addon, up to a 15 s timeout each)
    // would otherwise run to completion BEFORE the fast resolvePlayback task and
    // stall the video start behind it. Queuing playback first lets mpv start
    // loading while subtitles resolve; the mpvLoaded/addExternalSubtitles handoff
    // adds them whenever the fetch lands. (No-op for Plex/Jellyfin: getSubtitles
    // returns synchronously.)
    this->resolveExternalSubtitles();
}

void PlayerView::resolveExternalSubtitles() {
    // Per-video set, same across every source/quality — skip when already resolved
    // for the current item (startPlayback re-enters on quality/track switches).
    const std::string& key = this->item.ratingKey;
    if (key.empty() || key == this->externalSubsItem) return;
    this->externalSubsItem = key;
    this->externalSubs.clear();  // drop the previous item's subs before the switch lands

    ASYNC_RETAIN
    AppConfig::instance().backend().getSubtitles(
        this->item,
        [ASYNC_TOKEN, key](std::vector<media::Stream> subs) {
            ASYNC_RELEASE
            // a newer switch superseded this fetch -> its result is stale
            if (key != this->externalSubsItem) return;
            this->externalSubs = std::move(subs);
            // if the file is already playing, add now; otherwise MPV_LOADED will
            if (this->mpvLoaded) this->addExternalSubtitles();
        },
        [ASYNC_TOKEN, key](const std::string&) {
            ASYNC_RELEASE
            // resolution failed (offline / addon error): leave the set empty, the
            // player still plays; no dialog (subtitles are best-effort).
        });
}

void PlayerView::addExternalSubtitles() {
    if (this->externalSubs.empty()) return;
    auto& mpv = MPVCore::instance();
    auto& backend = AppConfig::instance().backend();

    // Preferred language: "auto" follows the app locale, "off" disables auto-
    // selection, otherwise an explicit 2-letter code (PLAYER_SUBTITLE_LANG).
    std::string pref = AppConfig::instance().getItem(AppConfig::PLAYER_SUBTITLE_LANG, std::string("auto"));
    if (pref == "auto") {
        std::string loc = brls::Application::getLocale();  // "es", "en-US", "zh-Hans"...
        pref = loc.substr(0, loc.find('-'));
    } else if (pref == "off") {
        pref.clear();
    }

    for (auto& s : this->externalSubs) {
        if (s.key.empty()) continue;
        std::string url = backend.subtitleSidecarUrl(s.key);
        // select the track matching the preferred language; add the rest as
        // "auto" so they stay pickable in the subtitle menu without stealing it.
        bool preferred = !pref.empty() && s.languageTag == pref;
        const char* flag = preferred ? "select" : "auto";
        mpv.command("sub-add", url.c_str(), flag, s.displayTitle.c_str(), s.languageTag.c_str());
    }
}

bool PlayerView::tryDirectPlayFallback() {
    auto& mpv = MPVCore::instance();
    // only recover a failed transcode, and only once per (re)load
    if (this->playMethod != "transcode" || this->directPlayFallback) return false;
    this->directPlayFallback = true;
    this->transcodeFallback = true;

    // read before reset() zeroes it; an open failure leaves playback_time at 0
    int64_t pos = mpv.playback_time > 0 ? int64_t(mpv.playback_time) * 1000 : this->lastSeekMs;
    brls::Logger::error("PlayerView: transcode playback failed ({}) — falling back to direct play at {} ms",
        mpv.getError(), pos);
    mpv.reset();            // release the (hardware) decoder held by the failed stream
    this->stopTranscode();  // drop the dead transcode session server-side
    this->startPlayback(pos, /*forceDirect=*/true);  // re-resolve, forcing direct play
    brls::Application::notify(fmt::format("{} ({})", "main/player/direct_fallback"_i18n, mpv.getError()));
    return true;  // handled: no error dialog
}

bool PlayerView::tryPlainUrlFallback() {
    auto& mpv = MPVCore::instance();
    if (this->playMethod != "directplay" || this->plainPartUrl) return false;
    if (AppConfig::instance().backend().type() != media::BackendType::Plex) return false;
    if (this->item.type == media::mediaTypeTrack) return false;
    this->plainPartUrl = true;

    // read before reset() zeroes it; an open failure leaves playback_time at 0
    int64_t pos = mpv.playback_time > 0 ? int64_t(mpv.playback_time) * 1000 : this->lastSeekMs;
    brls::Logger::error("PlayerView: direct playback failed ({}) — retrying the bare part URL at {} ms",
        mpv.getError(), pos);
    mpv.reset();
    this->startPlayback(pos, /*forceDirect=*/true);
    return true;  // handled: no error dialog
}

bool PlayerView::explainRefusal() {
    if (AppConfig::instance().backend().type() != media::BackendType::Plex) return false;
    std::string reason = MPVCore::instance().getError();
    if (!media::isForbidden(reason)) return false;
    brls::Logger::error("PlayerView: playback refused by the server ({})", reason);
    auto dialog = new brls::Dialog(fmt::format("{}\n({})", "main/player/plex_forbidden"_i18n, reason));
    dialog->addButton("hints/back"_i18n, []() { VideoView::close(true); });
    dialog->open();
    return true;  // handled: no generic error dialog
}

bool PlayerView::tryPlaybackInfoFallback() {
    // A direct play that failed may be one this server only serves through the URL
    // it hands out in PlaybackInfo, without ever flagging the source as remote.
    // Only Jellyfin/Emby (the others do not have that URL), only once per load, and
    // only when we did not already ask (a remote source did).
    if (this->playbackInfoFallback || this->stream.remote || this->playMethod == "transcode") return false;
    const auto type = AppConfig::instance().backend().type();
    if (type != media::BackendType::Jellyfin && type != media::BackendType::Emby) return false;
    this->playbackInfoFallback = true;
    brls::Logger::info("playback {}: direct play failed, retrying with PlaybackInfo", this->itemId);
    // If PlaybackInfo has nothing new, the same URL is tried once more and fails
    // again — the flag then lets the chain continue to the transcode rescue.
    this->startPlayback(this->lastSeekMs, true, 0, true);
    return true;
}

bool PlayerView::tryTranscodeFallback() {
    auto& mpv = MPVCore::instance();
    // only recover a failed direct play, and only once per (re)load
    if (this->playMethod != "directplay" || this->transcodeFallback) return false;
    // music excluded: the Plex audio transcode path falls back to direct play
    // on any doubt, so it would replay the same URL
    // "Always direct" means never transcode, so the rescue below would contradict
    // the setting (it is the one path that turns a cap on).
    if (MPVCore::PLAYBACK_MODE == MPVCore::PlaybackMode::Direct) return false;
    if (!AppConfig::instance().backend().caps().transcode) return false;
    if (this->item.type == media::mediaTypeTrack) return false;
    // A cloud source (.strm behind a CDN) has no encoder behind it: the server-side
    // transcode would first have to pull the whole file through the server, and the
    // front-ends serving these sources refuse or 500 it. Retrying there replaces the
    // real direct-play error (403/404 against the CDN, the one worth reading) with a
    // 500 from a URL that never had a chance, after spending the seconds of a
    // re-resolve + reconnect. Direct play is the only path for these — the same rule
    // startPlayback() applies by zeroing the bitrate cap (docs/playback.md).
    if (this->stream.remote) {
        brls::Logger::info("PlayerView: direct playback of a remote source failed — no transcode to fall back to");
        return false;
    }
    this->transcodeFallback = true;
    this->directPlayFallback = true;

    // read before reset() zeroes it; an open failure leaves playback_time at 0
    int64_t pos = mpv.playback_time > 0 ? int64_t(mpv.playback_time) * 1000 : this->lastSeekMs;
    brls::Logger::error("PlayerView: direct playback failed ({}) — falling back to transcode at {} ms",
        mpv.getError(), pos);
    mpv.reset();
    // 8 Mbps: a preset of the in-player quality menu, enough for 1080p
    this->fallbackBitrate = 8000000;
    this->startPlayback(pos, /*forceDirect=*/false, this->fallbackBitrate);
    brls::Application::notify(fmt::format("{} ({})", "main/player/transcode_fallback"_i18n, mpv.getError()));
    return true;  // handled: no error dialog
}

void PlayerView::stopTranscode() {
    if (this->transcodeSession.empty()) return;
    auto& conf = AppConfig::instance();
    // Fire-and-forget: getAction copies url+token, so it is safe even if this
    // PlayerView is being destroyed. A stale session id just 404s server-side.
    plex::getAction(conf.getUrl(), conf.getToken(), nullptr, plex::apiTranscodeStop,
        HTTP::encode_form({{"session", this->transcodeSession}}));
    this->transcodeSession.clear();
}

void PlayerView::reportTimeline(const std::string& state, int64_t timeMs) {
    media::PlayState st = state == "paused"    ? media::PlayState::Paused
                          : state == "stopped" ? media::PlayState::Stopped
                                               : media::PlayState::Playing;
    AppConfig::instance().backend().reportProgress(this->itemId, st, timeMs, this->item.duration, this->sessionId);
}

void PlayerView::reportStop() {
    int64_t timeMs = int64_t(MPVCore::instance().playback_time) * 1000;
    this->reportTimeline("stopped", timeMs);
    this->maybeScrobble(timeMs);
    brls::Logger::debug("PlayerView reportStop {}", this->sessionId);
}

void PlayerView::maybeScrobble(int64_t timeMs) {
    // state=stopped is NOT enough to mark as watched: explicit scrobble required
    if (this->scrobbled || this->item.duration <= 0) return;
    if (double(timeMs) / double(this->item.duration) < SCROBBLE_THRESHOLD) return;
    this->scrobbled = true;
    AppConfig::instance().backend().markWatched(this->itemId);
}

bool PlayerView::toggleQuality() {
    // A cloud file plays as-is: every tier of that menu means "ask the server to
    // re-encode", and the file it would have to pull is the one already playing.
    // startPlayback() hides the OSD button, but the bound key still lands here.
    if (this->stream.remote) {
        brls::Application::notify("main/player/quality_remote"_i18n);
        return true;
    }
    // "Always direct" leaves nothing to pick: same story, a different reason.
    if (MPVCore::PLAYBACK_MODE == MPVCore::PlaybackMode::Direct) {
        brls::Application::notify("main/player/quality_direct"_i18n);
        return true;
    }
    std::vector<std::string> options = {"main/player/auto"_i18n};
    std::vector<int64_t> values = {0};
    int64_t videoBitRate = this->stream.bitrate * 1000;  // Plex: kbps -> bps

    if (videoBitRate >= 15000000) options.push_back("20 Mbps"), values.push_back(20000000);
    if (videoBitRate >= 10000000) options.push_back("15 Mbps"), values.push_back(15000000);
    if (videoBitRate >= 8000000) options.push_back("10 Mbps"), values.push_back(10000000);
    if (videoBitRate >= 6000000) options.push_back("8 Mbps"), values.push_back(8000000);
    if (videoBitRate >= 4000000) options.push_back("6 Mbps"), values.push_back(6000000);
    if (videoBitRate >= 3000000) options.push_back("4 Mbps"), values.push_back(4000000);
    if (videoBitRate >= 1500000) options.push_back("3 Mbps"), values.push_back(3000000);
    if (videoBitRate >= 720000) options.push_back("1.5 Mbps"), values.push_back(1500000);
    options.push_back("720 kbps"), values.push_back(720000);
    options.push_back("420 kbps"), values.push_back(420000);

    auto it = std::find(values.begin(), values.end(), this->qualityCap);
    if (it == values.end()) it = values.begin();

    brls::Dropdown* dropdown = new brls::Dropdown(
        "main/player/quality"_i18n, options,
        [this, values](int selected) {
            // Remembered for this title (the whole show, for an episode), so opening
            // it again from Continue Watching does not re-decide differently from the
            // library. "Auto" stores 0, i.e. it forgets the override.
            this->qualityCap = values[selected];
            AppConfig::instance().setPlaybackQualityFor(this->qualityKey, this->qualityTitle, this->qualityCap);
            this->fallbackBitrate = 0;
            MPVCore::instance().getCustomEvent()->fire(QUALITY_CHANGE, nullptr);
            return true;
        },
        std::distance(values.begin(), it));

    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}
