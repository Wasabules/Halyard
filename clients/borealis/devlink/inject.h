/* inject.h - the timing of synthetic input, as a pure state machine.
 *
 * WHY A SEPARATE, PURE MODULE. What makes injection hard is not pressing a
 * button, it is pressing it for the RIGHT LENGTH OF TIME. Borealis polls
 * controller state once per frame: a press shorter than a frame is simply never
 * seen (the shutdown audit already paid for that - see KB, AF2/AF6), and a press
 * that never ends leaves the console stuck on a button nobody is holding. All of
 * that is arithmetic on a clock, so it lives here: no Borealis, no libnx, no
 * clock of its own, no allocation. The caller passes `now_ms` and reads the
 * result. `tests/test_inject.c` checks it with no console and no window.
 *
 * WHERE IT IS PLUGGED IN. `inject.cpp` ORs the sampled state into the platform's
 * `updateUnifiedControllerState` / `updateTouchStates`, through a weak symbol
 * (see patches/borealis/). That is the LOWEST common layer, and choosing it is
 * the whole point: everything downstream then sees a press it cannot tell from a
 * real one - Borealis' actions and focus navigation, our screens' `up()/down()`,
 * the hold-to-exit timers that poll STATE rather than events, and the stream,
 * which forwards the pad to the virtual machine. Injecting higher - calling
 * `Application::onControllerButtonPressed`, or a screen's virtual method - would
 * have covered the menus and silently missed the other three.
 *
 * THE A/B SWAP IS NOT OUR BUSINESS. Borealis applies it in `application.cpp`
 * AFTER the input manager has filled the state, so an injected BUTTON_A goes
 * through exactly the same path as the physical one: `btn a` presses what the
 * console calls A, whatever the user's setting.
 *
 * Created 2026-09-12 (INJ-1).
 */
#ifndef DEVLINK_INJECT_H
#define DEVLINK_INJECT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sized on brls::_BUTTON_MAX (21) and _AXES_MAX (6) with room to spare. The
 * header does NOT include Borealis: a compile-time check in `inject.cpp` ties
 * the two together, so a Borealis that grows its enum breaks the build there
 * rather than writing past the end here. */
#define INJECT_BUTTON_MAX 24
#define INJECT_AXIS_MAX   8

/* Bounds on a request. A press must outlast a frame to be seen at all, hence a
 * floor well above 16 ms; the ceiling keeps a forgotten command from pinning a
 * button for the whole session - the worst failure mode of such a tool, because
 * it looks like broken hardware. */
#define INJECT_MS_MIN     20
#define INJECT_MS_MAX     10000
#define INJECT_MS_DEFAULT 120      /* ~7 frames at 60 Hz: seen, and still brisk */

typedef struct {
    /* 0 = released. Otherwise the instant, in ms, at which it releases. */
    long long button_until[INJECT_BUTTON_MAX];
    long long axis_until[INJECT_AXIS_MAX];
    float     axis_value[INJECT_AXIS_MAX];

    /* The touch runs from (x0,y0) to (x1,y1) between `touch_from` and
     * `touch_until`. A tap is the degenerate case where the two points are
     * equal - one code path, not two. */
    long long touch_from, touch_until;
    float     x0, y0, x1, y1;
} inject_state_t;

static inline void inject_clear(inject_state_t *st)
{
    size_t i;
    if (!st) return;
    for (i = 0; i < INJECT_BUTTON_MAX; i++) st->button_until[i] = 0;
    for (i = 0; i < INJECT_AXIS_MAX; i++) { st->axis_until[i] = 0; st->axis_value[i] = 0.0f; }
    st->touch_from = st->touch_until = 0;
    st->x0 = st->y0 = st->x1 = st->y1 = 0.0f;
}

static inline int inject_clamp_ms(int ms)
{
    if (ms <= 0)              return INJECT_MS_DEFAULT;
    if (ms < INJECT_MS_MIN)   return INJECT_MS_MIN;
    if (ms > INJECT_MS_MAX)   return INJECT_MS_MAX;
    return ms;
}

/* Holds `button` until `now + ms`.
 *
 * EXTENDS, never shortens: two overlapping requests on the same button leave it
 * held until the later of the two. Taking the newest unconditionally would let
 * a 20 ms tap cut short a 3 s hold that a script had deliberately started -
 * and the symptom would be "the hold-to-exit never fires", blamed on the UI. */
static inline bool inject_press(inject_state_t *st, int button, long long now, int ms)
{
    long long until;
    if (!st || button < 0 || button >= INJECT_BUTTON_MAX) return false;
    until = now + inject_clamp_ms(ms);
    if (until > st->button_until[button]) st->button_until[button] = until;
    return true;
}

/* Releases it at once. The escape hatch when a script has lost track. */
static inline bool inject_release(inject_state_t *st, int button)
{
    if (!st || button < 0 || button >= INJECT_BUTTON_MAX) return false;
    st->button_until[button] = 0;
    return true;
}

/* Holds `axis` at `value` (-1..1, clamped) until `now + ms`. Unlike a button,
 * the value REPLACES: a stick has one position, and the last order wins. */
static inline bool inject_axis(inject_state_t *st, int axis, float value,
                               long long now, int ms)
{
    if (!st || axis < 0 || axis >= INJECT_AXIS_MAX) return false;
    if (value < -1.0f) value = -1.0f;
    if (value >  1.0f) value =  1.0f;
    st->axis_value[axis] = value;
    st->axis_until[axis] = now + inject_clamp_ms(ms);
    return true;
}

/* A touch travelling from (x0,y0) to (x1,y1) over `ms`. A tap passes the same
 * point twice. */
static inline bool inject_touch(inject_state_t *st, float x0, float y0,
                                float x1, float y1, long long now, int ms)
{
    if (!st) return false;
    st->touch_from  = now;
    st->touch_until = now + inject_clamp_ms(ms);
    st->x0 = x0; st->y0 = y0; st->x1 = x1; st->y1 = y1;
    return true;
}

/* Reads the state at `now`.
 *
 * `buttons` and `axes` are ORed / added into, never zeroed: the caller is the
 * platform's input manager, which has already put the REAL controller there.
 * A script driving the console must not stop a human from taking over - and on
 * a device where the two share one screen, that is not a nicety.
 *
 * Returns true when a touch is active, and then writes its interpolated
 * position. */
static inline bool inject_sample(const inject_state_t *st, long long now,
                                 bool *buttons, size_t nb,
                                 float *axes, size_t na,
                                 float *tx, float *ty)
{
    size_t i;
    if (!st) return false;

    if (buttons)
        for (i = 0; i < nb && i < INJECT_BUTTON_MAX; i++)
            if (st->button_until[i] > now) buttons[i] = true;

    if (axes)
        for (i = 0; i < na && i < INJECT_AXIS_MAX; i++)
            if (st->axis_until[i] > now) {
                float v = axes[i] + st->axis_value[i];
                if (v < -1.0f) v = -1.0f;
                if (v >  1.0f) v =  1.0f;
                axes[i] = v;
            }

    if (st->touch_until > now && st->touch_from <= now) {
        /* Linear interpolation. The span cannot be zero: `inject_touch` adds a
         * clamped duration of at least INJECT_MS_MIN, so there is no division
         * by zero to guard - and no branch pretending there might be. */
        const long long span = st->touch_until - st->touch_from;
        const long long done = now - st->touch_from;
        const float k = (float)((double)done / (double)span);
        if (tx) *tx = st->x0 + (st->x1 - st->x0) * k;
        if (ty) *ty = st->y0 + (st->y1 - st->y0) * k;
        return true;
    }
    return false;
}

/* Is anything at all being injected? Lets the hook return immediately in the
 * overwhelmingly common case - no script attached - so the cost on a normal
 * session is one comparison per frame. */
static inline bool inject_busy(const inject_state_t *st, long long now)
{
    size_t i;
    if (!st) return false;
    for (i = 0; i < INJECT_BUTTON_MAX; i++) if (st->button_until[i] > now) return true;
    for (i = 0; i < INJECT_AXIS_MAX; i++)   if (st->axis_until[i]   > now) return true;
    return st->touch_until > now;
}

#ifdef __cplusplus
}
#endif

#endif /* DEVLINK_INJECT_H */
