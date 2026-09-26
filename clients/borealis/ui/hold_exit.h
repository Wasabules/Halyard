/* === HOLD-1 2026-09-12 - THE HOLD THAT CLOSES A TESTER ===
 *
 * Reported on the Windows client: the pause menu's link-quality page showed
 * "Maintiens B pour revenir" and could not be left at all, while the game kept
 * running behind it. The cause was not the measurement - it was that the whole
 * block measuring the hold sat inside `#ifdef __SWITCH__`, in all THREE testers
 * (`net_test`, `mouse_test`, `pad_test`). Off console it was not compiled, so
 * `draw()` could never return true, and those pages have no other way out:
 * `closeNetTest()`, `closeMouseTest()` and `closePadTest()` are called from
 * nowhere.
 *
 * The timing is extracted here so it is the SAME in the three of them and can
 * be checked without a console, a window or a gamepad. What stays at the call
 * site is the one thing that genuinely differs: how the platform reads the
 * button (libnx on console, Borealis elsewhere - see `hold_exit.hpp`).
 *
 * Releasing CANCELS, and that is the whole reason this is a state machine
 * rather than a timestamp comparison: without it a short press followed by a
 * pause eventually exits on its own, which reads as a page closing for no
 * reason.
 *
 * The state belongs to the CALLER, as a module-scope object - never a function
 * `static`. This repo names that family of defects first for good reason, and
 * a tester that kept its progress across two openings would resume mid-hold.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    held;    /* was the button already down on the previous step? */
    double since;   /* when it went down; meaningless while held == 0 */
} hold_exit_t;

/* One frame of the hold.
 *
 * Returns 1 exactly once, on the frame the hold completes, and resets itself so
 * the next opening of the page starts from zero. `progress` is filled with the
 * fraction elapsed, clamped to 0..1, for the bar the testers draw.
 *
 * `now` is the caller's clock in seconds; `need_s` the duration to hold. The
 * clock arrives as a PARAMETER - that is what lets this be tested offline, and
 * it is the same reason `pad_mouse.h` takes its time that way.
 *
 * A non-positive `need_s` would divide by zero or complete instantly; it is
 * treated as "completes as soon as the button is down", which is the only
 * reading that is not a crash.
 */
static inline int hold_exit_step(hold_exit_t *h, int pressed, double now,
                                 double need_s, float *progress)
{
    float p = 0.0f;
    if (!h) { if (progress) *progress = 0.0f; return 0; }

    if (!pressed) {
        /* Released: forget everything. Not "pause the count" - a hold that
         * survived a release is precisely the defect described above. */
        h->held  = 0;
        h->since = 0.0;
        if (progress) *progress = 0.0f;
        return 0;
    }

    if (!h->held) {
        h->held  = 1;
        h->since = now;
    }

    if (need_s <= 0.0) {
        h->held  = 0;
        h->since = 0.0;
        if (progress) *progress = 1.0f;
        return 1;
    }

    /* A clock that goes backwards (or a caller mixing two clocks) would give a
     * negative fraction and an empty bar forever. Clamping at zero keeps the
     * page usable; the hold simply restarts. */
    p = (float)((now - h->since) / need_s);
    if (p < 0.0f) p = 0.0f;

    if (p >= 1.0f) {
        h->held  = 0;
        h->since = 0.0;
        if (progress) *progress = 1.0f;
        return 1;
    }

    if (progress) *progress = p;
    return 0;
}

#ifdef __cplusplus
}
#endif
