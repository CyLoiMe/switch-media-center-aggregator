/*
    SMCA — Jellyfin/Emby implementation of media::Backend (see jellyfin/backend.hpp).
    Translates the Jellyfin API into the neutral media:: model. Reads the active
    server/token/userId from AppConfig live.

    Validated to COMPILE against the documented API; transcode (master.m3u8) and
    download paths are best-effort and need checking against a live server.
*/

#include "api/jellyfin/backend.hpp"
#include "api/jellyfin/types.hpp"
#include "api/jellyfin/playback_info.hpp"
#include "api/media/resolution.hpp"
#include "utils/config.hpp"
#include "utils/misc.hpp"
#include "utils/wan_filter.hpp"
#include <borealis/core/logger.hpp>
#include <borealis/core/i18n.hpp>
#include <algorithm>
#include <map>
#include <optional>
#include <sstream>

using namespace brls::literals;  // hub titles localized like the rest of the UI

namespace jellyfin {

namespace {

std::string userId() { return AppConfig::instance().getUserId(); }
std::string base() { return AppConfig::instance().getUrl(); }
std::string token() { return AppConfig::instance().getToken(); }

/// A library whose contents this app browses (movie/show/artist/video/mixed).
/// "video" is an Emby "Photos & videos" (homevideos) library: the home rows pick
/// up its videos, the photos in it have no renderer and are not queried.
bool browsableLibrary(const std::string& type) {
    return type == media::mediaTypeMovie || type == media::mediaTypeShow ||
           type == media::mediaTypeArtist || type == media::mediaTypeVideo ||
           type == media::mediaTypeMixed;
}

/// Tri-state (nullopt = no filtering) -> the pure predicate's pointer form.
bool allowsLibrary(const std::optional<std::vector<std::string>>& allow, const std::string& libraryId) {
    return wanfilter::allows(allow ? &*allow : nullptr, libraryId);
}

/// The account's browsable libraries, minus the ones the WAN allow-list excludes.
/// Blocking: call inside brls::async. `allow` is a UI-thread snapshot
/// (AppConfig::allowedLibraries) — the backend never decides policy itself.
std::vector<media::Section> visibleLibraries(const std::string& b, const std::string& tok,
    const std::string& uid, const std::optional<std::vector<std::string>>& allow) {
    auto views = parseContainer<media::Section>(
        getSync(b + fmt::format(fmt::runtime(apiViews), uid), tok), parseSection);
    std::vector<media::Section> out;
    for (auto& v : views.Items)
        if (browsableLibrary(v.type) && allowsLibrary(allow, v.key)) out.push_back(v);
    return out;
}

/// A page filter for the lists that are NOT built per library — favorites and
/// playlist items mix libraries and their endpoints take no library scope. The
/// page is therefore filtered after the fact, exactly, so a hidden library still
/// contributes neither a row nor its artwork. Asking per library instead would
/// break the server-side sort and the page offsets.
struct WanPage {
    std::optional<std::vector<std::string>> allow;  // never nullopt: filtering only
    std::string b, tok, uid;
    std::map<std::string, std::string> shows;  // show id -> library id ("" = none)
};

/// Library of an arbitrary id, resolved from its Ancestors. Cached per show so a
/// page of episodes costs one request per SHOW, not per episode. Blocking: call
/// inside brls::async.
std::string libraryOf(WanPage& w, const std::string& id) {
    auto cached = w.shows.find(id);
    if (cached != w.shows.end()) return cached->second;
    std::string lib;
    try {
        auto j = getSync(w.b + fmt::format(fmt::runtime(apiAncestors), id, w.uid), w.tok);
        if (j.is_array())
            for (auto& a : j)
                if (jstr(a, "Type") == "CollectionFolder") {
                    lib = jstr(a, "Id");
                    break;
                }
    } catch (const std::exception& ex) {
        brls::Logger::warning("JellyfinBackend: no library for {} ({})", id, ex.what());
    }
    w.shows.emplace(id, lib);
    return lib;
}

/// Keep the items whose library the allow-list admits. Survivors carry that
/// library, so the playback guard does not have to ask again when they play.
std::vector<media::Item> filterPage(WanPage& w, std::vector<media::Item> items) {
    std::vector<media::Item> out;
    out.reserve(items.size());
    for (auto& it : items) {
        const std::string lookup = wanfilter::libraryLookupId(
            it.type, it.ratingKey, it.parentRatingKey, it.grandparentRatingKey);
        const std::string lib = lookup.empty() ? it.parentRatingKey : libraryOf(w, lookup);
        if (!allowsLibrary(w.allow, lib)) continue;
        it.librarySectionID = lib;
        out.push_back(std::move(it));
    }
    return out;
}

/// Walk the pages of `urlAt` until one keeps something or the list ends. A page
/// that is entirely hidden must not read as the end of the list: the callers
/// drive their "is there more" from the items they receive.
template <typename Fn>
media::Container<media::Item> fetchFiltered(WanPage& w, size_t start, size_t size, Fn urlAt) {
    media::Container<media::Item> out;
    size_t off = start;
    for (;;) {
        auto c = parseContainer<media::Item>(getSync(urlAt(off), w.tok), parseItem);
        out.TotalRecordCount = c.TotalRecordCount;
        const bool rawEmpty = c.Items.empty();
        for (auto& it : filterPage(w, std::move(c.Items))) out.Items.push_back(std::move(it));
        if (!out.Items.empty() || rawEmpty || size == 0 || off + size >= (size_t)c.TotalRecordCount) break;
        off += size;
    }
    // Offset of the page that produced these items — not the requested one. A
    // caller whose pages were consumed but emptied must continue AFTER them, or
    // it would ask for a hidden page again on the next round.
    out.StartIndex = (long)off;
    return out;
}

/// Plex-style sort token -> Jellyfin SortBy field
std::string mapSort(const std::string& f) {
    if (f == "addedAt") return "DateCreated,SortName";
    if (f == "originallyAvailableAt") return "PremiereDate,SortName";
    if (f == "rating") return "CommunityRating,SortName";
    if (f == "viewCount") return "PlayCount,SortName";
    if (f == "userRating") return "CriticRating,SortName";
    return "SortName";  // titleSort and default
}

std::string kindTypes(media::MediaKind k) {
    if (k == media::MediaKind::Movie) return "Movie";
    if (k == media::MediaKind::Show) return "Series";
    if (k == media::MediaKind::Artist) return "MusicArtist";
    if (k == media::MediaKind::Album) return "MusicAlbum";
    if (k == media::MediaKind::Track) return "Audio";
    // Emby/Jellyfin home videos: the item type is `Video`, not `Movie`.
    if (k == media::MediaKind::Video) return "Video";
    return "Movie,Series";
}

/// Async fetch of a paginated container of items.
template <typename T, typename Fn>
void fetchContainer(const std::string& url, Fn parse, media::Then<media::Container<T>> then, media::OnError error) {
    std::string tok = token();
    brls::async([url, tok, parse, then, error]() {
        try {
            auto j = getSync(url, tok);
            auto c = parseContainer<T>(j, parse);
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

/// Async fetch of a single item (detail endpoint returns the bare object).
void fetchItem(const std::string& url, media::Then<media::Item> then, media::OnError error) {
    std::string tok = token();
    brls::async([url, tok, then, error]() {
        try {
            auto j = getSync(url, tok);
            media::Item it = parseItem(j);
            brls::sync(std::bind(then, std::move(it)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

/// Async fetch of a list of items wrapped as a single titled hub.
void fetchHub(const std::string& url, const std::string& title, const std::string& hubId,
    media::Then<media::Container<media::Hub>> then, media::OnError error) {
    std::string tok = token();
    brls::async([url, tok, title, hubId, then, error]() {
        try {
            auto items = parseContainer<media::Item>(getSync(url, tok), parseItem);
            media::Container<media::Hub> c;
            if (!items.Items.empty()) {
                media::Hub hub;
                hub.title = title;
                hub.hubIdentifier = hubId;
                hub.items = std::move(items.Items);
                c.Items.push_back(std::move(hub));
            }
            c.TotalRecordCount = (long)c.Items.size();
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

/// Fire-and-forget POST / DELETE (played/favorite/progress).
void postAction(const std::string& url) {
    std::string tok = token();
    brls::async([url, tok]() {
        try {
            HTTP::post(url, "", headers(tok), HTTP::Timeout{});
        } catch (const std::exception& ex) {
            brls::Logger::warning("jellyfin POST {}: {}", url, ex.what());
        }
    });
}

void deleteAction(const std::string& url) {
    std::string tok = token();
    brls::async([url, tok]() {
        try {
            HTTP s;
            HTTP::set_option(s, headers(tok));
            std::ostringstream sink;
            s._delete(url, &sink);
        } catch (const std::exception& ex) {
            brls::Logger::warning("jellyfin DELETE {}: {}", url, ex.what());
        }
    });
}

/// POST /Items/{id}/PlaybackInfo and return the play URL the server hands out for
/// `sourceId` (empty when it gives none we can use — see jellyfin::directStreamUrl).
/// Blocking: resolvePlayback() already runs inside brls::async.
std::string playbackInfoUrl(const std::string& itemId, const std::string& sourceId, const std::string& tok) {
    try {
        HTTP::Form form = {{"UserId", userId()}};
        std::string url = base() + fmt::format("/Items/{}/PlaybackInfo?{}", itemId, HTTP::encode_form(form));
        return directStreamUrl(postSync(url, tok, "{}"), sourceId, base(), tok);
    } catch (const std::exception& ex) {
        // a server without PlaybackInfo (or a slow one) keeps the built URL: this
        // path only helps remote sources and must never break the others
        brls::Logger::warning("jellyfin PlaybackInfo {}: {}", itemId, ex.what());
        return "";
    }
}

}  // namespace

const media::Capabilities& JellyfinBackend::caps() const {
    static const media::Capabilities c = []() {
        media::Capabilities caps;
        caps.listKind = media::ListKind::Favorites;  // Jellyfin/Emby favorites
        caps.skipIntro = false;                      // MediaSegments not wired yet
        return caps;
    }();
    return c;
}

// ---- navigation ----------------------------------------------------------------

void JellyfinBackend::listSections(media::Then<media::Container<media::Section>> then, media::OnError error) {
    std::string url = base() + fmt::format(fmt::runtime(apiViews), userId());
    // The sidebar is where a hidden library would first show up; drop it before
    // the tabs are built (and before any of its artwork can be fetched).
    auto allow = AppConfig::instance().allowedLibraries();
    fetchContainer<media::Section>(url, parseSection,
        [allow, then](const media::Container<media::Section>& c) {
            media::Container<media::Section> out = c;
            out.Items.erase(std::remove_if(out.Items.begin(), out.Items.end(),
                                [&allow](const media::Section& s) { return !allowsLibrary(allow, s.key); }),
                out.Items.end());
            out.TotalRecordCount = (long)out.Items.size();
            if (then) then(std::move(out));
        },
        error);
}

void JellyfinBackend::getHomeHubs(
    int count, bool, media::Then<media::Container<media::Hub>> then, media::OnError error) {
    // The Plex /hubs endpoint returns the whole promoted home (Recently Added
    // per library, …) in one call. Jellyfin/Emby have no equivalent, so we
    // compose the home the way their own web client does: a "Next Up" row, then
    // one "Recently Added" row per movie/show library. (Continue Watching is a
    // separate call, getContinueWatching.) Without this the home was empty on a
    // fresh server — Next Up alone is empty until something has been watched.
    std::string b = base(), tok = token(), uid = userId();
    int cnt = count;
    // titles resolved on the UI thread (i18n map lookup) before going async
    std::string nextupTitle = "main/home/nextup"_i18n;
    std::string latestLabel = "main/home/latest"_i18n;
    // WAN allow-list snapshot (nullopt = no filtering). Every row below is built
    // from `visibleLibraries()`, so a hidden library contributes neither a row nor
    // the artwork its row would have fetched.
    auto allow = AppConfig::instance().allowedLibraries();
    brls::async([b, tok, uid, cnt, nextupTitle, latestLabel, allow, then, error]() {
        try {
            media::Container<media::Hub> out;
            std::vector<media::Section> visible = visibleLibraries(b, tok, uid, allow);

            // Next Up, per SHOW library: one global call would happily return an
            // episode from a hidden one.
            {
                media::Hub h;
                h.title = nextupTitle;
                h.hubIdentifier = "home.nextup";  // not "home.ondeck": home_tab skips that one
                for (auto& v : visible) {
                    int remaining = cnt - (int)h.items.size();
                    if (remaining <= 0) break;
                    if (v.type != media::mediaTypeShow && v.type != media::mediaTypeMixed) continue;
                    HTTP::Form f = {{"userId", uid}, {"ParentId", v.key}, {"Limit", std::to_string(remaining)},
                        {"Fields", itemFieldsValue}};
                    auto c = parseContainer<media::Item>(
                        getSync(b + fmt::format(fmt::runtime(apiNextUp), HTTP::encode_form(f)), tok), parseItem);
                    for (auto& it : c.Items) h.items.push_back(std::move(it));
                }
                if (!h.items.empty()) out.Items.push_back(std::move(h));
            }

            // Recently Added, one row per visible library
            for (auto& v : visible) {
                HTTP::Form f = {{"ParentId", v.key}, {"Limit", std::to_string(cnt)}, {"Fields", itemFieldsValue}};
                auto c = parseContainer<media::Item>(
                    getSync(b + fmt::format(fmt::runtime(apiLatest), uid, HTTP::encode_form(f)), tok), parseItem);
                if (c.Items.empty()) continue;
                media::Hub h;
                h.title = fmt::format("{} · {}", latestLabel, v.title);
                h.hubIdentifier = "home.latest." + v.key;
                h.items = std::move(c.Items);
                out.Items.push_back(std::move(h));
            }

            out.TotalRecordCount = (long)out.Items.size();
            brls::sync(std::bind(then, std::move(out)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void JellyfinBackend::getSectionHubs(
    const std::string& sectionId, int count, media::Then<media::Container<media::Hub>> then, media::OnError error) {
    HTTP::Form form = {{"ParentId", sectionId}, {"Limit", std::to_string(count)}, {"Fields", itemFieldsValue}};
    std::string url = base() + fmt::format(fmt::runtime(apiLatest), userId(), HTTP::encode_form(form));
    fetchHub(url, "main/home/latest"_i18n, "home.latest", then, error);
}

void JellyfinBackend::getContinueWatching(
    int count, media::Then<media::Container<media::Hub>> then, media::OnError error) {
    std::string b = base(), tok = token(), uid = userId();
    int cnt = count;
    std::string title = "main/home/resume"_i18n;
    auto allow = AppConfig::instance().allowedLibraries();
    // Per-library, not one global Resume call: that call returns episodes of any
    // library, and a hidden one must not contribute a row entry (its poster would
    // then be fetched over the tunnel).
    brls::async([b, tok, uid, cnt, title, allow, then, error]() {
        try {
            media::Container<media::Hub> out;
            media::Hub hub;
            hub.title = title;
            hub.hubIdentifier = "home.continue";
            for (auto& v : visibleLibraries(b, tok, uid, allow)) {
                int remaining = cnt - (int)hub.items.size();
                if (remaining <= 0) break;
                HTTP::Form form = {
                    {"ParentId", v.key},
                    {"Limit", std::to_string(remaining)},
                    {"MediaTypes", "Video"},
                    {"Fields", itemFieldsValue},
                };
                auto c = parseContainer<media::Item>(
                    getSync(b + fmt::format(fmt::runtime(apiResume), uid, HTTP::encode_form(form)), tok), parseItem);
                for (auto& it : c.Items) hub.items.push_back(std::move(it));
            }
            if (!hub.items.empty()) out.Items.push_back(std::move(hub));
            out.TotalRecordCount = (long)out.Items.size();
            brls::sync(std::bind(then, std::move(out)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void JellyfinBackend::getLibraryGrid(const std::string& sectionId, const media::GridQuery& q, size_t start,
    size_t size, media::Then<media::Container<media::Item>> then, media::OnError error) {
    // Music artists are virtual entities: /Items?IncludeItemTypes=MusicArtist
    // returns nothing, they must come from the dedicated /Artists endpoint (issue #11).
    if (q.kind == media::MediaKind::Artist) {
        HTTP::Form form = {
            {"ParentId", sectionId},
            {"UserId", userId()},
            {"SortBy", "SortName"},
            {"SortOrder", q.descending ? "Descending" : "Ascending"},
            {"StartIndex", std::to_string(start)},
            {"Limit", std::to_string(size)},
            {"Fields", itemFieldsValue},
        };
        std::string url = base() + "/Artists?" + HTTP::encode_form(form);
        fetchContainer<media::Item>(url, parseItem, then, error);
        return;
    }
    HTTP::Form form = {
        {"ParentId", sectionId},
        {"Recursive", "true"},
        {"IncludeItemTypes", kindTypes(q.kind)},
        {"SortBy", mapSort(q.sortField)},
        {"SortOrder", q.descending ? "Descending" : "Ascending"},
        {"StartIndex", std::to_string(start)},
        {"Limit", std::to_string(size)},
        {"Fields", itemFieldsValue},
    };
    if (q.unwatchedOnly) form["Filters"] = "IsUnplayed";
    if (!q.genreId.empty()) form["GenreIds"] = q.genreId;
    std::string url = base() + fmt::format(fmt::runtime(apiItems), userId(), HTTP::encode_form(form));
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::getArtistTracks(
    const std::string& artistId, media::Then<media::Container<media::Item>> then, media::OnError error) {
    // all tracks of an artist, flattened across albums (issue #11)
    HTTP::Form form = {
        {"ArtistIds", artistId},
        {"IncludeItemTypes", "Audio"},
        {"Recursive", "true"},
        {"UserId", userId()},
        {"SortBy", "Album,ParentIndexNumber,IndexNumber"},
        {"Fields", itemFieldsValue},
    };
    std::string url = base() + fmt::format(fmt::runtime(apiItems), userId(), HTTP::encode_form(form));
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::getArtistAlbums(
    const std::string& artistId, media::Then<media::Container<media::Item>> then, media::OnError error) {
    // artist -> albums: Jellyfin artists are virtual, so filter albums by
    // ArtistIds rather than folder ParentId (which returns nothing).
    HTTP::Form form = {
        {"ArtistIds", artistId},
        {"IncludeItemTypes", "MusicAlbum"},
        {"Recursive", "true"},
        {"UserId", userId()},
        {"SortBy", "PremiereDate,ProductionYear,SortName"},
        {"Fields", itemFieldsValue},
    };
    std::string url = base() + fmt::format(fmt::runtime(apiItems), userId(), HTTP::encode_form(form));
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::getCollectionChildren(const std::string& collectionId, size_t start, size_t size,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    HTTP::Form form = {
        {"ParentId", collectionId},
        {"StartIndex", std::to_string(start)},
        {"Limit", std::to_string(size)},
        {"Fields", itemFieldsValue},
    };
    std::string url = base() + fmt::format(fmt::runtime(apiItems), userId(), HTTP::encode_form(form));
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::getHubPage(const std::string& hubKey, size_t start, size_t size,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    // Jellyfin hubs carry no "more" key; treat hubKey as a relative path with pagination.
    std::string sep = hubKey.find('?') == std::string::npos ? "?" : "&";
    std::string url =
        base() + hubKey + sep + fmt::format("StartIndex={}&Limit={}&Fields={}", start, size, itemFieldsValue);
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::getItemDetail(
    const std::string& id, bool, media::Then<media::Item> then, media::OnError error) {
    std::string url = base() + fmt::format(fmt::runtime(apiItem), userId(), id);
    fetchItem(url, then, error);
}

media::Section JellyfinBackend::sectionOfSync(const std::string& id) {
    // the library is the CollectionFolder ancestor, with the same Id as its
    // entry in /Users/{id}/Views
    auto j = getSync(base() + fmt::format(fmt::runtime(apiAncestors), id, userId()), token());
    if (j.is_array())
        for (auto& a : j)
            if (jstr(a, "Type") == "CollectionFolder") return parseSection(a);
    brls::Logger::warning("JellyfinBackend: no library for {}", id);
    return {};
}

std::optional<media::Item> JellyfinBackend::getItemDetailSync(const std::string& id) {
    media::Item it = parseItem(getSync(base() + fmt::format(fmt::runtime(apiItem), userId(), id), token()));
    if (it.ratingKey.empty()) return std::nullopt;
    return it;
}

namespace {
std::string childrenUrl(const std::string& id) {
    // direct children of any item (a show -> its seasons; a season -> its episodes)
    HTTP::Form form = {{"ParentId", id}, {"UserId", userId()}, {"Fields", itemFieldsValue}};
    return base() + fmt::format(fmt::runtime(apiItems), userId(), HTTP::encode_form(form));
}
}  // namespace

void JellyfinBackend::getChildren(
    const std::string& id, media::Then<media::Container<media::Item>> then, media::OnError error) {
    fetchContainer<media::Item>(childrenUrl(id), parseItem, then, error);
}

std::vector<media::Item> JellyfinBackend::getChildrenSync(const std::string& id) {
    return parseContainer<media::Item>(getSync(childrenUrl(id), token()), parseItem).Items;
}

void JellyfinBackend::getAllEpisodes(const std::string& showId, bool,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    HTTP::Form form = {{"userId", userId()}, {"Fields", itemFieldsValue}};
    std::string url = base() + fmt::format(fmt::runtime(apiEpisodes), showId, HTTP::encode_form(form));
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::getNextUp(
    const std::string& showId, std::function<void(media::Item, bool)> then, media::OnError) {
    HTTP::Form nextForm = {{"userId", userId()}, {"seriesId", showId}, {"Limit", "1"}, {"Fields", itemFieldsValue}};
    std::string nextUrl = base() + fmt::format(fmt::runtime(apiNextUp), HTTP::encode_form(nextForm));
    HTTP::Form firstForm = {{"userId", userId()}, {"Limit", "1"}, {"Fields", itemFieldsValue}};
    std::string firstUrl = base() + fmt::format(fmt::runtime(apiEpisodes), showId, HTTP::encode_form(firstForm));
    std::string tok = token();
    brls::async([nextUrl, firstUrl, tok, then]() {
        media::Item item;
        bool fromStart = false;
        try {
            auto next = parseContainer<media::Item>(getSync(nextUrl, tok), parseItem);
            if (!next.Items.empty()) {
                item = next.Items.front();
            } else {
                auto first = parseContainer<media::Item>(getSync(firstUrl, tok), parseItem);
                if (!first.Items.empty()) {
                    item = first.Items.front();
                    fromStart = true;
                }
            }
        } catch (const std::exception& ex) {
            brls::Logger::warning("jellyfin getNextUp: {}", ex.what());
        }
        brls::sync([item, fromStart, then]() { then(item, fromStart); });
    });
}

void JellyfinBackend::getExtras(
    const std::string& id, media::Then<media::Container<media::Item>> then, media::OnError error) {
    std::string url = base() + fmt::format(fmt::runtime(apiSpecialFeatures), userId(), id);
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::getRelated(
    const std::string& id, int count, media::Then<media::Container<media::Hub>> then, media::OnError error) {
    HTTP::Form form = {{"userId", userId()}, {"limit", std::to_string(count)}, {"Fields", itemFieldsValue}};
    std::string url = base() + fmt::format(fmt::runtime(apiSimilar), id, HTTP::encode_form(form));
    fetchHub(url, "main/media/similar"_i18n, "related", then, error);
}

void JellyfinBackend::getPersonMedia(const std::string& personId, int count,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    HTTP::Form form = {
        {"PersonIds", personId},
        {"Recursive", "true"},
        {"IncludeItemTypes", "Movie,Series"},
        {"Limit", std::to_string(count)},
        {"Fields", itemFieldsValue},
    };
    std::string url = base() + fmt::format(fmt::runtime(apiItems), userId(), HTTP::encode_form(form));
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::search(const std::string& query, media::MediaKind kind, int limit,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    auto allow = AppConfig::instance().allowedLibraries();
    if (!allow) {
        HTTP::Form form = {
            {"searchTerm", query},
            {"Recursive", "true"},
            {"IncludeItemTypes", kindTypes(kind)},
            {"Limit", std::to_string(limit)},
            {"Fields", itemFieldsValue},
        };
        std::string url = base() + fmt::format(fmt::runtime(apiItems), userId(), HTTP::encode_form(form));
        fetchContainer<media::Item>(url, parseItem, then, error);
        return;
    }

    // WAN allow-list active: one query per visible library. A single response
    // cannot be split by library afterwards — an episode's ParentId is its season,
    // not the library it lives in — so asking per library is the exact filter.
    std::string b = base(), tok = token(), uid = userId();
    std::string types = kindTypes(kind);
    brls::async([b, tok, uid, query, types, limit, allow, then, error]() {
        try {
            media::Container<media::Item> out;
            for (auto& v : visibleLibraries(b, tok, uid, allow)) {
                int remaining = limit - (int)out.Items.size();
                if (remaining <= 0) break;
                HTTP::Form form = {
                    {"searchTerm", query},
                    {"Recursive", "true"},
                    {"ParentId", v.key},
                    {"IncludeItemTypes", types},
                    {"Limit", std::to_string(remaining)},
                    {"Fields", itemFieldsValue},
                };
                auto c = parseContainer<media::Item>(
                    getSync(b + fmt::format(fmt::runtime(apiItems), uid, HTTP::encode_form(form)), tok), parseItem);
                for (auto& it : c.Items) out.Items.push_back(std::move(it));
            }
            out.TotalRecordCount = (long)out.Items.size();
            brls::sync(std::bind(then, std::move(out)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void JellyfinBackend::getRecentlyAdded(
    size_t start, size_t size, media::Then<media::Container<media::Item>> then, media::OnError error) {
    auto allow = AppConfig::instance().allowedLibraries();
    if (!allow) {
        HTTP::Form form = {{"Limit", std::to_string(size)}, {"Fields", itemFieldsValue}};
        std::string url = base() + fmt::format(fmt::runtime(apiLatest), userId(), HTTP::encode_form(form));
        fetchContainer<media::Item>(url, parseItem, then, error);
        return;
    }

    // Filtered: merged across the visible libraries (Latest has no cross-library
    // filter). `start` indexes the server's global ordering, which a merge cannot
    // reproduce; callers ask for the first page, so the list is truncated to
    // `size` instead.
    (void)start;
    std::string b = base(), tok = token(), uid = userId();
    size_t want = size;
    brls::async([b, tok, uid, want, allow, then, error]() {
        try {
            media::Container<media::Item> out;
            for (auto& v : visibleLibraries(b, tok, uid, allow)) {
                if (out.Items.size() >= want) break;
                HTTP::Form form = {
                    {"ParentId", v.key},
                    {"Limit", std::to_string(want - out.Items.size())},
                    {"Fields", itemFieldsValue},
                };
                auto c = parseContainer<media::Item>(
                    getSync(b + fmt::format(fmt::runtime(apiLatest), uid, HTTP::encode_form(form)), tok), parseItem);
                for (auto& it : c.Items) out.Items.push_back(std::move(it));
            }
            out.TotalRecordCount = (long)out.Items.size();
            brls::sync(std::bind(then, std::move(out)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void JellyfinBackend::getGenres(const std::string& sectionId, media::MediaKind kind,
    media::Then<media::Container<media::Section>> then, media::OnError error) {
    HTTP::Form form = {
        {"parentId", sectionId},
        {"userId", userId()},
        {"IncludeItemTypes", kindTypes(kind)},
    };
    std::string url = base() + fmt::format(fmt::runtime(apiGenres), HTTP::encode_form(form));
    fetchContainer<media::Section>(url, parseSection, then, error);
}

void JellyfinBackend::getCollections(const std::string& sectionId, size_t start, size_t size,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    HTTP::Form form = {
        {"ParentId", sectionId},
        {"Recursive", "true"},
        {"IncludeItemTypes", "BoxSet"},
        {"StartIndex", std::to_string(start)},
        {"Limit", std::to_string(size)},
        {"Fields", itemFieldsValue},
    };
    std::string url = base() + fmt::format(fmt::runtime(apiItems), userId(), HTTP::encode_form(form));
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::getPlaylists(
    size_t start, size_t size, media::Then<media::Container<media::Item>> then, media::OnError error) {
    HTTP::Form form = {
        {"Recursive", "true"},
        {"IncludeItemTypes", "Playlist"},
        {"StartIndex", std::to_string(start)},
        {"Limit", std::to_string(size)},
        {"Fields", itemFieldsValue},
    };
    std::string url = base() + fmt::format(fmt::runtime(apiItems), userId(), HTTP::encode_form(form));
    fetchContainer<media::Item>(url, parseItem, then, error);
}

void JellyfinBackend::getPlaylistItems(const std::string& playlistId, size_t start, size_t size,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    auto allow = AppConfig::instance().allowedLibraries();
    std::string b = base(), tok = token(), uid = userId();
    if (!allow) {
        HTTP::Form form = {
            {"userId", uid},
            {"StartIndex", std::to_string(start)},
            {"Limit", std::to_string(size)},
            {"Fields", itemFieldsValue},
        };
        std::string url = b + fmt::format(fmt::runtime(apiPlaylistItems), playlistId, HTTP::encode_form(form));
        fetchContainer<media::Item>(url, parseItem, then, error);
        return;
    }

    // A playlist is user-made and spans libraries by nature, and its items
    // endpoint takes no library scope — so the page is filtered afterwards.
    brls::async([b, tok, uid, playlistId, start, size, allow, then, error]() {
        try {
            WanPage page{allow, b, tok, uid, {}};
            auto out = fetchFiltered(page, start, size, [&b, &uid, &playlistId, size](size_t off) {
                HTTP::Form form = {
                    {"userId", uid},
                    {"StartIndex", std::to_string(off)},
                    {"Limit", std::to_string(size)},
                    {"Fields", itemFieldsValue},
                };
                return b + fmt::format(fmt::runtime(apiPlaylistItems), playlistId, HTTP::encode_form(form));
            });
            brls::sync(std::bind(then, std::move(out)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

// ---- item actions --------------------------------------------------------------

void JellyfinBackend::markWatched(const std::string& id) {
    postAction(base() + fmt::format(fmt::runtime(apiPlayedItem), userId(), id));
}

void JellyfinBackend::markUnwatched(const std::string& id) {
    deleteAction(base() + fmt::format(fmt::runtime(apiPlayedItem), userId(), id));
}

// ---- playback ------------------------------------------------------------------

media::PlaybackSource JellyfinBackend::resolvePlayback(
    const media::Item& item, const media::Media& version, const media::PlaybackOptions& opts) {
    std::string tok = token();
    auto extra = [&]() {
        std::stringstream e;
        e << fmt::format("network-timeout={}", HTTP::TIMEOUT / 100);
        if (HTTP::PROXY_STATUS) e << ",http-proxy=\"" << HTTP::PROXY << "\"";
        return e;
    };

    // direct play (static): version.parts[0].key = /{Videos|Audio}/{id}/stream?static=true&...
    // Default for anything with no bitrate cap or when forced (mpv handles
    // mp3/flac/aac natively). Audio with a cap falls through to the audio
    // transcode branch below.
    bool audio = item.type == media::mediaTypeTrack;
    if (opts.bitrateCap <= 0 || opts.forceDirectPlay) {
        std::stringstream e = extra();
        if (opts.seekMs > 0) e << ",start=" << misc::sec2Time(opts.seekMs / 1000);
        // Remote source (.strm -> cloud drive): the server may only serve it
        // through the URL IT hands out in PlaybackInfo. Cloud-drive front-ends
        // (TgtoDrive and the like) rewrite DirectStreamUrl to the CDN's signed
        // link and reject the /Videos/{id}/stream URL built below with a 403.
        if (version.remote) {
            if (!version.sourceId.empty()) {
                const std::string direct = playbackInfoUrl(item.ratingKey, version.sourceId, tok);
                // A DirectStreamUrl on the server's own host IS the proxy: the server
                // pulling the cloud file, i.e. the 20 s HttpClient timeout that
                // produced the HTTP 500. With the .strm target known, the gateway's
                // own URL is the one that plays, so don't take the proxy URL.
                const bool viaServer =
                    !direct.empty() && net::hostFromUrl(direct) == net::hostFromUrl(base());
                if (!direct.empty() && !(viaServer && !version.remotePath.empty())) {
                    brls::Logger::info("jellyfin: remote source, playing the server's DirectStreamUrl");
                    return {direct, e.str(), false, "directplay"};
                }
                if (viaServer)
                    brls::Logger::info("jellyfin: DirectStreamUrl is the server's proxy, prefer the source's own URL");
            }
            // No DirectStreamUrl: the `.strm` target the server reported as the
            // media source's Path is still better than the server's own proxy.
            // /Videos/{id}/stream makes the SERVER fetch the cloud file through
            // itself, and Emby answers HTTP 500 once its 20 s HttpClient timeout
            // expires (seen with a local 115 gateway that needs longer).
            if (!version.remotePath.empty()) {
                brls::Logger::info("jellyfin: remote source, playing the media source's own URL");
                return {version.remotePath, e.str(), false, "directplay"};
            }
            brls::Logger::info("jellyfin: no remote URL, keeping the built stream URL (the server will fetch it)");
        }
        std::string url = withToken(base() + version.parts.front().key, tok);
        return {url, e.str(), false, "directplay"};
    }

    // ---- audio (music) transcode — best-effort, issue #11 --------------------
    // Jellyfin Universal Audio endpoint: accepts mp3 direct, else transcodes to
    // aac in an HLS (ts) stream, capped at MaxStreamingBitrate. Placed before
    // the video master path because audio items must use /Audio/, not /Videos/.
    // UNVERIFIED against a live server; direct play (static) stays the default.
    if (audio) {
        std::stringstream e = extra();
        if (opts.seekMs > 0) e << ",start=" << misc::sec2Time(opts.seekMs / 1000);  // mpv-side seek on HLS
        HTTP::Form aform = {
            {"UserId", userId()},
            {"DeviceId", AppConfig::instance().getDeviceId()},
            {"MaxStreamingBitrate", std::to_string(opts.bitrateCap)},  // bps
            {"Container", "mp3"},                                      // client can direct-play mp3
            {"AudioCodec", "aac"},                                     // transcode target codec
            {"TranscodingContainer", "ts"},
            {"TranscodingProtocol", "hls"},
            {"api_key", tok},
        };
        std::string url = base() + fmt::format(fmt::runtime(apiAudioUniversal), item.ratingKey, HTTP::encode_form(aform));
        return {url, e.str(), true, "transcode"};
    }

    // transcode (best-effort HLS master)
    HTTP::Form form = {
        {"mediaSourceId", version.sourceId.empty() ? item.ratingKey : version.sourceId},
        {"VideoCodec", opts.videoCodec},
        {"AudioCodec", "aac,mp3"},
        {"VideoBitrate", std::to_string(opts.bitrateCap)},
        {"TranscodingMaxAudioChannels", "2"},
        {"PlaySessionId", opts.sessionId},
        {"api_key", tok},
    };
    if (opts.audioStreamId > 0) form["AudioStreamIndex"] = std::to_string(opts.audioStreamId);
    if (opts.subtitleStreamId > 0) {
        form["SubtitleStreamIndex"] = std::to_string(opts.subtitleStreamId);
        form["SubtitleMethod"] = "Encode";  // burn-in
    }
    if (opts.seekMs > 0) form["StartTimeTicks"] = std::to_string(opts.seekMs * TICKS_PER_MS);
    if (opts.maxHeight > 0) {
        form["MaxWidth"] = std::to_string(media::resolutionBoxWidth(opts.maxHeight));
        form["MaxHeight"] = std::to_string(opts.maxHeight);
    }
    std::string url = base() + fmt::format(fmt::runtime(apiVideoMaster), item.ratingKey, HTTP::encode_form(form));
    return {url, extra().str(), true, "transcode"};
}

std::string JellyfinBackend::subtitleSidecarUrl(const std::string& streamKey) const {
    if (streamKey.empty()) return "";
    return withToken(base() + streamKey, token());
}

void JellyfinBackend::reportStart(const std::string& id, int64_t posMs, const std::string& sessionId) {
    // /Sessions/Playing: the server only tracks a playback (and its "now playing"
    // entry) from this call. Progress reports alone still store the position on
    // Stopped, which is why it worked without it — but the session was invisible.
    nlohmann::json body = {
        {"ItemId", id},
        {"PositionTicks", posMs * TICKS_PER_MS},
        {"PlaySessionId", sessionId},
        {"CanSeek", true},
        {"IsPaused", false},
    };
    const std::string url = base() + std::string(apiSessionsPlaying);
    const std::string tok = token();
    const std::string payload = body.dump();
    brls::async([url, tok, payload]() {
        try {
            postSync(url, tok, payload);
        } catch (const std::exception& ex) {
            brls::Logger::warning("jellyfin start: {}", ex.what());
        }
    });
}

void JellyfinBackend::reportProgress(
    const std::string& id, media::PlayState state, int64_t posMs, int64_t durMs, const std::string& sessionId) {
    nlohmann::json body = {
        {"ItemId", id},
        {"PositionTicks", posMs * TICKS_PER_MS},
        {"PlaySessionId", sessionId},
        {"IsPaused", state == media::PlayState::Paused},
    };
    std::string_view ep = state == media::PlayState::Stopped ? apiSessionsStopped : apiSessionsProgress;
    std::string url = base() + std::string(ep);
    std::string tok = token();
    std::string b = body.dump();
    brls::async([url, tok, b]() {
        try {
            postSync(url, tok, b);
        } catch (const std::exception& ex) {
            brls::Logger::warning("jellyfin progress: {}", ex.what());
        }
    });
}

// ---- url helpers ---------------------------------------------------------------

std::string JellyfinBackend::imageUrl(const std::string& path, int width, int height) const {
    if (path.empty()) return "";
    if (path.rfind("http", 0) == 0) return path;  // external (passed through)
    if (path[0] != '/') return "";  // "@host/x" would move the api_key to another host
    std::string url = base() + path;
    std::string sep = url.find('?') == std::string::npos ? "?" : "&";
    // Emby ignores fillWidth/fillHeight and serves the original image.
    if (width > 0) {
        url += sep + "maxWidth=" + std::to_string(width);
        sep = "&";
    }
    if (height > 0) {
        url += sep + "maxHeight=" + std::to_string(height);
        sep = "&";
    }
    url += sep + "api_key=" + token();
    return url;
}

std::string JellyfinBackend::downloadUrl(const std::string& partKey) const {
    // partKey = /Videos/{id}/stream?static=true&mediaSourceId=... (original quality)
    return withToken(base() + partKey, token());
}

std::string JellyfinBackend::offlineFileUrl(const std::string& itemId, const std::string& partKey,
    const std::string& sourceId, bool remote) const {
    // A remote source (.strm on a cloud drive) may only be served through the URL
    // PlaybackInfo hands out: the front-ends that serve it reject the assembled
    // /Videos/{id}/stream this falls back to with a 403. One request is what makes
    // the download possible at all.
    if (remote && !sourceId.empty()) {
        const std::string direct = playbackInfoUrl(itemId, sourceId, token());
        if (!direct.empty()) {
            brls::Logger::info("jellyfin: remote download, using the server's DirectStreamUrl");
            return direct;
        }
        brls::Logger::info("jellyfin: no usable DirectStreamUrl for the download, keeping the built stream URL");
    }
    return this->downloadUrl(partKey);
}

HTTP::Header JellyfinBackend::authHeaders() const { return headers(token()); }

// ---- personal list: Jellyfin/Emby favorites -----------------------------------

bool JellyfinBackend::canList(const media::Item& item) const {
    return item.type == media::mediaTypeMovie || item.type == media::mediaTypeShow ||
           item.type == media::mediaTypeSeason || item.type == media::mediaTypeEpisode ||
           item.type == media::mediaTypeCollection;
}

void JellyfinBackend::listWatchlist(const std::string& sortField, media::MediaKind kind, size_t start, size_t size,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    auto allow = AppConfig::instance().allowedLibraries();
    std::string b = base(), tok = token(), uid = userId();
    std::string sort = mapSort(sortField);
    std::string types = kindTypes(kind);
    if (!allow) {
        HTTP::Form form = {
            {"Filters", "IsFavorite"},
            {"Recursive", "true"},
            {"IncludeItemTypes", types},
            {"SortBy", sort},
            {"StartIndex", std::to_string(start)},
            {"Limit", std::to_string(size)},
            {"Fields", itemFieldsValue},
        };
        std::string url = b + fmt::format(fmt::runtime(apiItems), uid, HTTP::encode_form(form));
        fetchContainer<media::Item>(url, parseItem, then, error);
        return;
    }

    // Favorites are cross-library — `Filters=IsFavorite` has no library scope —
    // so on the metered address the page is filtered afterwards. Keeps the
    // server-side sort and the offsets intact, unlike one query per library.
    brls::async([b, tok, uid, sort, types, start, size, allow, then, error]() {
        try {
            WanPage page{allow, b, tok, uid, {}};
            auto out = fetchFiltered(page, start, size, [&b, &uid, &sort, &types, size](size_t off) {
                HTTP::Form form = {
                    {"Filters", "IsFavorite"},
                    {"Recursive", "true"},
                    {"IncludeItemTypes", types},
                    {"SortBy", sort},
                    {"StartIndex", std::to_string(off)},
                    {"Limit", std::to_string(size)},
                    {"Fields", itemFieldsValue},
                };
                return b + fmt::format(fmt::runtime(apiItems), uid, HTTP::encode_form(form));
            });
            brls::sync(std::bind(then, std::move(out)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void JellyfinBackend::getWatchlistState(const media::Item& item, media::Then<bool> then, media::OnError error) {
    std::string url = base() + fmt::format(fmt::runtime(apiItem), userId(), item.ratingKey);
    std::string tok = token();
    brls::async([url, tok, then, error]() {
        try {
            nlohmann::json j = getSync(url, tok);
            bool fav = false;
            auto ud = j.find("UserData");
            if (ud != j.end() && ud->is_object()) fav = jbool(*ud, "IsFavorite");
            brls::sync([then, fav]() {
                if (then) then(fav);
            });
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void JellyfinBackend::setWatchlisted(
    const media::Item& item, bool add, std::function<void()> then, media::OnError) {
    std::string url = base() + fmt::format(fmt::runtime(apiFavoriteItem), userId(), item.ratingKey);
    if (add)
        postAction(url);
    else
        deleteAction(url);
    if (then) brls::sync(then);
}

}  // namespace jellyfin
