/*
    SMCA — playback events that outlive a single player.

    A tab is built once and then cached (`AutoSidebarItem::getAttachedView()` returns
    the same view on every re-attach), so whatever it fetched stays as it was until
    the tab is rebuilt or the user presses refresh. The Home "Continue watching" row
    makes that visible: a playback that ended while the row was on screen — or while
    its tab was cached in the background — leaves it stale, and the film just watched
    is missing from it.

    Firing this when a player closes lets the owner of that row refetch just the row,
    whatever tab happens to be on screen.
*/

#pragma once

#include "utils/event.hpp"

/// Fired on the UI thread when a PlayerView is destroyed, after the last progress
/// report. Payload is unused: `("PlaybackEnded", nullptr)`.
inline MPVCustomEvent& playbackEndedEvent() {
    static MPVCustomEvent event;
    return event;
}
