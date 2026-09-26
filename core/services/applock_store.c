/* See applock_store.h and applock.h for the why. */
#include "applock_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#if !defined(_WIN32)
#  include <unistd.h>
#endif

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/pwdbased.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/chacha20_poly1305.h>
#include <wolfssl/wolfcrypt/hash.h>

#include "config.h"
#include "journal.h"
#include "atomic_file.h"   /* the JOURNAL_* macros; core/log.h was only pulling this in */

#define allog(...)  JOURNAL_INFO_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)
#define alwarn(...) JOURNAL_WARN_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)

#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>       /* _commit - AF4 */
#  define applock_mkdir(p) _mkdir(p)
#else
#  include <unistd.h>
#  define applock_mkdir(p) mkdir((p), 0755)
#endif

#define APPLOCK_PATH SHADOW_DATA_DIR "applock.txt"
#define APPLOCK_TMP  SHADOW_DATA_DIR "applock.new"

/* The data directory WITHOUT its trailing slash, the same way oauth.c defines
 * TOKEN_DIR and for the same reason: `mkdir` tolerates a trailing slash on
 * POSIX and on Windows, but nothing guarantees it for a path handed in through
 * `-DSHADOW_DATA_DIR`, and this is the one call whose failure is silent. */
#ifdef __SWITCH__
#  define APPLOCK_DIR "/switch/halyard"
#elif defined(_WIN32)
#  define APPLOCK_DIR "./halyard-data"
#else
#  define APPLOCK_DIR "/tmp/halyard"
#endif

static unsigned g_derive_ms = 0;

/* The master key, and nothing else in the process holds it. See the header on
 * why it is never handed out. */
static uint8_t g_master[APPLOCK_MASTER_LEN];
static int     g_master_ready = 0;

int applock_master_ready(void) { return g_master_ready; }

void applock_master_forget(void)
{
    memset(g_master, 0, sizeof g_master);
    g_master_ready = 0;
}

static int random_bytes(uint8_t *out, size_t n)
{
    WC_RNG rng;
    int rc = wc_InitRng(&rng);
    if (rc != 0) { alwarn("[lock] RNG FAILED rc=%d", rc); return 0; }
    rc = wc_RNG_GenerateBlock(&rng, out, (word32)n);
    wc_FreeRng(&rng);
    if (rc != 0) { alwarn("[lock] randomness FAILED rc=%d", rc); return 0; }
    return 1;
}

int applock_master_create(void)
{
    if (!random_bytes(g_master, sizeof g_master)) { applock_master_forget(); return 0; }
    g_master_ready = 1;
    return 1;
}

/* [nonce 12][ciphertext][tag 16], the same layout the streaming path uses for
 * its own chacha20-poly1305 - one shape to remember rather than two.
 *
 * A fresh nonce per sealing, from the same generator as the salts: with
 * ChaCha20-Poly1305 a reused nonce under the same key does not merely weaken
 * the ciphertext, it breaks the construction outright. */
static int seal_with(const uint8_t key[APPLOCK_KEK_LEN],
                     const uint8_t *in, size_t n,
                     uint8_t *out, size_t cap, size_t *out_n)
{
    if (!in || !out || cap < n + APPLOCK_SEAL_OVERHEAD) return 0;
    if (!random_bytes(out, APPLOCK_NONCE_LEN)) return 0;
    if (wc_ChaCha20Poly1305_Encrypt(key, out, NULL, 0, in, (word32)n,
                                    out + APPLOCK_NONCE_LEN,
                                    out + APPLOCK_NONCE_LEN + n) != 0) {
        alwarn("[lock] sealing FAILED");
        return 0;
    }
    if (out_n) *out_n = n + APPLOCK_SEAL_OVERHEAD;
    return 1;
}

static int open_with(const uint8_t key[APPLOCK_KEK_LEN],
                     const uint8_t *in, size_t n,
                     uint8_t *out, size_t cap, size_t *out_n)
{
    const size_t body = (n >= APPLOCK_SEAL_OVERHEAD) ? n - APPLOCK_SEAL_OVERHEAD : 0;
    if (!in || !out || body == 0 || cap < body) return 0;
    /* The tag is the whole verification: a wrong key, a flipped byte or a
     * truncated field all fail here, and none of them can produce plaintext. */
    if (wc_ChaCha20Poly1305_Decrypt(key, in, NULL, 0,
                                    in + APPLOCK_NONCE_LEN, (word32)body,
                                    in + APPLOCK_NONCE_LEN + body, out) != 0)
        return 0;
    if (out_n) *out_n = body;
    return 1;
}

int applock_master_seal(const uint8_t *in, size_t n,
                        uint8_t *out, size_t cap, size_t *out_n)
{
    if (!g_master_ready) return 0;
    return seal_with(g_master, in, n, out, cap, out_n);
}

int applock_master_open(const uint8_t *in, size_t n,
                        uint8_t *out, size_t cap, size_t *out_n)
{
    if (!g_master_ready) return 0;
    return open_with(g_master, in, n, out, cap, out_n);
}

unsigned applock_last_derive_ms(void) { return g_derive_ms; }

static int ensure_dir(void)
{
    struct stat st;
    if (stat(APPLOCK_DIR, &st) == 0) return 1;
    if (applock_mkdir(APPLOCK_DIR) == 0) return 1;
    alwarn("[lock] mkdir %s: %s", APPLOCK_DIR, strerror(errno));
    return 0;
}

/* ── La derivation ────────────────────────────────────────────────────────── */

/* PBKDF2-HMAC-SHA256. The cost is the point: a four-digit PIN has ten thousand
 * possibilities, and what keeps that from being enumerated in a second by
 * someone who copied the card is the price of ONE attempt.
 *
 * That said, the throttling in `applock.c` is the defence that actually
 * matters here, because the attacker this lock is for has the console and not a
 * PC. The derivation cost is what remains if the card leaves the console - and
 * against a PIN or a pattern, what remains is very little: see the measurement
 * and the reasoning on APPLOCK_ITER_DEFAULT. */
/* Produces BOTH halves in one pass: `out[0..31]` is the verifier that goes on
 * the card, `out[32..63]` is the key that seals the master key and is never
 * written anywhere. They must come from one derivation and not two - two would
 * double the cost of every unlock for no gain - and they must be DIFFERENT,
 * because the verifier is in plain sight in the record file. If the sealing key
 * were the verifier, reading the file would decrypt the token. */
/* Milliseconds on a monotonic clock.
 *
 * `clock()` was used here and it MEASURED NOTHING ON CONSOLE: the log said
 * `derivation 0 ms (60000 tours)` for a derivation that cannot take zero
 * milliseconds. HOS's newlib does not give it a usable resolution, so the one
 * instrument put here to CALIBRATE the iteration count was a dead counter -
 * exactly the family CLAUDE.md names, an instrument that displays without
 * measuring. `CLOCK_MONOTONIC` is what the rest of this repo uses. */
static unsigned monotonic_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (unsigned)((unsigned long long)t.tv_sec * 1000ULL
                    + (unsigned long long)t.tv_nsec / 1000000ULL);
}

static int derive(const char *plain, const uint8_t *salt, uint32_t iterations,
                  uint8_t out[APPLOCK_DERIVE_LEN])
{
    unsigned t0, t1;
    int rc;
    if (!plain || !salt || !out) return 0;
    t0 = monotonic_ms();
    rc = wc_PBKDF2(out, (const byte *)plain, (int)strlen(plain),
                   salt, APPLOCK_SALT_LEN, (int)iterations,
                   APPLOCK_DERIVE_LEN, WC_SHA256);
    t1 = monotonic_ms();
    if (t1 >= t0) g_derive_ms = t1 - t0;
    if (rc != 0) {
        alwarn("[lock] derivation FAILED rc=%d", rc);
        return 0;
    }
    return 1;
}

int applock_store_set_secret(applock_secret_t *s, const char *plain,
                             uint32_t iterations, const uint8_t *master)
{
    uint8_t both[APPLOCK_DERIVE_LEN];
    int ok = 0;

    if (!s || !plain) return 0;
    if (iterations < APPLOCK_ITER_MIN || iterations > APPLOCK_ITER_MAX)
        iterations = APPLOCK_ITER_DEFAULT;

    memset(s, 0, sizeof *s);
    /* A fresh salt for EVERY write, including when the same secret is set
     * again: reusing one would let a reader of two successive files see that
     * the secret did not change. */
    if (!random_bytes(s->salt, APPLOCK_SALT_LEN)) return 0;
    if (!derive(plain, s->salt, iterations, both)) goto out;

    memcpy(s->hash, both, APPLOCK_HASH_LEN);
    if (master) {
        if (!seal_with(both + APPLOCK_HASH_LEN, master, APPLOCK_MASTER_LEN,
                       s->wrap, sizeof s->wrap, NULL))
            goto out;
    }
    ok = 1;

out:
    /* The sealing half never leaves this frame. */
    memset(both, 0, sizeof both);
    /* Leave nothing half-written: a salt with no hash is exactly the "method
     * armed with no secret" case applock.c has to defend against. */
    if (!ok) memset(s, 0, sizeof *s);
    return ok;
}

/* ── Le fichier ───────────────────────────────────────────────────────────── */

int applock_store_load(applock_record_t *out)
{
    char buf[APPLOCK_TEXT_MAX + 1];
    FILE *f;
    size_t n;

    if (!out) return 0;
    applock_record_init(out);

    f = fopen(APPLOCK_PATH, "rb");
    /* AF4 2026-09-10 - the save's fallback (below) has a window where only the
     * new record exists: the old one deleted, the rename not done yet. A cut
     * there used to read as "no file", i.e. NO LOCK. The complete file wins
     * when both exist: a record beside it is a write that never finished. */
    if (!f) f = fopen(APPLOCK_TMP, "rb");
    if (!f) return 0;                       /* no file = no lock, the normal case */
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    if (!applock_parse(buf, n, out)) {
        /* Said out loud. A record we cannot read means the lock is OFF, and the
         * owner has to be able to find that out from the log rather than by
         * noticing the application opens without asking. */
        alwarn("[lock] %s unreadable - the application is NOT locked", APPLOCK_PATH);
        applock_record_init(out);
        return 0;
    }
    return applock_is_armed(out);
}

int applock_configured(void)
{
    applock_record_t r;
    return applock_store_load(&r);
}

int applock_store_save(const applock_record_t *r)
{
    char buf[APPLOCK_TEXT_MAX];
    FILE *f;
    int n;
    size_t w;

    if (!r) return 0;
    n = applock_serialize(r, buf, sizeof buf);
    if (n < 0) { alwarn("[lock] serialisation FAILED"); return 0; }
    if (!ensure_dir()) return 0;

    /* === WRITTEN BESIDE, THEN RENAMED OVER ===
     *
     * Opening the record itself with "wb" TRUNCATES it before a single byte of
     * the new one is written. This function is called on every attempt - it is
     * how the failure counter reaches the card before the guess is checked - so
     * an interruption in that window (the power button, a full card, a card
     * pulled out) leaves an empty or half-written record. And a record we
     * cannot parse means NO LOCK: one badly-timed failed guess would remove the
     * lock entirely, which is the opposite of what the counter is for.
     *
     * So the new record is written next to the old one and moved over it.
     * `rename` within one directory is atomic on every filesystem we run on, so
     * a reader sees the old record or the new one and never a piece of either.
     * The flush before it is what makes that promise real: renaming a file
     * whose bytes are still in a buffer would move an empty file into place. */
    f = fopen(APPLOCK_TMP, "wb");
    if (!f) { alwarn("[lock] write %s: %s", APPLOCK_TMP, strerror(errno)); return 0; }
    w = fwrite(buf, 1, (size_t)n, f);
    if (fflush(f) != 0) w = 0;
    /* The bytes out of our buffer are not the bytes on the card. Without this
     * the rename can land before the data does, and a power cut between the two
     * leaves an empty file with the right name - exactly the case the rename
     * was there to prevent. AF4 2026-09-10 - Windows included: it has no
     * `fsync` and was simply skipped; `_commit` is its equivalent. */
#if defined(_WIN32)
    if (w == (size_t)n && _commit(_fileno(f)) != 0) {
#else
    if (w == (size_t)n && fsync(fileno(f)) != 0) {
#endif
        /* Not fatal by itself: some filesystems refuse it. The write still
         * happened; we simply cannot promise it survived a power cut. */
        alwarn("[lock] fsync refused: %s", strerror(errno));
    }
    fclose(f);
    if (w != (size_t)n) { alwarn("[lock] incomplete write"); shadow_file_remove(APPLOCK_TMP); return 0; }

    /* === RENAME FIRST, AND ONLY UNLINK IF IT REFUSES (2026-09-02) ===
     *
     * POSIX `rename` replaces the destination atomically, which is the whole
     * point of writing beside and moving over. TWO of our three platforms do
     * not honour that:
     *
     *  - Windows refuses to rename onto an existing name outright.
     *  - THE SWITCH DOES TOO. libnx maps `rename` to Nintendo's FS, which
     *    returns "path already exists" rather than replacing. This is what the
     *    user hit: setting a pattern writes the record TWICE (once from
     *    `applock_store_rewrap`, once for the cleared failure counter), so the
     *    first call created `applock.txt` and the second failed on the rename -
     *    and the screen said "cannot write to the card, attempt refused" after
     *    the secret had in fact already been stored.
     *
     * So: try the atomic path first, and fall back to unlink-then-rename only
     * when it is refused. The fallback has a window where the record is not
     * at its name; it is taken only on platforms that leave no alternative,
     * and a crash inside it leaves `applock.new` on the card - which the load
     * falls back to (AF4 2026-09-10) and the next save overwrites. */
    if (shadow_file_rename(APPLOCK_TMP, APPLOCK_PATH) != 0) {
        shadow_file_remove(APPLOCK_PATH);
        if (shadow_file_rename(APPLOCK_TMP, APPLOCK_PATH) != 0) {
            alwarn("[lock] replace %s: %s", APPLOCK_PATH, strerror(errno));
            /* AF4 2026-09-10 - the new record is KEPT. The old one is already
             * gone: deleting this one too left no record at all, and no record
             * means no lock. `applock_store_load` reads it from here. */
            return 0;
        }
    }
    return 1;
}

int applock_store_clear(void)
{
    /* AF4 2026-09-10 - an interrupted save's leftover goes first: the load now
     * falls back to it, so leaving it would bring back the lock the user has
     * just removed. If it cannot go, neither does the record - the lock stays
     * on, and says so, rather than coming back unannounced at the next launch. */
    if (shadow_file_remove(APPLOCK_TMP) != 0 && errno != ENOENT) {
        alwarn("[lock] remove %s: %s", APPLOCK_TMP, strerror(errno));
        return 0;
    }
    if (shadow_file_remove(APPLOCK_PATH) == 0) return 1;
    if (errno == ENOENT) return 1;          /* already gone: the wanted state */
    alwarn("[lock] remove %s: %s", APPLOCK_PATH, strerror(errno));
    return 0;
}

/* ── La verification ──────────────────────────────────────────────────────── */

static const applock_secret_t *secret_for(const applock_record_t *r, uint32_t method)
{
    switch (method) {
        case APPLOCK_PIN:      return &r->pin;
        case APPLOCK_PASSWORD: return &r->password;
        case APPLOCK_PATTERN:  return &r->pattern;
        default:               return NULL;
    }
}

applock_result_t applock_store_verify(applock_record_t *r, uint32_t method,
                                      const char *plain, int64_t now,
                                      int64_t *retry_in)
{
    const applock_secret_t *s;
    uint8_t both[APPLOCK_DERIVE_LEN];
    int match;

    if (retry_in) *retry_in = 0;
    if (!r || !plain) return APPLOCK_ERROR;

    if (applock_locked_out(r, now, retry_in)) return APPLOCK_THROTTLED;
    if (!(applock_usable_methods(r) & method)) return APPLOCK_NO_METHOD;
    s = secret_for(r, method);
    if (!s) return APPLOCK_NO_METHOD;

    /* === THE ORDER, AND IT IS THE WHOLE POINT ===
     *
     * Count the failure and PUT IT ON THE CARD before checking anything. If the
     * guess turns out to be right we clear it a few hundred milliseconds later
     * and the user never knows.
     *
     * The other order - check, then record if wrong - is the one everybody
     * writes, and it can be defeated by holding the power button during the
     * derivation: the counter never reaches the card, and the attempts are free
     * again. That is not a theoretical attack on a console whose power button
     * is right there. */
    applock_note_failure(r, now);
    if (!applock_store_save(r)) {
        /* The card refused. Refusing the attempt is the only safe answer: going
         * ahead would give an unlimited number of free guesses to anyone who
         * can make the write fail. */
        alwarn("[lock] could not record the attempt - try refused");
        return APPLOCK_ERROR;
    }

    if (!derive(plain, s->salt, r->iterations, both)) return APPLOCK_ERROR;
    match = applock_equal_ct(both, s->hash, APPLOCK_HASH_LEN);

    if (match && applock_has_wrap(s)) {
        /* === THE MASTER KEY ARRIVES HERE, AND NOWHERE ELSE ===
         *
         * The second half of the derivation - never stored - unseals this
         * method's copy of the master key. From now on the token can be read.
         *
         * A failure to unseal is NOT treated as a wrong secret: the verifier
         * already said the secret is right. It means the sealed field is
         * damaged, and the honest consequence is that the application opens and
         * the token cannot be read - which shows up as a re-login, not as a
         * lock that refuses a correct PIN. */
        size_t got = 0;
        if (open_with(both + APPLOCK_HASH_LEN, s->wrap, APPLOCK_WRAP_LEN,
                      g_master, sizeof g_master, &got) &&
            got == APPLOCK_MASTER_LEN) {
            g_master_ready = 1;
        } else {
            applock_master_forget();
            alwarn("[lock] sealed key unreadable - the token will have to be made again");
        }
    }

    /* Both halves are secrets in their own right: the verifier is the stored
     * hash's twin, and the sealing key opens the token. */
    memset(both, 0, sizeof both);

    if (!match) {
        applock_locked_out(r, now, retry_in);
        return APPLOCK_WRONG;
    }

    applock_note_success(r);
    applock_store_save(r);
    allog("[lock] opened - derivation %u ms (%u rounds)",
          applock_last_derive_ms(), r->iterations);
    return APPLOCK_OK;
}

int applock_store_rewrap(applock_record_t *r, uint32_t method, const char *plain)
{
    applock_secret_t *slot;
    if (!r || !plain) return 0;
    if (!g_master_ready) { alwarn("[lock] rewrap: no master key loaded"); return 0; }
    slot = (method == APPLOCK_PATTERN)  ? &r->pattern
         : (method == APPLOCK_PASSWORD) ? &r->password
         : (method == APPLOCK_PIN)      ? &r->pin
                                        : NULL;
    if (!slot) { alwarn("[lock] rewrap: unknown method %u", method); return 0; }
    if (!applock_store_set_secret(slot, plain, r->iterations, g_master)) {
        alwarn("[lock] rewrap: derivation or sealing FAILED");
        return 0;
    }
    r->methods |= method;
    r->version = APPLOCK_VERSION;
    if (!applock_store_save(r)) { alwarn("[lock] rewrap: write FAILED"); return 0; }
    allog("[lock] method %u armed - derivation %u ms", method, applock_last_derive_ms());
    return 1;
}
