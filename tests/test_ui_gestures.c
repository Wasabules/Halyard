/* test_ui_gestures - touch gesture recognition (ui/gestures.h).
 *
 * Each check names the COUNTER-CASE: the input that would break the gesture if
 * the rule were dropped. This module decides what each finger on the screen does
 * during a game; a regression here is invisible on reading and obvious in use.
 */
#include "../clients/borealis/ui/gestures.h"
#include <stdio.h>

static int failures = 0, checks = 0;
#define OK(cond, what) do {                                                   \
    checks++;                                                                 \
    if (!(cond)) { printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, what);  \
                   failures++; }                                                \
} while (0)

/* One frame: returns the number of events and fills `ev`. */
static int img(gestures_t *g, int d, int x, int y, uint64_t t,
               gesture_evt *ev, int *crans)
{
    int c = 0;
    const int n = gestures_update(g, d, x, y, y, t, ev, 4, &c);
    if (crans) *crans = c;
    return n;
}

static void cfg_simple(gestures_config *c, int distinct1)
{
    gestures_defaults(c);
    c->double_distinct[1] = distinct1;
}

int main(void)
{
    printf("== touch gestures ==\n");
    gesture_evt ev[4];
    int crans;

    /* --- Tape simple ------------------------------------------------------ */
    {
        gestures_config c; cfg_simple(&c, 0);
        gestures_t g; gestures_init(&g, &c);
        OK(img(&g, 1, 100, 100,      0, ev, 0) == 0, "poser n'emet rien");
        OK(img(&g, 0,   0,   0, 100000, ev, 0) == 1, "releasing emits the tap");
        OK(ev[0].what == GESTURE_TAP_1, "a one-finger tap");
        OK(ev[0].x == 100 && ev[0].y == 100,
           "position = the one at TOUCH DOWN; at n==0 the driver returns nothing, "
           "reading the position on the release frame would give 0,0");
        OK(ev[0].second == 0, "first tap");
    }

    /* --- Too long, or too much drift: no longer a tap ---------------------- */
    {
        gestures_config c; cfg_simple(&c, 0);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 1, 100, 100, 0, ev, 0);
        OK(img(&g, 0, 0, 0, 400000, ev, 0) == 0,
           "400 ms > tap_max: no tap (otherwise a resting press becomes a click)");

        gestures_init(&g, &c);
        img(&g, 1, 100, 100, 0, ev, 0);
        img(&g, 1, 200, 100, 50000, ev, 0);
        OK(img(&g, 0, 0, 0, 100000, ev, 0) == 0,
           "100 px of drift: that is a slide, not a tap");
    }

    /* --- The finger count FLICKERS (the central counter-case) -------------- */
    {
        gestures_config c; gestures_defaults(&c);
        gestures_t g; gestures_init(&g, &c);
        /* The HID driver returns 1, 2, 1, 2 while the second finger is going down. */
        img(&g, 1, 100, 100,     0, ev, 0);
        img(&g, 2, 100, 100, 10000, ev, 0);
        img(&g, 1, 100, 100, 20000, ev, 0);
        img(&g, 2, 100, 100, 30000, ev, 0);
        const int n = img(&g, 0, 0, 0, 60000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_TAP_2,
           "classified on the MAXIMUM number of fingers seen; on the instantaneous count the "
           "gesture would end as TAP_1 and give a left click instead of a right one");
    }

    /* --- Double-tape SANS action propre : emission immediate --------------- */
    {
        gestures_config c; cfg_simple(&c, 0);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 1, 100, 100, 0, ev, 0);
        OK(img(&g, 0, 0, 0, 50000, ev, 0) == 1, "the 1st tap is emitted RIGHT AWAY");
        OK(ev[0].what == GESTURE_TAP_1, "a single tap");
        /* seconde tape 200 ms plus tard, 12 px a cote */
        img(&g, 1, 112, 106, 250000, ev, 0);
        const int n = img(&g, 0, 0, 0, 300000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_TAP_1, "2nd tap: still a tap");
        OK(ev[0].second == 1, "marked as the second");
        OK(ev[0].x == 100 && ev[0].y == 100,
           "SNAPPED BACK to the first: without that the two clicks are 12 px apart and "
           "Windows (4x4 px) does not see a double click - the reported defect");
    }

    /* --- Double tap WITH an action of its own: the single one is DEFERRED --- */
    {
        gestures_config c; cfg_simple(&c, 1);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 1, 100, 100, 0, ev, 0);
        OK(img(&g, 0, 0, 0, 50000, ev, 0) == 0,
           "nothing on release: we cannot know whether a double is coming");
        img(&g, 1, 104, 102, 200000, ev, 0);
        const int n = img(&g, 0, 0, 0, 240000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_DOUBLE_1, "the double comes out, the single one is cancelled");
        OK(ev[0].second == 1, "seconde");
    }
    {
        gestures_config c; cfg_simple(&c, 1);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 1, 100, 100, 0, ev, 0);
        img(&g, 0, 0, 0, 50000, ev, 0);
        OK(img(&g, 0, 0, 0, 300000, ev, 0) == 0, "still inside the window");
        const int n = img(&g, 0, 0, 0, 460000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_TAP_1,
           "the window expires WITH no finger on screen: that is the only moment the "
           "deferred tap can come out; checking only on touch would lose it");
    }

    /* --- Three taps do not make two double-taps ---------------------------- */
    {
        gestures_config c; cfg_simple(&c, 1);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 1, 100, 100,      0, ev, 0);
        img(&g, 0,   0,   0,  50000, ev, 0);
        img(&g, 1, 100, 100, 200000, ev, 0);
        int n = img(&g, 0, 0, 0, 240000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_DOUBLE_1, "1er double");
        img(&g, 1, 100, 100, 400000, ev, 0);
        n = img(&g, 0, 0, 0, 440000, ev, 0);
        OK(n == 0, "3rd tap: reset, it becomes a deferred single again");
        n = img(&g, 0, 0, 0, 900000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_TAP_1, "and comes out as a single one when the window expires");
    }

    /* --- Two taps too FAR APART do not make a double-tap ------------------- */
    {
        gestures_config c; cfg_simple(&c, 1);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 1, 100, 100, 0, ev, 0);
        img(&g, 0, 0, 0, 50000, ev, 0);
        img(&g, 1, 400, 100, 200000, ev, 0);   /* 300 px plus loin */
        const int n = img(&g, 0, 0, 0, 240000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_TAP_1 && ev[0].second == 0,
           "the 1st deferred tap comes out, the 2nd starts over - two clicks far "
           "apart are not a double click");
    }

    /* --- Appui long : emis PENDANT l'appui --------------------------------- */
    {
        gestures_config c; gestures_defaults(&c);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 1, 100, 100, 0, ev, 0);
        OK(img(&g, 1, 100, 100, 300000, ev, 0) == 0, "not long yet");
        const int n = img(&g, 1, 101, 100, 520000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_LONG_1,
           "emitted DURING the press, not on release: that is what makes it possible to "
           "hold the button down for a drag");
        OK(img(&g, 1, 101, 100, 700000, ev, 0) == 0, "once only");
        OK(img(&g, 0, 0, 0, 900000, ev, 0) == 0,
           "and NO tap on release: otherwise a drag would end in a click");
    }

    /* --- Defilement -------------------------------------------------------- */
    {
        gestures_config c; gestures_defaults(&c);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 2, 100, 100, 0, ev, &crans);
        OK(crans == 0, "1st two-finger frame: no notch (no reference)");
        img(&g, 2, 100, 122, 20000, ev, &crans);
        OK(crans == 1, "22 px downwards = one notch upwards");
        img(&g, 2, 100, 133, 40000, ev, &crans);
        OK(crans == 0, "11 px: below the notch, nothing");
        img(&g, 2, 100, 144, 60000, ev, &crans);
        OK(crans == 1,
           "11 + 11: the REMAINDER is kept between frames; reset to zero, a "
           "slow slide would never scroll");
        img(&g, 2, 100,  90, 80000, ev, &crans);
        OK(crans == -2, "a clear upward move: two notches downwards");
        const int n = img(&g, 0, 0, 0, 100000, ev, 0);
        OK(n == 0,
           "a gesture that SCROLLED emits no tap on release, otherwise "
           "every scroll would end in a right click");
    }

    /* --- Bornes ------------------------------------------------------------ */
    {
        gestures_config c; gestures_defaults(&c);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 7, 100, 100, 0, ev, 0);       /* main a plat */
        const int n = img(&g, 0, 0, 0, 50000, ev, 0);
        OK(n == 1 && ev[0].fingers == 3,
           "beyond 3 fingers we clamp to 3: without that GESTURE_TAP(7) would fall "
           "outside the settings table");
        OK(GESTURE_TAP(3) < GESTURE_COUNT && GESTURE_LONG(3) < GESTURE_COUNT,
           "the index macros stay inside the table");
    }

    /* === CROSSING A BOUNDARY IS NOT A RELEASE ===
     *
     * When the on-screen keyboard opens, the caller removes from the count the
     * fingers that go under its strip. The module then sees fingers == 0 while
     * the finger is STILL down - and a short, barely-moved release is a tap,
     * hence a left click sent to the remote machine. Nobody asked for it, and it
     * was intermittent: a slow crossing (> tap_max) did not click. */
    {
        gestures_config c; cfg_simple(&c, 0);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 1, 100, 100,     0, ev, 0);
        img(&g, 1, 100, 120, 50000, ev, 0);
        /* the finger crosses: the caller cancels BEFORE returning a zero count */
        gestures_cancel(&g);
        const int n = img(&g, 0, 0, 0, 90000, ev, 0);
        OK(n == 0,
           "COUNTER-CASE: after gestures_cancel, a zero count emits NOTHING - "
           "without that a finger sliding towards the keyboard clicks in the VM");
    }
    {   /* and the next gesture starts cleanly, dragging no state along */
        gestures_config c; cfg_simple(&c, 0);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 1, 100, 100, 0, ev, 0);
        gestures_cancel(&g);
        img(&g, 0, 0, 0, 50000, ev, 0);
        img(&g, 1, 300, 300, 100000, ev, 0);
        const int n = img(&g, 0, 0, 0, 140000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_TAP_1 && ev[0].second == 0,
           "the next gesture is a SINGLE tap: an aborted gesture must not "
           "serve as the first half of a double");
        OK(ev[0].x == 300 && ev[0].y == 300, "and it carries its own position");
    }
    {   /* a cancelled two-finger gesture does not leave max_fingers at 2 */
        gestures_config c; gestures_defaults(&c);
        gestures_t g; gestures_init(&g, &c);
        img(&g, 2, 100, 100, 0, ev, 0);
        gestures_cancel(&g);
        img(&g, 0, 0, 0, 30000, ev, 0);
        img(&g, 1, 100, 100, 60000, ev, 0);
        const int n = img(&g, 0, 0, 0, 90000, ev, 0);
        OK(n == 1 && ev[0].what == GESTURE_TAP_1,
           "COUNTER-CASE: max_fingers is reset - otherwise a finger flickering "
           "on the boundary locks in 2 fingers and the next tap comes out as "
           "clic DROIT");
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
