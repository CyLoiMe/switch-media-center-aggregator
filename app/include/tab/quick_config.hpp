/*
    SMCA — Settings → Quick config: the console half of the LAN portal.

    One page, four states, and nothing that outlives it:

      Idle      the listener is up but no code exists       A: mint a PIN
      Armed     address + PIN on screen, nobody claimed it  A: rotate the PIN
      Claimed   one device is editing                       A: apply, X: disconnect
      Paused    five wrong PINs, the code is dead           A: mint a new one

    B leaves the page, and `~QuickConfig` closes the listener: a portal that could
    survive its own page would be a portal that runs while the user does not know
    it does (utils/net_portal.md, and the reason there is no resume path here).

    The page owns no state — every label is rebuilt from Portal::phase()/pin()/
    changes(), which is why the web side never has to notify it: it polls itself in
    draw(), once per frame, and rebuilds only when the signature changed.
*/

#pragma once

#include <string>

#include <borealis.hpp>

#include "net/portal.hpp"

class QuickConfig : public brls::Box {
public:
    QuickConfig();
    ~QuickConfig() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override;

    /// Registers the page's own B. See the note in quick_config.cpp: it must be the
    /// nearest handler, or B bubbles into the shell's "close the app?" dialog.
    void willAppear(bool resetState = false) override;

    brls::View* getDefaultFocus() override;

    static brls::View* create();

private:
    /// Thin wrappers: an exception escaping an action listener or draw() reaches
    /// std::terminate, i.e. "the software was closed because an error occurred".
    /// A page that can fail must fail into its own status line instead.
    void refresh();
    void primary();    ///< A: arm / apply
    void secondary();  ///< X: disconnect (claimed) or discard the draft

    void refreshImpl();
    void primaryImpl();
    void secondaryImpl();
    bool warned = false;  ///< one log line per page, not one per frame

    BRLS_BIND(brls::Label, labelUrl, "qc/url");
    BRLS_BIND(brls::Label, labelPin, "qc/pin");
    BRLS_BIND(brls::Label, labelHint, "qc/hint");
    BRLS_BIND(brls::Label, labelStatus, "qc/status");
    BRLS_BIND(brls::Label, labelPending, "qc/pending");

    /// phase + pin + ip + pending count: the page rebuilds only when this changes,
    /// so draw() at 60 Hz costs a string compare rather than a layout pass.
    std::string signature;
};
