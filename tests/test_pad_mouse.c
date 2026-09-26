/* test_pad_mouse.c - the Joy-Cons as a mouse (core/input/pad_mouse.h).
 *
 * Every rule this module carries is one that is INVISIBLE on reading and
 * obvious in the hand: a pointer that does not move below some deflection, one
 * that snaps to the axes, one whose speed follows the frame rate, one that
 * teleports after a sleep. Each check below names the symptom it prevents.
 */
#include <stdio.h>
#include "../core/input/pad_mouse.h"

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                 \
    if (!(cond)) { printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, what);   \
                   failures++; }                                              \
} while (0)

static pad_mouse_cfg base_cfg(void)
{
    pad_mouse_cfg c;
    c.sensitivity_pct = 100;
    c.deadzone_pct    = 15;
    c.invert_y        = 0;
    c.scroll_step_pct = 100;
    return c;
}

/* Runs `frames` frames of `dt_us` with a constant input, returns the total. */
static void run(pad_mouse_state *st, const pad_mouse_cfg *c, pad_mouse_mode_t m,
                pad_mouse_in in, uint32_t dt_us, int frames,
                int *tot_dx, int *tot_dy, int *tot_notch)
{
    pad_mouse_out o;
    *tot_dx = *tot_dy = *tot_notch = 0;
    for (int i = 0; i < frames; i++) {
        pad_mouse_step(st, c, m, &in, dt_us, &o);
        *tot_dx += o.dx; *tot_dy += o.dy; *tot_notch += o.notches;
    }
}

int main(void)
{
    const pad_mouse_cfg cfg = base_cfg();
    pad_mouse_state st;
    pad_mouse_out o;
    pad_mouse_in in = {0};
    int dx, dy, nn;

    printf("== Joy-Cons as a mouse (stick and gyro -> pixels) ==\n");

    /* --- Off is off ------------------------------------------------------ */
    pad_mouse_reset(&st);
    in.sx = 1.0f; in.sy = 1.0f;
    pad_mouse_step(&st, &cfg, PAD_MOUSE_OFF, &in, 16667, &o);
    CHECK(o.dx == 0 && o.dy == 0 && o.notches == 0,
          "OFF emits nothing, whatever the sticks say");

    /* COUNTER-CASE 1 - THE SLOW MOVEMENT THAT NEVER ARRIVES.
     * A stick pushed a quarter of the way asks for a fraction of a pixel per
     * frame at 60 Hz. Truncating every frame loses it entirely and the pointer does
     * not move at all - which reads as a dead zone twice too large. The
     * remainder is carried, so a second of that gesture DOES move the pointer. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sx = 0.25f;
    pad_mouse_step(&st, &cfg, PAD_MOUSE_STICK, &in, 16667, &o);
    CHECK(o.dx == 0, "one frame of a small deflection is under a pixel...");
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx > 0, "COUNTER-CASE: ...but a second of it DOES move the pointer");

    /* And nothing is lost along the way: the total over N frames is the
     * continuous speed, to within one pixel. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sx = 1.0f;
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx >= 995 && dx <= 1005,
          "full deflection for one second travels the reference distance");

    /* COUNTER-CASE 2 - THE POINTER THAT SNAPS TO THE AXES.
     * With a PER-AXIS dead zone, a shallow diagonal has one axis under the
     * threshold and the movement comes out purely horizontal. The dead zone is
     * radial, so a shallow diagonal keeps BOTH components. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sx = 0.9f; in.sy = 0.2f;
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx != 0 && dy != 0, "COUNTER-CASE: a shallow diagonal keeps both axes");

    /* A diagonal never travels faster than a straight push: the magnitude is
     * clamped after the radial combination. Pushed to the corner, the total
     * distance must not exceed the reference. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sx = 1.0f; in.sy = 1.0f;
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx <= 1005 && (-dy) <= 1005,
          "a corner push is not 1.41x faster than a straight one");

    /* COUNTER-CASE 3 - THE JUMP AS THE DEAD ZONE IS CROSSED.
     * Just past the dead zone the speed must be NEAR ZERO. Without the rescale
     * it starts at the speed the dead zone's edge maps to, and the pointer
     * leaps as it begins to move. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sx = 0.151f;      /* dead zone is 15 % */
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx >= 0 && dx <= 3, "COUNTER-CASE: just past the dead zone, the pointer crawls");

    /* Inside the dead zone, nothing at all - including over a long hold, which
     * is what a resting thumb does. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sx = 0.10f; in.sy = -0.10f;
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 600, &dx, &dy, &nn);
    CHECK(dx == 0 && dy == 0, "a thumb resting inside the dead zone never drifts");

    /* COUNTER-CASE 4 - THE SPEED THAT FOLLOWS THE FRAME RATE.
     * The stream's frame rate is not stable. Multiplying by a constant per frame
     * would halve the pointer's speed at 30 fps, felt as the pointer randomly
     * slowing down. The same gesture for the same WALL time must travel the same
     * distance. */
    {
        int dx60, dx30, dy_, n_;
        pad_mouse_reset(&st);
        in = (pad_mouse_in){0}; in.sx = 1.0f;
        run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx60, &dy_, &n_);
        pad_mouse_reset(&st);
        run(&st, &cfg, PAD_MOUSE_STICK, in, 33333, 30, &dx30, &dy_, &n_);
        CHECK(dx30 >= dx60 - 3 && dx30 <= dx60 + 3,
              "COUNTER-CASE: one second at 30 fps travels as far as at 60 fps");
    }

    /* COUNTER-CASE 5 - THE POINTER THAT TELEPORTS AFTER A SLEEP.
     * `dt` comes from a clock. The console sleeps, the applet resumes, and the
     * first frame back carries seconds. Unclamped, the pointer lands in a
     * corner and the user reads it as a crash. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sx = 1.0f;
    pad_mouse_step(&st, &cfg, PAD_MOUSE_STICK, &in, 5000000u, &o);   /* 5 s */
    CHECK(o.dx <= 101, "COUNTER-CASE: a 5 s gap moves at most one clamped frame");
    CHECK(o.dx > 0, "...and it is not simply dropped either");

    /* A zero dt must not divide by anything nor emit a full step. */
    pad_mouse_reset(&st);
    pad_mouse_step(&st, &cfg, PAD_MOUSE_STICK, &in, 0, &o);
    CHECK(o.dx >= 0 && o.dx <= 2, "a zero dt is clamped up, not exploded");

    /* COUNTER-CASE 6 - THE NaN.
     * It fails BOTH `< lo` and `> hi`, so it goes through a naive clamp intact -
     * and once in the remainder it poisons every following frame, for the whole
     * session. Same trap as ui/anim.h and ui/nav.h. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0};
    in.sx = 0.0f / 0.0f;                       /* NaN */
    pad_mouse_step(&st, &cfg, PAD_MOUSE_STICK, &in, 16667, &o);
    CHECK(o.dx == 0 && o.dy == 0, "COUNTER-CASE: a NaN input moves nothing");
    in = (pad_mouse_in){0}; in.sx = 1.0f;
    pad_mouse_step(&st, &cfg, PAD_MOUSE_STICK, &in, 16667, &o);
    CHECK(o.dx > 0, "COUNTER-CASE: and it has not poisoned the remainder either");

    /* --- Direction and inversion ---------------------------------------- */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sy = 1.0f;       /* stick pushed UP */
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dy < 0, "pushing up moves the pointer UP (screen y grows downwards)");
    {
        pad_mouse_cfg inv = base_cfg(); inv.invert_y = 1;
        int dy2;
        pad_mouse_reset(&st);
        run(&st, &inv, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy2, &nn);
        CHECK(dy2 > 0, "and the inversion setting flips exactly that");
    }
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sx = -1.0f;
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx < 0, "pushing left moves the pointer left");

    /* --- Sensitivity ------------------------------------------------------ */
    {
        pad_mouse_cfg slow = base_cfg(), fast = base_cfg();
        int d_slow, d_fast, dy_, n_;
        slow.sensitivity_pct = 50; fast.sensitivity_pct = 200;
        in = (pad_mouse_in){0}; in.sx = 1.0f;
        pad_mouse_reset(&st); run(&st, &slow, PAD_MOUSE_STICK, in, 16667, 60, &d_slow, &dy_, &n_);
        pad_mouse_reset(&st); run(&st, &fast, PAD_MOUSE_STICK, in, 16667, 60, &d_fast, &dy_, &n_);
        CHECK(d_fast > d_slow * 3, "200 % travels about four times 50 %");
        /* An absurd sensitivity is CLAMPED, not obeyed: a hand-edited settings
         * file must not be able to make the pointer unusable. */
        pad_mouse_cfg mad = base_cfg(); mad.sensitivity_pct = 100000;
        int d_mad;
        pad_mouse_reset(&st); run(&st, &mad, PAD_MOUSE_STICK, in, 16667, 60, &d_mad, &dy_, &n_);
        CHECK(d_mad <= 4100, "an absurd sensitivity is clamped to 400 %");
    }

    /* --- The response curve gives the small deflections back their precision */
    {
        int d_half, d_full, dy_, n_;
        in = (pad_mouse_in){0}; in.sx = 0.5f;
        pad_mouse_reset(&st); run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &d_half, &dy_, &n_);
        in.sx = 1.0f;
        pad_mouse_reset(&st); run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &d_full, &dy_, &n_);
        CHECK(d_half * 3 < d_full,
              "half a push is far slower than half speed - that is the point of the curve");
        CHECK(d_half > 0, "...but it is not dead either");
    }

    /* --- Gyro ------------------------------------------------------------- */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.gy = 0.5f;       /* yaw */
    run(&st, &cfg, PAD_MOUSE_GYRO, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx > 0, "turning the console right moves the pointer right");
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.gx = 0.5f;       /* pitch */
    run(&st, &cfg, PAD_MOUSE_GYRO, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dy < 0, "tilting it up moves the pointer up");

    /* The gyro must NOT be dead-zoned: its precision is exactly the slow
     * movement a dead zone would eat. A tenth of the stick's dead zone still
     * moves the pointer. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.gy = 0.015f;
    run(&st, &cfg, PAD_MOUSE_GYRO, in, 16667, 120, &dx, &dy, &nn);
    CHECK(dx > 0, "a very slow rotation still moves the pointer");

    /* And the sticks do NOT drive the pointer in gyro mode: the two would fight,
     * and the thumb resting on a stick would drift the aim. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.sx = 1.0f; in.sy = 1.0f;
    run(&st, &cfg, PAD_MOUSE_GYRO, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx == 0 && dy == 0, "in gyro mode the pointer stick is ignored");

    /* An absurd gyro reading is clamped rather than integrated. The bound is
     * expressed in terms of the constant, not as a magic number: the first
     * version of this check hardcoded 1000 px, and when PM7 raised the scale to
     * its measured value the check failed while the clamp was working perfectly.
     * A test that has to be edited every time a constant moves is testing the
     * constant, not the rule. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.gy = 1e9f;
    pad_mouse_step(&st, &cfg, PAD_MOUSE_GYRO, &in, 16667, &o);
    {
        const float max_frame = PAD_MOUSE_GYRO_MAX * PAD_MOUSE_REF_GYRO_PX * 0.016667f;
        CHECK(o.dx > 0 && (float)o.dx <= max_frame + 1.0f,
              "an absurd angular velocity is clamped to one frame at the ceiling");
    }

    /* COUNTER-CASE PM7 - THE POINTER THAT DRIFTS ON ITS OWN.
     * Measured on the console at rest: 0.0085 turns/s. At the scale PM7
     * establishes that is ~130 px/s - the pointer crosses the screen in fifteen
     * seconds with the controller lying on a table. The header used to claim,
     * without measuring, that the noise was "far below one pixel per frame". */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.gx = -0.0085f; in.gy = 0.0006f;
    run(&st, &cfg, PAD_MOUSE_GYRO, in, 16667, 600, &dx, &dy, &nn);
    CHECK(dx == 0 && dy == 0,
          "COUNTER-CASE: ten seconds of the MEASURED rest reading move nothing");

    /* And the threshold SUBTRACTS: just above it, the pointer crawls instead of
     * starting at the threshold's speed - and slow deliberate movement, which is
     * what a gyroscope is for, survives. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.gy = PAD_MOUSE_GYRO_DEADZONE * 2.0f;
    run(&st, &cfg, PAD_MOUSE_GYRO, in, 16667, 60, &dx, &dy, &nn);
    {
        const int expected = (int)(PAD_MOUSE_GYRO_DEADZONE * PAD_MOUSE_REF_GYRO_PX);
        CHECK(dx > expected - 12 && dx < expected + 12,
              "twice the threshold moves by ONE threshold's worth, not two");
    }

    /* A 45 degree turn crosses a 1920-wide screen: that is the whole point of
     * the scale PM7 chose, and it is the number the user feels. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0};
    in.gy = 45.0f / 360.0f + PAD_MOUSE_GYRO_DEADZONE;   /* 45 deg in one second */
    run(&st, &cfg, PAD_MOUSE_GYRO, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx > 1800 && dx < 2050, "a 45 degree turn in one second crosses ~1920 px");

    /* --- The wheel -------------------------------------------------------- */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.scroll = 1.0f;
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(nn > 0, "holding the scroll stick emits notches");
    CHECK(nn == 1, "one second at full deflection = one notch at the default step");
    pad_mouse_reset(&st);
    in.scroll = -1.0f;
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(nn == -1, "and the other way round");

    /* Scrolling is per SECOND too, not per frame: at 30 fps the same hold gives
     * the same number of notches. */
    {
        /* 2.5 s, deliberately BETWEEN two notch boundaries. The first version of
         * this check used exactly 2 s, which is a notch boundary: 120 frames of
         * 16667 us make 2.00004 s and 60 of 33333 make 1.99998 s, so the two
         * landed on either side of it and differed by one notch. That was the
         * TEST sitting on a knife edge, not the module following the frame rate
         * - and the honest fix is to ask the question away from the edge rather
         * than to loosen the assertion until it passes. */
        int n60, n30;
        pad_mouse_reset(&st);
        in = (pad_mouse_in){0}; in.scroll = 1.0f;
        run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 150, &dx, &dy, &n60);
        pad_mouse_reset(&st);
        run(&st, &cfg, PAD_MOUSE_STICK, in, 33333, 75, &dx, &dy, &n30);
        CHECK(n60 == n30, "COUNTER-CASE: the wheel does not follow the frame rate");
        CHECK(n60 == 2, "2.5 s at the default step is two notches");
    }

    /* The scroll stick honours the dead zone: a resting thumb must not scroll a
     * document on its own, which is far more noticeable than a drifting
     * pointer. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.scroll = 0.10f;
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 600, &dx, &dy, &nn);
    CHECK(nn == 0, "a thumb resting on the scroll stick never scrolls");

    /* Reset clears the remainder AND the scroll accumulator: entering mouse
     * mode must not fire a notch left over from the previous time. */
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0}; in.scroll = 1.0f;
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 50, &dx, &dy, &nn);
    pad_mouse_reset(&st);
    in = (pad_mouse_in){0};
    run(&st, &cfg, PAD_MOUSE_STICK, in, 16667, 60, &dx, &dy, &nn);
    CHECK(dx == 0 && dy == 0 && nn == 0,
          "reset clears everything - entering mouse mode emits nothing by itself");

    /* --- Null arguments: this is called from the render path -------------- */
    pad_mouse_step(NULL, &cfg, PAD_MOUSE_STICK, &in, 16667, &o);
    CHECK(o.dx == 0 && o.dy == 0, "a null state is refused, not dereferenced");
    pad_mouse_step(&st, NULL, PAD_MOUSE_STICK, &in, 16667, &o);
    CHECK(o.dx == 0 && o.dy == 0, "a null config too");
    pad_mouse_step(&st, &cfg, PAD_MOUSE_STICK, NULL, 16667, &o);
    CHECK(o.dx == 0 && o.dy == 0, "a null input too");
    pad_mouse_step(&st, &cfg, PAD_MOUSE_STICK, &in, 16667, NULL);   /* must not crash */
    pad_mouse_reset(NULL);
    checks += 2;

    /* === PM9 - WHICH SENSOR DRIVES, AND HOW THE PAIR IS COMBINED ===
     * These live in the pure module precisely so they can be checked here: an
     * axis assignment that is swapped, or an average that is really a sum, is
     * invisible on reading and obvious in the hand - after you have gone looking
     * in the wrong place. */
    {
        int pick = 0; float gx = 0, gy = 0;

        /* Left only, right only. */
        pad_mouse_pick_gyro(1, 0.10f, 0.20f, 0.30f, 0.40f, 1, 1, &pick, &gx, &gy);
        CHECK(gx == 0.10f && gy == 0.20f, "'left' takes sensor 0 and nothing else");
        pad_mouse_pick_gyro(2, 0.10f, 0.20f, 0.30f, 0.40f, 1, 1, &pick, &gx, &gy);
        CHECK(gx == 0.30f && gy == 0.40f, "'right' takes sensor 1");

        /* COUNTER-CASE - THE AVERAGE THAT IS REALLY A SUM.
         * Held as a pair the two sensors report the SAME rotation. Summing would
         * double a gesture that has not doubled, and the pointer would run at
         * twice the sensitivity the setting shows - which reads as "the setting
         * is wrong", not as "the combination is wrong". */
        pad_mouse_pick_gyro(3, 0.20f, 0.20f, 0.20f, 0.20f, 1, 1, &pick, &gx, &gy);
        CHECK(gx == 0.20f && gy == 0.20f,
              "COUNTER-CASE: two sensors reading the same give THAT, not twice it");
        pad_mouse_pick_gyro(3, 0.10f, 0.10f, 0.30f, 0.30f, 1, 1, &pick, &gx, &gy);
        CHECK(gx == 0.20f && gy == 0.20f, "and the average of two different ones");

        /* COUNTER-CASE - THE SPLIT WITH ITS AXES SWAPPED.
         * `gy` is yaw and drives the horizontal, `gx` is pitch and drives the
         * vertical. Getting that pair the wrong way round gives a pointer that
         * answers the wrong hand on each axis - and reads exactly like a broken
         * sensor. */
        pad_mouse_pick_gyro(4, 0.11f, 0.22f, 0.33f, 0.44f, 1, 1, &pick, &gx, &gy);
        CHECK(gy == 0.44f, "COUNTER-CASE: split 4 takes the horizontal from sensor 1");
        CHECK(gx == 0.11f, "COUNTER-CASE: ...and the vertical from sensor 0");
        pad_mouse_pick_gyro(5, 0.11f, 0.22f, 0.33f, 0.44f, 1, 1, &pick, &gx, &gy);
        CHECK(gy == 0.22f && gx == 0.33f, "split 5 is exactly the other way round");

        /* A pairing asked for without a pair DEGRADES rather than going quiet. */
        pad_mouse_pick_gyro(3, 0.10f, 0.20f, 0.0f, 0.0f, 1, 0, &pick, &gx, &gy);
        CHECK(gx == 0.10f && gy == 0.20f, "'both' with one sensor falls back on it");
        pad_mouse_pick_gyro(4, 0.10f, 0.20f, 0.0f, 0.0f, 1, 0, &pick, &gx, &gy);
        CHECK(gx == 0.10f && gy == 0.20f, "and so does the split");
        pad_mouse_pick_gyro(2, 0.10f, 0.20f, 0.0f, 0.0f, 1, 0, &pick, &gx, &gy);
        CHECK(gx == 0.10f && gy == 0.20f,
              "'right' with only a left sensor uses the left rather than nothing");

        /* COUNTER-CASE - THE HAND THAT CHANGES MID-GESTURE.
         * Automatic must not follow the strongest sensor frame by frame: the
         * resting hand twitches, the pointer hands over, and the aim jumps with
         * nothing to explain it. It keeps the hand it is following until the
         * other is TWICE as active and above the drift threshold. */
        pick = 0;
        pad_mouse_pick_gyro(0, 0.50f, 0.50f, 0.30f, 0.30f, 1, 1, &pick, &gx, &gy);
        CHECK(pick == 0, "automatic follows the active hand");
        pad_mouse_pick_gyro(0, 0.50f, 0.50f, 0.60f, 0.60f, 1, 1, &pick, &gx, &gy);
        CHECK(pick == 0, "COUNTER-CASE: slightly more active does NOT take over");
        pad_mouse_pick_gyro(0, 0.05f, 0.05f, 0.60f, 0.60f, 1, 1, &pick, &gx, &gy);
        CHECK(pick == 1, "twice as active, and above the threshold, does");
        /* And a twitch below the drift threshold never takes over, however
         * quiet the other hand is. */
        pick = 0;
        pad_mouse_pick_gyro(0, 0.0f, 0.0f, PAD_MOUSE_GYRO_DEADZONE * 0.5f, 0.0f,
                            1, 1, &pick, &gx, &gy);
        CHECK(pick == 0, "COUNTER-CASE: a twitch under the drift threshold never takes over");

        /* No sensor at all: nothing, and no dereference. */
        gx = gy = 9.0f;
        pad_mouse_pick_gyro(0, 0.1f, 0.2f, 0.3f, 0.4f, 0, 0, &pick, &gx, &gy);
        CHECK(gx == 0.0f && gy == 0.0f, "no live sensor gives zero, not the last value");
        pad_mouse_pick_gyro(0, 0.1f, 0.2f, 0.3f, 0.4f, 1, 1, NULL, &gx, &gy);
        CHECK(gx != 0.0f || gy != 0.0f, "a null memory is accepted, not dereferenced");
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
