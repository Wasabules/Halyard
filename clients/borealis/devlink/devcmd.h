/* devcmd.h - the PARSER for the commands received from the dev machine.
 *
 * WHY THIS MODULE EXISTS. Checking a UI change costs a human gesture today:
 * launch the application, look at the screen, describe it. The log channel
 * (webrtc/log.c) is already bidirectional - it has carried `quit` since S45 - so
 * we run a small set of driving commands over it: `shot`, `state`, `btn`, `nav`,
 * `tap`, `quit`. This file ONLY turns a line into a struct; it presses nothing,
 * captures nothing, writes nowhere.
 *
 * THESE BYTES COME FROM THE NETWORK. This is the only place in the driving
 * channel where we read what we did not produce, so this is where safety is
 * decided:
 *   - we NEVER read past `n`, even when the line has no null terminator (log.c
 *     reads into a 256-byte stack buffer; believing in a `\0` that is not there
 *     means reading the log thread's stack);
 *   - every length is bounded BEFORE anything is written;
 *   - a number is validated digit by digit, with the bound applied on EVERY
 *     iteration, never afterwards - this repo has already paid for an integer
 *     overflow in a parser (KB §9, 2026-08-25: `pb_skip_field` returned a
 *     DECREASING offset and a 12-byte message hung the control thread forever);
 *   - what we do not recognise returns DEVCMD_UNKNOWN. Never a default
 *     behaviour: a half-understood command would press a button nobody asked
 *     for, and the run in progress would become uninterpretable.
 *
 * PURE - no state, no I/O, no socket, no getenv, no allocation. Entirely in the
 * header so it can be checked offline by tests/test_devcmd.c: no console, no
 * virtual machine, no network. Compiles as is under `gcc -Wall -Wextra -Werror`
 * and under C++17.
 *
 * INDIFFERENT TO THE SIGNEDNESS OF `char` - it is UNSIGNED on aarch64 and SIGNED
 * on x86: a 0x80..0xFF byte is therefore 255 on the console and -1 on the dev
 * machine. Every comparison in this file stays an equality or an ASCII range,
 * which gives the same answer under both conventions; the offline suite is
 * replayed with -funsigned-char so that this is a measurement rather than an
 * intention. That is what avoids the classic "works on the desktop, refuses
 * everything on the console".
 *
 * Created 2026-08-27 (the [devlink] channel).
 */
#ifndef DEVLINK_DEVCMD_H
#define DEVLINK_DEVCMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Maximum accepted line length. A legitimate command fits in a few dozen bytes
 * ("tap 16384 16384" = 16); beyond that it is noise or an attempt.
 *
 * WE REFUSE, WE DO NOT TRUNCATE. Cutting the line back to the bound would
 * fabricate commands nobody sent: "tap 1 99999..." cut at the bound becomes
 * "tap 1 999", a perfectly valid tap at an arbitrary point on the screen. A
 * silent truncation turns invalid input into an action. */
#define DEVCMD_LINE_MAX 512

/* Bound on the `tap` coordinates. The console's screen is 1280x720 (1920x1080
 * docked): 16384 leaves all imaginable headroom while keeping the product
 * `value * 10 + digit` very far from INT_MAX, which is what makes the number
 * parser incapable of overflowing. */
#define DEVCMD_COORD_MAX 16384

/* Size of devcmd_t's `name` field. The longest recognised name is "minus" (5);
 * 16 leaves room without ever acting as a landing buffer for network input - a
 * name that is too long is REFUSED, not truncated. */
#define DEVCMD_NAME_MAX 16

/* Size of the `path` field, used by `relaunch` alone.
 *
 * A real path here is "/switch/halyard.b.nro" (27). 128 leaves room for a
 * subdirectory without turning the field into a landing buffer: a path that does
 * not fit is REFUSED, like every other over-long token in this file. The struct
 * is built on the log drain thread's stack, whose size libnx chose, so this is
 * the one field worth keeping small on purpose. */
#define DEVCMD_PATH_MAX 128

/* Sizes for `env`. A real toggle is "SHADOW_VIDEO_NET_TCP" (20) and its value a
 * number or a short word; the longest in the repo on 2026-09-12 is 26. 48 and 64
 * leave room without turning either field into a landing buffer - over-long is
 * REFUSED here like everywhere else in this file. */
#define DEVCMD_ENV_KEY_MAX 48
#define DEVCMD_ENV_VAL_MAX 64

typedef enum {
    DEVCMD_UNKNOWN = 0,
    DEVCMD_SHOT,      /* capture l'ecran                                      */
    DEVCMD_STATE,     /* describes the current screen as text                 */
    DEVCMD_BTN,       /* a = devcmd_button_t, name = the canonical name       */
    DEVCMD_NAV,       /* a = devcmd_dir_t,    name = the canonical name       */
    DEVCMD_TAP,       /* a = x, b = y, both within [0, DEVCMD_COORD_MAX]      */
    DEVCMD_QUIT,      /* stop - exists since S45, do not break it             */
    DEVCMD_RELAUNCH,  /* stop, then load `path` instead (empty = this NRO)    */
    DEVCMD_VERSION,   /* which build is running, and from which .nro          */
    DEVCMD_ENV,       /* read or write a SHADOW_* toggle in env.txt           */
    DEVCMD_HOLD,      /* a = button, b = milliseconds                         */
    DEVCMD_STICK,     /* a = x, b = y in -100..100 ; name = "left"/"right"   */
    DEVCMD_SWIPE,     /* a,b = start ; c,d = end ; e = milliseconds           */
    DEVCMD_RELEASE    /* let go of everything at once                         */
} devcmd_kind_t;

/* Indices returned in `a` for DEVCMD_BTN. The order is stable: the caller uses
 * it as an index into a mapping table to HidNpadButton, which spares it from
 * re-comparing strings that came off the network. */
typedef enum {
    DEVCMD_BTN_A = 0, DEVCMD_BTN_B, DEVCMD_BTN_X, DEVCMD_BTN_Y,
    DEVCMD_BTN_L, DEVCMD_BTN_R, DEVCMD_BTN_ZL, DEVCMD_BTN_ZR,
    DEVCMD_BTN_PLUS, DEVCMD_BTN_MINUS,
    DEVCMD_BTN_COUNT
} devcmd_button_t;

/* Indices returned in `a` for DEVCMD_NAV (the equivalent of the D-pad). */
typedef enum {
    DEVCMD_NAV_UP = 0, DEVCMD_NAV_DOWN, DEVCMD_NAV_LEFT, DEVCMD_NAV_RIGHT,
    DEVCMD_NAV_COUNT
} devcmd_dir_t;

typedef struct {
    devcmd_kind_t kind;
    int            a, b;              /* how to read them: see devcmd_kind_t */
    int            c, d, e;           /* DEVCMD_SWIPE only: end point and duration */
    char           name[DEVCMD_NAME_MAX]; /* always null-terminated             */
    char           path[DEVCMD_PATH_MAX]; /* DEVCMD_RELAUNCH only; "" = this NRO */
    /* DEVCMD_ENV only. Both empty = list. A key with an EMPTY value means
     * REMOVE the toggle, which is why `a` carries the distinction rather than
     * the emptiness of `env_val`: "SHADOW_X=" (erase) and "SHADOW_X" (which is
     * refused) must not be able to collapse into the same struct. */
    char           env_key[DEVCMD_ENV_KEY_MAX];
    char           env_val[DEVCMD_ENV_VAL_MAX];
} devcmd_t;

/* -- Internal odds and ends (devcmd_-prefixed, stateless) ------------------ */

/* Separators ACCEPTED between words: space and tab. "btn   a" has to work - the
 * line may have been typed by hand in a terminal. */
static inline bool devcmd_is_sep(char c) { return c == ' ' || c == '\t'; }

/* End of line. The null is one of them: the caller may pass a C string with an
 * `n` wider than its contents (log.c already splits on "\r\n", but nothing
 * forces it to stay that way). */
static inline bool devcmd_is_terminator(char c)
{
    return c == '\0' || c == '\n' || c == '\r';
}

/* ASCII lowercase, written by hand.
 *
 * WHY NOT tolower(). It takes an `int` that must be EOF or fit in an
 * `unsigned char`; passing it a SIGNED `char` worth 0x80..0xFF - which any
 * network byte is, on aarch64 as on x86 - is undefined behaviour, and its answer
 * additionally depends on the machine's locale. A parser for commands coming off
 * the network can depend on neither. */
static inline char devcmd_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Cursor over the received bytes. `end` is the hard bound: no function in this
 * file dereferences `p` without first checking `p < end`. */
typedef struct { const char *p; const char *end; } devcmd_cursor_t;

/* Detaches the next word, folded to lowercase, into `dst` (always
 * null-terminated when `size > 0`).
 *
 * Returns the REAL length of the word, which may exceed `size - 1`: that is what
 * lets the caller REFUSE a word that is too long instead of working on its
 * truncation. A "btn" followed by 200 characters must not become a "btn"
 * followed by the first 15. Returns 0 when there is no word left. */
static inline size_t devcmd_token_ex(devcmd_cursor_t *cur, char *dst, size_t size,
                                     bool fold)
{
    size_t len = 0, written = 0;

    if (size > 0) dst[0] = '\0';
    while (cur->p < cur->end && devcmd_is_sep(*cur->p)) cur->p++;
    if (cur->p >= cur->end || devcmd_is_terminator(*cur->p)) return 0;

    while (cur->p < cur->end && !devcmd_is_sep(*cur->p)
           && !devcmd_is_terminator(*cur->p)) {
        const char c = fold ? devcmd_lower(*cur->p) : *cur->p;
        if (size > 0 && written + 1 < size) dst[written++] = c;
        len++;
        cur->p++;
    }
    if (size > 0) dst[written] = '\0';
    return len;
}

static inline size_t devcmd_token(devcmd_cursor_t *cur, char *dst, size_t size)
{
    return devcmd_token_ex(cur, dst, size, true);
}

/* The same, KEEPING the case. Only `relaunch` uses it, and it must: a file name
 * is case-sensitive on the SD card, so folding it would refuse
 * `/switch/Halyard.nro` with an error naming a path the sender never
 * typed. Verbs stay folded - they are a closed vocabulary, the path is data. */
static inline size_t devcmd_token_raw(devcmd_cursor_t *cur, char *dst, size_t size)
{
    return devcmd_token_ex(cur, dst, size, false);
}

/* True when only separators then the end of line remain.
 *
 * WHY WE REQUIRE THIS. "tap 1 2 3" and "shot maintenant" are not known commands
 * with a bit of noise on them: they are commands the sender believed to be
 * different from what we understand. Acting on the words we recognised and
 * throwing the rest away means doing something other than what was asked - on a
 * channel that presses buttons, that cannot be taken back. */
static inline bool devcmd_end_of_line(devcmd_cursor_t *cur)
{
    while (cur->p < cur->end && devcmd_is_sep(*cur->p)) cur->p++;
    return cur->p >= cur->end || devcmd_is_terminator(*cur->p);
}

/* Unsigned decimal integer, bounded by DEVCMD_COORD_MAX.
 *
 * THE BOUND IS APPLIED ON EVERY DIGIT, never at the end: that is what makes
 * overflow IMPOSSIBLE rather than unlikely. `v` is at most 16384 before the
 * multiplication, so `v * 10 + 9` is at most 163849 - three orders of magnitude
 * below INT_MAX. The naive form (accumulate everything then compare) overflows
 * the sign at 10 digits, and a signed overflow is UNDEFINED: the compiler is
 * then free to delete the comparison that follows, and
 * "tap 99999999999999999999 0" becomes a tap at some arbitrary coordinate.
 *
 * The sign is refused by construction ('-' and '+' are not digits): a negative
 * coordinate does not exist, and accepting one would send the touch off screen,
 * or worse into a negative index in the caller. */
static inline bool devcmd_number(const char *token, size_t len, int *out)
{
    int v = 0;
    size_t i;

    if (len == 0) return false;
    for (i = 0; i < len; i++) {
        const char c = token[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
        if (v > DEVCMD_COORD_MAX) return false;
    }
    *out = v;
    return true;
}

/* Is this an acceptable path for `relaunch`?
 *
 * THIS IS THE ONLY PLACE IN THE CHANNEL WHERE NETWORK BYTES NAME SOMETHING TO
 * EXECUTE. Every other command presses a button or reads the screen; this one
 * hands a file name to `envSetNextLoad`, and hbloader will run whatever it
 * points at. So the rule is an allow-list, never a deny-list: we say what a
 * legitimate path looks like and refuse everything else, rather than trying to
 * enumerate the ways a path can be hostile.
 *
 *   - it starts with "/switch/" - the homebrew directory, and nothing else;
 *   - it ends with ".nro" - hbloader loads nothing else anyway, and saying so
 *     here turns "I pointed at the wrong file" into a refusal instead of a
 *     console that reboots;
 *   - its characters are letters, digits, and `_ - . /`. No space, no quote, no
 *     byte above 0x7F;
 *   - no "..", ANYWHERE, and no empty segment ("//"). A relative segment is how
 *     "/switch/" stops meaning "/switch/";
 *   - there is a REAL NAME between the prefix and the suffix: at least one
 *     character, and not a leading dot. "/switch/.nro" is exactly 12 bytes and
 *     passes every other rule while naming a hidden file with no name at all -
 *     found by `tests/test_devcmd.c`, which is why the bound is expressed as
 *     "prefix + name + suffix" rather than as a number somebody has to keep in
 *     step with the two strings.
 *
 * The dot is allowed because a file name needs one, which is exactly why ".."
 * has to be refused explicitly rather than by banning the character.
 *
 * PURE, and checked offline: `tests/test_devcmd.c` names each refusal. */
static inline bool devcmd_path_ok(const char *p, size_t len)
{
    static const char PREFIX[] = "/switch/";
    static const char SUFFIX[] = ".nro";
    const size_t pre = sizeof(PREFIX) - 1;   /* 8 */
    const size_t suf = sizeof(SUFFIX) - 1;   /* 4 */
    size_t i;

    if (!p) return false;
    if (len <= pre + suf || len >= DEVCMD_PATH_MAX) return false;   /* room for a name */
    if (memcmp(p, PREFIX, pre) != 0) return false;
    if (memcmp(p + len - suf, SUFFIX, suf) != 0) return false;
    if (p[pre] == '.') return false;         /* "/switch/.nro", "/switch/.x.nro" */

    for (i = 0; i < len; i++) {
        const char c = p[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9')
                     || c == '_' || c == '-' || c == '.' || c == '/';
        if (!ok) return false;
        /* Checked on the PAIR, so it catches "..", "/." at the end and "//"
         * wherever they sit - including inside a name that otherwise passes. */
        if (i + 1 < len) {
            if (p[i] == '.' && p[i + 1] == '.') return false;
            if (p[i] == '/' && p[i + 1] == '/') return false;
        }
    }
    /* A path may not END on a separator or a dot: "/switch/a./" is not a file,
     * and the suffix test above already implies it - this makes it explicit
     * rather than a consequence someone could break by relaxing the suffix. */
    if (p[len - 1] == '/' || p[len - 1] == '.') return false;
    return true;
}

/* Is this an acceptable name for a toggle?
 *
 * `SHADOW_` then capitals, digits and underscores. The prefix is not decoration:
 * `shadow_load_env_file` in main.cpp refuses every other key when it READS the
 * file, so accepting one here would write a line that is silently ignored - an
 * experiment we would believe we ran and did not, which is exactly the phantom
 * measurement that file's own comment warns about.
 *
 * Lowercase is refused rather than folded: environment variables are
 * case-sensitive, and `shadow_fps` is not a typo for `SHADOW_FPS` - it is a
 * different variable that nothing reads. */
static inline bool devcmd_env_key_ok(const char *k, size_t len)
{
    static const char PREFIX[] = "SHADOW_";
    const size_t pre = sizeof(PREFIX) - 1;
    size_t i;

    if (!k || len <= pre || len >= DEVCMD_ENV_KEY_MAX) return false;
    if (memcmp(k, PREFIX, pre) != 0) return false;
    for (i = pre; i < len; i++) {
        const char c = k[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

/* And an acceptable value?
 *
 * Printable ASCII without space, because the line written into `env.txt` is
 * `KEY=VALUE` cut on the first `=` and the end of line: a space would survive
 * into the value and a newline would forge a second toggle nobody asked for.
 * EMPTY IS VALID and means "remove this toggle" - see `devcmd_t.env_val`. */
static inline bool devcmd_env_val_ok(const char *v, size_t len)
{
    size_t i;
    if (!v) return false;
    if (len >= DEVCMD_ENV_VAL_MAX) return false;
    for (i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)v[i];
        if (c <= 0x20 || c >= 0x7f) return false;
        if (c == '=') return false;       /* one `=` per line, and it is the separator */
    }
    return true;
}

/* Milliseconds, bounded like a coordinate and for the same reason: the bound is
 * applied on EVERY digit, so the accumulator cannot overflow. 100000 is far
 * beyond any legitimate hold and still four orders below INT_MAX. */
#define DEVCMD_MS_MAX 100000

static inline bool devcmd_ms(const char *token, size_t len, int *out)
{
    int v = 0;
    size_t i;
    if (len == 0) return false;
    for (i = 0; i < len; i++) {
        const char c = token[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
        if (v > DEVCMD_MS_MAX) return false;
    }
    *out = v;
    return true;
}

/* A SIGNED value for a stick, in -100..100 (a percentage of full deflection).
 * Signed, because a stick goes both ways - which is why it does not reuse
 * `devcmd_number`, whose refusal of '-' is deliberate for a coordinate. The
 * bound is still applied digit by digit. */
static inline bool devcmd_signed100(const char *token, size_t len, int *out)
{
    int v = 0, sign = 1;
    size_t i = 0;
    if (len == 0) return false;
    if (token[0] == '-') { sign = -1; i = 1; if (len == 1) return false; }
    for (; i < len; i++) {
        const char c = token[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
        if (v > 100) return false;
    }
    *out = sign * v;
    return true;
}

/* The only two accepted vocabularies. Anything not listed here is UNKNOWN:
 * there is no "default button". */
static inline int devcmd_button_index(const char *word)
{
    static const char *const noms[DEVCMD_BTN_COUNT] = {
        "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus"
    };
    int i;
    for (i = 0; i < (int)DEVCMD_BTN_COUNT; i++)
        if (strcmp(word, noms[i]) == 0) return i;
    return -1;
}

static inline int devcmd_dir_index(const char *word)
{
    static const char *const noms[DEVCMD_NAV_COUNT] = {
        "up", "down", "left", "right"
    };
    int i;
    for (i = 0; i < (int)DEVCMD_NAV_COUNT; i++)
        if (strcmp(word, noms[i]) == 0) return i;
    return -1;
}

static inline void devcmd_clear(devcmd_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = DEVCMD_UNKNOWN;
}

/* ── L'analyseur ──────────────────────────────────────────────────────────── */

/* Translates `line` (at most `n` bytes, no null terminator required) into a
 * command.
 *
 * Returns true if and only if `out->kind != DEVCMD_UNKNOWN`. On refusal `*out` is
 * ZEROED entirely: never an `a` or a `name` half filled in by a parse abandoned
 * midway, which the caller would read anyway.
 *
 * CASE IS IGNORED ("SHOT" == "shot", "BTN A" == "btn a"), and the returned name
 * is always the lowercase form. These lines are typed by hand in a terminal;
 * refusing a capital protects nothing - the vocabulary stays closed, and that is
 * what protects - while producing an "err" that is incomprehensible to the human
 * who just typed the right command. */
static inline bool devcmd_parse(const char *line, size_t n, devcmd_t *out)
{
    devcmd_cursor_t cur;
    devcmd_t         tmp;
    char             word[DEVCMD_NAME_MAX];
    size_t           len;

    if (!out) return false;
    devcmd_clear(out);
    if (!line || n == 0 || n > DEVCMD_LINE_MAX) return false;

    devcmd_clear(&tmp);
    cur.p   = line;
    cur.end = line + n;

    len = devcmd_token(&cur, word, sizeof(word));
    if (len == 0 || len >= sizeof(word)) return false;   /* empty line, or a nonsense verb */

    if (strcmp(word, "shot") == 0) {
        tmp.kind = DEVCMD_SHOT;
    } else if (strcmp(word, "state") == 0) {
        tmp.kind = DEVCMD_STATE;
    } else if (strcmp(word, "quit") == 0) {
        tmp.kind = DEVCMD_QUIT;
    } else if (strcmp(word, "version") == 0) {
        tmp.kind = DEVCMD_VERSION;
    } else if (strcmp(word, "env") == 0) {
        /* `env` alone lists. `env KEY=VALUE` sets. `env KEY=` removes.
         * `env KEY` without the `=` is REFUSED: it reads like a query, and
         * answering it as either a set or a list would be a guess. */
        char       brut[DEVCMD_ENV_KEY_MAX + DEVCMD_ENV_VAL_MAX + 2];
        const char *eq;
        size_t     klen, vlen;

        len = devcmd_token_raw(&cur, brut, sizeof(brut));
        if (len == 0) { tmp.kind = DEVCMD_ENV; }          /* list */
        else {
            if (len >= sizeof(brut)) return false;        /* refused, never truncated */
            eq = (const char *)memchr(brut, '=', len);
            if (!eq) return false;
            klen = (size_t)(eq - brut);
            vlen = len - klen - 1;
            if (!devcmd_env_key_ok(brut, klen)) return false;
            if (!devcmd_env_val_ok(eq + 1, vlen)) return false;
            memcpy(tmp.env_key, brut, klen);  tmp.env_key[klen] = '\0';
            memcpy(tmp.env_val, eq + 1, vlen); tmp.env_val[vlen] = '\0';
            tmp.a    = 1;                                 /* 1 = write, 0 = list */
            tmp.kind = DEVCMD_ENV;
        }
    } else if (strcmp(word, "relaunch") == 0) {
        /* The argument is OPTIONAL: bare, it reloads the NRO currently running
         * (the caller supplies its own path). With an argument it reloads that
         * one instead - which is what the iteration loop needs, because a
         * running NRO is locked and the new build has to arrive under another
         * name. See `devcmd_path_ok` for why the path is allow-listed. */
        char chemin[DEVCMD_PATH_MAX];

        len = devcmd_token_raw(&cur, chemin, sizeof(chemin));
        if (len > 0) {
            if (len >= sizeof(chemin)) return false;     /* refused, never truncated */
            if (!devcmd_path_ok(chemin, len)) return false;
            memcpy(tmp.path, chemin, len + 1);
        }
        tmp.kind = DEVCMD_RELAUNCH;
    } else if (strcmp(word, "btn") == 0 || strcmp(word, "nav") == 0) {
        /* Compares the WHOLE verb, not its first letter: a future verb
         * starting with 'b' would otherwise be silently treated as a button. */
        const bool button = (strcmp(word, "btn") == 0);
        char       arg[DEVCMD_NAME_MAX];
        int        idx;

        len = devcmd_token(&cur, arg, sizeof(arg));
        if (len == 0 || len >= sizeof(arg)) return false;  /* absent, or 200 characters */
        idx = button ? devcmd_button_index(arg) : devcmd_dir_index(arg);
        if (idx < 0) return false;
        tmp.kind = button ? DEVCMD_BTN : DEVCMD_NAV;
        tmp.a     = idx;
        memcpy(tmp.name, arg, strlen(arg) + 1);
    } else if (strcmp(word, "hold") == 0) {
        /* `hold <button> <ms>`: what `btn` cannot express. The hold-to-exit
         * pages (HOLD-1) read button STATE frame by frame, so nothing short of
         * a real, lasting hold ever reaches them. */
        char arg[DEVCMD_NAME_MAX];
        char chiffres[16];
        int  idx = 0, ms = 0;

        len = devcmd_token(&cur, arg, sizeof(arg));
        if (len == 0 || len >= sizeof(arg)) return false;
        idx = devcmd_button_index(arg);
        if (idx < 0) return false;
        len = devcmd_token(&cur, chiffres, sizeof(chiffres));
        if (len == 0 || len >= sizeof(chiffres) || !devcmd_ms(chiffres, len, &ms))
            return false;
        tmp.kind = DEVCMD_HOLD;
        tmp.a    = idx;
        tmp.b    = ms;
        memcpy(tmp.name, arg, strlen(arg) + 1);
    } else if (strcmp(word, "release") == 0) {
        tmp.kind = DEVCMD_RELEASE;
    } else if (strcmp(word, "stick") == 0) {
        /* `stick <side> <x> <y> [ms]`, x and y in -100..100. */
        char side[DEVCMD_NAME_MAX];
        char chiffres[16];
        int  x = 0, y = 0, ms = 0;

        len = devcmd_token(&cur, side, sizeof(side));
        if (len == 0 || len >= sizeof(side)) return false;
        if (strcmp(side, "left") != 0 && strcmp(side, "right") != 0) return false;
        len = devcmd_token(&cur, chiffres, sizeof(chiffres));
        if (len == 0 || len >= sizeof(chiffres) || !devcmd_signed100(chiffres, len, &x))
            return false;
        len = devcmd_token(&cur, chiffres, sizeof(chiffres));
        if (len == 0 || len >= sizeof(chiffres) || !devcmd_signed100(chiffres, len, &y))
            return false;
        len = devcmd_token(&cur, chiffres, sizeof(chiffres));   /* optional */
        if (len > 0) {
            if (len >= sizeof(chiffres) || !devcmd_ms(chiffres, len, &ms)) return false;
        }
        tmp.kind = DEVCMD_STICK;
        tmp.a    = x;
        tmp.b    = y;
        tmp.e    = ms;
        memcpy(tmp.name, side, strlen(side) + 1);
    } else if (strcmp(word, "swipe") == 0) {
        /* `swipe <x1> <y1> <x2> <y2> [ms]`. A tap is `tap`; this is the drag,
         * which is the only way to reach a gesture or the pattern lock. */
        char chiffres[32];
        int  v[4] = { 0, 0, 0, 0 };
        int  ms = 0, i;

        for (i = 0; i < 4; i++) {
            len = devcmd_token(&cur, chiffres, sizeof(chiffres));
            if (len == 0 || len >= sizeof(chiffres) || !devcmd_number(chiffres, len, &v[i]))
                return false;
        }
        len = devcmd_token(&cur, chiffres, sizeof(chiffres));   /* optional */
        if (len > 0) {
            if (len >= sizeof(chiffres) || !devcmd_ms(chiffres, len, &ms)) return false;
        }
        tmp.kind = DEVCMD_SWIPE;
        tmp.a = v[0]; tmp.b = v[1]; tmp.c = v[2]; tmp.d = v[3]; tmp.e = ms;
    } else if (strcmp(word, "tap") == 0) {
        /* 32 bytes: the longest acceptable number is 5 of them. A token
         * longer than this buffer is REFUSED (len >= sizeof), not truncated -
         * without which "99999999999999999999" could become a valid number.
         * The small buffer is deliberate: this parsing runs on the log drain
         * thread, whose stack is the one libnx gave it. */
        char   chiffres[32];
        int    x = 0, y = 0;

        len = devcmd_token(&cur, chiffres, sizeof(chiffres));
        if (len == 0 || len >= sizeof(chiffres) || !devcmd_number(chiffres, len, &x))
            return false;
        len = devcmd_token(&cur, chiffres, sizeof(chiffres));
        if (len == 0 || len >= sizeof(chiffres) || !devcmd_number(chiffres, len, &y))
            return false;
        tmp.kind = DEVCMD_TAP;
        tmp.a     = x;
        tmp.b     = y;
    } else {
        return false;
    }

    if (!devcmd_end_of_line(&cur)) return false;

    *out = tmp;
    return true;
}

/* -- SAFE rendering for the [devlink] lines -------------------------------- */

/* Canonical name of a kind - enough to write "[devlink] ok <command>". */
static inline const char *devcmd_kind_name(devcmd_kind_t g)
{
    switch (g) {
        case DEVCMD_SHOT:  return "shot";
        case DEVCMD_STATE: return "state";
        case DEVCMD_BTN:   return "btn";
        case DEVCMD_NAV:   return "nav";
        case DEVCMD_TAP:   return "tap";
        case DEVCMD_QUIT:  return "quit";
        case DEVCMD_RELAUNCH: return "relaunch";
        case DEVCMD_VERSION: return "version";
        case DEVCMD_ENV:   return "env";
        case DEVCMD_HOLD:  return "hold";
        case DEVCMD_STICK: return "stick";
        case DEVCMD_SWIPE: return "swipe";
        case DEVCMD_RELEASE: return "release";
        case DEVCMD_UNKNOWN: default: return "inconnue";
    }
}

/* Rewrites the PARSED command in its canonical form ("btn a", "tap 12 34").
 *
 * WHY NOT COPY THE RECEIVED LINE BACK. The dev machine reads a stream of lines
 * where only the prefix tells a reply from a trace. Copying network bytes into
 * the log means injecting whatever they contain, newlines included:
 * "btn a\n[devlink] shot-end" would fabricate an end-of-capture nobody sent, and
 * the tool on the other end would glue together a truncated image without saying
 * a word. What goes back out is therefore REBUILT from the struct, never copied.
 *
 * Returns false (and an empty string) when `size` is insufficient: a truncation
 * would turn "tap 1280 720" into "tap 1280 7". */
static inline bool devcmd_render(const devcmd_t *cmd, char *dst, size_t size)
{
    int written;

    if (!dst || size == 0) return false;
    dst[0] = '\0';
    if (!cmd || cmd->kind == DEVCMD_UNKNOWN) return false;

    if (cmd->kind == DEVCMD_BTN || cmd->kind == DEVCMD_NAV) {
        char nom_sur[DEVCMD_NAME_MAX];
        memcpy(nom_sur, cmd->name, sizeof(nom_sur));
        nom_sur[sizeof(nom_sur) - 1] = '\0';   /* a hand-built struct may carry
                                                * an unterminated name */
        written = snprintf(dst, size, "%s %s", devcmd_kind_name(cmd->kind), nom_sur);
    } else if (cmd->kind == DEVCMD_TAP) {
        written = snprintf(dst, size, "tap %d %d", cmd->a, cmd->b);
    } else if (cmd->kind == DEVCMD_HOLD) {
        char nom_sur[DEVCMD_NAME_MAX];
        memcpy(nom_sur, cmd->name, sizeof(nom_sur));
        nom_sur[sizeof(nom_sur) - 1] = '\0';
        written = snprintf(dst, size, "hold %s %d", nom_sur, cmd->b);
    } else if (cmd->kind == DEVCMD_STICK) {
        char nom_sur[DEVCMD_NAME_MAX];
        memcpy(nom_sur, cmd->name, sizeof(nom_sur));
        nom_sur[sizeof(nom_sur) - 1] = '\0';
        written = snprintf(dst, size, "stick %s %d %d", nom_sur, cmd->a, cmd->b);
    } else if (cmd->kind == DEVCMD_SWIPE) {
        written = snprintf(dst, size, "swipe %d %d %d %d",
                           cmd->a, cmd->b, cmd->c, cmd->d);
    } else if (cmd->kind == DEVCMD_ENV && cmd->a == 1) {
        /* Safe to render for the same reason as the path: both halves have been
         * through their allow-list, which admits no control byte, so neither can
         * forge a second line in the log. */
        char k[DEVCMD_ENV_KEY_MAX], v[DEVCMD_ENV_VAL_MAX];
        memcpy(k, cmd->env_key, sizeof(k)); k[sizeof(k) - 1] = '\0';
        memcpy(v, cmd->env_val, sizeof(v)); v[sizeof(v) - 1] = '\0';
        if (!devcmd_env_key_ok(k, strlen(k))
            || !devcmd_env_val_ok(v, strlen(v))) { dst[0] = '\0'; return false; }
        written = snprintf(dst, size, "env %s=%s", k, v);
    } else if (cmd->kind == DEVCMD_RELAUNCH && cmd->path[0]) {
        /* The path is rendered, and that is safe for the reason this whole
         * function exists: it has been through `devcmd_path_ok`, whose
         * allow-list contains no newline and no control byte, so it cannot
         * fabricate a second line in the log. A hand-built struct might still
         * carry an unterminated array, hence the same copy-and-terminate as the
         * name above - the reply must say WHICH path was understood, since that
         * is the one about to be executed. */
        char path_buf[DEVCMD_PATH_MAX];
        memcpy(path_buf, cmd->path, sizeof(path_buf));
        path_buf[sizeof(path_buf) - 1] = '\0';
        if (!devcmd_path_ok(path_buf, strlen(path_buf))) { dst[0] = '\0'; return false; }
        written = snprintf(dst, size, "relaunch %s", path_buf);
    } else {
        written = snprintf(dst, size, "%s", devcmd_kind_name(cmd->kind));
    }

    if (written < 0 || (size_t)written >= size) { dst[0] = '\0'; return false; }
    return true;
}

/* First word of the line, folded to lowercase, STRIPPED of everything that is
 * not [a-z0-9._-] - enough to write "[devlink] err <command> <reason>" without
 * copying a single dangerous byte from the network (same reason as above: the
 * newline is this protocol's hostile character). A discarded byte becomes '?',
 * so the trace shows there was something there.
 *
 * Returns false when the line has no first word, or when it is longer than
 * `size - 1`: what is too long is not shortened, it is reported. */
static inline bool devcmd_first_word_is(const char *line, size_t n,
                                          char *dst, size_t size)
{
    devcmd_cursor_t cur;
    size_t           len, i;

    if (!dst || size == 0) return false;
    dst[0] = '\0';
    if (!line || n == 0 || n > DEVCMD_LINE_MAX) return false;

    cur.p   = line;
    cur.end = line + n;
    len = devcmd_token(&cur, dst, size);
    if (len == 0 || len >= size) { dst[0] = '\0'; return false; }

    for (i = 0; dst[i] != '\0'; i++) {
        const char c = dst[i];
        const bool sur = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                         || c == '.' || c == '_' || c == '-';
        if (!sur) dst[i] = '?';
    }
    return true;
}

#endif /* DEVLINK_DEVCMD_H */
