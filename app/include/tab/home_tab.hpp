/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <cstdint>

#include <view/auto_tab_frame.hpp>
#include <view/presenter.hpp>

class RecylingVideo;
class LoadingSpinner;

class HomeTab : public AttachedView, public Presenter {
public:
    HomeTab();
    ~HomeTab() override;

    void onCreate() override;

    void doRequest() override;

    static brls::View* create();

private:
    void doResume(RecylingVideo* row);
    void doHubs();
    void tryRestoreFocus();
    /// Gives the focus back to the first row's first card, retrying for a few frames:
    /// a row that has just been handed a data source has no laid-out cells yet, so a
    /// single attempt lands on the scroll container and the user sees no focus at all.
    void focusRows(int remaining);

    // set when a refresh destroys the focused row (e.g. after closing the
    // player): the first rebuilt row takes the focus back so the user can
    // see where it landed
    bool restoreFocus = false;

    LoadingSpinner* spinner = nullptr;  // centered overlay while hubs load
    /// The "Continue watching" row, kept so a finished playback can refetch just that
    /// row: the tab is cached and never re-requests when it is re-attached, which is
    /// why a film just watched was missing from it (utils/playback_events.hpp).
    RecylingVideo* resumeRow = nullptr;
    /// Bumped by every doResume() call. An answer that belongs to an older request
    /// must not touch the row: doRequest() deletes the rows it replaces, and the
    /// answer for the deleted one used to be written into freed memory (Instruction
    /// Abort at 0 on hardware). The callback compares this before it writes.
    uint32_t resumeGeneration = 0;
    /// Same idea for doHubs(): its answer APPENDS rows, so a rebuild that happens
    /// while the request is in flight would otherwise leave the hubs duplicated on
    /// top of the rebuilt ones.
    uint32_t hubsGeneration = 0;
    MPVCustomEvent::Subscription playbackEndedSub{};
    // offline empty state: added to the tab root (definite height) with the
    // scroll hidden, so it centers — a grow child of the scroll content would
    // be measured with an indefinite height and stay top-aligned
    brls::View* offlineEmpty = nullptr;

    BRLS_BIND(brls::Box, boxHome, "home/box");
    BRLS_BIND(brls::ScrollingFrame, scroll, "home/scroll");
};
