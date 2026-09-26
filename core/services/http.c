#include "http.h"
#include "config.h"
#include "sockets_compat.h"
#include "journal.h"
#include "time_sync.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>

/* The process-wide DNS + TLS-session cache; see `shadow_curl_apply_share`. */
static CURLSH *g_share = NULL;

/* === WHICH CERTIFICATE STORE, AND WHAT THE WRONG ONE COSTS ============
 *
 * Measured on console 2026-09-13, timestamped curl trace, handshake to
 * tinag.shadow.tech:
 *
 *     [curl   48.7] SSL Trust Anchors:
 *     [curl  634.1]   CAfile: vs0:data/external/cert/CA_LIST.cer     <- 585 ms
 *     [curl  651.1] TLS handshake, Certificate (11):
 *     [curl 1344.5] TLS handshake, Server key exchange (12):          <- 693 ms
 *
 * 585 ms to LOAD the console's system store -- hundreds of roots, re-read and
 * parsed ON EVERY CONNECTION -- then 693 ms to verify the chain against it.
 * 1278 of the 1500 ms a handshake costs, and not one millisecond of crypto:
 * the offline bench had already shown computation accounts for about fifty
 * (modexp 5.9 ms, P-256 point 12.9 ms).
 *
 * The fix was already written -- a TWO-certificate bundle lives in our
 * resources -- but locked inside an `#ifdef __SWITCH__`, written when the
 * Switch was the only console. So the Vita fell through to curl's default,
 * which is the system store.
 *
 * The bundle carries ISRG Root X1, which is exactly the root of the Let's
 * Encrypt chains every Shadow endpoint presents. Narrowing the store disables
 * nothing: verification stays ON, against fewer roots but the right ones.
 * `SHADOW_CA_FILE` imposes a different one. */
#if defined(__SWITCH__)
#  define SHADOW_CA_DEFAULT "romfs:/cacert.pem"
#elif defined(__vita__) || defined(__psp2__)
#  define SHADOW_CA_DEFAULT "app0:resources/cacert.pem"
#else
#  define SHADOW_CA_DEFAULT NULL      /* le systeme sait, sur un bureau */
#endif

/* === SEEING INSIDE THE HANDSHAKE =====================================
 *
 * The guard was `#ifdef __SWITCH__` -- written when the Switch was the only
 * console, and wrong the day another one needed it. On Vita a TLS handshake
 * costs 1500 ms, of which the crypto bench showed only ~50 are computation
 * (modexp 5.9 ms, P-256 point 12.9 ms, RAND 0.06 ms). That leaves 1400 ms that
 * are neither computation nor entropy, and curl's own timings stop at the
 * phase boundaries: they say THAT TLS is slow, never WHERE.
 *
 * curl's verbose lines, TIMESTAMPED, do say: every handshake message appears
 * in them, so the gap between two lines names the wait.
 * `SHADOW_HTTP_TRACE=1` enables it off the Switch -- behind a toggle, because
 * a verbose trace on every request is expensive and only a campaign wants
 * it. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
/* S5 2026-08-22 - libcurl's internal messages (including those from the libnx
 * TLS backend: "Error reading/importing ca cert file %s - libnx: 0x%X") go
 * NEITHER through the return code NOR through stderr: only verbose mode exposes
 * them. Without them we cannot tell "bundle not read", "bundle refused" and
 * "root missing" apart. So we copy them into stderr.log, capped so they do not
 * drown the file. */
static int shadow_curl_debug_cb(CURL *h, curl_infotype type, char *data,
                                size_t size, void *user)
{
    (void)h; (void)user;
    /* S7 2026-08-22 - NO MORE stdio HERE.
     * This probe (S5) crashed the console: an `fprintf` called from an INTERNAL
     * curl callback (Curl_infof <- Curl_resolv_timeout <- Curl_connect) went
     * into a `_write_r` with a null newlib structure (Data Abort, faulting
     * address 0x0). The execution context of these callbacks does not guarantee
     * stdio reentrancy on HOS - the same family of trap as "no TLS on a thread
     * created by an external library" (KB §7). The probe did its job (it proved
     * DNS and TCP went through, and so cleared the network): we keep it disabled
     * by default and no longer write from this callback.
     * `SHADOW_CURL_VERBOSE=1` re-enables it - to be used on the desktop only. */
    /* === ON A CONSOLE, THE TOGGLE IS NOT ENOUGH ========================
     *
     * S7, just above, records that this `fprintf` from an INTERNAL curl
     * callback has already taken a console down, and concludes "desktop only".
     * That callback was widened to the PS Vita on 2026-09-13 with
     * SHADOW_CURL_VERBOSE=1 left on the card, and the console crashed 153 s
     * later in `_free_r`: corrupted heap, a block header holding a POINTER
     * where a size belongs. The comment was right and it was read afterwards.
     *
     * An instruction in a comment is not a protection. A condition is: on
     * console the toggle alone no longer does it -- SHADOW_CURL_VERBOSE_CONSOLE
     * is required too, and it appears in no procedure, so it will never be set
     * out of habit. The trace stays available for a campaign (it is what found
     * the certificate store being re-read on every connection), but no longer
     * through a file forgotten on the card. */
    static int chk = 0, on = 0;
    if (!chk) {
        chk = 1;
        on = getenv("SHADOW_CURL_VERBOSE") != NULL;
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
        if (on && !getenv("SHADOW_CURL_VERBOSE_CONSOLE")) on = 0;
#endif
    }
    if (!on) return 0;
    static int lines = 0;
    if (lines++ > 200) return 0;
    /* THE RAW READS TOO, with their size. Handshake messages from one and the
     * same flight are 100 to 680 ms apart, and the bench ruled out computation
     * (a 4096-bit modexp costs 74 ms, the whole chain ~100). What is left to
     * find out is whether the bytes ARRIVE slowly: if each read returns a few
     * hundred bytes one round trip apart, it is the transport, not us. One line
     * per read, size included, says so. */
    if (type == CURLINFO_SSL_DATA_IN || type == CURLINFO_SSL_DATA_OUT) {
        struct timespec tr;
        clock_gettime(CLOCK_MONOTONIC, &tr);
        static double r0 = -1.0;
        const double rn = (double)tr.tv_sec * 1000.0 + (double)tr.tv_nsec / 1e6;
        if (r0 < 0.0) r0 = rn;
        fprintf(stderr, "[curl %7.1f] TLS %s %u o\n", rn - r0,
                type == CURLINFO_SSL_DATA_IN ? "IN " : "OUT", (unsigned)size);
        return 0;
    }
    if (type != CURLINFO_TEXT) return 0;
    /* TIMESTAMPED, or the trace is useless here: what is being looked for is
     * not WHAT curl says but WHEN -- the gap between two handshake messages
     * names the wait. Time is relative to the first call, which gives readable
     * numbers where an absolute clock forces mental subtraction. */
    static double t0 = -1.0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const double now = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
    if (t0 < 0.0) t0 = now;
    fprintf(stderr, "[curl %7.1f] %.*s", now - t0, (int)size, data);
    return 0;
}
#endif

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    http_response *r = (http_response *)userdata;
    size_t n = size * nmemb;
    char *grown = realloc(r->data, r->len + n + 1);
    if (!grown) return 0;  // signals curl error
    r->data = grown;
    memcpy(r->data + r->len, ptr, n);
    r->len += n;
    r->data[r->len] = '\0';
    return n;
}

// Sniff the Date: response header on every HTTP call to keep the synced clock
// in step with Shadow servers. Switch RTC drifts by minutes/hours and Shadow
// rejects stale timestamps in its data-channel anti-replay.
static size_t header_cb(char *buf, size_t size, size_t nmemb, void *userdata) {
    (void)userdata;
    size_t total = size * nmemb;
    if (total > 6 && strncasecmp(buf, "Date: ", 6) == 0) {
        time_sync_observe_http_date(buf + 6);
    }
    return total;
}

bool http_global_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == 0;
}

void http_global_cleanup(void) {
    curl_global_cleanup();
}

void http_free(http_response *r) {
    if (!r) return;
    free(r->data);
    r->data = NULL;
    r->len = 0;
    r->status = 0;
}

/* === AF3 2026-09-10 - A REQUEST IN FLIGHT CAN BE ABANDONED AT EXIT ===
 *
 * Every request carries CURLOPT_TIMEOUT = 30 s (config.h), and nothing could
 * cut one short. At exit, `Threading::stop()` joins the thread that runs our
 * tasks: a boot request stuck on a server that accepts and then goes quiet
 * held the exit 30 s per request - ~3 min for the chained TINAG and OIDC
 * retries - behind a window that answers nothing on desktop. The remedy
 * already existed in this repo for the SSE (`sse_abort_cb`, proximus.c); this
 * is the same callback, for every request.
 *
 * A GRACE, not an immediate abort. The exit event fires while the last
 * requests are still legitimately running - the DELETE of our two clients
 * above all (~100 ms each, measured 2026-09-10). Aborting at once would leave
 * them on the server; waiting 30 s is what this fixes. libcurl calls the
 * callback at least once a second, so the worst exit is now grace + ~1 s. */
#define HTTP_SHUTDOWN_GRACE_MS 1500L
static volatile int    g_http_shutdown = 0;
static struct timespec g_http_shutdown_t0;

void http_request_shutdown(void) {
    if (g_http_shutdown) return;
    clock_gettime(CLOCK_MONOTONIC, &g_http_shutdown_t0);
    g_http_shutdown = 1;
}

static int http_abort_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                         curl_off_t ultotal, curl_off_t ulnow) {
    (void)userdata; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    if (!g_http_shutdown) return 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (long)(now.tv_sec - g_http_shutdown_t0.tv_sec) * 1000L
            + (now.tv_nsec - g_http_shutdown_t0.tv_nsec) / 1000000L;
    return ms >= HTTP_SHUTDOWN_GRACE_MS ? 1 : 0;   /* non-zero = abort */
}

/* === WHERE A REQUEST'S TIME ACTUALLY GOES ==============================
 *
 * "The connection is 10x slower than on the Switch" was measured as 27 seconds
 * between the connecting screen and the control channel, 19 of them with
 * nothing logged. My first answer was to share the TLS sessions - reasoning
 * that a 444 MHz CPU pays dearly for asymmetric crypto. Deployed and measured:
 * 27.4 s. Unchanged. The hypothesis was wrong, and it was a GUESS.
 *
 * curl already knows the answer and splits it four ways, so this prints it
 * rather than reasoning about it: name lookup, TCP connect, TLS handshake, and
 * then the wait for the server's first byte. Whichever of those four holds the
 * seconds is the one to attack - and if it is the last, the time is the
 * SERVER's and there is nothing here to fix. */
static void log_timing(CURL *h, const char *what, int ok)
{
    double dns = 0, conn = 0, tls = 0, first = 0, total = 0;
    /* A request that FAILED has no first byte, so `starttransfer` stays at 0
     * and the segments come out negative - "serveur -24 ms", which is the shape
     * of a reader being told something impossible. A failure has no breakdown
     * to give; it says so instead. */
    if (!ok) {
        curl_easy_getinfo(h, CURLINFO_TOTAL_TIME, &total);
        JOURNAL_INFO_(JOURNAL_CAT_NETWORK,
            "[HTTP] %s : ECHEC apres %.0f ms (pas de decoupage)", what, total * 1000.0);
        return;
    }
    curl_easy_getinfo(h, CURLINFO_NAMELOOKUP_TIME,    &dns);
    curl_easy_getinfo(h, CURLINFO_CONNECT_TIME,       &conn);
    curl_easy_getinfo(h, CURLINFO_APPCONNECT_TIME,    &tls);
    curl_easy_getinfo(h, CURLINFO_STARTTRANSFER_TIME, &first);
    curl_easy_getinfo(h, CURLINFO_TOTAL_TIME,         &total);
    /* Printed as the four SEGMENTS, not as curl's cumulative marks: a reader
     * comparing "connect 1.9" against "appconnect 2.1" has to subtract in their
     * head, and that is where a misreading costs a wrong fix. */
    /* curl leaves a mark at ZERO for a phase that did not happen - a reused
     * connection has no TCP and no TLS - so the segments are computed from the
     * last mark that DID happen. Subtracting blindly printed "tls -23", which
     * is the shape of a reader being told something impossible. */
    const double t_conn  = (conn  > 0) ? conn  : dns;
    const double t_tls   = (tls   > 0) ? tls   : t_conn;
    JOURNAL_INFO_(JOURNAL_CAT_NETWORK,
        "[HTTP] %s : dns %.0f + tcp %.0f + tls %.0f + serveur %.0f = %.0f ms",
        what, dns * 1000.0,
        (conn > 0 ? (conn - dns) : 0.0) * 1000.0,
        (tls  > 0 ? (tls - t_conn) : 0.0) * 1000.0,
        (first - t_tls) * 1000.0, total * 1000.0);
}

/* === ONE TLS HANDSHAKE INSTEAD OF TWELVE ==============================
 *
 * Every REST caller here, in `proximus.c`, `launcher.c` and `oauth.c` does
 * `curl_easy_init()` ... `curl_easy_cleanup()` around a single request. A fresh
 * handle means a fresh DNS lookup and, far more expensively, a FULL TLS
 * handshake with its asymmetric crypto - twelve times over one bootstrap.
 *
 * On a desktop that is a few milliseconds each and nobody ever noticed.
 * Measured on a PS Vita 2026-09-13 - a 444 MHz Cortex-A9 with no crypto
 * acceleration - the phase between "connecting screen built" and the control
 * channel opening took **27 seconds**, 19 of them with nothing logged at all,
 * which is exactly where those handshakes are. The protocol itself was never
 * the slow part: the control channel's round trips measure 26, 178, 25 and
 * 20 ms in the same session.
 *
 * A `CURLSH` shared between handles gives back the two things a fresh handle
 * throws away: the DNS answer and the TLS SESSION, so every request after the
 * first resumes instead of renegotiating.
 *
 * DNS and SSL_SESSION ONLY, deliberately - not CURL_LOCK_DATA_CONNECT. Sharing
 * live connections across handles that run on different threads (the two SSE
 * streams do) carries caveats a slow console is the worst place to discover.
 * The saved TCP handshake is one round trip; the saved TLS handshake is the
 * asymmetric crypto, which is the whole cost here.
 *
 * The lock callbacks are mandatory as soon as the share is touched from more
 * than one thread, and it is. */
static pthread_mutex_t g_share_mtx[CURL_LOCK_DATA_LAST];

static void share_lock_cb(CURL *h, curl_lock_data d, curl_lock_access a, void *u)
{
    (void)h; (void)a; (void)u;
    /* One unsigned comparison covers both ends: `curl_lock_data` is an
     * unsigned enum, so the `>= 0` half this used to carry was unreachable and
     * the compiler said so. What the bound is really for is a value curl adds
     * ABOVE the LAST we compiled against. */
    if ((unsigned)d < (unsigned)CURL_LOCK_DATA_LAST) pthread_mutex_lock(&g_share_mtx[d]);
}
static void share_unlock_cb(CURL *h, curl_lock_data d, void *u)
{
    (void)h; (void)u;
    if ((unsigned)d < (unsigned)CURL_LOCK_DATA_LAST) pthread_mutex_unlock(&g_share_mtx[d]);
}

static void share_init(void)
{
    /* === BOOT-1 2026-09-14 - THE WHOLE SHARE, BEHIND ONE TOGGLE ==========
     *
     * The note below says CONNECT was removed on a strong suspicion that was
     * never isolated ("this is not proof"). The boot now dies INSIDE the third
     * HTTPS request of the process, deterministically, with the second and
     * third going to the same Cloudflare address under different names - and
     * an SSL session cache consulted across handles is the remaining piece of
     * shared state on that path.
     *
     * So the share as a whole gets a switch, and each half gets its own. That
     * is what lets the next arm be chosen by uploading thirty bytes to the
     * card rather than by rebuilding: `SHADOW_HTTP_SHARE=0` removes it
     * entirely, `SHADOW_HTTP_SHARE_SSL=0` keeps only the DNS cache.
     *
     * Defaults are TODAY'S BEHAVIOUR: a toggle is for measuring, and shipping
     * a changed default on a hunch is what this file has already paid for. */
    {
        const char *e = getenv("SHADOW_HTTP_SHARE");
        if (e && !atoi(e)) return;             /* g_share stays NULL: no share */
    }
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
        pthread_mutex_init(&g_share_mtx[i], NULL);
    g_share = curl_share_init();
    if (!g_share) return;
    curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC,   share_lock_cb);
    curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock_cb);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    {
        const char *e = getenv("SHADOW_HTTP_SHARE_SSL");
        if (!e || atoi(e))
            curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
    /* === NOT CURL_LOCK_DATA_CONNECT, AND IT WAS TRIED =================
     *
     * It was added on 2026-09-13: sharing live CONNECTIONS drops the bootstrap
     * from 26.8 to 12.6 s, six requests out of twelve paying for neither TCP
     * nor TLS. The caveat was noted at the time -- "sharing connections
     * between handles running on different threads carries caveats" -- and
     * waved through on the strength of the gain.
     *
     * Two crashes followed, both in `_free_r`, corrupted heap, with a block
     * header holding ASCII TEXT. The second happened with no verbose trace
     * active at all, which clears the lead first suspected. The two SSE streams
     * run on threads of their own and draw from this same cache: a connection
     * handed to two transfers at once is a textbook heap corruption.
     *
     * This is not proof -- the double hand-out was never isolated -- but it is
     * the only change of the day that can produce THAT signature, and a crash
     * costs the whole session where connection reuse costs a few seconds of
     * connecting.
     *
     * DNS and SSL_SESSION stay: they are CACHES, consulted under the lock and
     * never handed out for exclusive use. And most of the day's gain was not
     * there anyway: the system certificate store re-read on every connection
     * weighed 585 ms per handshake, fixed separately.
     *
     * `SHADOW_HTTP_SHARE_CONNECT=1` puts it back, for anyone wanting to
     * measure again. */
    {
        const char *sc = getenv("SHADOW_HTTP_SHARE_CONNECT");
        if (sc && atoi(sc))
            curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    }
}

/* === THE REST SOCKETS' RECEIVE BUFFER =================================
 *
 * Timestamped raw-read trace during the handshake:
 *
 *     [curl  79.4] TLS IN 3410 B     <- the certificate arrives
 *     [curl 759.6] TLS IN    5 B     <- 680 ms later
 *     [curl 779.8] TLS IN  149 B     <- the Server key exchange
 *
 * The server sends Certificate, ServerKeyExchange and ServerHelloDone in the
 * SAME flight. 3410 bytes arrive, then silence. So this is not processing --
 * the offline bench had already ruled that out: a 4096-bit modexp, the size of
 * the chain's RSA root, costs 74 ms, and the whole chain about a hundred. The
 * bytes ARRIVE late.
 *
 * A stall at ~3.4 KB that resolves by itself after hundreds of milliseconds is
 * the signature of a saturated receive window: the stack advertises a tiny
 * window, the server fills it, stops, and waits for an update the console only
 * sends after its delayed-ack timer. N56 established exactly this mechanism on
 * the UDP video side of the other console.
 *
 * So the buffer is enlarged before the connect -- the only moment at which it
 * influences the advertised window. `SHADOW_HTTP_RCVBUF` sets it; at 0 the
 * system default stands, which is the revert path. */
static int shadow_sockopt_cb(void *clientp, curl_socket_t fd, curlsocktype purpose)
{
    (void)clientp;
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_OK;
    static int kb = -1;
    if (kb < 0) {
        const char *e = getenv("SHADOW_HTTP_RCVBUF");
        kb = e ? atoi(e) : 256;          /* Ko */
    }
    if (kb > 0) {
        const int want = kb * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&want, sizeof want);
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&want, sizeof want);
    }
    return CURL_SOCKOPT_OK;
}

void shadow_curl_apply_share(CURL *h)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, share_init);
    if (h && g_share) curl_easy_setopt(h, CURLOPT_SHARE, g_share);
}

static void common_setopts(CURL *h, http_response *out) {
    shadow_curl_apply_share(h);
    curl_easy_setopt(h, CURLOPT_SOCKOPTFUNCTION, shadow_sockopt_cb);
    curl_easy_setopt(h, CURLOPT_USERAGENT, SHADOW_USER_AGENT);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, SHADOW_HTTP_CONNECT_TIMEOUT);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, SHADOW_HTTP_TOTAL_TIMEOUT);
    /* AF3 - see http_abort_cb above. NOPROGRESS=0 is what makes libcurl call
     * the transfer-info callback at all. */
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, http_abort_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, NULL);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, NULL);
    // Force IPv4 - IPv6 is poorly supported by Switch HOS and some mobile
    // networks have broken IPv6. The Shadow servers accept IPv4 (cf. their
    // dual-stack DNS), so we force it for reliability (cf. memory
// project_shadow_real_protocol_confirmed).
    curl_easy_setopt(h, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    // Switch's libcurl uses libnx ssl service which validates against system certs.
    // We keep verification ON by default.
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef __SWITCH__
    /* === S4 2026-08-22 - EMBEDDED CERTIFICATE AUTHORITY STORE (Switch) ===
     *
     * Every Shadow entry point (tinag / api.eu / auth.eu) presents a
     * **Let's Encrypt -> ISRG Root X1/X2** chain. But the console's certificate
     * store is frozen in the firmware and does NOT contain the ISRG roots: the
     * verification therefore failed systematically on the very first call
     * ("SSL peer certificate ... was not OK", CURLE_PEER_FAILED_VERIFICATION),
     * which was wrongly displayed as "DNS or connection blocked".
     * `sslInitialize()` (S2) was necessary but not sufficient: the service
     * opens, only the root is missing.
     *
     * devkitPro's libcurl (the `Curl_ssl_libnx` backend) can import an authority
     * file through CURLOPT_CAINFO (it calls `sslContextImportServerPki`). So we
     * embed the Mozilla bundle in the romfs and point it at that. Verification
     * stays ON - the REST path carries the OAuth tokens, and disabling it would
     * open the door to interception. `SHADOW_CA_FILE` allows pointing at another
     * file (a copy on the SD card, say) without rebuilding.
     *
     * The Vita needs the same thing for a different reason - its system store
     * costs 585 ms to load on every connection - so the store is set for BOTH
     * consoles, from `SHADOW_CA_DEFAULT`, and this `#ifdef __SWITCH__` block
     * keeps only the history above. */
#endif
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
    {
        const char *ca = getenv("SHADOW_CA_FILE");
        curl_easy_setopt(h, CURLOPT_CAINFO, ca ? ca : SHADOW_CA_DEFAULT);
    }
#endif

    if (getenv("SHADOW_CURL_VERBOSE")) {
        /* The callback is CONSOLE-ONLY, and so is this line - it was not, and
         * the desktop build stopped compiling on an undeclared symbol without
         * anyone noticing, because the loop here builds the two consoles and
         * never the desktop. A definition and its use must carry the SAME
         * condition; that they did not is the whole defect.
         *
         * Off console there is nothing to copy: libcurl already writes its
         * verbose output to stderr, which a terminal shows. The callback exists
         * because a console has no terminal - and, per S7, calling it from
         * curl's internals is what took one down. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
        curl_easy_setopt(h, CURLOPT_DEBUGFUNCTION, shadow_curl_debug_cb);
#endif
        curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);
    }
}

bool http_get(const char *url, const char *bearer, http_response *out) {
    out->data = NULL;
    out->len = 0;
    out->status = 0;

    CURL *h = curl_easy_init();
    if (!h) return false;

    struct curl_slist *headers = NULL;
    if (bearer) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "Authorization: Bearer %s", bearer);
        headers = curl_slist_append(headers, buf);
    }
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    common_setopts(h, out);

    CURLcode rc = curl_easy_perform(h);
    log_timing(h, "http_get", rc == CURLE_OK);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->status);
    } else {
        fprintf(stderr, "http_get: curl_easy_perform failed: %s\n", curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return rc == CURLE_OK;
}

bool http_post_json(const char *url, const char *bearer, const char *body, size_t body_len, http_response *out) {
    out->data = NULL;
    out->len = 0;
    out->status = 0;

    CURL *h = curl_easy_init();
    if (!h) return false;

    struct curl_slist *headers = NULL;
    if (bearer) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "Authorization: Bearer %s", bearer);
        headers = curl_slist_append(headers, buf);
    }
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    /* Headers proximus.c always sends and which /N/devices in Spice mode
     * rejects when absent (400 Invalid parameters). */
    headers = curl_slist_append(headers, "X-Shadow-Agent: " SHADOW_X_AGENT);
    headers = curl_slist_append(headers, "Origin: https://pc.shadow.tech");
    headers = curl_slist_append(headers, "Referer: https://pc.shadow.tech/");

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    if (body && body_len > 0) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body_len);
    } else {
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, 0L);
    }
    common_setopts(h, out);

    CURLcode rc = curl_easy_perform(h);
    log_timing(h, "http_post_json", rc == CURLE_OK);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->status);
    } else {
        fprintf(stderr, "http_post_json: curl_easy_perform failed: %s\n", curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return rc == CURLE_OK;
}

bool http_post_form(const char *url, const char *form_body, http_response *out) {
    out->data = NULL;
    out->len = 0;
    out->status = 0;

    CURL *h = curl_easy_init();
    if (!h) return false;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, form_body ? form_body : "");
    common_setopts(h, out);

    CURLcode rc = curl_easy_perform(h);
    log_timing(h, "http_post_form", rc == CURLE_OK);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->status);
    } else {
        fprintf(stderr, "http_post_form: curl_easy_perform failed: %s\n", curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return rc == CURLE_OK;
}

/* UX1 2026-05-18 - Network resilience helpers.
 *
 * http_check_connectivity: a HEAD to 1.1.1.1 with a short timeout. It lets us
 * detect no-internet BEFORE trying the Shadow API (= clean UX).
 */
bool http_check_connectivity(void) {
    CURL *h = curl_easy_init();
    if (!h) return false;
    curl_easy_setopt(h, CURLOPT_URL, "https://1.1.1.1/cdn-cgi/trace");
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L);  /* HEAD request */
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    /* S4: verification stays OFF here on purpose - this check targets a literal
     * IP (1.1.1.1) whose certificate does not carry that name. But it therefore
     * proves NEITHER DNS NOR certificate validation: that is what made the
     * diagnosis misleading ("DNS or connection blocked" when only the ISRG root
     * was missing). The startup error message now says so, and the curl detail
     * goes into stderr.log. */
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);  /* a literal IP: the name is not in the cert */
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(h);
    log_timing(h, "http_put", rc == CURLE_OK);
    curl_easy_cleanup(h);
    return rc == CURLE_OK;
}

/* http_get_retry: exponential backoff retry (2 s, 4 s, 8 s...) on a transport
 * failure. Does NOT retry on an HTTP error status (= 4xx/5xx = the server
 * answered, the caller needs the feedback).
 */
bool http_get_retry(const char *url, const char *bearer, http_response *out, int max_retries) {
    if (max_retries < 1) max_retries = 1;
    if (max_retries > 5) max_retries = 5;
    int delay_s = 2;
    for (int attempt = 0; attempt < max_retries; attempt++) {
        if (attempt > 0) {
            fprintf(stderr, "http_get_retry: attempt %d/%d after %ds delay\n",
                    attempt + 1, max_retries, delay_s);
#ifdef __SWITCH__
            extern void svcSleepThread(long long);
            svcSleepThread((long long)delay_s * 1000000000LL);
#else
            struct timespec ts = {(long)delay_s, 0};
            nanosleep(&ts, NULL);
#endif
            delay_s *= 2;  /* exponential */
        }
        if (http_get(url, bearer, out)) {
            return true;
        }
        if (out) http_free(out);  /* free between retries */
    }
    return false;
}

/* See http.h: the minimal JSON extractor, shared by the REST callers. */
bool http_json_get_string(const char *json, const char *key,
                                  char *out, size_t cap) {
    if (!json || !key || !out || cap == 0) return false;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t')) p++;
    if (*p != '"') return false;
    p++;
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) p += 2;
        else p++;
    }
    size_t len = (size_t)(p - start);
    if (len >= cap) len = cap - 1;
    memcpy(out, start, len);
    out[len] = 0;
    return true;
}
