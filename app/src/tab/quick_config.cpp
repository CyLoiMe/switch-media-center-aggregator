#include "tab/quick_config.hpp"

#include <fmt/format.h>

#include "utils/config.hpp"
#include "view/auto_tab_frame.hpp"

using namespace brls::literals;

namespace {

std::string kindMark(draft::Kind kind) {
    switch (kind) {
        case draft::Kind::Add: return "+ ";
        default: return "· ";  // the two knobs
    }
}

std::string kindLabel(const draft::Change& change) {
    if (change.kind == draft::Kind::UserAgent) return "web/ua_label"_i18n;
    if (change.kind == draft::Kind::Proxy) return "web/proxy_label"_i18n;
    return change.name.empty() ? std::string("?") : change.name;
}

}  // namespace

QuickConfig::QuickConfig() {
    brls::Logger::debug("Tab QuickConfig: create");
    this->inflateFromXMLRes("xml/tabs/quick_config.xml");
    this->setFocusable(true);

    // The listener lives exactly as long as this page: opened here, closed in the
    // destructor. Nothing else calls open(), so the portal cannot come back by
    // itself after the user leaves.
    if (!portal::Portal::instance().open()) {
        this->labelHint->setText("main/setting/quick_config/no_listen"_i18n);
    }

    this->registerAction("hints/ok"_i18n, brls::BUTTON_A, [this](brls::View*) {
        this->primary();
        return true;
    });
    this->registerAction("hints/cancel"_i18n, brls::BUTTON_X, [this](brls::View*) {
        this->secondary();
        return true;
    });
}

QuickConfig::~QuickConfig() {
    brls::Logger::debug("Tab QuickConfig: delete");
    // The destructor runs inside the deletion pass of Application::frame(), and it
    // is implicitly noexcept: anything thrown here (a thread operation, an
    // allocation, a socket layer that decides to throw) is std::terminate, i.e.
    // "the software was closed because an error occurred" on the way out of a
    // settings page. Closing a listener cannot be allowed to fail that way.
    try {
        portal::Portal::instance().close();
    } catch (const std::exception& ex) {
        brls::Logger::error("quick config: stopping the portal failed: {}", ex.what());
    } catch (...) {
        brls::Logger::error("quick config: stopping the portal failed");
    }
    brls::Logger::debug("Tab QuickConfig: deleted");
}

brls::View* QuickConfig::getDefaultFocus() { return this; }

/// B leaves the page — and it has to be THIS page that answers it.
///
/// The registration lives in willAppear(), not in the constructor: pushDetailView()
/// registers its own B on the view and runs before addView(), which is what calls
/// willAppear(). Registering here replaces that action (View::registerAction keys on
/// the button), so the nearest handler for B is guaranteed to be the page.
///
/// Why it matters: when no ancestor of the focused view answers B, the key reaches
/// the AppletFrame, whose handler calls popContentView() — with a single activity
/// on the stack that is the "close the app?" dialog followed by Application::quit().
/// A settings page must never be able to offer that.
void QuickConfig::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    this->registerAction(
        "hints/back"_i18n, brls::BUTTON_B,
        [this](brls::View*) {
            brls::Logger::info("quick config: B, leaving the page");
            if (auto* audio = brls::Application::getAudioPlayer()) audio->play(brls::SOUND_BACK);
            // In the detail stack (the normal case): pop it. Outside the sidebar
            // (presented as an activity of its own): dismiss that activity.
            if (ui::popDetail(this)) return true;
            this->dismiss();
            return true;
        },
        false, true, brls::SOUND_NONE);
}

brls::View* QuickConfig::create() { return new QuickConfig(); }

void QuickConfig::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
    brls::FrameContext* ctx) {
    // The page is the only timer the portal has: this also expires a session that
    // went idle, so a device left in a pocket loses access without a background
    // thread watching the clock.
    this->refresh();
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void QuickConfig::primary() {
    try {
        this->primaryImpl();
    } catch (const std::exception& ex) {
        // The message goes to the log, not to the screen: it is English runtime
        // text (bad_alloc, a device error) and the page is localised.
        brls::Logger::error("quick config: A failed: {}", ex.what());
        this->labelStatus->setText("web/failed"_i18n);
    } catch (...) {
        brls::Logger::error("quick config: A failed");
        this->labelStatus->setText("web/failed"_i18n);
    }
}

void QuickConfig::primaryImpl() {
    auto& p = portal::Portal::instance();
    if (p.phase() == portal::Phase::Claimed) {
        if (p.changes().empty()) {
            this->labelStatus->setText("web/pending_none"_i18n);
        } else if (p.apply()) {
            this->labelStatus->setText("web/saved"_i18n);
            brls::Logger::info("quick config: applied");
        } else {
            // An entry without a login result: the page cannot produce one, so this
            // only happens if a caller skipped /api/draft/login.
            this->labelStatus->setText("web/failed"_i18n);
        }
        this->signature.clear();
        this->refresh();
        return;
    }
    // Idle / Paused / Armed: a (new) code. On the console this IS the deliberate
    // act that makes the portal reachable — which is why a wrong-PIN pause is
    // cleared here and nowhere else.
    p.arm();
    this->labelStatus->setText("");
    this->signature.clear();
    this->refresh();
}

void QuickConfig::secondary() {
    try {
        this->secondaryImpl();
    } catch (const std::exception& ex) {
        brls::Logger::error("quick config: X failed: {}", ex.what());
    } catch (...) {
        brls::Logger::error("quick config: X failed");
    }
}

void QuickConfig::secondaryImpl() {
    auto& p = portal::Portal::instance();
    if (p.phase() == portal::Phase::Claimed) {
        p.disconnect();  // also drops the draft: the device is gone, so is its work
        this->labelStatus->setText("web/disconnected"_i18n);
    } else {
        p.discard();
        this->labelStatus->setText("");
    }
    this->signature.clear();
    this->refresh();
}

void QuickConfig::refresh() {
    try {
        this->refreshImpl();
    } catch (const std::exception& ex) {
        if (this->warned) return;
        this->warned = true;
        brls::Logger::error("quick config: refresh failed: {}", ex.what());
    } catch (...) {
        if (this->warned) return;
        this->warned = true;
        brls::Logger::error("quick config: refresh failed");
    }
}

void QuickConfig::refreshImpl() {
    auto& p = portal::Portal::instance();
    const portal::Phase phase = p.phase();
    const std::string pin = p.pin();
    const std::string ip = p.clientIp();
    const auto changes = p.changes();

    std::string sig = std::to_string(static_cast<int>(phase)) + "|" + pin + "|" + ip + "|" +
                      std::to_string(changes.size()) + "|" + p.url();
    if (sig == this->signature) return;
    this->signature = sig;

    const std::string url = p.url();

    switch (phase) {
        case portal::Phase::Idle:
            this->labelHint->setText("main/setting/quick_config/hint_idle"_i18n);
            this->labelUrl->setText(url);
            this->labelPin->setText("");
            break;
        case portal::Phase::Armed:
            this->labelHint->setText("main/setting/quick_config/hint_armed"_i18n);
            this->labelUrl->setText(url.empty() ? "main/setting/quick_config/no_ip"_i18n : url);
            this->labelPin->setText(pin);
            break;
        case portal::Phase::Claimed:
            this->labelHint->setText(
                fmt::format("{} {}", "main/setting/quick_config/hint_claimed"_i18n, ip));
            this->labelUrl->setText(url);
            // The code is spent: showing it again would only help a passer-by.
            this->labelPin->setText("");
            break;
        case portal::Phase::Paused:
            this->labelHint->setText("main/setting/quick_config/hint_paused"_i18n);
            this->labelUrl->setText(url);
            this->labelPin->setText("");
            break;
    }

    std::string pending = "main/setting/quick_config/pending_title"_i18n;
    if (changes.empty()) {
        pending += "\n" + std::string("web/pending_none"_i18n);
    } else {
        for (const auto& c : changes) pending += "\n" + kindMark(c.kind) + kindLabel(c);
    }
    this->labelPending->setText(pending);
}
