/* applock - the pure core of the application lock.
 *
 * === WHAT THIS PROTECTS, AND WHAT IT DOES NOT ===
 *
 * State this first, because a lock that is trusted for more than it does is
 * worse than no lock: it makes people careless.
 *
 * It DOES protect against someone who picks up the console and opens the app -
 * a guest, a sibling, a colleague. That is the realistic threat for a homebrew
 * application on a shared console, and it is the one asked for.
 *
 * It does NOT protect the data on the card. `refresh_token` sits next to this
 * file and is only XOR-obfuscated with a secret compiled into the binary
 * (shadow/oauth.c says so itself: "Not strong crypto [...] Real security would
 * need a device-bound key"). Anyone who removes the SD card reads it, lock or
 * no lock. Nor does it survive a rebuilt binary. The screen and the
 * documentation must say this; nothing here should imply otherwise.
 *
 * === WHY A PURE MODULE ===
 *
 * Everything that can be got wrong silently lives here: the throttling
 * schedule, the record's parsing, the constant-time comparison, the pattern's
 * canonical form. None of it needs a console, a file, or a crypto library, so
 * all of it is checked offline by tests/test_applock.c. What is left in
 * `applock_store.c` is file I/O and one call to PBKDF2 - the parts a test
 * cannot check without the machine.
 *
 * === THE RULES THAT ARE EASY TO GET WRONG ===
 *
 *  - The failure counter is persisted BEFORE the guess is checked, never after.
 *    Written after, pulling the power on a wrong guess rolls it back, and the
 *    throttling - the only thing that makes a four-digit PIN mean anything -
 *    can be defeated with a power cable.
 *
 *  - A malformed record means NO LOCK, loudly logged. Failing closed would
 *    brick the application with no way back except deleting the file, which is
 *    exactly what an attacker would do anyway. Failing open costs nothing that
 *    was not already lost and keeps the owner out of a trap.
 *
 *  - The lockout deadline is wall-clock and the console's clock is
 *    user-settable. Moving the clock BACK therefore lengthens the wait rather
 *    than shortening it (we compare `now < deadline`), which is the direction
 *    that fails safe. Moving it forward defeats the wait - accepted, and
 *    documented, because the threat model above does not include an owner
 *    attacking their own console's clock.
 */
#ifndef SHADOW_APPLOCK_H
#define SHADOW_APPLOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The three ways to unlock. They are a BITMASK and not an enum: the ask was
 * "password and/or PIN and/or pattern", so several can be armed at once and any
 * ONE of them opens the application - the Android model.
 *
 * The consequence has to be said where the user chooses: arming several methods
 * makes the lock exactly as strong as the WEAKEST of them. Two ways in is two
 * chances to guess, never twice the security. */
#define APPLOCK_PIN      1u
#define APPLOCK_PASSWORD 2u
#define APPLOCK_PATTERN  4u
#define APPLOCK_ALL      (APPLOCK_PIN | APPLOCK_PASSWORD | APPLOCK_PATTERN)

/* 1 = before encryption (two fields per secret). 2 = with a sealed master key.
 * A version we do not know is refused, which means NO LOCK - see the header
 * comment on why that direction is the safe one. */
#define APPLOCK_VERSION 2

#define APPLOCK_SALT_LEN 16
#define APPLOCK_HASH_LEN 32

/* === THE WRAPPED MASTER KEY ===
 *
 * The lock alone does not protect the card: `refresh_token` sits beside this
 * file, obfuscated with a key compiled into the binary. Encryption closes that,
 * and the shape it has to take is decided by one requirement from the ask -
 * "password and/or PIN and/or pattern", any ONE of which opens the application.
 *
 * A key derived from one secret could therefore not decrypt anything for the
 * other two. So there is ONE random master key, and each armed method stores
 * its own COPY of it, sealed under a key derived from that method's secret. Any
 * secret unseals the same master key; none of them IS the master key.
 *
 * The sealing key must not be the value we store to check the secret - the
 * stored hash is in plain sight in this file, and if it were also the sealing
 * key, reading the file would decrypt the token. So the derivation produces
 * SIXTY-FOUR bytes in one pass: the first thirty-two are the verifier that goes
 * on the card, the last thirty-two are the sealing key and are never written
 * anywhere. One PBKDF2 run, two independent halves.
 *
 * Consequence to state plainly, because it is the price: forget every secret
 * and the token is gone with them. That costs one re-login, not the account. */
#define APPLOCK_MASTER_LEN 32
#define APPLOCK_KEK_LEN    32
#define APPLOCK_DERIVE_LEN (APPLOCK_HASH_LEN + APPLOCK_KEK_LEN)
#define APPLOCK_NONCE_LEN  12
#define APPLOCK_TAG_LEN    16
/* [nonce 12][sealed master key 32][tag 16] */
#define APPLOCK_WRAP_LEN   (APPLOCK_NONCE_LEN + APPLOCK_MASTER_LEN + APPLOCK_TAG_LEN)

/* Lengths. The minimums are what makes each method worth arming at all; the
 * maximums keep the record a fixed, small size.
 *
 * The pattern's minimum of 4 is Android's, and for its reason: a 3-node pattern
 * has 320 possibilities, which is worse than a 3-digit PIN and looks stronger
 * than it is. At 4 nodes there are 1624. */
#define APPLOCK_PIN_MIN      4
#define APPLOCK_PIN_MAX     12
#define APPLOCK_PASSWORD_MIN  4
#define APPLOCK_PASSWORD_MAX 63
#define APPLOCK_PATTERN_MIN   4
#define APPLOCK_PATTERN_MAX   9   /* the 3x3 grid has nine nodes, each used once */

/* PBKDF2-HMAC-SHA256 rounds. Stored in the record rather than compiled in, so a
 * console that gets faster - or a count found to be too slow in practice - can
 * be changed without orphaning existing locks.
 *
 * === MEASURED ON THE CONSOLE, 2026-09-03: 553 ms FOR 60 000 ROUNDS ===
 *
 * (55 ms for the same count on an x86_64 laptop, so the console is ~10x slower -
 * which is what a Cortex-A57 at 1020 MHz predicts.) Kept at 60 000, and the
 * reasoning matters more than the number:
 *
 * Half a second is what the user waits after their last keypress, once or twice
 * a session. That is affordable. Raising it is where it stops being affordable:
 * current guidance for PBKDF2-HMAC-SHA256 is on the order of 600 000 rounds,
 * which would be 5.5 SECONDS here. The console cannot buy modern resistance at
 * any price it can pay.
 *
 * And it must be said plainly, because it changes what this setting is for: for
 * a 4-digit PIN or a 3x3 pattern the iteration count buys almost NOTHING against
 * someone who has taken the card. Ten thousand PINs, or a few thousand patterns,
 * fall in seconds on a GPU whatever we choose here - low-entropy secrets are not
 * saved by a KDF. What actually protects them is the THROTTLE, and the throttle
 * only exists while the card stays in the console.
 *
 * So the rounds are here for the PASSWORD method, which is the only one with the
 * entropy to make them count. That is also why lowering this would be a false
 * economy and raising it a false comfort. */
#define APPLOCK_ITER_DEFAULT 60000u
#define APPLOCK_ITER_MIN      1000u    /* below this the throttle is the only defence */
#define APPLOCK_ITER_MAX   2000000u    /* above this an unlock would hang the UI */

/* One method's stored secret. The salt is per METHOD, not per install: it costs
 * nothing and means two methods sharing a weak secret ("1234" as both PIN and
 * password) do not produce two identical hashes, which would tell a reader of
 * the file that they are the same. */
typedef struct {
    uint8_t salt[APPLOCK_SALT_LEN];
    uint8_t hash[APPLOCK_HASH_LEN];
    /* The master key sealed under this method's key. All-zero = this method
     * cannot unseal anything, which is what a record written before encryption
     * existed looks like: the secret still opens the application, the token
     * simply stays in the older obfuscated form. */
    uint8_t wrap[APPLOCK_WRAP_LEN];
} applock_secret_t;

typedef struct {
    uint32_t version;
    uint32_t methods;       /* bitmask; 0 = no lock */
    uint32_t iterations;
    applock_secret_t pin;
    applock_secret_t password;
    applock_secret_t pattern;
    /* Consecutive failures, and the wall-clock second the lockout expires.
     * Both persisted: a throttle that a restart resets is not a throttle. */
    uint32_t fails;
    int64_t  lock_until;
} applock_record_t;

/* ── Le dossier ───────────────────────────────────────────────────────────── */

/* Zeroes the record and sets the fields that are not "all bits zero"
 * (`version`, `iterations`). Always call before filling one in. */
void applock_record_init(applock_record_t *r);

/* Parses the on-card text. Returns 1 on success, 0 if the text is not a record
 * we understand - in which case `*out` is left as `applock_record_init` leaves
 * it, i.e. NO LOCK. See the header comment for why that is the safe direction.
 *
 * Unknown keys are IGNORED, not rejected: a record written by a later version
 * must not lock this one out of its own settings. */
int applock_parse(const char *text, size_t len, applock_record_t *out);

/* Writes the record as text. Returns the number of bytes written (excluding the
 * terminating NUL), or -1 if `cap` is too small - never a truncated record,
 * which would parse as a different one. `APPLOCK_TEXT_MAX` is always enough. */
/* Three methods, each carrying a salt, a hash and a sealed key in hex, plus the
 * scalar lines and the comment. Measured at 706 bytes for a full record; the
 * value below leaves room and `applock_serialize` REFUSES rather than truncates
 * if it is ever wrong. */
#define APPLOCK_TEXT_MAX 1200
int applock_serialize(const applock_record_t *r, char *out, size_t cap);

/* 1 if this method carries a sealed master key. */
int applock_has_wrap(const applock_secret_t *s);

/* 1 if at least one method is armed AND its secret is present. A `methods` bit
 * whose secret is all-zero is treated as absent: that combination can only come
 * from a hand-edited or half-written file, and honouring it would demand a
 * secret nobody can produce. */
int applock_is_armed(const applock_record_t *r);

/* The methods that are actually usable, i.e. `methods` filtered as above. */
uint32_t applock_usable_methods(const applock_record_t *r);

/* ── L'etranglement ───────────────────────────────────────────────────────── */

/* Seconds to wait after `fails` consecutive failures. 0 for the first few:
 * typing the wrong PIN once is what people do, and punishing it teaches nothing
 * while making the lock hateful to live with.
 *
 * Capped, on purpose: the owner who has forgotten must be able to keep trying.
 * An uncapped schedule protects nothing extra - the card can be wiped - and
 * turns a memory lapse into a dead application. */
uint32_t applock_penalty_seconds(uint32_t fails);

/* Records one failure: increments `fails` and sets `lock_until` from the
 * schedule. Call this and PERSIST IT BEFORE checking the guess. */
void applock_note_failure(applock_record_t *r, int64_t now);

/* Records a success: clears the counter and the deadline. */
void applock_note_success(applock_record_t *r);

/* 1 while a lockout is in force. `remaining` (may be NULL) receives the seconds
 * left, clamped to >= 1 so a caller never displays "0 s remaining" on a screen
 * that still refuses input. */
int applock_locked_out(const applock_record_t *r, int64_t now, int64_t *remaining);

/* ── Les saisies ──────────────────────────────────────────────────────────── */

/* Validity of what the user typed, BEFORE it is hashed. Each returns 1 or 0.
 * A PIN is digits only; a password is any printable ASCII (the software
 * keyboard cannot produce anything else here, and accepting bytes we cannot
 * re-type would create a lock nobody can open). */
int applock_pin_valid(const char *s);
int applock_password_valid(const char *s);

/* A pattern is a sequence of node indices 0..8, each appearing at most once. */
int applock_pattern_valid(const uint8_t *nodes, size_t n);

/* === THE ANDROID RULE ===
 *
 * Dragging from node 0 to node 2 passes OVER node 1, and Android counts node 1
 * as visited. Without that rule the same finger movement produces different
 * patterns depending on invisible details of where the touch samples landed,
 * which is a lock that sometimes refuses the right gesture - the worst failure
 * a lock can have.
 *
 * Takes the nodes the finger actually latched onto, returns the canonical
 * sequence with the crossed-over nodes inserted. A node already visited is not
 * inserted again (Android does the same: the line then passes through it).
 *
 * Returns the canonical length, or -1 if the input is not a valid tap sequence
 * or `cap` is too small. `out` may be the same buffer as `nodes`. */
int applock_pattern_canonical(const uint8_t *nodes, size_t n,
                              uint8_t *out, size_t cap);

/* The bytes that get hashed for a pattern: one byte per node, values '0'..'8'.
 * Written as its own function so the test pins the encoding - changing it would
 * silently invalidate every existing pattern, and the only symptom would be a
 * user who "suddenly types it wrong". */
int applock_pattern_bytes(const uint8_t *nodes, size_t n, char *out, size_t cap);

/* ── Les outils ───────────────────────────────────────────────────────────── */

/* Comparison in constant time. Returns 1 when equal.
 *
 * `memcmp` returns as soon as two bytes differ, so the time it takes leaks how
 * many leading bytes were right. Against a local attacker holding the console
 * that is not the practical way in - but a comparison that leaks is free to fix
 * and impossible to notice once it is wrong. */
int applock_equal_ct(const uint8_t *a, const uint8_t *b, size_t n);

/* Hex, lower case, for the record's fields. `applock_from_hex` returns 1 only
 * when the WHOLE input is `n` valid hex pairs - a partial parse would silently
 * accept a truncated field and produce a hash nobody can match. */
void applock_to_hex(const uint8_t *in, size_t n, char *out);
int  applock_from_hex(const char *in, size_t n, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
