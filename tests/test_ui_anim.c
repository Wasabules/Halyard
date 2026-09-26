/* test_ui_anim.c - the UI animations.
 *
 * Each check names its COUNTER-CASE: the exact input that would produce a
 * defect VISIBLE in the hand. A failure here therefore says what has just been
 * undone.
 */
#include "../clients/borealis/ui/anim.h"
#include <stdio.h>
#include <math.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

static bool proche(float a, float b) { return fabsf(a - b) < 0.001f; }

static void easings(void)
{
    CHECK(proche(anim_ease_out(0.0f), 0.0f), "ease-out: starts at 0");
    CHECK(proche(anim_ease_out(1.0f), 1.0f), "ease-out: reaches 1");
    CHECK(anim_ease_out(0.33f) > 0.6f,
            "ease-out: most of the movement happens early - that is what "
            "gives the impression that the UI responds");

    /* COUNTER-CASE - PROGRESS OUT OF RANGE.
     * A progress value comes from a clock: (t - t0) / duration. When the applet
     * resumes from sleep, t jumps and the progress exceeds 1. An unbounded curve
     * then returns a value above 1, and the highlight goes OFF SCREEN instead of
     * stopping on its target. */
    CHECK(proche(anim_ease_out(4.0f), 1.0f),
            "CONTRE-CAS : progression > 1 (reprise d'applet) bornee a 1");
    CHECK(proche(anim_ease_out(-3.0f), 0.0f),
            "CONTRE-CAS : progression negative bornee a 0");

    /* COUNTER-CASE - THE NaN.
     * A NaN fails BOTH `< 0` and `> 1`: it goes through a naive clamp intact,
     * propagates into the geometry and makes what we draw DISAPPEAR, with no
     * message. The same trap exists in nav.h, on the scrolling. */
    CHECK(proche(anim_clamp01(NAN), 1.0f),
            "COUNTER-CASE: NaN treated as movement FINISHED, not propagated");
    CHECK(!isnan(anim_ease_out(NAN)),
            "COUNTER-CASE: no curve may RETURN a NaN");
    CHECK(!isnan(anim_smooth(NAN)), "COUNTER-CASE: same for ease-in-out");

    /* Monotonicity: an animation must never go backwards. */
    float prec = -1.0f;
    for (int i = 0; i <= 20; i++) {
        const float v = anim_ease_out((float)i / 20.0f);
        CHECK(v >= prec - 0.0001f, "ease-out: never decreasing");
        prec = v;
    }
}

static void mouvement(void)
{
    /* A fixed point does not move, whatever instant is asked for. */
    anim_t f = anim_fixed(42.0f);
    CHECK(proche(anim_value(&f, 0.0), 42.0f), "point fixe a t=0");
    CHECK(proche(anim_value(&f, 999.0), 42.0f), "point fixe bien plus tard");
    CHECK(anim_done(&f, 0.0), "a fixed point is already finished");

    /* A simple move, 0 to 100 over 1 s, starting at t=10. */
    anim_t a = anim_towards(&f, 100.0f, 10.0, 1.0f);
    a.start = 0.0f;   /* we force the start for a readable computation */
    CHECK(proche(anim_value(&a, 10.0), 0.0f), "at the start: the starting value");
    CHECK(proche(anim_value(&a, 11.0), 100.0f), "at the end: exactly the target");
    CHECK(proche(anim_value(&a, 50.0), 100.0f),
            "well past the end: we stay on the target, we do not carry on");
    CHECK(anim_value(&a, 10.5) > 50.0f,
            "at half time: more than half the way (ease-out)");

    /* COUNTER-CASE - ZERO DURATION.
     * A duration of 0 divides by zero in the progress computation. The result
     * would be an infinity, then a NaN in the product - hence an invisible
     * value. */
    anim_t z = anim_towards(&f, 7.0f, 0.0, 0.0f);
    CHECK(proche(anim_value(&z, 0.0), 7.0f),
            "COUNTER-CASE: a zero duration = an immediate jump, not a division by zero");
    CHECK(anim_done(&z, 0.0), "COUNTER-CASE: a zero duration is finished from the start");

    /* COUNTER-CASE - THE CLOCK GOING BACKWARDS.
     * The console sleeps, the applet resumes, or the clock is reset: `t` can be
     * LOWER than `t0`. Without clamping the progress is negative, the movement
     * starts backwards and then jumps to its target. */
    CHECK(proche(anim_value(&a, 5.0), 0.0f),
            "COUNTER-CASE: a time BEFORE the start - we stay at the start, "
            "the movement does not start backwards");

    /* Null pointer: must not crash. */
    CHECK(!isnan(anim_value(NULL, 1.0)), "pointeur nul tolere");
    CHECK(anim_done(NULL, 1.0), "pointeur nul : considere termine");
}

static void target_change(void)
{
    /* COUNTER-CASE - THE JUMP WHEN THE TARGET CHANGES.
     * We aim at 100, and halfway there we change our mind for 200. That happens
     * as soon as a direction is HELD down: a new target every few frames.
     * If the new animation started from the OLD TARGET (100) instead of the
     * current position (~87), the highlight would jump forward before setting
     * off again. Subtle on a still image, very visible controller in hand. */
    anim_t a = anim_fixed(0.0f);
    a = anim_towards(&a, 100.0f, 0.0, 1.0f);

    const float halfway = anim_value(&a, 0.5);
    CHECK(halfway > 0.0f && halfway < 100.0f, "halfway, between the two");

    anim_t b = anim_towards(&a, 200.0f, 0.5, 1.0f);
    CHECK(proche(anim_value(&b, 0.5), halfway),
            "COUNTER-CASE: the new animation starts from the CURRENT position, "
            "the highlight does not jump");
    CHECK(proche(anim_value(&b, 1.5), 200.0f), "and does reach the new target");

    /* COUNTER-CASE - AIMING AT THE TARGET WE ALREADY HAVE.
     * At the end of a list, holding the direction asks for the same target on
     * every frame. Restarting the animation would make it start from zero
     * forever: the movement would NEVER arrive and the highlight would vibrate
     * in place. */
    anim_t c = anim_towards(&b, 200.0f, 0.9, 1.0f);
    CHECK(c.t0 == b.t0 && proche(c.start, b.start),
            "COUNTER-CASE: re-aiming at the same target does not restart the animation");
    CHECK(proche(anim_value(&c, 1.5), 200.0f),
            "COUNTER-CASE: and the movement does arrive at its destination");

    /* An animation COPIES with no consequence: that is what lets it live in an
     * array that gets resized while it plays. */
    anim_t copie = b;
    CHECK(proche(anim_value(&copie, 1.0), anim_value(&b, 1.0)),
            "a copied animation behaves identically");
}

int main(void)
{
    printf("== UI animations (pure functions of time) ==\n");
    easings();
    mouvement();
    target_change();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
