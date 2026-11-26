// Standalone logic test — the LAN portal's draft: what it normalises and what it
// would apply. See utils/config_draft.hpp.

#include <cstdio>
#include <string>

#include <utils/config_draft.hpp>

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__);  \
            ++failures;                                      \
        }                                                    \
    } while (0)

int main() {
    // --- addresses typed on a phone -----------------------------------------
    {
        // no scheme is the common case: a phone keyboard does not offer "://"
        CHECK(draft::normalizeUrl("192.168.3.10:8096") == "http://192.168.3.10:8096");
        CHECK(draft::normalizeUrl("  http://host:8096/  ") == "http://host:8096");
        CHECK(draft::normalizeUrl("https://media.example.com") == "https://media.example.com");
        CHECK(draft::normalizeUrl("http://host:8096/emby") == "http://host:8096/emby");
        // wrong scheme, no host, or anything with a space is refused instead of
        // being coerced into a URL that cannot work
        CHECK(draft::normalizeUrl("ftp://host").empty());
        CHECK(draft::normalizeUrl("http://").empty());
        CHECK(draft::normalizeUrl("http:///").empty());
        CHECK(draft::normalizeUrl("").empty());
        CHECK(draft::normalizeUrl("   ").empty());
        CHECK(draft::normalizeUrl("host name:8096").empty());
        CHECK(draft::normalizeUrl("http://host:8096/a b").empty());
    }

    // --- proxy --------------------------------------------------------------
    {
        CHECK(draft::normalizeProxy("192.168.3.244:17890") == "http://192.168.3.244:17890");
        CHECK(draft::normalizeProxy("http://192.168.3.244:17890") == "http://192.168.3.244:17890");
        CHECK(draft::normalizeProxy("").empty());       // empty = no proxy
        CHECK(draft::normalizeProxy("   ").empty());
    }

    // --- user agent ---------------------------------------------------------
    {
        CHECK(draft::normalizeUserAgent("  Mozilla/5.0 (X)  ") == "Mozilla/5.0 (X)");
        CHECK(draft::normalizeUserAgent("Mozilla/5.0\r\nX-Evil: 1").empty());  // header splitter
        CHECK(draft::normalizeUserAgent(std::string(600, 'a')).empty());       // longer than any UA
        CHECK(draft::normalizeUserAgent("").empty());
    }

    // --- the pending list ---------------------------------------------------
    {
        draft::Draft d;
        CHECK(d.empty());
        CHECK(d.changes().empty());

        d.uaSet = true;
        d.ua = "UA";
        CHECK(d.changes().size() == 1);
        CHECK(d.changes()[0].kind == draft::Kind::UserAgent);

        d.proxyCleared = true;
        auto changes = d.changes();
        CHECK(changes.size() == 2);
        CHECK(changes[1].kind == draft::Kind::Proxy);

        draft::Connection added;
        added.name = "客厅2";
        added.url = "http://192.168.3.12:8096";
        added.token = "t";
        d.upsert(added);

        draft::Connection edited;
        edited.id = "srv-1";
        edited.user_id = "user-1";
        edited.name = "旧 NAS";
        edited.token = "t";
        d.upsert(edited);

        changes = d.changes();
        CHECK(changes.size() == 4);
        CHECK(changes[2].kind == draft::Kind::Add);
        CHECK(changes[2].name == "客厅2");
        CHECK(changes[3].kind == draft::Kind::Edit);
        CHECK(changes[3].name == "旧 NAS");

        // editing the same id replaces instead of appending
        draft::Connection again = edited;
        again.name = "旧 NAS 改名";
        d.upsert(again);
        CHECK(d.changes().size() == 4);

        // a connection that only ever existed in the draft is dropped outright
        CHECK(d.remove(""));
        CHECK(!d.remove("srv-does-not-exist"));

        // an applied connection is marked for deletion instead
        draft::Connection removal;
        removal.id = "srv-1";
        removal.user_id = "user-1";
        removal.name = "旧 NAS 改名";
        d.upsert(removal);
        CHECK(d.remove("srv-1"));
        CHECK(d.find("srv-1") != nullptr);
        CHECK(d.find("srv-1")->removal);
        CHECK(d.changes().back().kind == draft::Kind::Remove);
    }

    // --- ready(): nothing may be applied before its login succeeded ---------
    {
        // The page never posts an unresolved entry: "add" runs the login first and
        // carries the ids the server returned. So an entry without a token can
        // only come from a caller that skipped the login.
        draft::Draft d;
        draft::Connection c;
        c.id = "srv-new";
        c.name = "新服务器";
        c.url = "http://host:8096";
        d.upsert(c);
        CHECK(!d.ready());              // no token yet

        draft::Connection done = c;
        done.token = "abc";
        done.user_id = "u1";
        d.upsert(done);
        CHECK(d.changes().size() == 1);  // replaced, not appended
        CHECK(d.ready());

        draft::Connection rm;
        rm.id = "old";
        rm.user_id = "u-old";
        rm.removal = true;
        d.upsert(rm);
        CHECK(d.ready());               // a removal needs no token
    }

    if (failures == 0) printf("test_config_draft: ok\n");
    return failures == 0 ? 0 : 1;
}
