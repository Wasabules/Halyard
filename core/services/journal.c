/* journal - see journal.h for the three axes and the migration strategy.
 *
 * === WHAT COMES FROM `webrtc/log.c`, AND WHY WE KEEP IT ===
 *
 * The write mechanism is not rewritten: it was tuned on console and every detail
 * of it answers a measurement.
 *
 *   - NO `fflush` per line. On Switch a flush is a synchronous SD write of 1 to
 *     5 ms; every thread (session, async work, audio, Borealis) comes through
 *     here at 60 Hz at least, and those milliseconds saturated the lock. A 64 KB
 *     internal buffer (`_IOFBF`) makes `vfprintf` a `memcpy`, and the lock is
 *     held for a few microseconds only.
 *   - ONE drain thread that empties every 500 ms, waking every 100 ms to observe
 *     its abort flag - the granularity HOS imposes, since it kills threads
 *     brutally on `process_exit` and leaks their handles.
 *   - The network channel is NON-BLOCKING, bounded to 300 ms, tried ONCE, and
 *     disabled PERMANENTLY on the first send error: a log that stalls the
 *     application it observes would be worse than no log.
 *
 * === WHAT IS NEW ===
 *
 * The filter, placed BEFORE the formatting (see `journal_write`), the two
 * severity/category columns, and the file names.
 */
#include "journal.h"
#include "atomic_file.h"

#include "config.h"            /* SHADOW_DATA_DIR */
#include "sockets_compat.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>             /* SEC2: isxdigit */
#include <string.h>
#include <sys/stat.h>          /* S86: the size of an archive without opening it */
#include <time.h>


#if defined(_WIN32) && !defined(localtime_r)
/* MinGW UCRT does not expose localtime_r; we go through localtime_s (arguments
 * swapped). */
static inline struct tm *shadow_localtime_r(const time_t *t, struct tm *out) {
    return localtime_s(out, t) == 0 ? out : NULL;
}
#  define localtime_r(t, out) shadow_localtime_r(t, out)
#endif

/* === S81 - THE FILE NAMES ===
 *
 * `webrtc.log` and `webrtc_prev.log` were the names of a module abandoned in
 * May: the WebRTC path is no longer compiled on console, and this log has had
 * nothing to do with WebRTC for a long time. A name that designates something
 * other than its contents is a trap for whoever opens the SD card.
 *
 * `-precedent` rather than `_prev`: this file is read from a file browser, not
 * from a terminal. (S86 replaced that single archive with numbered archives; the
 * `-precedent` name survives only to pick up an SD card written by S81, see
 * `JOURNAL_FILE_S81` -- which the data directory's rename has since made
 * unreachable.) */
#define JOURNAL_FILE       SHADOW_DATA_DIR "halyard.log"

/* === S86 2026-08-29 - ONE FILE PER LAUNCH ===
 *
 * The archives are numbered: `halyard.1.log` is the previous session,
 * `.2.log` the one before. See `journal.h` for the why (the segmentation) and
 * for the `SHADOW_JOURNAL_SESSIONS` toggle.
 *
 * The name is BUILT, never discovered: neither `dirent` nor `glob` is guaranteed
 * on Switch, and a logging module cannot depend on a facility that is missing
 * precisely on the target.
 */
#define JOURNAL_FILE_PATTERN SHADOW_DATA_DIR "halyard.%d.log"

/* S81's single archive, ADOPTED as archive 1 on the first launch rather than
 * left to age beside the numbered scheme with nothing counting or cleaning it.
 *
 * It is now a DEAD path and kept only so the intent is on record: the 2026-09-13
 * rename moved the data directory, so a card written by an older build carries
 * that file under the OLD directory, which this build never opens. Whatever
 * still sits there is orphaned by the move, not by this constant -- and removing
 * the adoption would change nothing except lose the explanation. */
#define JOURNAL_FILE_S81   SHADOW_DATA_DIR "halyard-precedent.log"

static FILE           *g_f = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_drain;
static bool            g_drain_started = false;
static volatile bool   g_drain_stop  = false;

/* S86 - has the rotation already happened in THIS process?
 *
 * PROCESS state, not session state: one launch = one shift, full stop. The flag
 * became indispensable once `g_f` could go back to null mid-run (a purge whose
 * `freopen` fails) - without it, the reopen that follows would restart the shift
 * MID-SESSION, and the current launch's log would be moved to archive 1 while we
 * are writing into it. The repo knows this family: a `static` carrying session
 * state. This one carries none, and that is exactly what makes it correct. */
static bool            g_rotation_done = false;

/* The filter. Two plain integers, read without a lock by `journal_enabled()`:
 * they are written by the main thread (settings) and read by all the others. A
 * read that sees the old value for one frame writes one line too many or one too
 * few - a nil consequence, against a lock taken thousands of times a second on
 * the video path. */
static volatile int      g_threshold = JOURNAL_INFO;
static volatile uint32_t g_cats  = JOURNAL_ALL_CATEGORIES;
static bool              g_env_read = false;
static bool              g_env_forced = false;   /* SHADOW_JOURNAL_NIVEAU pose */

static void (*g_cmd)(const char *) = NULL;

/* AUTH-1's gate, installed by the application (see journal.h). Process-wide
 * configuration set once at startup -- not session state, which is the `static`
 * this repo is right to distrust. NULL = closed. */
static const journal_mirror_gate_t *g_gate = NULL;
static int g_gate_warned = 0;

static int gate_allowed(void)
{
    return (g_gate && g_gate->allowed) ? g_gate->allowed() : 0;
}
static void gate_offer(const char *peer)
{
    if (g_gate && g_gate->offer) { g_gate->offer(peer); return; }
    /* Once per process, and only when a peer was actually reached -- so the
     * line appears exactly when someone is waiting for a mirror that will
     * never come, and never on a build that has no mirror at all.
     * Called from the drain thread, outside the lock: logging is safe here. */
    if (!g_gate_warned) {
        g_gate_warned = 1;
        JOURNAL_WARN_(JOURNAL_CAT_SYSTEM,
                      "[AUTH-1] mirror reached %s but NO authorisation gate is "
                      "installed - nothing will be sent", peer ? peer : "?");
    }
}
static void gate_forget(void)
{
    if (g_gate && g_gate->forget) g_gate->forget();
}
static int  g_sink_fd    = -1;
/* 0 = to be attempted, 1 = connected, 2 = waiting before the next attempt.
 *
 * === DEVL-7 2026-09-12 - THE CHANNEL RECONNECTS ON ITS OWN =================
 *
 * State 2 used to mean "given up, for the whole session". One attempt, at
 * startup, whatever happened. Two consequences paid over and over:
 *
 *   - a listener restarted, a laptop asleep, a Wi-Fi hiccup - and the channel
 *     was gone until the application itself was restarted;
 *   - and the rule everyone had to remember: start `devlink listen` BEFORE the
 *     application, because its single attempt happened at startup. Miss it and
 *     the whole session was mute.
 *
 * Now state 2 is a WAIT, not a grave. The delay doubles from
 * SINK_RETRY_MIN_MS to SINK_RETRY_MAX_MS so a machine that is simply absent
 * costs a connect every 30 s rather than one every tick - the caution the
 * original design was right about, expressed as a pace instead of a surrender.
 *
 * And the attempt now runs on the DRAIN thread, never inside `sink_write`:
 * that one is called from any thread that logs, while holding `g_lock`. A
 * connect there - even bounded to 300 ms - stalls video, audio and the UI. */
static int  g_sink_state  = 0;
static long long g_sink_next_try_ms = 0;   /* when to attempt again */
/* AUTH-1: filled by `sink_open_locked` UNDER the lock, consumed by the drain
 * thread OUTSIDE it - see the comment where it is written. */
static char g_peer_pending[160] = "";
static int  g_sink_backoff_ms = 0;         /* grows, capped */

#define SINK_RETRY_MIN_MS  1000
#define SINK_RETRY_MAX_MS 30000
static char g_stamp[24] = "";

/* --------------------------------------------------------------- noms */

static const char *const CAT_NAMES[JOURNAL_CAT_COUNT] = {
    "heritage", "session", "video", "audio", "input",
    "manette", "reseau", "auth", "ihm", "systeme",
};

const char *journal_category_name(journal_category_t c)
{
    /* One unsigned comparison covers both ends: the `< 0` half is unreachable
     * on an unsigned enum, and whether this enum IS unsigned is the ABI's
     * choice -- which is why it warned on one console and not the other. */
    if ((unsigned)c >= (unsigned)JOURNAL_CAT_COUNT) return "?";
    return CAT_NAMES[(int)c];
}

const char *journal_severity_name(journal_severity_t s)
{
    switch (s) {
        case JOURNAL_ERROR: return "ERROR";
        case JOURNAL_WARN: return "ALERTE";
        case JOURNAL_INFO:   return "INFO";
        case JOURNAL_DEBUG:  return "DEBUG";
        case JOURNAL_TRACE:  return "TRACE";
        default:             return "?";
    }
}

/* A LETTER in the line, not a word: the log is read in columns, and six extra
 * characters per line over several hundred thousand lines push the useful
 * message off the screen. */
static char letter(journal_severity_t s)
{
    switch (s) {
        case JOURNAL_ERROR: return 'E';
        case JOURNAL_WARN: return 'A';
        case JOURNAL_INFO:   return 'I';
        case JOURNAL_DEBUG:  return 'D';
        case JOURNAL_TRACE:  return 'T';
        default:             return '?';
    }
}

/* -------------------------------------------------------------- filtre */

/* `SHADOW_JOURNAL_NIVEAU` is read ONCE, lazily, like every toggle in the repo.
 * When it is set it wins over the UI setting: `env.txt` serves the A/B runs, and
 * a UI setting must not silently override an experiment in progress. */
static void read_env(void)
{
    if (g_env_read) return;
    g_env_read = true;

    const char *e = getenv("SHADOW_JOURNAL_NIVEAU");
    if (e) {
        const int v = atoi(e);
        if (v >= JOURNAL_ERROR && v <= JOURNAL_TRACE) {
            g_threshold = v;
            g_env_forced = true;
        }
    }
    const char *c = getenv("SHADOW_JOURNAL_CATEGORIES");
    if (c) {
        const unsigned long m = strtoul(c, NULL, 0);
        if (m != 0) g_cats = (uint32_t)m;
    }
}

void journal_set_level(journal_severity_t threshold)
{
    read_env();
    if (g_env_forced) return;              /* l'experience en cours gagne */
    /* JOURNAL_ERROR is 0, so the low clamp can only ever fire on a signed
     * enum. Clamping the unsigned value against the high end alone is the
     * same behaviour on both, and says so. */
    if ((unsigned)threshold > (unsigned)JOURNAL_TRACE) threshold = JOURNAL_TRACE;
    g_threshold = (int)threshold;
}

journal_severity_t journal_level(void)
{
    read_env();
    return (journal_severity_t)g_threshold;
}

void journal_set_categories(uint32_t mask)
{
    read_env();
    /* An EMPTY mask would turn everything off, failures included, and would read
     * as an application that no longer logs. We refuse it: it is almost
     * certainly a botched computation in the caller. */
    if (mask == 0) return;
    g_cats = mask;
}

uint32_t journal_categories(void)
{
    read_env();
    return g_cats;
}

bool journal_enabled(journal_severity_t sev, journal_category_t cat)
{
    read_env();
    if ((int)sev > g_threshold) return false;
    /* A failure goes through the mask. Filtering on one subject must not make
     * another one's failure INVISIBLE: it is precisely when you have filtered
     * that you do not think to look elsewhere. */
    if (sev <= JOURNAL_WARN) return true;
    if ((unsigned)cat >= (unsigned)JOURNAL_CAT_COUNT) return true;
    return (g_cats & (1u << (unsigned)cat)) != 0u;
}

/* ------------------------------------------------- S86: the files */

/* Name of the log with index `index`: 0 = the current one, 1..N = the archives.
 * Built, never discovered - see `JOURNAL_FICHIER_MOTIF`. */
static void journal_name(char *dst, size_t n, int index)
{
    if (index <= 0) snprintf(dst, n, "%s", JOURNAL_FILE);
    else            snprintf(dst, n, JOURNAL_FILE_PATTERN, index);
}

/* Size of a file without opening it. `stat` and not `fopen`+`fseek`+`ftell`:
 * the inventory touches up to eleven files, and eleven opens on an SD card would
 * be visible when the settings screen opens. */
static bool file_size(const char *path, uint64_t *bytes)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (bytes) *bytes = (uint64_t)(st.st_size < 0 ? 0 : st.st_size);
    return true;
}

/* Number of archives kept. A `static` caching a `getenv` - the only form of
 * `static` this module allows itself. Bounded on read: an absurd value in
 * `env.txt` must not translate into a loop of renames on the SD card.
 *
 * The lazy initialisation is NOT synchronised, and ThreadSanitizer flags it - as
 * it already flags `read_env()`'s since S81. It is the same trade-off, owned in
 * the same place: two threads entering together compute the SAME value, because
 * it comes from a `getenv` that does not move for the life of the process. What
 * a lock would buy here is formal conformance, against taking a lock on a path
 * called from the UI. To be changed only if `read_env` is synchronised too, and
 * then both at once - fixing them separately would leave the log half safe, which
 * reads as entirely safe. */
static int sessions(void)
{
    static int g_sessions = -1;
    if (g_sessions < 0) {
        const char *e = getenv("SHADOW_JOURNAL_SESSIONS");
        int v = e ? atoi(e) : 3;
        if (v < 0) v = 0;
        if (v > JOURNAL_SESSIONS_MAX) v = JOURNAL_SESSIONS_MAX;
        g_sessions = v;
    }
    return g_sessions;
}

int journal_sessions_kept(void) { return sessions(); }

bool journal_path(int index, char *dst, size_t size)
{
    if (!dst || index < 0 || index > JOURNAL_SESSIONS_MAX) return false;
    char tmp[JOURNAL_PATH_MAX];
    journal_name(tmp, sizeof tmp, index);
    const size_t l = strlen(tmp);
    if (l + 1 > size) return false;
    memcpy(dst, tmp, l + 1);
    return file_size(dst, NULL);
}

/* Shifts the archives by one and makes the ending session archive 1. Assumes
 * `g_verrou` is held, called ONCE per launch.
 *
 * === THE TRAP, AND IT IS SILENT ===
 *
 * The shift goes from the OLDEST towards the most recent. Taken the other way -
 * `.1` to `.2` first, then `.2` to `.3` - every rename overwrites the destination
 * the next one is about to move to, and a single file is left at the end:
 * exactly the defect being fixed, but one that only shows after N launches, the
 * day you go looking for an old session that no longer exists. So we go down from
 * N to 1.
 *
 * The sweep goes up to `JOURNAL_SESSIONS_MAX` and not up to N: lowering
 * `SHADOW_JOURNAL_SESSIONS` must really free the card, not abandon in place files
 * that nothing enumerates or counts any more. */
static void rotate_locked(void)
{
    if (g_rotation_done) return;
    g_rotation_done = true;

    const int n = sessions();
    char a[JOURNAL_PATH_MAX], b[JOURNAL_PATH_MAX];

    /* Picking up an SD card written by S81: its single archive becomes archive
     * 1, and the shift that follows will put it in its real place. Once only -
     * on the next launch the file no longer exists. The guard on the existence
     * of `.1.log` avoids overwriting an already-numbered archive. */
    if (n > 0) {
        journal_name(a, sizeof a, 1);
        if (!file_size(a, NULL) && file_size(JOURNAL_FILE_S81, NULL))
            shadow_file_rename(JOURNAL_FILE_S81, a);
    } else {
        shadow_file_remove(JOURNAL_FILE_S81);
    }

    /* Everything beyond the requested count goes. */
    for (int i = (n > 0 ? n : 1); i <= JOURNAL_SESSIONS_MAX; ++i) {
        journal_name(a, sizeof a, i);
        shadow_file_remove(a);
    }
    /* Then we go down: the oldest first. */
    for (int i = n - 1; i >= 1; --i) {
        journal_name(a, sizeof a, i);
        journal_name(b, sizeof b, i + 1);
        shadow_file_rename(a, b);
    }
    /* And the ending session becomes archive 1. `n == 0` = no archive
     * requested: the caller's `fopen("w")` will truncate the file in place. */
    if (n > 0) {
        journal_name(b, sizeof b, 1);
        shadow_file_rename(JOURNAL_FILE, b);
    }
}

/* Assumes `g_verrou` is held. This line is a file's START MARKER: the analysis
 * tools segment on it, and a log emptied by the user must carry one too -
 * otherwise it starts in the middle of a timeline (the timestamp is relative to
 * process startup, not to the file) with nothing to say so. */
static void header_locked(const char *reason)
{
    if (!g_f) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(g_f,
            "=== halyard - %s %04d-%02d-%02d %02d:%02d:%02d - level %s ===\n",
            reason, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            journal_severity_name(journal_level()));
}

/* ---------------------------------------------------------- fil videur */

static void sink_open_locked(void);
static void sink_schedule_retry(void);

static void *drain(void *arg)
{
    (void)arg;
    /* A 100 ms granularity so `g_videur_stop` is observed quickly on exit. */
    struct timespec ts = { 0, 100 * 1000 * 1000 };
    int ticks = 0;

    while (!g_drain_stop) {
        nanosleep(&ts, NULL);

        /* DEVL-7: the (re)connection lives HERE, on this thread and nowhere
         * else. `sink_open_locked` returns at once when the channel is up or
         * when the wait has not elapsed, so the common cost is one comparison
         * per 100 ms tick.
         *
         * A pleasant consequence, and the reason the old rule can go: the
         * listener no longer has to be started BEFORE the application. Arm it
         * whenever - the next attempt finds it. */
        pthread_mutex_lock(&g_lock);
        sink_open_locked();
        char peer_just_reached[160];
        peer_just_reached[0] = 0;
        if (g_peer_pending[0]) {
            memcpy(peer_just_reached, g_peer_pending, sizeof peer_just_reached);
            g_peer_pending[0] = 0;
        }
        pthread_mutex_unlock(&g_lock);
        /* OUTSIDE the lock: this call logs, and logging takes `g_lock`. */
        if (peer_just_reached[0]) gate_offer(peer_just_reached);

        /* Commands coming from the development machine. A NON-BLOCKING read:
         * this thread also serves to flush the log, and blocking it would
         * deprive the application of its traces exactly when it needs them
         * most. */
        {
            pthread_mutex_lock(&g_lock);
            const int fd = (g_sink_state == 1) ? g_sink_fd : -1;
            pthread_mutex_unlock(&g_lock);
            if (fd >= 0 && g_cmd && gate_allowed()) {
                char buf[256];
                /* AF9 - no toggling any more: the socket is non-blocking for
                 * its whole life. Flipping it here, OUTSIDE `g_lock`, raced a
                 * `send` running under it - O_NONBLOCK belongs to the socket,
                 * so the concurrent send could get EAGAIN and used to take it
                 * as a definitive failure, cutting the mirror mid-burst. */
                const ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
                /* === DEVL-7: THIS is where a loss is noticed =================
                 *
                 * `recv` returning 0 means the peer CLOSED. Waiting for a
                 * `send` to fail instead would never notice on a quiet
                 * application: nothing is sent, so nothing fails, and the
                 * channel stays "connected" to a socket nobody holds. Measured
                 * the day it was written - the listener was restarted and the
                 * mirror never came back, precisely because the application was
                 * idle at that moment.
                 *
                 * A negative return with a real error says the same thing;
                 * EAGAIN is just "nothing to read" on a non-blocking socket and
                 * must not be mistaken for it. */
                if (n == 0) {
                    pthread_mutex_lock(&g_lock);
                    if (g_sink_fd >= 0) { shadow_closesocket(g_sink_fd); g_sink_fd = -1; }
                    sink_schedule_retry();
                    pthread_mutex_unlock(&g_lock);
                    /* AUTH-1: a new connection is a new question. Keeping the
                     * previous yes would let a different machine inherit it
                     * simply by taking the port after the first one left. */
                    gate_forget();
                } else if (n < 0) {
                    const int e = shadow_sock_errno();
                    if (e != EAGAIN && e != EWOULDBLOCK) {
                        pthread_mutex_lock(&g_lock);
                        if (g_sink_fd >= 0) { shadow_closesocket(g_sink_fd); g_sink_fd = -1; }
                        sink_schedule_retry();
                        pthread_mutex_unlock(&g_lock);
                    }
                }
                if (n > 0) {
                    buf[n] = 0;
                    /* Several commands can arrive glued together: TCP is a stream, not
                     * a message carrier. */
                    char *save = NULL;
                    for (char *l = strtok_r(buf, "\r\n", &save); l;
                         l = strtok_r(NULL, "\r\n", &save))
                        if (*l) g_cmd(l);
                }
            }
        }

        if (++ticks >= 5) {                 /* 500 ms */
            ticks = 0;
            pthread_mutex_lock(&g_lock);
            if (g_f) fflush(g_f);
            pthread_mutex_unlock(&g_lock);
        }
    }
    return NULL;
}

/* Suppose `g_verrou` tenu. */
static void open_locked(void)
{
    if (!g_f) {
        /* S86 - one file per launch, N numbered archives. The shift only
         * happens on the FIRST pass (`g_rotation_faite`): this function is also
         * the reopen path after a purge whose `freopen` failed, and replaying the
         * rotation would then put it right in the middle of a session. */
        rotate_locked();
        g_f = fopen(JOURNAL_FILE, "w");
        if (g_f) {
            setvbuf(g_f, NULL, _IOFBF, 64 * 1024);
            header_locked("nouvelle session");
        }
    }
    if (!g_drain_started) {
        /* NO `pthread_detach`: we want to be able to join on exit, so the thread
         * finishes before HOS kills it. */
        if (pthread_create(&g_drain, NULL, drain, NULL) == 0)
            g_drain_started = true;
    }
}

/* ------------------------------------------------------------ caviardage */

/* UX7 B11 - strips the tokens before writing.
 * `SHADOW_LOG_RAW=1` disables it for a local diagnosis.
 *
 * === S81b 2026-08-29 - THE ORIGINAL VERSION LET THE MOST COMMON CASE
 * THROUGH ===
 *
 * It knew only two shapes: `Bearer ey...` and `refresh_token=...`. It therefore
 * did NOT see a token passed as a URL parameter - and that is precisely the shape
 * ours takes: the WebSocket opening request reads
 * `GET /2/signaling?token=eyJ0eXAi...` and was logged IN FULL, 900 characters of
 * valid JWT, into a file that `switch-sync.sh logs` brings back and that we paste
 * into reports.
 *
 * The defect was two years old and visible nowhere. It became visible on the
 * first run of the rewritten log, because the output is finally readable - which
 * is an argument for this kind of work: you do not see what you do not read. Same
 * family as SEC1, where an ed25519 private key had been going into the log for
 * months.
 *
 * `token=` covers `?token=`, `&token=`, `access_token=`, `streaming_token=` and
 * `refresh_token=` with a single pattern. We do NOT try to recognise a JWT by its
 * shape: an opaque token (Shadow's `streamingtoken` is one) has no recognisable
 * shape, and missing it would be exactly the error being fixed. */
static int g_redact_read = 0;
static int g_redact    = 1;

/* === THE LAST LINE OF DEFENCE LIVES IN log_redact.h =====================
 *
 * It used to be four literal markers here - `token=`, `Bearer `,
 * `chacha20_key=`, `auth_hash=`. The audit of 2026-09-13 found five leaks in
 * `smoke_test.c` that every one of them missed, because that file writes
 * `tok=` and its own labels: a marker list is a list of the mistakes you
 * already know about.
 *
 * The rules moved to `log_redact.h` and became rules of SHAPE - a JWT looks
 * like a JWT wherever it is printed - and, being a pure header, they finally
 * have a test (`tests/test_log_redact.c`, 55 checks). Until then the function
 * whose whole job is catching what the call sites missed was itself unverified.
 *
 * Read that header before changing anything here: it explains what is
 * deliberately NOT masked (spaced protocol hex) and why masking it would be a
 * worse failure than the one being defended against. */
#include "log_redact.h"

static void redact(char *buf)
{
    if (!g_redact_read) {
        const char *e = getenv("SHADOW_LOG_RAW");
        g_redact = (e && atoi(e) == 1) ? 0 : 1;
        g_redact_read = 1;
    }
    if (!g_redact) return;
    log_redact(buf);
}

/* -------------------------------------------------------------- canal */

/* A monotonic clock in milliseconds. NOT `clock()`, which measures processor
 * time and reads as almost nothing on HOS - a mistake this repo already paid
 * for in the app lock. */
static long long sink_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* Arms the next attempt, doubling the wait. Called on every failure AND on
 * every loss, so a machine that is absent is probed ever more slowly while one
 * that comes back is found within a second. */
static void sink_schedule_retry(void)
{
    g_sink_backoff_ms = (g_sink_backoff_ms == 0)
                      ? SINK_RETRY_MIN_MS
                      : (g_sink_backoff_ms * 2 > SINK_RETRY_MAX_MS
                         ? SINK_RETRY_MAX_MS : g_sink_backoff_ms * 2);
    g_sink_next_try_ms = sink_now_ms() + g_sink_backoff_ms;
    g_sink_state = 2;
}

static void sink_open_locked(void)
{
    if (g_sink_state == 1) return;
    if (g_sink_state == 2 && sink_now_ms() < g_sink_next_try_ms) return;
    sink_schedule_retry();     /* pessimistic: success clears it at the end */

    char path[256];
    snprintf(path, sizeof(path), "%slogsink.txt", SHADOW_DATA_DIR);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char dest[128] = {0};
    if (!fgets(dest, sizeof(dest), f)) { fclose(f); return; }
    fclose(f);

    char *nl = strpbrk(dest, "\r\n");
    if (nl) *nl = 0;
    char *pt = strrchr(dest, ':');
    if (!pt) return;
    *pt = 0;
    const char *host = dest, *port = pt + 1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return;

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return; }
    shadow_set_nonblocking(fd, 1);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
        struct timeval tv = { 0, 300000 };            /* 300 ms, no more */
        if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) {
            shadow_closesocket(fd); freeaddrinfo(res); return;
        }
        int err = 0; socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (err != 0) { shadow_closesocket(fd); freeaddrinfo(res); return; }
    }
    freeaddrinfo(res);
    /* AF9 2026-09-10 - the socket STAYS non-blocking for its whole life. It
     * used to be switched back to blocking here, and every line is sent from
     * `journal_write` while it holds `g_lock`: a mirror that stopped READING
     * without closing (a paused listener, a laptop gone to sleep) filled the
     * send buffer, `send()` blocked, and with it every thread that logs - video,
     * audio, UI. The session froze, and `journal_close` then waited forever
     * for the drain thread. The 300 ms bound CLAUDE.md promised covered only
     * the connect. */
    g_sink_fd   = fd;
    g_sink_state = 1;
    g_sink_backoff_ms = 0;     /* found: the next loss retries within a second */

    /* AUTH-1: who did we just reach? `dest` was cut at the colon above to split
     * host and port, so it is put back together for the identity the owner will
     * see and the allow-list will key on.
     *
     * DEPOSITED, NOT ANNOUNCED. This function runs under `g_lock`, and
     * the gate's `offer` LOGS - which re-enters `journal_write`, which takes
     * `g_lock`. Calling it from here deadlocked the whole journal, file
     * included, within seconds: the application stayed alive and went
     * completely silent. The comment on `sink_write_locked` already warned
     * about exactly this, a few lines below. The drain thread picks the peer up
     * after releasing the lock. */
    snprintf(g_peer_pending, sizeof g_peer_pending, "%s:%s", host, port);
}

static void sink_write_locked(const char *s, size_t n)
{
    /* DEVL-7: NO connect from here. This runs on whatever thread is logging,
     * under `g_lock`; the drain thread owns the (re)connection. */
    if (g_sink_state != 1 || g_sink_fd < 0) return;
    /* AUTH-1: not one line before the owner has said yes. Refusing commands
     * while still mirroring would protect the console and leak the session. */
    if (!gate_allowed()) return;
    if (send(g_sink_fd, s, n, 0) < 0) {
        /* AF9 - a full buffer is not a dead mirror: the line is DROPPED from
         * the mirror and the session goes on. The file log, written just
         * before under the same lock, still has it - the mirror is a
         * convenience, the file is the record. No log line here: this runs
         * under `g_lock`, and logging would re-enter `journal_write`. */
        const int e = shadow_sock_errno();
        if (e == EAGAIN || e == EWOULDBLOCK) return;
        /* DEVL-7: a real error is a LOSS, not an end. The socket is closed and
         * the next attempt is scheduled; the line itself is gone from the
         * mirror, and the file log still has it - the mirror is a convenience,
         * the file is the record. */
        shadow_closesocket(g_sink_fd);
        g_sink_fd   = -1;
        sink_schedule_retry();
    }
}

void journal_set_command_handler(void (*handler)(const char *line))
{
    g_cmd = handler;
}

void journal_set_mirror_gate(const journal_mirror_gate_t *gate)
{
    g_gate = gate;
}

void journal_reconnect_sink(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_sink_state == 1 && g_sink_fd >= 0) {
        shadow_closesocket(g_sink_fd);
        g_sink_fd = -1;
    }
    g_sink_state = 0;                      /* we allow another attempt */
    sink_open_locked();
    const int ok = (g_sink_state == 1);
    pthread_mutex_unlock(&g_lock);

    journal_write(JOURNAL_INFO, JOURNAL_CAT_SYSTEM,
                   ok ? "[devlink] channel connected on demand"
                      : "[devlink] connection refused - is the machine listening?");
}

/* === WHEN THE LIBC DOES NOT KNOW `%z` ==================================
 *
 * The vitasdk's newlib is built with `_WANT_IO_C99_FORMATS` UNDEFINED
 * (`newlib.h` line 18), so `%z`, `%j` and `%t` do not exist there. That is not
 * merely cosmetic, and the console proved it on 2026-09-13: an unknown length
 * modifier makes newlib print the letters ("step zu of the ladder",
 * "len=zu hex=...") AND **not consume its argument**, so every conversion
 * after it reads the wrong slot of the va_list. `vid_reasm.c`'s
 * `"... len=%zu hex=%s"` therefore handed `strlen` the value 0x500 and the
 * application died on a Data abort - in `_svfprintf_r`, on the first video
 * packet, which is to say the moment the picture path finally worked.
 *
 * There were 106 `%z` in this code base and 42 of them are followed by another
 * conversion: 42 latent crashes, not one bug.
 *
 * THE CONDITION IS THE LIBC, NOT THE PLATFORM. `#ifdef __vita__` would be the
 * subtractive habit this port keeps paying for - and it would also be WRONG
 * the day vitasdk enables the flag. `newlib.h` publishes the answer, so we ask
 * it. On a libc that supports C99 formats this whole thing compiles to
 * nothing.
 *
 * The rewrite is safe precisely because it is scoped to that libc: there
 * `size_t` is `unsigned int` (32-bit ARM), so dropping the `z` turns `%zu`
 * into `%u`, which consumes exactly the four bytes the caller pushed and
 * prints the right number. On a 64-bit target the same edit would be wrong,
 * which is another reason not to do it by platform. */
#if defined(__has_include)
#  if __has_include(<newlib.h>)
#    include <newlib.h>
#  endif
#endif

#if defined(__NEWLIB__) && !defined(_WANT_IO_C99_FORMATS)
#  define SHADOW_LIBC_LACKS_Z_FORMAT 1
#else
#  define SHADOW_LIBC_LACKS_Z_FORMAT 0
#endif

#if SHADOW_LIBC_LACKS_Z_FORMAT
/* Copies `fmt` into `out`, dropping the `z` length modifier. Returns `fmt`
 * itself when there is nothing to do, so a line without `%z` - the vast
 * majority - pays one `strstr` and no copy. */
static const char *strip_z_modifier(const char *fmt, char *out, size_t cap)
{
    /* Pre-filter on the LETTER, not on "%z": a width or a precision sits
     * between the two ("%8zu", "%*zu", "%.3zu"), and looking for the pair
     * returned those formats untouched. Caught by the extracted-function test
     * before it ever ran, which is the only reason it is not a second
     * console round trip. A literal 'z' in the text merely costs one copy. */
    if (!strchr(fmt, 'z')) return fmt;

    size_t o = 0;
    for (const char *p = fmt; *p && o + 1 < cap; ) {
        if (*p != '%') { out[o++] = *p++; continue; }
        out[o++] = *p++;                       /* the '%' */
        if (*p == '%') { if (o + 1 < cap) out[o++] = *p++; continue; }
        /* flags, width, precision - copied verbatim; only the length
         * modifier is our business. */
        while (*p && strchr("-+ #0'", *p) && o + 1 < cap) out[o++] = *p++;
        while (*p && (*p == '*' || (*p >= '0' && *p <= '9')) && o + 1 < cap)
            out[o++] = *p++;
        if (*p == '.') {
            if (o + 1 < cap) out[o++] = *p++;
            while (*p && (*p == '*' || (*p >= '0' && *p <= '9')) && o + 1 < cap)
                out[o++] = *p++;
        }
        if (*p == 'z') { p++; continue; }      /* the whole point */
        /* any other length modifier stays as written */
        while (*p && strchr("hlLjt", *p) && o + 1 < cap) out[o++] = *p++;
    }
    out[o < cap ? o : cap - 1] = '\0';
    return out;
}
#endif

/* ------------------------------------------------------------ ecriture */

void journal_write(journal_severity_t sev, journal_category_t cat,
                    const char *fmt, ...)
{
    /* THE FILTER IS HERE, BEFORE EVERYTHING ELSE - before the lock, before
     * `vsnprintf`, before the file is opened. That is what makes a filtered line
     * really free: the obvious version filters after the formatting, and then
     * pays the full cost of every line it throws away. */
    if (!journal_enabled(sev, cat)) return;

    pthread_mutex_lock(&g_lock);
    open_locked();
    if (!g_f) { pthread_mutex_unlock(&g_lock); return; }

    char line[4096];
#if SHADOW_LIBC_LACKS_Z_FORMAT
    char fmt_fixed[1024];
    fmt = strip_z_modifier(fmt, fmt_fixed, sizeof fmt_fixed);
#endif
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    redact(line);

    /* === S27 - TIMESTAMP EVERY LINE ===
     * The log had no time at all: impossible to see WHERE the seconds go. A
     * timestamp relative to startup makes every wait visible at a glance. It
     * also goes out on the network - the first version did not send it, and the
     * remote log became useless for exactly the purpose that had motivated
     * it. */
    {
        static struct timespec t0;
        static int t0_set = 0;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!t0_set) { t0 = now; t0_set = 1; }
        long long ms = (long long)(now.tv_sec - t0.tv_sec) * 1000
                     + (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms < 0) ms = 0;
        snprintf(g_stamp, sizeof(g_stamp), "[%lld.%03lld] ",
                 ms / 1000, ms % 1000);
    }

    /* The category is padded to eight characters: the log is read in COLUMNS,
     * and a message that starts at a different offset on every line is far
     * slower to scan by eye. */
    fprintf(g_f, "%s%c/%-8s %s\n", g_stamp, letter(sev),
            journal_category_name(cat), line);

    {
        char net[4400];
        const int n = snprintf(net, sizeof(net), "%s%c/%-8s %s\n", g_stamp,
                               letter(sev), journal_category_name(cat), line);
        if (n > 0) sink_write_locked(net, (size_t)n);
    }
    pthread_mutex_unlock(&g_lock);
}

void journal_flush(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_f) fflush(g_f);
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------- S86 : size, purge, lecture */

void journal_size(journal_size_t *out)
{
    if (!out) return;
    out->bytes = 0;
    out->bytes_current = 0;
    out->files = 0;

    /* The CURRENT log is measured with `ftell`, not with `stat`. It is buffered
     * at 64 KB: `stat` ignores everything not yet written and can underestimate
     * by a minute's worth of trace, which gives a number that does not move
     * while you watch the screen. `ftell` returns the LOGICAL position, buffer
     * included, without touching the card.
     *
     * The lock is held only for that `ftell`. No `stat` under it: those are I/O,
     * and the module's rule is that no long I/O happens under the lock the
     * logging path depends on. */
    pthread_mutex_lock(&g_lock);
    const bool opened = (g_f != NULL);
    long pos = -1;
    if (g_f) pos = ftell(g_f);
    pthread_mutex_unlock(&g_lock);

    if (opened) {
        out->bytes_current = (pos > 0) ? (uint64_t)pos : 0u;
        out->files++;
    } else {
        /* Nothing has been logged yet in this process: the file may exist all the
         * same, left by a previous launch. */
        uint64_t o = 0;
        if (file_size(JOURNAL_FILE, &o)) {
            out->bytes_current = o;
            out->files++;
        }
    }
    out->bytes = out->bytes_current;

    /* A BOUNDED enumeration with no `dirent`: we build the names. We sweep up
     * to the ceiling and not up to N, so archives left by a more generous
     * setting are counted - otherwise the screen announces a smaller size than
     * the card really carries, which is the opposite of the service rendered. */
    for (int i = 1; i <= JOURNAL_SESSIONS_MAX; ++i) {
        char path[JOURNAL_PATH_MAX];
        uint64_t o = 0;
        journal_name(path, sizeof path, i);
        if (file_size(path, &o)) { out->bytes += o; out->files++; }
    }
    {   /* The S81 archive, as long as an SD card not yet relaunched carries it. */
        uint64_t o = 0;
        if (file_size(JOURNAL_FILE_S81, &o)) { out->bytes += o; out->files++; }
    }
}

int journal_purge(void)
{
    int n = 0;

    /* The ARCHIVES first, WITH THE LOCK RELEASED. The logging path never
     * touches them, and a `remove` on an SD card costs milliseconds: taking them
     * under `g_verrou` would make every logging thread wait - the video first -
     * while the user presses a settings button. */
    for (int i = 1; i <= JOURNAL_SESSIONS_MAX; ++i) {
        char path[JOURNAL_PATH_MAX];
        journal_name(path, sizeof path, i);
        if (shadow_file_remove(path) == 0) n++;
    }
    if (shadow_file_remove(JOURNAL_FILE_S81) == 0) n++;

    /* Then the CURRENT log. It is OPEN and the drain thread writes into it
     * every 500 ms: closing it out from under it is a crash, and truncating it
     * through the filesystem would let the 64 KB buffer then flush at the old
     * position, i.e. a file with a hole in it.
     *
     * The right primitive is `freopen` on THE SAME stream: the `FILE*` object is
     * reused, its address does not change, and the contents restart from zero.
     * All of it under the lock, which the drain thread takes too - so it cannot
     * observe the instant the descriptor is replaced.
     *
     * `setvbuf` must be REDONE: `freopen` returns a fresh stream, hence with the
     * default buffering. Without this line, the purge would silently turn the log
     * into line-by-line writing, that is, one synchronous SD write per line - the
     * performance failure this whole module avoids. */
    bool empty = false;
    pthread_mutex_lock(&g_lock);
    if (g_f) {
        FILE *fresh = freopen(JOURNAL_FILE, "w", g_f);
        if (fresh) {
            g_f = fresh;
            setvbuf(g_f, NULL, _IOFBF, 64 * 1024);
            header_locked("log emptied on demand");
            empty = true;
        } else {
            /* A `freopen` that fails leaves the stream CLOSED: keeping `g_f` would
             * be a use after free. We set it back to null, and the next logged
             * line will reopen through `open_locked` - without restarting the
             * rotation, cf. `g_rotation_faite`. */
            g_f = NULL;
        }
    } else if (shadow_file_remove(JOURNAL_FILE) == 0) {
        empty = true;
    }
    pthread_mutex_unlock(&g_lock);
    if (empty) n++;

    /* Outside the lock: `journal_write` takes it again. */
    journal_write(JOURNAL_INFO, JOURNAL_CAT_SYSTEM,
                   "[log] purge on demand - %d file(s) deleted or emptied", n);
    return n;
}

int journal_last_lines(char *buf, size_t buf_size,
                             const char **lines, int max_lines)
{
    return journal_last_lines_index(0, buf, buf_size,
                                          lines, max_lines);
}

int journal_last_lines_index(int index, char *buf, size_t buf_size,
                                   const char **lines, int max_lines)
{
    if (!buf || !lines || max_lines <= 0 || buf_size < 2) return 0;

    /* An archive is a CLOSED file: nobody writes it, no flush is needed, and
     * falling back on the write descriptor makes no sense for it. Only the
     * current session needs those two precautions. */
    char path[JOURNAL_PATH_MAX];
    if (index > 0) {
        if (!journal_path(index, path, sizeof path)) return -1;
        FILE *fa = fopen(path, "rb");
        if (!fa) return -1;
        long end = -1;
        if (fseek(fa, 0, SEEK_END) == 0) end = ftell(fa);
        if (end < 0) { fclose(fa); return -1; }
        const size_t win = buf_size - 1;
        const long beg = ((unsigned long)end > win) ? end - (long)win : 0;
        if (fseek(fa, beg, SEEK_SET) != 0) { fclose(fa); return -1; }
        const size_t n_read = fread(buf, 1, win, fa);
        fclose(fa);
        buf[n_read] = '\0';
        char *q = buf;
        if (beg > 0) {
            char *nl2 = strchr(q, '\n');
            if (!nl2) return 0;
            q = nl2 + 1;
        }
        int m = 0;
        while (*q && m < max_lines) {
            char *nl2 = strchr(q, '\n');
            if (nl2) *nl2 = '\0';
            if (*q) lines[m++] = q;
            if (!nl2) break;
            q = nl2 + 1;
        }
        return m;
    }

    /* A MANDATORY STEP - see `journal.h`. Without this flush, the lines the
     * user has just caused sleep in the 64 KB buffer and the screen shows the log
     * as it was potentially minutes ago. */
    journal_flush();

    /* We read through a SECOND descriptor, read-only, and above all NOT through
     * `g_f`: moving a write stream's position to go and read the end would break
     * the next write, and a read of several tens of kilobytes held under
     * `g_verrou` would block every logging thread. The file may grow while we
     * read it - with no consequence: we only read a window at the end as it
     * was. */
    const size_t window = buf_size - 1;
    size_t lu = 0;
    long   start = 0;

    FILE *f = fopen(JOURNAL_FILE, "rb");
    if (f) {
        long end = -1;
        if (fseek(f, 0, SEEK_END) == 0) end = ftell(f);
        if (end < 0) { fclose(f); return -1; }
        start = ((unsigned long)end > window) ? end - (long)window : 0;
        if (fseek(f, start, SEEK_SET) != 0) { fclose(f); return -1; }
        lu = fread(buf, 1, window, f);
        fclose(f);
    } else {
        /* === S99 2026-08-29 - WHEN THE CONSOLE REFUSES A SECOND DESCRIPTOR ===
         *
         * Reported: the panel shows the SIZE of the log, and "cannot read" when
         * asked to read it. The size is read with `stat`, the read opened a
         * second descriptor - and HOS commonly refuses to open for reading a file
         * already held for writing. Hence a screen that proves the file exists
         * while claiming it cannot open it.
         *
         * So we fall back on the write descriptor. It is already flushed
         * (`journal_flush` above), and we RESTORE its position before returning:
         * without that the next write would resume in the middle of the file and
         * overwrite it.
         *
         * The lock is held for the duration of the read. This is an explicit user
         * gesture, not a hot path: a few tens of kilobytes once, against a screen
         * that does not work. */
        pthread_mutex_lock(&g_lock);
        if (!g_f) { pthread_mutex_unlock(&g_lock); return -1; }
        const long pos = ftell(g_f);
        long end = -1;
        if (fseek(g_f, 0, SEEK_END) == 0) end = ftell(g_f);
        if (end < 0) {
            if (pos >= 0) fseek(g_f, pos, SEEK_SET);
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
        start = ((unsigned long)end > window) ? end - (long)window : 0;
        if (fseek(g_f, start, SEEK_SET) == 0)
            lu = fread(buf, 1, window, g_f);
        /* The write position comes back EXACTLY where it was. */
        if (pos >= 0) fseek(g_f, pos, SEEK_SET);
        else          fseek(g_f, 0, SEEK_END);
        pthread_mutex_unlock(&g_lock);
    }
    buf[lu] = '\0';

    char *p = buf;
    if (start > 0) {
        /* The window falls in the middle of a line. We throw it away: a line
         * cut off on its left loses its timestamp and its category, so it reads
         * as a different line from what it is. */
        char *nl = strchr(p, '\n');
        if (!nl) return 0;                 /* a single line, longer than the window */
        p = nl + 1;
    }

    int n = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        /* No newline: a write in progress, the line is incomplete. We stop
         * rather than display half a sentence. */
        if (!nl) break;
        *nl = '\0';
        if (nl > p && nl[-1] == '\r') nl[-1] = '\0';
        if (*p) {
            if (n < max_lines) {
                lines[n++] = p;
            } else {
                /* A sliding window: we keep the LAST ones. The shift is
                 * O(max_lines) per line read, which stays negligible next to the
                 * disk read and avoids a second pass. */
                memmove(&lines[0], &lines[1],
                        (size_t)(max_lines - 1) * sizeof(*lines));
                lines[max_lines - 1] = p;
            }
        }
        p = nl + 1;
    }
    return n;
}

void journal_close(void)
{
    if (g_drain_started) {
        g_drain_stop = true;
        pthread_join(g_drain, NULL);      /* a clean exit before HOS kills us */
        g_drain_started = false;
    }
    pthread_mutex_lock(&g_lock);
    if (g_f) {
        fflush(g_f);
        fclose(g_f);
        g_f = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}
