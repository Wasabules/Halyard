/* test_rear_touch.c - the rear touchpad as the four buttons a Vita lacks.
 *
 * Every check here is a rule from `core/input/rear_touch.h`, and the ones
 * labelled COUNTER-CASE are the mistakes the design exists to avoid. They
 * matter more than the positive checks: a gesture recogniser that fires when it
 * should is easy, one that stays quiet when it should is the whole problem.
 *
 * Time is passed in, so a 300 ms hold costs nothing to replay.
 */
#include "../core/input/rear_touch.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
        checks++;                                                             \
        if (!(cond)) { failures++;                                            \
            printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); }      \
    } while (0)

static rear_touch_config cfg(void)
{
    rear_touch_config c;
    rear_touch_defaults(&c);
    c.enabled = true;
    return c;
}

/* One finger, one frame. */
static rear_touch_out one(const rear_touch_config *c, rear_touch_state *s,
                          float x, float y, uint32_t t)
{
    rear_point p = { x, y, 1 };
    return rear_touch_step(c, s, &p, 1, t);
}
static rear_touch_out none_(const rear_touch_config *c, rear_touch_state *s,
                            uint32_t t)
{
    return rear_touch_step(c, s, NULL, 0, t);
}

static void zones(void)
{
    printf("-- the four quadrants\n");
    CHECK(rear_touch_zone_of(0.2f, 0.2f) == REAR_ZONE_TL, "top-left");
    CHECK(rear_touch_zone_of(0.8f, 0.2f) == REAR_ZONE_TR, "top-right");
    CHECK(rear_touch_zone_of(0.2f, 0.8f) == REAR_ZONE_BL, "bottom-left");
    CHECK(rear_touch_zone_of(0.8f, 0.8f) == REAR_ZONE_BR, "bottom-right");
    /* The boundary belongs to the second half, consistently, so a finger on the
     * line does not flicker between two buttons. */
    CHECK(rear_touch_zone_of(0.5f, 0.5f) == REAR_ZONE_BR, "the exact middle goes one way, always");
}

static void defaults_are_safe(void)
{
    printf("-- the defaults do not fire on hands that merely hold the console\n");
    rear_touch_config c; rear_touch_defaults(&c);
    CHECK(!c.enabled, "off until asked for: it changes what the pad does");
    CHECK(c.action[REAR_ZONE_BL] == REAR_ACT_NONE
       && c.action[REAR_ZONE_BR] == REAR_ACT_NONE,
          "RULE 1: the lower half is unmapped - that is where the palms rest");
    CHECK(c.action[REAR_ZONE_TL] != REAR_ACT_NONE
       && c.action[REAR_ZONE_TR] != REAR_ACT_NONE, "the top two are the useful ones");

    /* Disabled must produce nothing whatever the finger does. */
    rear_touch_state s; memset(&s, 0, sizeof s);
    rear_touch_out o = one(&c, &s, 0.2f, 0.2f, 1000);
    CHECK(!o.zl && !o.zr && !o.l3 && !o.r3 && !o.l2 && !o.r2,
          "COUNTER-CASE: disabled emits nothing at all");
}

static void tap(void)
{
    printf("-- a tap: brief AND still\n");
    rear_touch_config c = cfg();
    c.action[REAR_ZONE_BL]  = REAR_ACT_L3;
    c.gesture[REAR_ZONE_BL] = REAR_GEST_TAP;
    rear_touch_state s; memset(&s, 0, sizeof s);

    /* Down, then up 100 ms later without moving. */
    rear_touch_out o = one(&c, &s, 0.2f, 0.8f, 1000);
    CHECK(!o.l3, "nothing while the finger is still down: a tap fires on release");
    o = none_(&c, &s, 1100);
    CHECK(o.l3, "released inside the window and still: the button fires");
    /* It must outlive the finger, or a game sampling at 60 Hz misses it. */
    o = none_(&c, &s, 1100 + c.tap_hold_ms - 10);
    CHECK(o.l3, "still held a frame later - a 1-frame press would be missed");
    o = none_(&c, &s, 1100 + c.tap_hold_ms + 1);
    CHECK(!o.l3, "and released once the hold has elapsed");

    /* COUNTER-CASE: too slow. */
    memset(&s, 0, sizeof s);
    one(&c, &s, 0.2f, 0.8f, 2000);
    o = none_(&c, &s, 2000 + c.tap_ms + 50);
    CHECK(!o.l3, "COUNTER-CASE: a long rest is not a tap - RULE 2, the time bound");

    /* COUNTER-CASE: quick but travelled. Both bounds, or a swipe becomes a press. */
    memset(&s, 0, sizeof s);
    one(&c, &s, 0.2f, 0.8f, 3000);
    one(&c, &s, 0.2f, 0.95f, 3050);          /* well past tap_slop */
    o = none_(&c, &s, 3100);
    CHECK(!o.l3, "COUNTER-CASE: a quick SWIPE is not a tap - RULE 2, the space bound");
}

static void hold(void)
{
    printf("-- a hold: pressed exactly as long as the finger is\n");
    rear_touch_config c = cfg();
    c.action[REAR_ZONE_TL]  = REAR_ACT_ZL;
    c.gesture[REAR_ZONE_TL] = REAR_GEST_HOLD;
    rear_touch_state s; memset(&s, 0, sizeof s);

    rear_touch_out o = one(&c, &s, 0.2f, 0.2f, 1000);
    CHECK(o.zl, "down: pressed at once, no waiting");
    o = one(&c, &s, 0.2f, 0.2f, 5000);
    CHECK(o.zl, "still pressed four seconds later");
    o = none_(&c, &s, 5010);
    CHECK(!o.zl, "RULE 4: the finger is gone, so is the press");
}

static void slide(void)
{
    printf("-- an analog trigger: TRAVEL, not position\n");
    rear_touch_config c = cfg();          /* top zones are SLIDE by default */
    rear_touch_state s; memset(&s, 0, sizeof s);

    /* Landing far down the panel must NOT read as a pulled trigger. */
    rear_touch_out o = one(&c, &s, 0.2f, 0.45f, 1000);
    CHECK(o.l2 == 0, "RULE 3, COUNTER-CASE: landing low does not pre-pull the trigger");

    o = one(&c, &s, 0.2f, 0.45f + c.slide_full * 0.5f, 1050);
    CHECK(o.l2 > 100 && o.l2 < 160, "half the travel reads about half");

    o = one(&c, &s, 0.2f, 0.45f + c.slide_full, 1100);
    CHECK(o.l2 == 255, "the full travel reads full");

    o = one(&c, &s, 0.2f, 0.45f + c.slide_full * 3.0f, 1150);
    CHECK(o.l2 == 255, "and it saturates rather than wrapping");

    /* Travel in the other direction pulls too: a finger curls either way
     * depending on how the console is held. */
    memset(&s, 0, sizeof s);
    one(&c, &s, 0.7f, 0.40f, 2000);
    o = one(&c, &s, 0.7f, 0.40f - c.slide_full, 2050);
    CHECK(o.r2 == 255, "travelling the other way pulls the same");

    o = none_(&c, &s, 2100);
    CHECK(o.r2 == 0, "RULE 4: lifted means back to zero, not stuck open");
}

static void two_fingers(void)
{
    printf("-- two fingers together: the click a right stick would carry\n");
    rear_touch_config c = cfg();
    rear_touch_state s; memset(&s, 0, sizeof s);

    rear_point p[2] = { { 0.2f, 0.8f, 1 }, { 0.8f, 0.8f, 2 } };
    rear_touch_out o = rear_touch_step(&c, &s, p, 2, 1000);
    CHECK(o.r3, "both down within the window: R3 fires");

    /* Once, not once per frame. */
    o = rear_touch_step(&c, &s, p, 2, 1010);
    const bool still = o.r3;   /* the hold window keeps it out, which is correct */
    CHECK(still, "and stays out for the hold window");

    /* COUNTER-CASE: fingers landing far apart in TIME are two separate
     * gestures, not a click. */
    memset(&s, 0, sizeof s);
    rear_point a1[1] = { { 0.2f, 0.8f, 1 } };
    rear_touch_step(&c, &s, a1, 1, 3000);
    rear_point b2[2] = { { 0.2f, 0.8f, 1 }, { 0.8f, 0.8f, 2 } };
    o = rear_touch_step(&c, &s, b2, 2, 3000 + c.two_finger_ms + 100);
    CHECK(!o.r3, "COUNTER-CASE: a second finger arriving late is not a two-finger click");
}

static void identity(void)
{
    printf("-- fingers are tracked by the panel's id, not by proximity\n");
    rear_touch_config c = cfg();
    c.action[REAR_ZONE_TL]  = REAR_ACT_ZL;  c.gesture[REAR_ZONE_TL] = REAR_GEST_HOLD;
    c.action[REAR_ZONE_TR]  = REAR_ACT_ZR;  c.gesture[REAR_ZONE_TR] = REAR_GEST_HOLD;
    rear_touch_state s; memset(&s, 0, sizeof s);

    /* Two fingers that CROSS. Matching by proximity would swap them, and swap
     * ZL with ZR in the middle of a game. The zone is fixed at touch-down, so
     * each keeps the button it started with. */
    rear_point p[2] = { { 0.2f, 0.2f, 7 }, { 0.8f, 0.2f, 9 } };
    rear_touch_out o = rear_touch_step(&c, &s, p, 2, 1000);
    CHECK(o.zl && o.zr, "both zones held");

    p[0].x = 0.8f; p[1].x = 0.2f;          /* they swap places, keeping their ids */
    o = rear_touch_step(&c, &s, p, 2, 1050);
    CHECK(o.zl && o.zr,
          "COUNTER-CASE: crossing fingers keep their own buttons - id, not proximity");
}

static void release_all(void)
{
    printf("-- rule 4's escape hatch\n");
    rear_touch_config c = cfg();
    c.action[REAR_ZONE_TL] = REAR_ACT_ZL; c.gesture[REAR_ZONE_TL] = REAR_GEST_HOLD;
    rear_touch_state s; memset(&s, 0, sizeof s);

    rear_touch_out o = one(&c, &s, 0.2f, 0.2f, 1000);
    CHECK(o.zl, "held");
    o = rear_touch_release_all(&s);
    CHECK(!o.zl && !o.zr && !o.l3 && !o.r3 && !o.l2 && !o.r2, "everything released");
    o = none_(&c, &s, 1010);
    CHECK(!o.zl, "and nothing comes back on its own");
}

static void robustness(void)
{
    printf("-- inputs that should not be able to break it\n");
    rear_touch_config c = cfg();
    rear_touch_state s; memset(&s, 0, sizeof s);

    rear_touch_out o = rear_touch_step(NULL, &s, NULL, 0, 0);
    CHECK(!o.zl && !o.l2, "no config: nothing, no crash");
    o = rear_touch_step(&c, NULL, NULL, 0, 0);
    CHECK(!o.zl && !o.l2, "no state: nothing, no crash");

    /* More points than the panel can report: clamped, not overflowed. */
    rear_point many[8];
    for (int i = 0; i < 8; i++) { many[i].x = 0.2f; many[i].y = 0.2f; many[i].id = (uint8_t)i; }
    o = rear_touch_step(&c, &s, many, 8, 1000);
    (void)o;
    CHECK(1, "more points than REAR_MAX_POINTS does not overflow");

    /* A finger that vanishes without a lift - a dropped panel frame - must not
     * leave anything held. */
    memset(&s, 0, sizeof s);
    c.action[REAR_ZONE_TL] = REAR_ACT_ZL; c.gesture[REAR_ZONE_TL] = REAR_GEST_HOLD;
    one(&c, &s, 0.2f, 0.2f, 1000);
    o = none_(&c, &s, 1016);
    CHECK(!o.zl, "COUNTER-CASE: a contact lost without a lift releases too - RULE 4");
}

int main(void)
{
    printf("== the rear touchpad, as arithmetic ==\n");
    zones();
    defaults_are_safe();
    tap();
    hold();
    slide();
    two_fingers();
    identity();
    release_all();
    robustness();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
