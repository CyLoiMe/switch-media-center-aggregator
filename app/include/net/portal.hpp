/*
    SMCA — the LAN config portal: one page on the console, one page in a phone
    browser, and nothing else.

    Why it exists: a server URL, an access token and a user agent are miserable to
    type with a stick and an on-screen keyboard, and copy-paste from a phone is
    one tap. What the console cannot offer is a text field; what a phone cannot
    offer is the config file.

    The guarantees this class is built around, in the order they matter:

    1. **It only listens while the page is open.** `open()` is called from the page's
       constructor, `close()` from its destructor, so a page that is popped (B) or
       replaced always stops the listener. There is no path that re-opens it, which
       is the point: a server that can come back by itself is a server that runs
       when the user does not know it does.
    2. **Nothing is written until Apply.** The phone edits a `draft::Draft`; the
       console page shows the pending list; `apply()` is the only writer. Discard
       is then free.
    3. **One device, once.** A six-digit PIN is minted by pressing A on the console
       and is consumed by the first client that presents it. After that, other
       clients get 409 and cannot even submit the PIN, so a script cannot race the
       real device. Five wrong PINs PAUSE the portal instead of rotating the code:
       un-pausing needs a physical press on the console, which is the one thing a
       script cannot do.
    4. **No request thread ever touches the config.** `AppConfig` is written from the
       UI thread only — `save()` has no lock and rewrites the whole file, and every
       page reads `servers`/`users` directly, so a network thread calling into it
       would be a data race by construction. Handlers post a command with
       `brls::sync` and wait, with a timeout, on a value that cannot dangle.

    The active connection is read-only here. Changing it means `resetBackend()` and
    rebuilding the shell — and the shell is the page that hosts this portal.
*/

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "net/http_server.hpp"
#include "utils/config_draft.hpp"

namespace portal {

enum class Phase {
    Idle,     ///< listening, no PIN issued (page just opened, or a session ended)
    Armed,    ///< a PIN is on screen, nobody has claimed it
    Claimed,  ///< one device holds the session
    Paused,   ///< too many wrong PINs: only a press on the console clears this
};

class Portal {
public:
    static Portal& instance();

    /// Bind and listen. Called when the page appears; false when no port could be
    /// bound (8080-8082 all taken), which the page reports instead of showing a
    /// URL nobody can reach.
    bool open();
    void close();
    bool running() const;
    /// "http://192.168.3.7:8080", empty when not running or the console has no IP.
    std::string url() const;
    int port() const;

    /// --- console-facing state (UI thread) --------------------------------
    /// Applies the 5-minute idle timeout before answering, so the page needs no
    /// timer of its own: it calls this every frame.
    Phase phase();
    std::string pin();
    std::string clientIp();
    int failures();
    int attemptsLeft();

    /// A: mint a PIN (from Idle, Paused or Armed — Armed rotates the code).
    void arm();
    /// X while a device holds the session: end it and drop the draft.
    void disconnect();

    /// What Apply would do. UI thread only (the draft lives there).
    std::vector<draft::Change> changes() const;
    bool apply();
    void discard();

private:
    Portal();
    /// At exit, with the page still open: letting a waiting request give up keeps
    /// the listener's teardown from stalling the process for its 5 s budget.
    ~Portal();
    Portal(const Portal&) = delete;
    Portal& operator=(const Portal&) = delete;

    Response handle(const Request& req);
    /// Runs `fn` on the UI thread and waits for it. False when it did not run: the
    /// wait is sliced and gives up as soon as the portal is closing, because
    /// `close()` joins the request thread — a request parked in a 5 s wait for a UI
    /// task that will never run would freeze the console for 5 s.
    bool runOnUi(std::function<void()> fn, int timeoutMs = 5000);
    /// Constant-time check of the session cookie; refreshes the idle timer.
    bool authed(const Request& req);
    /// Must run on the UI thread (the page's A button and the marshalled web call).
    bool applyNow();

    mutable std::mutex mutex;
    /// True from the first line of close() until the listener is gone: it is what
    /// lets a request thread stop waiting for the UI thread that is waiting for it.
    std::atomic<bool> stopping_{false};
    Phase phase_ = Phase::Idle;
    std::string pin_;
    std::string session_;
    std::string clientIp_;
    int failures_ = 0;
    std::chrono::steady_clock::time_point lastSeen_{};
    HttpServer server_;

    /// UI thread only: written by applyNow()/discard()/the marshalled draft calls,
    /// read by the page. Never touched from a request thread.
    draft::Draft draft_;

    /// The `web/*` strings, resolved ONCE on the UI thread (`open()`). Built at
    /// request time they would call brls::getStr from a worker, and getStr's
    /// `currentLocale[pointer]` is a non-const operator[] — on a missing key it
    /// INSERTS a node, i.e. it writes the global i18n table while the UI thread is
    /// reading it. Copying a ready-made object from a request thread only reads.
    nlohmann::json webStrings;
    /// Same reason: the locale is read from the UI thread once, not from a request
    /// thread per call.
    std::string localeText;
    /// The page and its script, read once in `open()`. A request then does no file
    /// I/O at all: one less thing that can fail on a thread with no caller to catch
    /// it, and a page load that costs a string copy.
    std::string pageHtml;
    std::string pageJs;
};

}  // namespace portal
