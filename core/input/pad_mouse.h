/* pad_mouse - the Joy-Cons as a wireless mouse. PURE: no state outside what the
 * caller hands it, no I/O, no getenv, no clock. Time is a PARAMETER.
 *
 * === WHAT THIS MODULE IS, AND WHAT IT IS NOT ===
 *
 * It converts, once per frame, a stick deflection OR a gyroscope reading into a
 * mouse movement in whole pixels, plus wheel notches. It knows nothing about
 * HID, about Shadow, or about the screen: `stream_view.cpp` reads the console
 * and posts the result through `shadow_input_post_mouse_*`.
 *
 * That split is not tidiness. Every hard part of "a stick is not a mouse" is
 * arithmetic - the dead zone, the response curve, the sub-pixel remainder, the
 * time base - and arithmetic is the only part that can be checked without a
 * console, a VM and a hand on the controller. `tests/test_pad_mouse.c` pins
 * each rule with the counter-case that motivated it.
 *
 * === THE FOUR RULES, AND WHY EACH ONE IS NOT OPTIONAL ===
 *
 * 1. THE REMAINDER IS CARRIED FROM ONE FRAME TO THE NEXT. At 60 Hz, a stick
 *    pushed a tenth of the way asks for a fraction of a pixel per frame.
 *    Truncating loses it entirely: the pointer simply does not move below some
 *    deflection, and the user reads that as a dead zone twice too large. This
 *    repo already learned it on the touch path ("the remainder is carried over
 *    from one frame to the next, without which small movements would be purely
 *    and simply lost - and that is what matters for fine aiming").
 *
 * 2. THE DEAD ZONE IS RADIAL, AND THE MAGNITUDE IS RESCALED PAST IT. A per-axis
 *    dead zone makes the pointer snap to the axes: pushing diagonally at a small
 *    angle gives pure horizontal movement, because one of the two axes is still
 *    under its threshold. And WITHOUT the rescale, crossing the dead zone starts
 *    the pointer at the speed the dead zone's edge maps to - it JUMPS. Both are
 *    immediately visible in the hand and invisible on reading.
 *
 * 3. THE SPEED IS PER SECOND, NOT PER FRAME. Multiplying by a constant each
 *    frame ties the pointer's speed to the frame rate: the same gesture crosses
 *    the screen at 60 fps and half of it at 30. The stream's frame rate is not
 *    stable by construction, so this would be felt as the pointer randomly
 *    slowing down. Same defect family as L6/L9/L16 in this repo - a path paced
 *    by an arbitrary clock instead of by its source.
 *
 * 4. dt IS CLAMPED. `t - t0` comes from a clock, and the console sleeps, the
 *    applet resumes, the stream stalls. An unclamped dt sends the pointer to the
 *    far corner on the first frame after a resume, which reads as a crash.
 *
 * === WHAT IS NOT ESTABLISHED, AND SAID SO ===
 *
 * The unit of `HidSixAxisSensorState.angular_velocity` is NOT measured by us.
 * It is widely taken to be rotations per second, and the reference speed below
 * was chosen to feel right on that assumption. This is why the gyro's
 * sensitivity is a SETTING and not a constant: if the unit is something else,
 * the setting absorbs it and nothing here is a lie. Saying "rad/s" in a comment
 * would have been a guess dressed as a fact.
 */
#ifndef SHADOW_PAD_MOUSE_H
#define SHADOW_PAD_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAD_MOUSE_OFF   = 0,
    PAD_MOUSE_STICK = 1,   /* the right stick drives the pointer */
    PAD_MOUSE_GYRO  = 2    /* the console's motion drives the pointer */
} pad_mouse_mode_t;

/* Reference speed at full deflection, at 100 % sensitivity: how many pixels the
 * pointer travels per second. 1000 crosses a 1920-wide screen in about two
 * seconds, which is the usual comfortable figure for a stick-driven pointer -
 * fast enough to reach a corner, slow enough to stop on a button. */
#define PAD_MOUSE_REF_PX_PER_S 1000.0f

/* === PM7 2026-09-02 - THE GYRO SCALE, NOW MEASURED ===
 *
 * Pixels per second per unit of angular velocity, at 100 %. It was 1400, chosen
 * on a guess, and the console has since said what the sensor actually reports:
 *
 *     [PM] manette : portable=0 styles=0x00000004      (Joy-Cons detached)
 *     [PM] capteurs obtenus : paire Joy-Con x2 (2)
 *     [PM] 1re lecture gyroscope : x=-0.0085 y=0.0006
 *
 * The position is the INTEGRAL of the velocity, so crossing the screen takes
 * `1920 / REF` turns whatever the frame rate. At 1400 that is 1.37 turns -
 * **494 degrees** - to move the pointer one screen width. You would have to spin
 * the controller one and a half times round. It was moving; it was moving by two
 * pixels, which is indistinguishable from not moving and is exactly how it was
 * reported.
 *
 * 15360 puts a 45-degree wrist turn at one screen width (1920 / (45/360)), which
 * is the range a wrist covers without moving the arm.
 *
 * THIS RESTS ON THE UNIT BEING TURNS PER SECOND, which we still have not proven
 * - but the rest reading now makes it plausible rather than assumed: 0.0085
 * turns/s is 3.06 deg/s, the ordinary drift of a MEMS gyroscope at rest. In
 * rad/s the same figure would be 0.49 deg/s, which is better than this class of
 * sensor achieves. The test page shows the value converted to deg/s so the
 * assumption can be FALSIFIED in one gesture: turn the controller a quarter turn
 * in one second and read it. If it says 90, the unit is turns per second. */
#define PAD_MOUSE_REF_GYRO_PX  15360.0f

/* === PM7 - AND THE DEAD ZONE THE MEASUREMENT MADE MANDATORY ===
 *
 * This header used to say, of the gyro: "no dead zone worth the name: the
 * sensor's noise is far below one pixel per frame". That was written without
 * measuring, and the measurement refutes it. At rest the sensor reports 0.0085
 * turns/s, which at the scale above is **130 pixels per second** - the pointer
 * would cross the screen on its own in fifteen seconds, resting on the table.
 *
 * The threshold SUBTRACTS rather than zeroing below itself. Zeroing would make
 * every slow, deliberate movement disappear - and slow, deliberate movement is
 * the one thing a gyroscope is better at than a stick. Subtracting kills the
 * drift and keeps everything above it proportional.
 *
 * 0.012 turns/s = 4.3 deg/s: above the drift observed, below any turn made on
 * purpose. */
#define PAD_MOUSE_GYRO_DEADZONE 0.012f

/* Upper bound on one reading, in turns per second. It was 30 - 10800 deg/s,
 * thirty times what a wrist can do - which is not a bound, it is a number large
 * enough never to fire. Its job is to stop ONE corrupt sample from teleporting
 * the pointer, so it has to sit just above what a human actually produces: a
 * fast wrist flick peaks around 1500 deg/s, hence 5 turns/s = 1800 deg/s.
 *
 * At the scale above, one clamped frame at 60 Hz moves about 1280 px - a screen
 * width. That is the right shape for a ceiling: the worst single sample costs
 * one screen, not a hundred. */
#define PAD_MOUSE_GYRO_MAX 5.0f

/* dt bounds. Below 1 ms two frames of the same millisecond would each get a
 * full step; above 100 ms we are no longer in a continuous gesture (sleep,
 * stall) and integrating it would teleport the pointer. */
#define PAD_MOUSE_DT_MIN_US  1000u
#define PAD_MOUSE_DT_MAX_US  100000u

typedef struct {
    int sensitivity_pct;   /* 10..400, 100 = the reference speed above */
    int deadzone_pct;      /* 0..50, radial */
    int invert_y;          /* 1 = pushing up moves the pointer down */
    int scroll_step_pct;   /* deflection-seconds for one wheel notch, 1..400 */
} pad_mouse_cfg;

/* One frame's worth of input, already normalised by the caller.
 * `sx`/`sy` and `scroll` are stick deflections in [-1, 1], y POSITIVE UP (the
 * console's convention). `gx`/`gy` are angular velocities: `gy` = yaw (turning
 * the console left/right) and `gx` = pitch (tilting it up/down). */
typedef struct {
    float sx, sy;      /* pointer stick */
    float gx, gy;      /* gyroscope */
    float scroll;      /* scroll stick, vertical axis */
} pad_mouse_in;

typedef struct {
    int dx, dy;        /* whole pixels, screen convention (y down) */
    int notches;       /* wheel notches, positive = scroll up */
} pad_mouse_out;

typedef struct {
    float rem_x, rem_y;    /* sub-pixel remainder - rule 1 */
    float scroll_accum;
} pad_mouse_state;

static inline void pad_mouse_reset(pad_mouse_state *st)
{
    if (!st) return;
    st->rem_x = st->rem_y = 0.0f;
    st->scroll_accum = 0.0f;
}

static inline float pad_mouse_clampf_(float v, float lo, float hi)
{
    /* NaN fails BOTH comparisons and would pass through a naive clamp intact,
     * then poison the remainder for the rest of the session - the same trap
     * `clients/borealis/ui/anim.h` and `nav.h` document. `v != v` is true only for NaN. */
    if (v != v) return 0.0f;
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Radial dead zone with rescale - rule 2. Returns the magnitude to use, in
 * [0, 1], and normalises the direction into `*ux`, `*uy`. */
static inline float pad_mouse_shape_(float x, float y, int deadzone_pct,
                                     float *ux, float *uy)
{
    float dz, mag, k;
    *ux = *uy = 0.0f;
    x = pad_mouse_clampf_(x, -1.0f, 1.0f);
    y = pad_mouse_clampf_(y, -1.0f, 1.0f);

    mag = x * x + y * y;
    if (mag <= 0.0f) return 0.0f;
    /* Integer square root is not needed: we only need the magnitude, and a
     * float sqrt is available everywhere this runs. Written by hand to keep the
     * module free of <math.h>, which the offline test would otherwise have to
     * link for one call. */
    {
        float g = mag > 1.0f ? mag : 1.0f;
        for (int i = 0; i < 12; i++) g = 0.5f * (g + mag / g);
        mag = g;
    }
    if (mag > 1.0f) mag = 1.0f;          /* a diagonal reaches 1.414 */

    if (deadzone_pct < 0)  deadzone_pct = 0;
    if (deadzone_pct > 50) deadzone_pct = 50;
    dz = (float)deadzone_pct / 100.0f;
    if (mag <= dz) return 0.0f;

    *ux = x / mag;
    *uy = y / mag;
    /* THE RESCALE. Without it the first pixel past the dead zone already moves
     * at the dead zone's speed, and the pointer jumps as it starts. */
    k = (mag - dz) / (1.0f - dz);
    return k > 1.0f ? 1.0f : k;
}

/* === PM9 - WHICH SENSOR DRIVES, AND HOW THE PAIR IS COMBINED ===
 *
 * Pure, and here rather than in the console file, because it is arithmetic and
 * arithmetic is the part that can be checked without a Joy-Con in each hand.
 * The first version lived next to the HID calls, where nothing could pin the
 * split's axis assignment - and an axis assignment that is wrong by a swap is
 * not something you notice by reading.
 *
 *   0 automatic  the one that moves, keeping the previous choice unless the
 *                other is twice as active AND above the drift threshold
 *   1 left       2 right
 *   3 both       the AVERAGE. Held as a pair the two sensors measure the SAME
 *                rotation: summing would double a gesture that has not doubled.
 *   4 split      sensor 1 turns for the horizontal, sensor 0 tilts for vertical
 *   5 split      the other way round
 *
 * `*pick` carries the automatic mode's memory between frames - the hysteresis
 * that stops the pointer changing hands mid-gesture when the resting hand
 * twitches.
 *
 * A pairing asked for without a pair DEGRADES to whichever sensor answers: a
 * setting that cannot be honoured must fall back, not switch the feature off
 * without a word. */
static inline void pad_mouse_pick_gyro(int src,
                                       float g0x, float g0y, float g1x, float g1y,
                                       int live0, int live1, int *pick,
                                       float *out_gx, float *out_gy)
{
    const int pair = (live0 && live1);
    int p = pick ? *pick : 0;

    *out_gx = *out_gy = 0.0f;
    if (!live0 && !live1) { if (pick) *pick = 0; return; }

    if (src == 3 && pair) {
        *out_gx = (g0x + g1x) * 0.5f;
        *out_gy = (g0y + g1y) * 0.5f;
        return;
    }
    if ((src == 4 || src == 5) && pair) {
        /* `gy` is yaw and drives the horizontal; `gx` is pitch and drives the
         * vertical. Naming them by what they DO is what makes the swap between
         * 4 and 5 readable - and testable. */
        *out_gy = (src == 4) ? g1y : g0y;      /* horizontal */
        *out_gx = (src == 4) ? g0x : g1x;      /* vertical   */
        return;
    }

    if (src == 1 && live0)      p = 0;
    else if (src == 2 && live1) p = 1;
    else if (src == 0 && pair) {
        const float m0 = (g0x < 0 ? -g0x : g0x) + (g0y < 0 ? -g0y : g0y);
        const float m1 = (g1x < 0 ? -g1x : g1x) + (g1y < 0 ? -g1y : g1y);
        const float dz = PAD_MOUSE_GYRO_DEADZONE;
        if (p == 0 && m1 > dz && m1 > m0 * 2.0f) p = 1;
        else if (p == 1 && m0 > dz && m0 > m1 * 2.0f) p = 0;
    } else {
        p = live0 ? 0 : 1;
    }
    if (p == 1 && !live1) p = 0;
    if (p == 0 && !live0) p = 1;
    if (pick) *pick = p;
    *out_gx = p ? g1x : g0x;
    *out_gy = p ? g1y : g0y;
}

/* One frame. `dt_us` is the time since the previous call. */
static inline void pad_mouse_step(pad_mouse_state *st, const pad_mouse_cfg *cfg,
                                  pad_mouse_mode_t mode, const pad_mouse_in *in,
                                  uint32_t dt_us, pad_mouse_out *out)
{
    float dt, sens, ux, uy, mag, vx, vy, fx, fy;
    int   step;

    if (!out) return;
    out->dx = out->dy = out->notches = 0;
    if (!st || !cfg || !in || mode == PAD_MOUSE_OFF) return;

    if (dt_us < PAD_MOUSE_DT_MIN_US) dt_us = PAD_MOUSE_DT_MIN_US;   /* rule 4 */
    if (dt_us > PAD_MOUSE_DT_MAX_US) dt_us = PAD_MOUSE_DT_MAX_US;
    dt = (float)dt_us / 1000000.0f;

    sens = (float)(cfg->sensitivity_pct < 10 ? 10
                 : (cfg->sensitivity_pct > 400 ? 400 : cfg->sensitivity_pct)) / 100.0f;

    if (mode == PAD_MOUSE_STICK) {
        mag = pad_mouse_shape_(in->sx, in->sy, cfg->deadzone_pct, &ux, &uy);
        /* THE RESPONSE CURVE. Squared, not linear: a linear stick is unusable
         * as a pointer - at a sensitivity that lets you cross the screen, the
         * small deflections are already too fast to stop on a button, and at a
         * sensitivity that allows aiming you can no longer reach the far edge.
         * Squaring gives the small deflections back their precision while
         * leaving the top speed untouched. */
        mag = mag * mag;
        vx = ux * mag * PAD_MOUSE_REF_PX_PER_S * sens;
        vy = uy * mag * PAD_MOUSE_REF_PX_PER_S * sens;
    } else {
        /* GYRO. The threshold SUBTRACTS instead of zeroing below itself - see
         * PAD_MOUSE_GYRO_DEADZONE: the drift has to die without taking the slow
         * deliberate movement with it. The rescale of rule 2 does not apply
         * here: there is no edge whose crossing would make the pointer jump,
         * because what is left after the subtraction starts at zero by
         * construction.
         * Yaw drives x, pitch drives y: turning the console right moves the
         * pointer right, tilting it down moves the pointer down. */
        {
            float gy = pad_mouse_clampf_(in->gy, -PAD_MOUSE_GYRO_MAX, PAD_MOUSE_GYRO_MAX);
            float gx = pad_mouse_clampf_(in->gx, -PAD_MOUSE_GYRO_MAX, PAD_MOUSE_GYRO_MAX);
            const float dz = PAD_MOUSE_GYRO_DEADZONE;
            gy = (gy >  dz) ? gy - dz : ((gy < -dz) ? gy + dz : 0.0f);
            gx = (gx >  dz) ? gx - dz : ((gx < -dz) ? gx + dz : 0.0f);
            vx = gy * PAD_MOUSE_REF_GYRO_PX * sens;
            vy = gx * PAD_MOUSE_REF_GYRO_PX * sens;
        }
    }

    /* Screen convention: y grows DOWNWARDS, the stick and the gyro grow upwards.
     * `invert_y` is the user's preference on top of that. */
    vy = -vy;
    if (cfg->invert_y) vy = -vy;

    fx = st->rem_x + vx * dt;                 /* rule 3 */
    fy = st->rem_y + vy * dt;
    out->dx = (int)fx;
    out->dy = (int)fy;
    st->rem_x = fx - (float)out->dx;          /* rule 1 */
    st->rem_y = fy - (float)out->dy;

    /* --- Wheel ---------------------------------------------------------
     * Accumulated in deflection-seconds, so holding the stick scrolls at a
     * steady rate whatever the frame rate, and a brief flick gives one notch
     * rather than none. */
    step = cfg->scroll_step_pct < 1 ? 1 : (cfg->scroll_step_pct > 400 ? 400
                                                                     : cfg->scroll_step_pct);
    {
        float s = pad_mouse_clampf_(in->scroll, -1.0f, 1.0f);
        float dz = (float)(cfg->deadzone_pct < 0 ? 0
                          : (cfg->deadzone_pct > 50 ? 50 : cfg->deadzone_pct)) / 100.0f;
        if (s > dz)       s = (s - dz) / (1.0f - dz);
        else if (s < -dz) s = (s + dz) / (1.0f - dz);
        else              s = 0.0f;

        st->scroll_accum += s * dt;
        {
            const float unit = (float)step / 100.0f;
            while (st->scroll_accum >= unit)  { out->notches += 1; st->scroll_accum -= unit; }
            while (st->scroll_accum <= -unit) { out->notches -= 1; st->scroll_accum += unit; }
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_PAD_MOUSE_H */
