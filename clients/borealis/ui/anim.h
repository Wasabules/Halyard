/* anim.h - interface animations, as PURE functions.
 *
 * === THE PRINCIPLE, AND WHY IT MATTERS HERE ===
 *
 * An animation is NOT an object that updates itself. It is a FUNCTION of time:
 * `value = f(from, to, t0, duration, t)`. We store five numbers, and every frame
 * we ask where the movement has got to.
 *
 * This follows the same rule as the rest of the framework - no hidden state, no
 * implicit lifetime. A conventional animation system keeps a list of live
 * animations, with an `update(dt)` to call and objects to remove when they die;
 * that is exactly the kind of structure that ends up holding a pointer to a view
 * that no longer exists. Here there is nothing to keep and nothing to remove,
 * and a forgotten animation does not cost a single byte.
 *
 * Practical corollary: an animation can be copied, compared and thrown away. It
 * can live in an array that gets resized while it is playing.
 *
 * === WHAT ACTUALLY ANIMATES IN THIS APPLICATION ===
 *
 * On a console you look at the screen from a distance, controller in hand:
 * movement is there to say WHERE you are, not to decorate. Three uses, and that
 * is all:
 *   - the focus outline SLIDING from one card to the next (without it you lose
 *     track when the list scrolls);
 *   - the scroll catching up with its target instead of jumping;
 *   - a fade-in when content arrives.
 * The durations are short (0.12 to 0.25 s): any longer and the interface feels
 * sluggish, and you end up waiting for the animation instead of navigating.
 *
 * No allocation, no I/O, no internal clock: time arrives as a parameter, read
 * ONCE per frame by the caller. Two animations each reading the clock on their
 * own drift apart by one frame, and that shows on an outline tracking a card.
 */
#ifndef UI_ANIM_H
#define UI_ANIM_H

#include <math.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ easing */

/* All of these take and return a progress in [0,1], bounds included. They clamp
 * their input: a progress computed from a clock can leave [0,1] when the applet
 * resumes, and an unclamped curve then returns an absurd value - an outline
 * flying off screen. */

static inline float anim_clamp01(float p)
{
    /* A NaN fails BOTH `< 0` and `> 1`: without this test it passes through
     * intact and propagates into the geometry, where it makes what we are
     * drawing disappear WITHOUT any error message. The same trap was found in
     * nav.h on the animated scroll - it is the same computation. */
    if (isnan(p)) return 1.0f;   /* treat the movement as FINISHED */
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;
    return p;
}

/* Ease out: starts fast, ends slow. This is the curve of interfaces that feel
 * responsive - the movement is already 80 % done within the first third of the
 * time, so the user never has the impression of waiting. */
static inline float anim_ease_out(float p)
{
    p = anim_clamp01(p);
    const float u = 1.0f - p;
    return 1.0f - u * u * u;
}

/* Ease in AND out: for things that appear or disappear, where an abrupt start
 * would be noticed. */
static inline float anim_smooth(float p)
{
    p = anim_clamp01(p);
    return p < 0.5f ? 4.0f * p * p * p
                    : 1.0f - powf(-2.0f * p + 2.0f, 3.0f) / 2.0f;
}

/* --------------------------------------------------------------- movement */

typedef struct {
    float  start;
    float  target;
    double t0;        /* start, in the caller's own time base */
    float  duration;  /* seconds; 0 = instantaneous */
} anim_t;

/* An animation already finished on `value`. This is the correct initial state:
 * `anim_valeur` returns `value` at any instant, so nothing moves until a target
 * has actually been aimed at. */
static inline anim_t anim_fixed(float value)
{
    anim_t a;
    a.start   = value;
    a.target   = value;
    a.t0       = 0.0;
    a.duration = 0.0f;
    return a;
}

/* Raw progress in [0,1]. */
static inline float anim_progress(const anim_t *a, double t)
{
    if (!a) return 1.0f;
    if (!(a->duration > 0.0f)) return 1.0f;   /* also covers a NaN duration */
    /* Time can GO BACKWARDS: applet resuming after sleep, or the clock being
     * reset. A negative progress would send the movement back the other way and
     * then jump. We clamp it. */
    const double dt = t - a->t0;
    return anim_clamp01((float)(dt / (double)a->duration));
}

static inline bool anim_done(const anim_t *a, double t)
{
    return anim_progress(a, t) >= 1.0f;
}

/* Current value, eased out. */
static inline float anim_value(const anim_t *a, double t)
{
    if (!a) return 0.0f;
    const float p = anim_ease_out(anim_progress(a, t));
    return a->start + (a->target - a->start) * p;
}

/* Aims at a new target.
 *
 * THE POINT THAT MATTERS: the start is the CURRENT value, not the old target.
 * Without that, changing your mind mid-movement - which happens as soon as a
 * direction is held down - makes the outline JUMP to the position it was
 * supposed to reach before setting off again. The flaw is barely visible on a
 * still image and very visible in the hand.
 *
 * Aiming at the target we already have restarts nothing: otherwise holding a
 * direction against the end of the list would restart the animation every frame
 * and the movement would never arrive. */
static inline anim_t anim_towards(const anim_t *current, float target,
                               double t, float duration)
{
    anim_t a;
    const float now_value = current ? anim_value(current, t) : target;

    if (current && current->target == target) return *current;

    a.start   = now_value;
    a.target   = target;
    a.t0       = t;
    a.duration = (duration > 0.0f) ? duration : 0.0f;
    return a;
}

/* Reference durations. Sharing them keeps each screen from picking its own: two
 * neighbouring movements running at different speeds are noticed far more than a
 * single slightly-too-slow speed everywhere. */
#define ANIM_FOCUS_S   0.14f   /* outline moving from one item to the next */
#define ANIM_SCROLL_S  0.20f   /* scroll catching up */
#define ANIM_FONDU_S   0.22f   /* content fading in */

#endif /* UI_ANIM_H */
