/* gestures.h - touch gesture recognition. PURE: no global state, no I/O, no
 * getenv, no clock. Time is a PARAMETER.
 *
 * === WHY THIS IS A SEPARATE MODULE ===
 *
 * Recognition has three traps you cannot see by reading the code, and that only
 * a test can pin down:
 *
 *   1. THE FINGER COUNT FLICKERS. The HID driver reports 1, then 2, then 1, then
 *      2 while the second finger is being put down. Classifying on the
 *      INSTANTANEOUS count makes a two-finger gesture indistinguishable from a
 *      tap. So we classify on the MAXIMUM seen since the gesture began.
 *
 *   2. THE DOUBLE TAP COSTS LATENCY - but only sometimes. If the double tap
 *      triggers something other than the single tap, we CANNOT emit the single
 *      tap on release: we have to wait for the window to close before we know.
 *      That is 400 ms of delay on every click. If instead the double tap merely
 *      repeats the single tap (the default case), no wait is needed at all - the
 *      remote system recognises the double click on its own. So the setting is
 *      what decides the latency, and `double_distinct` is where that is decided.
 *
 *   3. THE SECOND CLICK MUST BE SNAPPED BACK. Windows requires both clicks of a
 *      double to land inside a 4x4 pixel rectangle (SM_CXDOUBLECLK); a finger
 *      never comes down that precisely. So the event carries the position TO
 *      USE, which is that of the FIRST tap when it is a double.
 *
 * The caller supplies the finger count, the position of the first finger and the
 * y of the CENTRE of the first two (for scrolling), plus the clock. It gets back
 * events and a number of wheel notches.
 */

#ifndef UI_GESTURES_H
#define UI_GESTURES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_TAP_1, GESTURE_DOUBLE_1, GESTURE_LONG_1,
    GESTURE_TAP_2, GESTURE_DOUBLE_2, GESTURE_LONG_2,
    GESTURE_TAP_3, GESTURE_DOUBLE_3, GESTURE_LONG_3,
    GESTURE_COUNT
} gesture_t;

/* Indices into a settings table indexed by gesture. */
#define GESTURE_TAP(n)   ((gesture_t)(GESTURE_TAP_1   + ((n) - 1) * 3))
#define GESTURE_DOUBLE(n) ((gesture_t)(GESTURE_DOUBLE_1 + ((n) - 1) * 3))
#define GESTURE_LONG(n)   ((gesture_t)(GESTURE_LONG_1   + ((n) - 1) * 3))

typedef struct {
    gesture_t what;
    int       fingers;    /* 1 to 3 */
    int       x, y;       /* position TO USE (snapped back if second tap) */
    int       second;     /* 1 if this is the 2nd tap of a double */
} gesture_evt;

typedef struct {
    uint64_t tap_max_us;       /* beyond this it is no longer a tap */
    uint64_t long_min_us;      /* from when the press counts as "long" */
    uint64_t double_max_us;    /* window between two taps */
    int      tap_max_drift;    /* max movement still allowed for a tap */
    int      double_max_dist;  /* max distance between the two taps */
    int      notch_px;          /* touch pixels per wheel notch */
    /* Index 1..3: does the double tap have an action of its OWN? If so, the
     * single tap has to wait out the window (see trap 2). */
    int      double_distinct[4];
} gestures_config;

typedef struct {
    gestures_config cfg;
    int      prev_fingers;
    int      max_fingers;
    uint64_t start_us;
    int      x0, y0;
    int      drift;
    int      long_sent;
    /* TWO distinct things, and confusing them is a bug:
     *  - `pending` is an event TO BE EMITTED later. It only exists when the
     *    double tap has an action of its own;
     *  - `last_*` is the MEMORY of the previous tap, used to recognise a double
     *    even when nothing is being deferred (the position snap-back depends on
     *    it, and that is precisely the default case). */
    gesture_t  pending;
    int      pending_fingers;
    uint64_t pending_us;
    int      pending_x, pending_y;
    int      last_fingers;
    uint64_t last_us;          /* 0 = no tap remembered */
    int      last_x, last_y;
    /* Scrolling */
    int      accum;
    int      prev_center;
    int      scrolled;
} gestures_t;

static inline void gestures_defaults(gestures_config *c)
{
    c->tap_max_us      = 350000;
    c->long_min_us     = 500000;
    c->double_max_us   = 400000;
    c->tap_max_drift   = 40;
    c->double_max_dist = 70;
    c->notch_px        = 22;
    for (int i = 0; i < 4; i++) c->double_distinct[i] = 0;
}

static inline void gestures_init(gestures_t *g, const gestures_config *c)
{
    for (unsigned i = 0; i < sizeof *g; i++) ((unsigned char *)g)[i] = 0;
    g->cfg = *c;
}

static inline int gestures_abs_(int v) { return v < 0 ? -v : v; }

/* Cancels the gesture IN PROGRESS without emitting anything.
 *
 * Call this when the finger has NOT been lifted but leaves the observed area -
 * typically when it slides under the on-screen keyboard. Without it the caller
 * has to drop the finger count to zero, and the module reads that as a release:
 * but a short, barely-moved release IS a tap, hence a click sent to the remote
 * machine. A click nobody asked for.
 *
 * `last_us` is cleared too: an aborted gesture must not be usable as the first
 * half of a double tap. The tap ALREADY pending is left alone - it belongs to
 * the previous gesture, which is finished. */
static inline void gestures_cancel(gestures_t *g)
{
    g->prev_fingers = 0;
    g->max_fingers   = 0;
    g->long_sent    = 0;
    g->scrolled     = 0;
    g->accum        = 0;
    g->drift        = 0;
    g->last_us      = 0;
}

/* Emits the tap that was held pending. Factored out because two paths lead
 * here: the window expiring, and the start of a DIFFERENT gesture (putting three
 * fingers down right after a tap must not swallow the tap). */
static inline int gestures_flush_pending_(gestures_t *g, gesture_evt *out, int max, int n)
{
    if (g->pending == GESTURE_NONE || n >= max) return n;
    out[n].what    = g->pending;
    out[n].fingers  = g->pending_fingers;
    out[n].x       = g->pending_x;
    out[n].y       = g->pending_y;
    out[n].second = 0;
    g->pending = GESTURE_NONE;
    return n + 1;
}

/* One frame. `fingers` = 0..N, `x`/`y` = first finger, `center_y` = y of the
 * midpoint of the first two (ignored when fingers < 2). Returns the number of
 * events written; `notches` receives the wheel notch count (signed: positive =
 * wheel up). */
static inline int gestures_update(gestures_t *g, int fingers, int x, int y, int center_y,
                             uint64_t t_us, gesture_evt *out, int max, int *notches)
{
    int n = 0;
    if (notches) *notches = 0;
    if (fingers > 3) fingers = 3;        /* beyond that nothing is distinguishable */

    /* Start of a gesture */
    if (fingers > 0 && g->prev_fingers == 0) {
        /* A pending tap is NOT swallowed: if the new gesture turns out to be the
         * second tap it will be consumed below; otherwise it will expire
         * normally. */
        g->max_fingers  = fingers;
        g->start_us    = t_us;
        g->x0 = x; g->y0 = y;
        g->drift       = 0;
        g->long_sent   = 0;
        g->accum       = 0;
        g->scrolled    = 0;
        g->prev_center = center_y;
    }
    if (fingers > g->max_fingers) g->max_fingers = fingers;

    if (fingers > 0) {
        const int d = gestures_abs_(x - g->x0) + gestures_abs_(y - g->y0);
        if (d > g->drift) g->drift = d;

        /* Long press: emitted DURING the press, not on release - that is what
         * makes it usable for a drag (button held down). */
        if (!g->long_sent
                && (t_us - g->start_us) >= g->cfg.long_min_us
                && g->drift <= g->cfg.tap_max_drift
                && n < max) {
            n = gestures_flush_pending_(g, out, max, n);
            if (n < max) {
                out[n].what    = GESTURE_LONG(g->max_fingers);
                out[n].fingers  = g->max_fingers;
                out[n].x = x; out[n].y = y;
                out[n].second = 0;
                n++;
                g->long_sent = 1;
            }
        }

        /* Two-finger (or more) scrolling. The remainder of the accumulator is
         * KEPT from one frame to the next: a slow drag advances a few pixels per
         * frame and would be entirely lost by a bare division. */
        if (fingers >= 2 && g->prev_fingers >= 2 && g->cfg.notch_px > 0) {
            g->accum += center_y - g->prev_center;
            while (g->accum >= g->cfg.notch_px || g->accum <= -g->cfg.notch_px) {
                const int sgn = (g->accum > 0) ? 1 : -1;
                g->accum -= sgn * g->cfg.notch_px;
                /* Fingers moving DOWN: the content follows the finger, so we go
                 * UP through the document - wheel up. */
                if (notches) *notches += sgn;
                g->scrolled = 1;
            }
        }
        g->prev_center = center_y;
    }

    /* Release */
    if (fingers == 0 && g->prev_fingers > 0) {
        const uint64_t held = t_us - g->start_us;
        const int nd = g->max_fingers;
        if (!g->long_sent && !g->scrolled
                && held < g->cfg.tap_max_us
                && g->drift <= g->cfg.tap_max_drift
                && nd >= 1 && nd <= 3) {
            const int second = (g->last_us != 0
                    && g->last_fingers == nd
                    && (t_us - g->last_us) < g->cfg.double_max_us
                    && gestures_abs_(g->x0 - g->last_x) <= g->cfg.double_max_dist
                    && gestures_abs_(g->y0 - g->last_y) <= g->cfg.double_max_dist);
            if (second) {
                /* The pending tap is CANCELLED, not emitted: the double acts in
                 * its place. */
                g->pending = GESTURE_NONE;
                if (n < max) {
                    out[n].what    = g->cfg.double_distinct[nd]
                                   ? GESTURE_DOUBLE(nd) : GESTURE_TAP(nd);
                    out[n].fingers  = nd;
                    /* Position of the FIRST tap: this is the snap-back without
                     * which Windows does not see a double click (4x4 px). */
                    out[n].x       = g->last_x;
                    out[n].y       = g->last_y;
                    out[n].second = 1;
                    n++;
                }
                /* A THIRD tap starts over as a single: without this, three taps
                 * would give two overlapping doubles. */
                g->last_us = 0;
            } else {
                n = gestures_flush_pending_(g, out, max, n);
                if (g->cfg.double_distinct[nd]) {
                    /* The double has an action of its own: impossible to decide
                     * now. THIS is where the 400 ms of latency on the single tap
                     * are born, and nowhere else. */
                    g->pending         = GESTURE_TAP(nd);
                    g->pending_fingers = nd;
                    g->pending_us      = t_us;
                    g->pending_x = g->x0; g->pending_y = g->y0;
                } else if (n < max) {
                    /* Nothing specific to the double: emit RIGHT AWAY. */
                    out[n].what    = GESTURE_TAP(nd);
                    out[n].fingers  = nd;
                    out[n].x = g->x0; out[n].y = g->y0;
                    out[n].second = 0;
                    n++;
                }
                g->last_fingers = nd;
                g->last_us      = t_us;
                g->last_x = g->x0; g->last_y = g->y0;
            }
        } else {
            /* A gesture that is not a tap (long, drag, too slow): it must not
             * serve as the first half of a double either. */
            n = gestures_flush_pending_(g, out, max, n);
        }
        g->max_fingers = 0;
        g->long_sent  = 0;
    }

    /* Expiry of the double-tap window - checked EVERY FRAME, including with no
     * finger down: that is the only moment the deferred tap can come out. */
    if (g->pending != GESTURE_NONE && (t_us - g->pending_us) >= g->cfg.double_max_us)
        n = gestures_flush_pending_(g, out, max, n);

    g->prev_fingers = fingers;
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* UI_GESTURES_H */
