/* test_inject.c - INJ-1: the timing of synthetic input (devlink/inject.h).
 *
 * No console, no window, no Borealis: the clock is an argument, so the whole
 * state machine is replayable. What is checked here is exactly what cannot be
 * seen by looking at a screen - that a press outlasts a frame, that it ends,
 * and that it never erases the human holding the real controller.
 */
#include "../clients/borealis/devlink/inject.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;

#define CHECK(cond, what) do {                                              \
    checks++;                                                               \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* A frame of the render loop: zeroed state, then the injection ORed in - the
 * same order as the patched input manager. */
static bool frame(const inject_state_t *st, long long now, int button)
{
    bool b[INJECT_BUTTON_MAX];
    memset(b, 0, sizeof b);
    inject_sample(st, now, b, INJECT_BUTTON_MAX, NULL, 0, NULL, NULL);
    return b[button];
}

static void press_then_release(void)
{
    inject_state_t st;
    inject_clear(&st);

    CHECK(!frame(&st, 1000, 3), "nothing pressed at rest");

    inject_press(&st, 3, 1000, 100);
    CHECK(frame(&st, 1000, 3), "held from the instant of the request");
    CHECK(frame(&st, 1099, 3), "still held one ms before the end");
    CHECK(!frame(&st, 1100, 3), "released at the end, not after");
    CHECK(!frame(&st, 5000, 3), "and it STAYS released");
}

static void outlives_a_frame(void)
{
    inject_state_t st;
    inject_clear(&st);

    /* THE COUNTER-CASE THIS MODULE EXISTS FOR. Borealis reads controller STATE
     * once per frame: a press shorter than a frame falls between two reads and
     * is never seen. The audit of the shutdown screens already paid for it
     * (KB, AF2/AF6: "a press shorter than a frame is lost"). The floor is
     * therefore part of the contract, not a nicety. */
    inject_press(&st, 5, 0, 1);          /* one millisecond asked for */
    CHECK(frame(&st, 16, 5), "a 1 ms request still covers a 60 Hz frame");
    CHECK(frame(&st, 19, 5), "and reaches the floor of 20 ms");
    CHECK(!frame(&st, 20, 5), "without exceeding it");

    CHECK(inject_clamp_ms(0) == INJECT_MS_DEFAULT, "0 = the default");
    CHECK(inject_clamp_ms(-5) == INJECT_MS_DEFAULT, "a negative one too");
    CHECK(inject_clamp_ms(999999) == INJECT_MS_MAX,
            "an absurd duration is CAPPED: a forgotten command must not pin a"
            " button for the whole session");
}

static void a_hold_is_never_cut_short(void)
{
    inject_state_t st;
    inject_clear(&st);

    /* A script starts a 3 s hold (hold-to-exit), then something sends a short
     * tap on the same button. Taking the newest unconditionally would cut the
     * hold, and the symptom - "the hold never fires" - would be blamed on the
     * UI rather than on the tool driving it. */
    inject_press(&st, 7, 0, 3000);
    inject_press(&st, 7, 100, 20);
    CHECK(frame(&st, 2000, 7),
            "COUNTER-CASE: a short tap does not cut a hold already running");
    CHECK(!frame(&st, 3000, 7), "the hold still ends at its own time");

    /* The other direction: a longer request EXTENDS. */
    inject_clear(&st);
    inject_press(&st, 7, 0, 100);
    inject_press(&st, 7, 0, 500);
    CHECK(frame(&st, 400, 7), "a longer request extends");

    inject_release(&st, 7);
    CHECK(!frame(&st, 0, 7), "the explicit release is immediate");
}

static void never_erases_the_human(void)
{
    inject_state_t st;
    bool b[INJECT_BUTTON_MAX];
    float ax[INJECT_AXIS_MAX];
    inject_clear(&st);

    /* The real controller is already in the arrays when we are called: a script
     * driving the console must not stop a human from taking over. On a device
     * where both share one screen, that is not a nicety. */
    memset(b, 0, sizeof b);
    b[2] = true;                                /* the human holds button 2 */
    inject_press(&st, 9, 0, 100);
    inject_sample(&st, 10, b, INJECT_BUTTON_MAX, NULL, 0, NULL, NULL);
    CHECK(b[2] && b[9], "the injected press is ADDED to the real one, not substituted");

    memset(b, 0, sizeof b);
    inject_sample(&st, 10000, b, INJECT_BUTTON_MAX, NULL, 0, NULL, NULL);
    CHECK(!b[9], "once expired it adds nothing");

    /* Axes add and saturate, like the platform's own merge of two controllers. */
    memset(ax, 0, sizeof ax);
    ax[0] = 0.8f;
    inject_axis(&st, 0, 0.8f, 0, 100);
    inject_sample(&st, 10, NULL, 0, ax, INJECT_AXIS_MAX, NULL, NULL);
    CHECK(ax[0] > 0.99f && ax[0] <= 1.0f, "axes add and saturate at 1");

    inject_axis(&st, 1, -5.0f, 0, 100);
    memset(ax, 0, sizeof ax);
    inject_sample(&st, 10, NULL, 0, ax, INJECT_AXIS_MAX, NULL, NULL);
    CHECK(ax[1] >= -1.0f, "an out-of-range value is clamped on the way in");
}

static void the_stick_replaces(void)
{
    inject_state_t st;
    float ax[INJECT_AXIS_MAX];
    inject_clear(&st);

    /* Unlike a button, a stick has ONE position: the last order wins. Adding
     * them would make two successive moves land somewhere neither asked for. */
    inject_axis(&st, 0, 1.0f, 0, 1000);
    inject_axis(&st, 0, -1.0f, 0, 1000);
    memset(ax, 0, sizeof ax);
    inject_sample(&st, 10, NULL, 0, ax, INJECT_AXIS_MAX, NULL, NULL);
    CHECK(ax[0] < -0.99f, "the stick REPLACES: the last order wins");
}

static void the_swipe(void)
{
    inject_state_t st;
    float x = -1, y = -1;
    inject_clear(&st);

    CHECK(!inject_sample(&st, 0, NULL, 0, NULL, 0, &x, &y), "no touch at rest");

    inject_touch(&st, 100, 200, 300, 400, 1000, 200);
    CHECK(inject_sample(&st, 1000, NULL, 0, NULL, 0, &x, &y)
            && x == 100 && y == 200, "starts at the first point");
    CHECK(inject_sample(&st, 1100, NULL, 0, NULL, 0, &x, &y)
            && x > 199 && x < 201 && y > 299 && y < 301, "halfway, halfway");
    CHECK(!inject_sample(&st, 1200, NULL, 0, NULL, 0, &x, &y), "and it ends");

    /* A tap is the same path with both points equal - one mechanism, not two. */
    inject_touch(&st, 640, 360, 640, 360, 0, 50);
    CHECK(inject_sample(&st, 25, NULL, 0, NULL, 0, &x, &y) && x == 640 && y == 360,
            "a tap is a swipe that does not move");
}

static void bounds_and_absurdities(void)
{
    inject_state_t st;
    inject_clear(&st);

    CHECK(!inject_press(&st, -1, 0, 100), "a negative index is refused");
    CHECK(!inject_press(&st, INJECT_BUTTON_MAX, 0, 100), "an out-of-range index is refused");
    CHECK(!inject_press(NULL, 0, 0, 100), "a null state is refused");
    CHECK(!inject_axis(&st, INJECT_AXIS_MAX, 0.5f, 0, 100), "an out-of-range axis is refused");
    CHECK(!inject_busy(&st, 0), "nothing asked for, nothing busy");

    inject_press(&st, 0, 0, 100);
    CHECK(inject_busy(&st, 50), "busy while something is held");
    CHECK(!inject_busy(&st, 100), "and idle again afterwards");

    /* `inject_busy` is what lets the hook cost one comparison per frame on a
     * session with no script attached. If it ever said "busy" at rest, every
     * frame would pay for a feature nobody is using. */
    inject_clear(&st);
    CHECK(!inject_busy(&st, 1000000), "at rest, whatever the clock");

    /* A sample bounded by the caller's array must not read our own further. */
    {
        bool small[2] = { false, false };
        inject_press(&st, 10, 0, 100);
        inject_sample(&st, 10, small, 2, NULL, 0, NULL, NULL);
        CHECK(!small[0] && !small[1], "a short array is not written past");
    }
}

int main(void)
{
    printf("== INJ-1: the timing of synthetic input ==\n");
    press_then_release();
    outlives_a_frame();
    a_hold_is_never_cut_short();
    never_erases_the_human();
    the_stick_replaces();
    the_swipe();
    bounds_and_absurdities();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
