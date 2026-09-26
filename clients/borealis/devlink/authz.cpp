/* authz.cpp - the wiring of authz.h: the stored list, and the question asked.
 *
 * What is DECIDED lives in authz.h and is tested offline. What is here is the
 * file, the lock, and the prompt the owner answers.
 */

#include "authz.hpp"
#include "authz.h"

#include "../../../core/services/config.h"
#include "../../../core/services/atomic_file.h"
#include "../../../core/services/journal.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define azlog(...) JOURNAL_INFO_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)

namespace devlink {
namespace {

std::mutex  g_lock;
std::string g_peer;         /* who is at the other end, "" when nobody */
bool        g_allowed = false;
bool        g_asked   = false;   /* the prompt is up, or has been answered */
bool        g_pending = false;   /* the UI has a question to show */

std::string listPath()
{
    char p[256];
    std::snprintf(p, sizeof p, "%sdevlink_autorises.txt", SHADOW_DATA_DIR);
    return std::string(p);
}

std::string readList()
{
    std::string out;
    FILE *f = atomic_file_open_read(listPath().c_str(), "rb");
    if (!f) return out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

/* Appends one line. Rewriting the whole file would be the tidier scheme, but a
 * decision must never be LOST because a rewrite was interrupted - and this file
 * grows by one line per machine, once. */
void appendLine(const std::string &line)
{
    std::string body = readList();
    if (!body.empty() && body.back() != '\n') body += '\n';
    body += line;
    body += '\n';

    char tmp[256];
    const std::string path = listPath();
    FILE *f = atomic_file_open(path.c_str(), tmp, sizeof tmp);
    if (!f) return;
    std::fwrite(body.data(), 1, body.size(), f);
    atomic_file_commit(f, tmp, path.c_str(), !std::ferror(f));
}

}  // namespace

void offerPeer(const char *peer)
{
    if (!peer) return;
    const size_t n = std::strlen(peer);
    if (!authz_peer_ok(peer, n)) {
        azlog("[authz] unreadable peer, channel refused");
        return;
    }

    /* === NEVER LOG WHILE HOLDING THIS LOCK ===================================
     *
     * `azlog` goes through `journal_write`, which takes the JOURNAL's lock -
     * and the journal, on its own send path, calls `devlink_authz_allowed()`,
     * which takes THIS one. Holding both in opposite orders is a textbook lock
     * inversion, and it is not theoretical: it deadlocked the entire journal,
     * file included, within a second of launch. The application stayed alive
     * and went completely silent, which reads as "logging is broken" rather
     * than "two locks met".
     *
     * So the decision is taken under the lock and the sentence is said after
     * it. Same discipline as the journal's own "no log line here" rule, and as
     * the repo's rule 3: never hold a lock across code you do not control. */
    const char *say = NULL;
    {
        std::lock_guard<std::mutex> v(g_lock);
        if (g_peer == peer && g_asked) return;      /* already settled this session */

        g_peer    = peer;
        g_allowed = false;
        g_asked   = false;
        g_pending = false;

        const std::string body = readList();
        switch (authz_lookup(body.data(), body.size(), peer)) {
        case AUTHZ_ALLOWED: g_allowed = true; g_asked = true; say = "already allowed"; break;
        case AUTHZ_DENIED:                    g_asked = true; say = "already refused";   break;
        default:
            /* The question is not asked from HERE: this runs on the drain
             * thread, and a dialogue must be opened on the UI thread. We only
             * raise the flag; `pendingPeer()` is what the UI polls. */
            g_pending = true;
            say = "is asking to connect - waiting for an answer";
            break;
        }
    }
    azlog("[authz] %s %s", peer, say);
}

bool allowed()
{
    std::lock_guard<std::mutex> v(g_lock);
    return g_allowed;
}

std::string pendingPeer()
{
    std::lock_guard<std::mutex> v(g_lock);
    return g_pending ? g_peer : std::string();
}

void answer(bool yes, bool remember)
{
    std::string peer;
    {
        std::lock_guard<std::mutex> v(g_lock);
        if (!g_pending) return;
        g_pending = false;
        g_asked   = true;
        g_allowed = yes;
        peer      = g_peer;
    }
    /* Outside the lock, for the reason spelled out on `offerPeer`. */
    azlog("[authz] %s: %s%s", peer.c_str(),
          yes ? "allowed" : "refused", remember ? " (retenu)" : " (cette fois)");
    if (remember) appendLine(yes ? peer : ("-" + peer));
}

void forget()
{
    std::lock_guard<std::mutex> v(g_lock);
    g_peer.clear();
    g_allowed = false;
    g_asked   = false;
    g_pending = false;
}

}  // namespace devlink

/* The gate handed to the journal, which owns the socket and must not send a
 * single line before the owner has said yes.
 *
 * It used to be three loose `extern "C"` symbols that `shadow/journal.c`
 * declared by hand. That was a dependency from the core logger to a dev tool
 * that NO include graph could see, and it broke `halyard-cli` -- which
 * compiles no C++ -- silently, until something linked. The journal now declares
 * what it needs (`journal_mirror_gate_t`) and `main.cpp` installs this. */
static int  gate_allowed(void)              { return devlink::allowed() ? 1 : 0; }
static void gate_offer(const char *peer)    { devlink::offerPeer(peer); }
static void gate_forget(void)               { devlink::forget(); }

extern "C" const journal_mirror_gate_t *devlink_journal_gate(void)
{
    /* Static storage: the journal keeps the pointer for the process's life. */
    static const journal_mirror_gate_t gate = { gate_allowed, gate_offer, gate_forget };
    return &gate;
}
