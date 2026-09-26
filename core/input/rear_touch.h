/* rear_touch.h - the PS Vita's rear touchpad as the four buttons it lacks.
 *
 * === WHAT THIS IS FOR =================================================
 *
 * A Vita has no ZL, no ZR, and no stick clicks. Those four are exactly what
 * `pad_map.hpp` lists and the console cannot produce, so half a PC game's
 * bindings are unreachable - aim-down-sights, sprint, crouch-toggle, melee.
 * The rear panel is right under the fingers that would press them.
 *
 * === WHY IT IS ARITHMETIC IN A HEADER, AND NOT CODE IN THE VIEW =======
 *
 * Every hard part of "a touchpad is not a button" is arithmetic: what counts
 * as a tap rather than a rest, how far a finger must travel before a trigger
 * reads full, which of two fingers is which. None of it needs a console to be
 * checked, and all of it is the kind of thing that is wrong in a way you only
 * notice mid-game. `tests/test_rear_touch.c` pins each rule with the case that
 * motivated it, the same way `pad_mouse.h` does for the stick-as-mouse.
 *
 * Time is a PARAMETER, never read inside. That is what lets a test replay a
 * 300 ms hold in a loop with no sleeping, and it is why the module can be
 * exercised at all.
 *
 * The state is the CALLER'S. No `static` lives here: session state in a
 * function `static` is the defect family this repository has paid for four
 * times over, and a gesture recogniser is session state by definition.
 *
 * === THE FOUR RULES, AND WHY NONE IS OPTIONAL =========================
 *
 * 1. FINGERS REST ON THIS PANEL. It is where the hands hold the console, so a
 *    zone that fires on contact fires while you are simply holding the thing.
 *    That is why the default gesture is a TAP - touch and release, briefly,
 *    without travelling - and why only the two TOP zones are mapped by
 *    default: the lower half is where the palms sit.
 *
 * 2. A TAP IS BOUNDED IN BOTH TIME AND SPACE. Time alone would turn a quick
 *    swipe into a button press; space alone would turn a long rest into one.
 *    Both bounds, or the recogniser fires on things the user did not do.
 *
 * 3. AN ANALOG TRIGGER MEASURES TRAVEL, NOT POSITION. Reading the absolute
 *    finger position would mean the trigger jumps to whatever value the finger
 *    happens to land on. Travel from the touch-down point starts every pull at
 *    zero, which is what a physical trigger does.
 *
 * 4. A PRESS THAT STARTS MUST END. A finger lifted outside its zone, a touch
 *    lost because the panel dropped a frame, the app losing focus - each one
 *    would leave a button held on the VM for ever. Anything this module
 *    reports as pressed, it also reports released when the finger is gone,
 *    and `rear_touch_release_all` exists for the cases that never get there.
 */
#ifndef REAR_TOUCH_H
#define REAR_TOUCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The panel, in fractions of its active area. Normalising here keeps the module
 * independent of `SceTouchPanelInfo`, which differs between the front and rear
 * panels and between models - the caller converts once, and the arithmetic
 * below never has to know. */
typedef struct {
    float x, y;          /* 0..1, origin top-left as the panel reports it */
    uint8_t id;          /* the panel's finger id: what makes tracking possible */
} rear_point;

#define REAR_MAX_POINTS 4        /* the panel reports at most this many */

/* The four quadrants. Top/bottom is the meaningful split: the top is reached
 * deliberately with the index fingers, the bottom is where the hands rest. */
typedef enum {
    REAR_ZONE_TL = 0,
    REAR_ZONE_TR,
    REAR_ZONE_BL,
    REAR_ZONE_BR,
    REAR_ZONE_COUNT
} rear_zone;

/* What a zone produces. The digital ones emit a button; the analog ones drive
 * an axis, because Shadow carries L2/R2 as axes and not as buttons. */
typedef enum {
    REAR_ACT_NONE = 0,
    REAR_ACT_ZL,             /* digital, on tap or hold */
    REAR_ACT_ZR,
    REAR_ACT_L3,
    REAR_ACT_R3,
    REAR_ACT_ZL_ANALOG,      /* travel drives the L2 axis */
    REAR_ACT_ZR_ANALOG,
    REAR_ACT_COUNT
} rear_action;

/* How a zone reacts. */
typedef enum {
    REAR_GEST_TAP = 0,       /* touch and release, brief and still */
    REAR_GEST_HOLD,          /* pressed for as long as the finger is down */
    REAR_GEST_SLIDE          /* analog: travel from the touch-down point */
} rear_gesture;

typedef struct {
    rear_action  action[REAR_ZONE_COUNT];
    rear_gesture gesture[REAR_ZONE_COUNT];

    /* A tap must be shorter than this and travel less than `tap_slop`. */
    uint32_t tap_ms;
    float    tap_slop;       /* fraction of the panel */

    /* How long a tap-produced press is held, so the VM sees a real press even
     * though the finger is already gone. Shorter than a frame and a game that
     * samples at 60 Hz can miss it entirely. */
    uint32_t tap_hold_ms;

    /* Travel that reads as a full pull, as a fraction of the panel. */
    float    slide_full;

    /* Two fingers put down together, briefly: one more gesture, because L3 and
     * R3 are clicks and a click is what two fingers naturally do. */
    rear_action two_finger;
    uint32_t    two_finger_ms;   /* both must land within this of each other */

    bool enabled;
} rear_touch_config;

/* Per-finger tracking. The panel's own id is what ties a moving contact to the
 * one that landed - matching by proximity instead would swap two fingers that
 * cross, and swap the buttons with them. */
typedef struct {
    bool     active;
    uint8_t  id;
    rear_zone zone;
    float    x0, y0;         /* where it landed */
    uint32_t t0;             /* when it landed */
    bool     moved;          /* travelled past tap_slop: no longer a tap */
    bool     emitted;        /* a HOLD press is out; it must be released */
} rear_finger;

typedef struct {
    rear_finger finger[REAR_MAX_POINTS];
    /* A tap emits a press that outlives the finger. Zero means idle. */
    uint32_t    tap_until_ms[REAR_ACT_COUNT];
    /* Two-finger detection: when the first of a pair landed. */
    uint32_t    pair_t0;
    bool        pair_fired;
} rear_touch_state;

/* What the module produces for one frame. */
typedef struct {
    bool    zl, zr, l3, r3;  /* digital, already debounced */
    uint8_t l2, r2;          /* 0..255, the analog axes */
} rear_touch_out;

/* Sensible defaults: the two top zones give the two triggers, analog, because
 * that is what the panel does better than a button ever could. The bottom two
 * are LEFT UNMAPPED - see rule 1, that is where the hands rest. Two fingers
 * give R3, the click a right stick would carry. */
static inline void rear_touch_defaults(rear_touch_config *c)
{
    if (!c) return;
    for (int i = 0; i < REAR_ZONE_COUNT; i++) {
        c->action[i]  = REAR_ACT_NONE;
        c->gesture[i] = REAR_GEST_TAP;
    }
    c->action[REAR_ZONE_TL]  = REAR_ACT_ZL_ANALOG;
    c->gesture[REAR_ZONE_TL] = REAR_GEST_SLIDE;
    c->action[REAR_ZONE_TR]  = REAR_ACT_ZR_ANALOG;
    c->gesture[REAR_ZONE_TR] = REAR_GEST_SLIDE;
    c->tap_ms        = 220;
    c->tap_slop      = 0.06f;
    c->tap_hold_ms   = 90;
    c->slide_full    = 0.28f;
    c->two_finger    = REAR_ACT_R3;
    c->two_finger_ms = 140;
    c->enabled       = false;   /* opt-in: it changes what the pad does */
}

static inline rear_zone rear_touch_zone_of(float x, float y)
{
    const int right = (x >= 0.5f);
    const int bottom = (y >= 0.5f);
    return (rear_zone)((bottom ? 2 : 0) + (right ? 1 : 0));
}

static inline void rear_touch_reset(rear_touch_state *s)
{
    if (!s) return;
    for (int i = 0; i < REAR_MAX_POINTS; i++) s->finger[i].active = false;
    for (int i = 0; i < REAR_ACT_COUNT; i++)  s->tap_until_ms[i] = 0;
    s->pair_t0 = 0;
    s->pair_fired = false;
}

static inline float rear_touch_dist(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;          /* squared: no sqrt needed to compare */
}

static inline void rear_touch_apply(rear_action a, bool on, uint8_t value,
                                    rear_touch_out *o)
{
    switch (a) {
        case REAR_ACT_ZL: if (on) o->zl = true; break;
        case REAR_ACT_ZR: if (on) o->zr = true; break;
        case REAR_ACT_L3: if (on) o->l3 = true; break;
        case REAR_ACT_R3: if (on) o->r3 = true; break;
        case REAR_ACT_ZL_ANALOG: if (value > o->l2) o->l2 = value; break;
        case REAR_ACT_ZR_ANALOG: if (value > o->r2) o->r2 = value; break;
        default: break;
    }
}

/* One frame. `pts`/`n` is what the panel reports now; `now_ms` is a monotonic
 * millisecond clock the CALLER owns. Returns what the pad should send. */
static inline rear_touch_out rear_touch_step(const rear_touch_config *c,
                                             rear_touch_state *s,
                                             const rear_point *pts, size_t n,
                                             uint32_t now_ms)
{
    rear_touch_out o;
    o.zl = o.zr = o.l3 = o.r3 = false;
    o.l2 = o.r2 = 0;
    if (!c || !s || !c->enabled) { if (s) rear_touch_reset(s); return o; }
    if (n > REAR_MAX_POINTS) n = REAR_MAX_POINTS;

    /* --- retire the fingers that are gone, emitting their taps ----------- */
    for (int i = 0; i < REAR_MAX_POINTS; i++) {
        rear_finger *f = &s->finger[i];
        if (!f->active) continue;
        bool still = false;
        for (size_t k = 0; k < n; k++)
            if (pts[k].id == f->id) { still = true; break; }
        if (still) continue;

        /* Lifted. A brief, still contact is a tap; anything else is not, and
         * rule 4 says a HOLD that started must end - which it does simply by
         * this finger no longer being active. */
        const rear_action act = c->action[f->zone];
        if (c->gesture[f->zone] == REAR_GEST_TAP && !f->moved
            && (now_ms - f->t0) <= c->tap_ms && act != REAR_ACT_NONE)
            s->tap_until_ms[act] = now_ms + c->tap_hold_ms;
        f->active = false;
    }

    /* --- take up the new ones -------------------------------------------- */
    for (size_t k = 0; k < n; k++) {
        bool known = false;
        for (int i = 0; i < REAR_MAX_POINTS; i++)
            if (s->finger[i].active && s->finger[i].id == pts[k].id) { known = true; break; }
        if (known) continue;
        for (int i = 0; i < REAR_MAX_POINTS; i++) {
            rear_finger *f = &s->finger[i];
            if (f->active) continue;
            f->active = true;
            f->id     = pts[k].id;
            f->zone   = rear_touch_zone_of(pts[k].x, pts[k].y);
            f->x0     = pts[k].x;
            f->y0     = pts[k].y;
            f->t0     = now_ms;
            f->moved  = false;
            f->emitted = false;
            break;
        }
    }

    /* --- what the fingers currently down are producing --------------------- */
    int down = 0;
    for (size_t k = 0; k < n; k++) {
        rear_finger *f = NULL;
        for (int i = 0; i < REAR_MAX_POINTS; i++)
            if (s->finger[i].active && s->finger[i].id == pts[k].id) { f = &s->finger[i]; break; }
        if (!f) continue;
        down++;

        const float slop2 = c->tap_slop * c->tap_slop;
        if (rear_touch_dist(pts[k].x, pts[k].y, f->x0, f->y0) > slop2) f->moved = true;

        const rear_action act = c->action[f->zone];
        if (act == REAR_ACT_NONE) continue;

        switch (c->gesture[f->zone]) {
            case REAR_GEST_HOLD:
                f->emitted = true;
                rear_touch_apply(act, true, 255, &o);
                break;
            case REAR_GEST_SLIDE: {
                /* Rule 3: TRAVEL, not position. Downward travel pulls, which
                 * matches the way a finger curls on the back of the console. */
                float d = pts[k].y - f->y0;
                if (d < 0.0f) d = -d;
                const float full = c->slide_full > 0.001f ? c->slide_full : 0.28f;
                float u = d / full;
                if (u > 1.0f) u = 1.0f;
                rear_touch_apply(act, u > 0.0f, (uint8_t)(u * 255.0f + 0.5f), &o);
                break;
            }
            case REAR_GEST_TAP:
            default:
                break;   /* a tap is emitted on release, above */
        }
    }

    /* --- two fingers, landed together ------------------------------------- */
    if (down >= 2 && c->two_finger != REAR_ACT_NONE) {
        uint32_t first = 0, second = 0;
        int seen = 0;
        for (int i = 0; i < REAR_MAX_POINTS; i++) {
            if (!s->finger[i].active) continue;
            const uint32_t t = s->finger[i].t0;
            if (seen == 0) { first = second = t; }
            else { if (t < first) first = t; if (t > second) second = t; }
            seen++;
        }
        if (seen >= 2 && (second - first) <= c->two_finger_ms) {
            if (!s->pair_fired) {
                s->pair_fired = true;
                s->tap_until_ms[c->two_finger] = now_ms + c->tap_hold_ms;
            }
        }
    } else if (down < 2) {
        s->pair_fired = false;
    }

    /* --- taps still being held out ---------------------------------------- */
    for (int a = 0; a < REAR_ACT_COUNT; a++) {
        if (!s->tap_until_ms[a]) continue;
        /* Signed compare: a tap whose deadline has passed is done. */
        if ((int32_t)(now_ms - s->tap_until_ms[a]) >= 0) { s->tap_until_ms[a] = 0; continue; }
        rear_touch_apply((rear_action)a, true, 255, &o);
    }
    return o;
}

/* Rule 4's escape hatch: everything released, nothing pending. For losing
 * focus, suspending forwarding, or ending a session. */
static inline rear_touch_out rear_touch_release_all(rear_touch_state *s)
{
    rear_touch_out o;
    o.zl = o.zr = o.l3 = o.r3 = false;
    o.l2 = o.r2 = 0;
    rear_touch_reset(s);
    return o;
}

#ifdef __cplusplus
}
#endif
#endif /* REAR_TOUCH_H */
