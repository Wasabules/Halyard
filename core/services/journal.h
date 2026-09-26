/* journal - the application's log: severity, category, message.
 *
 * === WHY THIS MODULE EXISTS (S81, 2026-08-29) ===
 *
 * There was a single function, `journal_uncategorised(fmt, ...)`, and a single thing one
 * could do with it: write. No level, no subject, no filter. Three consequences,
 * all of them observed:
 *
 *   1. THE LOG IS UNREADABLE BY EYE. One session mixes video decoding, gamepad
 *      packets, REST requests and touch gestures into a single stream where
 *      nothing tells a FAILURE from a routine measurement. Nobody reads it any
 *      more, they grep it - and you have to know what to look for first.
 *   2. IT IS EXPENSIVE WHEN IT IS NOT USED. It grows by several megabytes per
 *      session, written to an SD card, for a user who plays and will never
 *      diagnose anything.
 *   3. NOTHING CAN BE TURNED OFF SELECTIVELY. Diagnosing the audio meant putting
 *      up with the video, which writes ten times more.
 *
 * === THE THREE AXES, AND WHAT THEY DO NOT DO ===
 *
 * SEVERITY - how serious it is. This is what debug mode drives.
 * CATEGORY - what it is about. It is DECLARED BY THE MODULE, at the top of the
 * file, not inferred from the message text: a category guessed from a prefix
 * gets it silently wrong the day someone rephrases a sentence.
 * MESSAGE - the text, unchanged.
 *
 * What this module does NOT do: guess. An unclassified line is still written
 * (category `LEGACY`, severity `INFO`). The opposite - assuming an unknown line
 * is noise and dropping it - would amount to deleting by default the traces this
 * repo lives on, on the strength of a heuristic. This repo has already paid for
 * that kind of bet.
 *
 * === MIGRATION WITHOUT TOUCHING A THOUSAND CALL SITES ===
 *
 * Every module already had its alias (`vlog`, `alog`, `clog`, `mlog`...), which
 * IS a category that does not know it. So it is enough to redirect the alias,
 * one line per file, for the category to become real on all of its lines at
 * once, without touching a single one. Two macros per module:
 *
 *     #define vlog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO,  __VA_ARGS__)
 *     #define vdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
 *
 * The existing calls stay at INFO - so nothing disappears - and the bulky lines
 * move to `*dbg` one at a time, in the light of what they write.
 */
#ifndef SHADOW_JOURNAL_H
#define SHADOW_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>            /* size_t - S86: this header must stand on its own */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Severite ---------------------------------------------------------- */

/* The order is the FILTER's: everything <= the threshold is written. The values
 * are explicit because they travel - `env.txt`, the development channel, a
 * settings file written by an earlier version. */
typedef enum {
    JOURNAL_ERROR = 0,   /* the session is compromised; we are about to stop */
    JOURNAL_WARN = 1,   /* abnormal, but we carry on - a fallback, an abandon */
    JOURNAL_INFO   = 2,   /* milestones: session opened, channel established, setting applied */
    JOURNAL_DEBUG  = 3,   /* mesures periodiques, etats internes, experiences */
    JOURNAL_TRACE  = 4,   /* per packet, per frame: unreadable and expensive */
} journal_severity_t;

/* --- Categorie --------------------------------------------------------- */

/* One module = one category, declared at the top of its file. `LEGACY` is the
 * category of the lines that still go through `journal_uncategorised()` without having been
 * classified: it EXISTS so that this is visible in the log, not so that they are
 * left there. */
typedef enum {
    JOURNAL_CAT_LEGACY = 0,
    JOURNAL_CAT_SESSION,      /* bootstrap, control channel, lifecycle */
    JOURNAL_CAT_VIDEO,        /* reception, reassembly, decoding, rendering */
    JOURNAL_CAT_AUDIO,
    JOURNAL_CAT_INPUT,        /* mouse, keyboard, touch */
    JOURNAL_CAT_GAMEPAD,      /* gamepad and rumble */
    JOURNAL_CAT_NETWORK,       /* sockets, TLS, REST */
    JOURNAL_CAT_AUTH,         /* OAuth, tokens, provisioning */
    JOURNAL_CAT_UI,          /* screens, navigation, settings */
    JOURNAL_CAT_SYSTEM,      /* console, hardware, startup */
    JOURNAL_CAT_COUNT
} journal_category_t;

/* Short name of a category, as it appears in the log. Never null: an
 * out-of-range value returns `"?"` rather than reading outside the table. */
const char *journal_category_name(journal_category_t c);
const char *journal_severity_name(journal_severity_t s);

/* --- Ecriture ---------------------------------------------------------- */

void journal_write(journal_severity_t sev, journal_category_t cat,
                    const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Would this line be written? To be tested BEFORE an expensive formatting (a
 * hex dump, a concatenation): `journal_write`'s filter comes after `vsnprintf`,
 * hence too late to save anything.
 *
 * Deliberately `inline`-able and lock-free: it is a read of two integers, called
 * thousands of times a second on the video path. */
bool journal_enabled(journal_severity_t sev, journal_category_t cat);

#define JOURNAL_ERROR_(cat, ...) journal_write(JOURNAL_ERROR, (cat), __VA_ARGS__)
#define JOURNAL_WARN_(cat, ...) journal_write(JOURNAL_WARN, (cat), __VA_ARGS__)
#define JOURNAL_INFO_(cat, ...)   journal_write(JOURNAL_INFO,   (cat), __VA_ARGS__)
#define JOURNAL_DEBUG_(cat, ...)  journal_write(JOURNAL_DEBUG,  (cat), __VA_ARGS__)
#define JOURNAL_TRACE_(cat, ...)  journal_write(JOURNAL_TRACE,  (cat), __VA_ARGS__)

/* --- Filtre ------------------------------------------------------------ */

/* Severity threshold: everything <= it is written. Default `JOURNAL_INFO`, that
 * is, "the milestones and the failures, nothing more".
 *
 * `SHADOW_JOURNAL_NIVEAU` (0..4) sets it and takes PRIORITY over the UI setting:
 * that is the repo's rule for every toggle - `env.txt` serves the A/B runs, and a
 * UI setting must not silently override an experiment in progress. */
void               journal_set_level(journal_severity_t threshold);
journal_severity_t journal_level(void);

/* Category mask, one bit per `journal_category_t`. Default: all of them. It
 * serves to isolate a subject - diagnosing the audio without putting up with the
 * video, which writes ten times more. `JOURNAL_ERROR` and `JOURNAL_WARN` go
 * through the mask: a failure must never be invisible because someone had
 * filtered on another subject. */
void     journal_set_categories(uint32_t mask);
uint32_t journal_categories(void);

#define JOURNAL_ALL_CATEGORIES 0xFFFFFFFFu

/* --- Cycle de vie ------------------------------------------------------ */

/* Flushes the buffer. Useful before a crash dump or at the end of a session. */
void journal_flush(void);

/* Stops the drain thread and closes the file. To be called before `exit()`:
 * otherwise HOS kills the thread brutally and leaks its handles - and a leak
 * costs a console reboot. */
void journal_close(void);

/* === S86, 2026-08-29 - MANAGING THE LOGS, NOT ONLY WRITING THEM ===
 *
 * A user request, console in hand: "have an idea of the size the logs take, a
 * rotation system to separate the logs per application launch so everything is
 * not mixed together, be able to clean up, and view the recent logs".
 *
 * The first three points are measurable gaps; the fourth is a screen. What
 * follows is the CORE, with no UI: `journal.c` knows neither Borealis nor
 * `ui::tr`. The settings screen calls these four functions.
 *
 * WHAT THIS PART COSTS, BECAUSE THE CALLER IS A UI: none of these functions is
 * meant to be called every frame. They touch the SD card. Call them when a
 * screen opens, or on an explicit user action, never from a Borealis `info()` -
 * G47 has already cost a UI at 5 fps for exactly this mistake
 * (`ctrl_gamepad_present()` called 60 times a second from a display lambda).
 */

/* Maximum number of ARCHIVES kept, hence the largest file index. A deliberate
 * ceiling: without `dirent` or `glob` (see below), the inventory is made by
 * building the names one by one, and a single-digit ceiling bounds that
 * enumeration to ten `stat` calls - a few milliseconds, no more. */
#define JOURNAL_SESSIONS_MAX 9

/* Size to reserve for a path returned by `journal_path()`. */
#define JOURNAL_PATH_MAX 256

/* --- Rotation per launch ------------------------------------------------ */

/* One session = one file. `halyard.log` is the CURRENT one;
 * `halyard.1.log` the previous, `.2.log` the one before that, and so on -
 * THE HIGHEST NUMBER IS THE OLDEST. The shift happens once per launch, on the
 * first line written.
 *
 * WHY: S81 kept only two (`halyard.log` and `halyard-precedent.log`).
 * Two launches were therefore enough to lose the session from the day before -
 * and on a console, where the application is relaunched between every run, "the
 * day before" often means "three minutes ago, just before I reproduced the
 * defect". Worse, the note `feedback_log_segment_per_launch.md`
 * documents a FALSE conclusion drawn from an unsegmented log: the timestamps
 * restart from zero on every launch, so two different sessions have lines with
 * the same timestamps, and a correlation made on them had inverted a causality
 * ("the click cut the video", when the clicks arrived 7 s AFTER the freeze). One
 * file per launch removes that class of error at the root.
 *
 * `SHADOW_JOURNAL_SESSIONS` sets the number of archives, 3 by default - an SD
 * card is not a disk, and three sessions cover the normal back-and-forth "I
 * launch, it fails, I relaunch, I look". Bounded by `JOURNAL_SESSIONS_MAX`. The
 * REVERT value is `1`: it restores the S81 two-file behaviour (the current one
 * plus one archive). `0` keeps no archive at all. */
int journal_sessions_kept(void);

/* === S101 2026-08-29 - READING THE PREVIOUS SESSIONS ===
 *
 * `journal_last_lines` only reads the CURRENT session. But the one you want to
 * read back is almost always the previous one: you notice a defect, you quit,
 * you come back - and the current log now contains nothing but the startup. The
 * screen then displayed "nothing has been written yet", which is true and
 * useless.
 *
 * `index`: 0 = the current session, 1..N = the archives, from the most recent to
 * the oldest. Same return contract as the version without an index. */
int journal_last_lines_index(int index, char *buf, size_t buf_size,
                                   const char **lines, int max_lines);

/* Path of the log with index `index`: 0 = the current one, 1..N = the archives.
 * `dst` is ALWAYS filled in when the index is valid; the returned value says
 * whether the file EXISTS, which is the question a viewer screen asks ("which
 * sessions can I offer?"). Returns `false` without writing anything when the
 * index is out of range or `dst` is too small. */
bool journal_path(int index, char *dst, size_t size);

/* --- Taille occupee ----------------------------------------------------- */

typedef struct {
    uint64_t bytes;          /* total, journal en cours + archives */
    uint64_t bytes_current;  /* the current log, on its own */
    int      files;        /* how many files exist (1 = only the current one) */
} journal_size_t;

/* Inventory of the logs present on the card.
 *
 * BOUNDED AND NON-BLOCKING by construction: at most eleven `stat` calls, no
 * content read, no directory walk - neither `dirent` nor `glob`, which are not
 * guaranteed on Switch; the names are BUILT, not discovered. The journal's lock
 * is held only for an in-memory `ftell`, and no I/O happens under it: a settings
 * screen opening must not be able to suspend the thread logging the video.
 *
 * `bytes_current` comes from `ftell`, not from `stat`: the log is buffered at
 * 64 KB, so `stat` underestimates by everything not yet written, whereas `ftell`
 * returns the logical position - buffer included - without forcing a single
 * write to the card. */
void journal_size(journal_size_t *out);

/* --- Purge -------------------------------------------------------------- */

/* Deletes the archives and EMPTIES the current log without closing it.
 * Returns the number of files deleted or emptied.
 *
 * "Without closing it" is not a comfort detail: the drain thread writes into
 * that `FILE*` every 500 ms, and closing it out from under it is a crash. The
 * primitive is therefore a `freopen()` on the SAME stream - the `FILE*` pointer
 * does not change, only its contents are reset - and not an `fclose`/`fopen`.
 * All of it under the lock, which the drain thread takes too.
 *
 * The ARCHIVES, on the other hand, are deleted WITH THE LOCK RELEASED: the
 * logging path never touches them, and a `remove` on an SD card costs
 * milliseconds we do not charge to every thread that logs. */
int journal_purge(void);

/* --- Reading back the last lines ---------------------------------------- */

/* Returns the last lines of the CURRENT log, for a viewer screen.
 *
 * THE CALLER PROVIDES ALL THE STORAGE and this module allocates nothing: `buf`
 * receives the bytes, `lines` receives pointers INTO `buf`. Both must stay alive
 * as long as the result is read. Returns the number of lines written
 * (0..`max_lines`), or -1 when the log is unreadable. `lines[0]` is the OLDEST of
 * the returned lines.
 *
 * The file is several megabytes and is NEVER read whole: we seek to `buf_size`
 * bytes from the end and read only that window. The cost is therefore the one the
 * caller chose when sizing its buffer, not the session's.
 *
 * === THE TRAP, AND THE REASON FOR THE INTERNAL `fflush` ===
 *
 * The log is buffered at 64 KB (`_IOFBF`, and that is deliberate: see the header
 * comment of `journal.c`). The lines just written are therefore NOT in the file -
 * they sleep in the buffer, and the drain thread will only push them on its next
 * round. A naive read of the file shows the log as it was up to several MINUTES
 * ago on a quiet session, that is, everything except what the user just
 * triggered and opened the screen for. So this function empties the buffer
 * BEFORE reading. It is the only I/O it imposes, and it is also the one the drain
 * thread already does twice a second.
 *
 * Two lines are deliberately omitted: the one that starts before the beginning of
 * the window (truncated on the left, it would read as a false line) and a
 * possible last line with no carriage return (a write in progress). Empty lines
 * are skipped. */
int journal_last_lines(char *buf, size_t buf_size,
                             const char **lines, int max_lines);

/* === BIDIRECTIONAL CHANNEL TO THE DEVELOPMENT MACHINE ===
 *
 * The same socket that carries the log brings its commands back. Without it,
 * changing a test plan meant quitting the application, launching ftpd, dropping
 * a file and relaunching - four gestures to change three numbers, when the whole
 * point of the automation was to remove them.
 *
 * The handler is called from the DRAIN THREAD, not from the main thread: it must
 * be brief and block nothing. One line = one command, without the newline. A null
 * handler = commands ignored. */
void journal_set_command_handler(void (*handler)(const char *line));

/* === THE MIRROR'S GATE (AUTH-1) ===
 *
 * The journal owns the socket, so it is the journal that must not send a single
 * line before the console's owner has said yes. But WHO is allowed is decided by
 * a prompt on screen, which is the application's business, not the logger's.
 *
 * So the logger asks and the application answers, exactly as it already does for
 * the command handler above. `devlink/authz.cpp` provides the implementation and
 * `main.cpp` installs it; the pure part of the decision (peer validation, the
 * stored allow-list) is `devlink/authz.h`, tested offline.
 *
 * It used to be the other way round: journal.c DECLARED the three
 * `devlink_authz_*` symbols by hand, since it is C and the implementation is
 * C++. That made a dependency no include graph could see, and it broke
 * `halyard-cli` -- which compiles no C++ -- and with it `bench_runner.py`
 * and every A/B script, silently, until something linked.
 *
 * NO GATE INSTALLED MEANS CLOSED, and says so once in the log. Failing open
 * would hand the channel to whoever reaches it first, which is the whole thing
 * AUTH-1 exists to prevent; failing closed SILENTLY would cost a debugging
 * session to a missing line of setup.
 *
 * `offer` and `forget` are called from the drain thread, OUTSIDE the journal's
 * lock -- `offer` logs, and logging takes that lock. `allowed` is called from
 * both the drain thread and any thread that writes a line, so it must be brief
 * and take no lock of its own that could be held while logging. */
typedef struct {
    int  (*allowed)(void);            /* may the mirror send, right now? */
    void (*offer)(const char *peer);  /* we reached `peer`: ask the owner */
    void (*forget)(void);             /* the peer is gone; a new one must re-ask */
} journal_mirror_gate_t;

/* Installs the gate. The struct must outlive the journal (a static is the
 * intent). A null pointer removes it, which CLOSES the mirror. */
void journal_set_mirror_gate(const journal_mirror_gate_t *gate);

/* Retries the connection to the development machine, NOW.
 *
 * The channel only attempts its connection once, at startup: this protection
 * stops an application whose dev machine is not listening from retrying on every
 * line. But it made the channel unreachable without a restart, while it is armed
 * from the settings, hence while running. A DELIBERATE user action only, never
 * automatic. */
void journal_reconnect_sink(void);

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_JOURNAL_H */
