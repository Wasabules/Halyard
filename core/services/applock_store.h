/* applock_store - the application lock's two impure halves: the file, and the
 * key derivation.
 *
 * Everything that can be reasoned about offline lives in `applock.h` and is
 * checked by tests/test_applock.c. What is left here needs the machine: reading
 * and writing the card, PBKDF2, and a source of randomness. Keeping the split
 * means a change to the throttling or the record format is proved by a test
 * suite, and only the twenty lines that touch wolfcrypt are not.
 *
 * READ applock.h FIRST - in particular what this lock does not protect.
 */
#ifndef SHADOW_APPLOCK_STORE_H
#define SHADOW_APPLOCK_STORE_H

#include "applock.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APPLOCK_OK = 0,        /* the secret matched; the application may open */
    APPLOCK_WRONG,         /* it did not */
    APPLOCK_THROTTLED,     /* a lockout is in force; nothing was even checked */
    APPLOCK_NO_METHOD,     /* that method is not armed on this install */
    APPLOCK_ERROR,         /* the card refused, or the derivation failed */
} applock_result_t;

/* Reads the record from the card. Returns 1 when a record was read AND is
 * armed. Absent file, unreadable file, malformed record: returns 0 and leaves
 * `out` blank, i.e. NO LOCK - the reasoning is in applock.h, and the malformed
 * case is logged at WARN so it does not pass unnoticed. */
int applock_store_load(applock_record_t *out);

/* "Is the application locked?" - the question a caller usually has, without a
 * record to hold. Same answer as `applock_store_load` into a throwaway, and it
 * exists so callers do not have to declare one just to discard it. Re-reads the
 * card on every call, which is what makes it correct after the lock is armed or
 * removed from the settings screen. */
int applock_configured(void);

/* Writes the record. Returns 1 on success. Best effort on the directory: it is
 * created if missing, like the token's. */
int applock_store_save(const applock_record_t *r);

/* Removes the lock entirely - the record file is deleted rather than blanked,
 * so nothing is left to half-parse. Returns 1 when the card no longer holds a
 * record (including when it did not to begin with). */
int applock_store_clear(void);

/* === CHECKING A SECRET ===
 *
 * `plain` is the PIN as digits, the password as typed, or the pattern as the
 * canonical node string from `applock_pattern_bytes` ("0125").
 *
 * The record is updated IN PLACE and written back BEFORE the comparison, never
 * after: written after, pulling the power on a wrong guess rolls the counter
 * back, and the throttling - the only thing that makes a four-digit PIN mean
 * anything - is defeated with a power cable. That ordering is the whole reason
 * this function owns the write rather than leaving it to the caller.
 *
 * SLOW ON PURPOSE (~0.2-0.4 s): call it off the render thread. */
applock_result_t applock_store_verify(applock_record_t *r, uint32_t method,
                                      const char *plain, int64_t now,
                                      int64_t *retry_in);

/* Derives and stores one method's secret, with a fresh random salt, and seals
 * the master key under it.
 *
 * `master` is the 32-byte master key to seal, or NULL to store the secret with
 * nothing sealed (the shape a record had before encryption). Returns 1 on
 * success. Also slow. */
int applock_store_set_secret(applock_secret_t *s, const char *plain,
                             uint32_t iterations, const uint8_t *master);

/* === THE MASTER KEY ===
 *
 * Held in this module and nowhere else: it is never returned to a caller, so no
 * screen, no log and no crash dump can carry it by accident. Callers say what
 * they want done WITH it, not what it is.
 *
 * It arrives at unlock, when `applock_store_verify` succeeds and unseals it, and
 * lives for the rest of the process. */

/* 1 when a master key is loaded, i.e. the lock has been opened this session and
 * the record carried a sealed key. */
int applock_master_ready(void);

/* Generates a fresh master key in memory. For the first method ever armed on an
 * install: there is nothing to unseal yet, so there is nothing to inherit. */
int applock_master_create(void);

/* Forgets the master key. Called when the lock is removed - after whatever was
 * encrypted with it has been read back out. */
void applock_master_forget(void);

/* Seals / unseals with the master key. `out` receives
 * [nonce 12][ciphertext n][tag 16] and therefore needs n + 28 bytes; opening
 * gives back n - 28. Both return 0 when no master key is loaded, so a caller
 * that forgot to check `applock_master_ready` fails closed rather than writing
 * plaintext. */
int applock_master_seal(const uint8_t *in, size_t n,
                        uint8_t *out, size_t cap, size_t *out_n);
int applock_master_open(const uint8_t *in, size_t n,
                        uint8_t *out, size_t cap, size_t *out_n);
#define APPLOCK_SEAL_OVERHEAD (APPLOCK_NONCE_LEN + APPLOCK_TAG_LEN)

/* Re-seals the master key under `plain` for `method`, in the record on the
 * card. Used when a method is added while the lock is already open. */
int applock_store_rewrap(applock_record_t *r, uint32_t method,
                         const char *plain);

/* Milliseconds the last derivation took. Published so the iteration count can
 * be set from a MEASUREMENT on the console rather than from a guess - see the
 * comment on APPLOCK_ITER_DEFAULT. 0 before the first derivation. */
unsigned applock_last_derive_ms(void);

#ifdef __cplusplus
}
#endif
#endif
