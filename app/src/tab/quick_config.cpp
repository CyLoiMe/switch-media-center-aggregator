#include "tab/quick_config.hpp"

#include <fmt/format.h>

#include "utils/config.hpp"

using namespace brls::literals;

namespace {

std::string kindMark(draft::Kind kind) {
    switch (kind) {
        case draft::Kind::Add: return "+ ";
        case draft::Kind::Edit: return "~ ";
        case draft::Kind::Remove: return "- ";
        default: return "· ";
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
    portal::Portal::instance().close();
}

brls::View* QuickConfig::getDefaultFocus() { return this; }

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
