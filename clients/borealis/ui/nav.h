/* nav.h - the MATHEMATICS of a list screen: focus, scrolling, touch.
 *
 * WHY THIS MODULE EXISTS. Three crashes in a single day, all of the same
 * family: `Application::currentFocus` is a view POINTER, and as soon as a view
 * is destroyed while it holds focus, the next virtual call jumps into a freed
 * vtable (Atmosphere report "Instruction Abort"). Borealis' own guards were
 * useless here: `giveFocus(nullptr)` is a complete no-op, and
 * `Box::getDefaultFocus()` walks back down through `lastFocusedView`, so
 * "move the focus somewhere else" moved nothing at all.
 *
 * Hence the non-negotiable rule of our own layer: THE FOCUS IS AN INTEGER
 * INDEX. An integer cannot be freed, and an out-of-range index gets CLAMPED -
 * it is never dereferenced. Screens are DATA redrawn every frame; use-after-free
 * becomes structurally impossible. This file therefore holds everything in a
 * list screen that is arithmetic: where the focus goes, how far to scroll, which
 * item a finger landed on.
 *
 * PURE - no global state, no I/O, no getenv, no allocation, no graphics type.
 * Header-only because these are few-line functions called every frame, and so
 * that tests/test_ui_nav.c can check them offline: no console, no virtual
 * machine, no GL context.
 *
 * COORDINATE FRAMES - there are two, and they hold for the whole file:
 *   - `item_y` and `contenu_h` are in CONTENT space (origin = top of the first
 *     item, independent of scrolling);
 *   - `y_local` is in VISIBLE FRAME space (origin = top of the frame, which is
 *     what a touch event reports);
 *   - `scroll` converts between them: y_content = y_local + scroll.
 * Mixing the two gives a list that answers next to where you touched as soon as
 * it has been scrolled once - the easiest mistake to make in this file.
 *
 * Created 2026-08-27 (replacement for the Borealis widget layer).
 */
#ifndef UI_NAV_H
#define UI_NAV_H

#include <stdbool.h>

/* Complete state of a list. Purely descriptive: these five numbers are enough to
 * draw the screen, and nothing else needs to survive from one frame to the next.
 * `nb` changes when the list changes (a virtual machine leaving the inventory),
 * so `focus` can go out of range WITHOUT anyone being at fault. Every function
 * below assumes that. */
typedef struct {
    int   nb;         /* item count; 0 = empty list, negative = corrupt        */
    int   focus;      /* focused index; may be out of range, see above         */
    float scroll;     /* current scroll, in pixels, content space              */
    float view_h;      /* height of the visible frame, in pixels                */
    float content_h;  /* total height of the content, in pixels                */
} nav_t;

/* --- Focus --------------------------------------------------------------- */

/* Clamps the focus into range. THIS IS THE MODULE'S ANTI-CRASH PRIMITIVE: the
 * renderer calls it before drawing the accent outline, and every index coming
 * from elsewhere (restored state, a list that shrank between two frames) goes
 * through it.
 *
 * COUNTER-CASE - THE EMPTY LIST. Returning 0 when `nb == 0` would be exactly the
 * class of bug we are running from: the caller would draw a focus on item 0 of
 * an array that has none, and would confirm it on the A button. We return -1,
 * which looks like no valid index and makes any careless use fail loudly. A
 * negative `nb` (a struct that was never initialised) is treated as an empty
 * list, not as an invitation to index backwards. */
static inline int nav_clamp_focus(const nav_t *n)
{
    if (!n || n->nb <= 0) return -1;
    if (n->focus < 0) return 0;
    if (n->focus > n->nb - 1) return n->nb - 1;
    return n->focus;
}

/* Moves the focus by `delta` items and STOPS at the ends.
 *
 * THE CALL: the default STOPS, it does not wrap. On a long list (virtual machine
 * inventory, settings list) wrapping disorients: one press too many at the
 * bottom silently sends you back to the very top, and someone holding the
 * direction down never sees the boundary go by. Stopping gives physical
 * feedback instead - the list refuses to move, so you know you are at the end.
 * Wrapping stays available through nav_deplacer_boucle() below, but it has to be
 * ASKED FOR: it is a per-screen choice, not a silent default.
 *
 * The computation starts from the CLAMPED focus, never from the raw field: a
 * corrupt index is repaired here, it does not propagate into the result.
 *
 * COUNTER-CASE - THE HUGE DELTA. A "next page" is commonly +10, and fast
 * scrolling can accumulate far more. `focus + delta` computed in `int` OVERFLOWS
 * for a delta near INT_MAX, and signed overflow is undefined behaviour: the
 * compiler is entitled to conclude that the following bound check is always
 * true. So we sum in `long long`, where no pair of `int`s can overflow. */
static inline int nav_move(const nav_t *n, int delta)
{
    const int f = nav_clamp_focus(n);
    if (f < 0) return -1;

    long long target = (long long)f + (long long)delta;
    if (target < 0) target = 0;
    if (target > (long long)n->nb - 1) target = (long long)n->nb - 1;
    return (int)target;
}

/* The WRAPPING variant: this is the one the pause menu uses. With a handful of
 * entries that all fit on screen, wrapping is useful (one more press down to get
 * back to "Resume") and carries no risk of disorientation since the whole list
 * stays visible.
 *
 * The modulo is computed in `long long` for the same reason as above, then
 * brought back into [0, nb-1]: C's `%` returns a NEGATIVE remainder for a
 * negative dividend, so a plain `(f + delta) % nb` would return a negative index
 * as soon as you go up past the first item - precisely the invalid index this
 * module forbids itself. */
static inline int nav_move_wrap(const nav_t *n, int delta)
{
    const int f = nav_clamp_focus(n);
    if (f < 0) return -1;

    long long target = ((long long)f + (long long)delta) % (long long)n->nb;
    if (target < 0) target += (long long)n->nb;
    return (int)target;
}

/* --- Scrolling ----------------------------------------------------------- */

/* Largest reachable scroll: what sticks out of the frame, and nothing more.
 *
 * COUNTER-CASE - CONTENT SHORTER THAN THE VIEW. `contenu_h - vue_h` is then
 * NEGATIVE. Returned as is, it allows a negative scroll and the list FLOATS
 * above its frame, detached from its title. We clamp at 0 here, once, and the
 * whole rest of the module relies on it. */
static inline float nav_scroll_max(const nav_t *n)
{
    if (!n) return 0.0f;
    const float max = n->content_h - n->view_h;
    return max > 0.0f ? max : 0.0f;
}

/* Clamps a wanted scroll into [0, nav_scroll_max()].
 *
 * The NaN test is not idle paranoia: an animated scroll is computed by an
 * interpolation `(t - t0) / (t1 - t0)`, and a zero duration - two frames with
 * the same timestamp, which happens when the applet regains control after sleep
 * - produces 0/0 = NaN. And NaN fails BOTH `< 0` and `> max`: it would pass
 * through a naive clamp untouched, and a list whose scroll is NaN vanishes
 * entirely from the screen without a single message. We return to the top of the
 * list, which is always a valid state. */
static inline float nav_scroll_clamp(const nav_t *n, float wanted)
{
    if (!(wanted == wanted)) return 0.0f;    /* NaN */

    const float max = nav_scroll_max(n);
    if (wanted < 0.0f) return 0.0f;
    if (wanted > max)  return max;
    return wanted;
}

/* Returns true if the item [item_y, item_y + item_h] is ENTIRELY visible.
 *
 * "Entirely", not "somewhat": the caller uses this to decide whether to scroll,
 * and a half-cut item has to be brought fully into view. This function knows
 * nothing about the breathing margin - it answers a geometric question. Its
 * exact relation to the next function is: nav_scroll_pour_rendre_visible(...,
 * margin = 0) leaves the scroll alone if and only if nav_est_visible() returns
 * true. The test checks that equivalence across a whole list, because two
 * functions that contradict each other would produce a scroll that moves while
 * believing it is still. */
static inline bool nav_is_visible(const nav_t *n, float item_y, float item_h)
{
    if (!n) return false;
    return item_y >= n->scroll && item_y + item_h <= n->scroll + n->view_h;
}

/* The MINIMAL scroll that makes the item entirely visible, keeping `margin`
 * pixels of breathing room above and below.
 *
 * COUNTER-CASE - THE JITTER. If this function always re-centred the focused
 * item, every press would shift the list even when the next item is already in
 * plain sight: the text slides constantly and becomes tiring to read on a
 * 6-inch screen. So we move ONLY when the item sticks out of the frame, and we
 * scroll by the strict minimum. Corollary, checked by the test: the function is
 * IDEMPOTENT - applying it to its own result moves nothing.
 *
 * COUNTER-CASE - THE ITEM TALLER THAN THE FRAME (a virtual machine card with its
 * description, on a screen in handheld mode). It can then NEVER satisfy both
 * edges at once, and the two possible mistakes are equally bad:
 *   - aligning the top and the bottom in turn makes the list OSCILLATE between
 *     two positions every frame;
 *   - merely disabling the downward move leaves an item sitting BELOW the frame
 *     off screen forever, while the focus is drawn on it - you end up navigating
 *     blind in a list that no longer moves.
 * So we align its TOP (an item is read from its beginning): that is defined in
 * both directions, and it is a fixed point, hence no oscillation.
 *
 * The result is clamped: an already-corrupt scroll is repaired on the way
 * through, and the bottom margin of the last item is sacrificed rather than
 * scrolling past the end of the content. */
static inline float nav_scroll_to_reveal(const nav_t *n, float item_y,
                                                   float item_h, float margin)
{
    if (!n) return 0.0f;

    const float want_top    = item_y - margin;
    const float want_bottom = item_y + item_h + margin;
    const bool  fits        = (item_h + 2.0f * margin) <= n->view_h;

    float scroll = n->scroll;
    if (want_top < scroll) {
        scroll = want_top;                          /* scroll up */
    } else if (want_bottom > scroll + n->view_h) {
        scroll = fits ? want_bottom - n->view_h      /* scroll down just enough */
                      : want_top;                   /* too tall: show the start */
    }
    return nav_scroll_clamp(n, scroll);
}

/* --- Touch --------------------------------------------------------------- */

/* Index of the item touched at `y_local` (VISIBLE FRAME space, see the header),
 * or -1 if the finger hit no item at all.
 *
 * `item_h` is the height of one item and `gap` the space separating it from the
 * next; the assumed layout is therefore uniform, the same one used by
 * nav_y_de_index() and nav_contenu_h_uniforme().
 *
 * COUNTER-CASE - THE GAP BETWEEN TWO CARDS. A plain `y / (item_h + gap)` returns
 * an index for EVERY value, including a finger landing exactly in the void
 * between two cards. The user then sees a virtual machine start that they never
 * touched - on a list screen that is a destructive action triggered by a few
 * pixels of imprecision. So we check that the point falls INSIDE the item and
 * not in the gap that follows it.
 *
 * A `y_local` outside the frame returns -1: a touch event arrives with
 * whole-screen coordinates, and nothing tells the caller that it was aimed at
 * this list. */
static inline int nav_index_at_position(const nav_t *n, float y_local,
                                          float item_h, float gap)
{
    if (!n || n->nb <= 0) return -1;
    if (!(y_local == y_local)) return -1;            /* NaN */
    if (item_h <= 0.0f) return -1;

    const float step = item_h + gap;
    if (step <= 0.0f) return -1;                     /* absurd negative gap */

    if (y_local < 0.0f || y_local > n->view_h) return -1;

    const float y = y_local + n->scroll;             /* -> content space */
    if (y < 0.0f) return -1;

    const long long idx = (long long)(y / step);     /* y >= 0: truncation = floor */
    if (idx >= (long long)n->nb) return -1;

    if (y - (float)idx * step > item_h) return -1;   /* landed in the gap */
    return (int)idx;
}

/* --- Geometry of a uniform list ------------------------------------------ */

/* Top of item `index`, in CONTENT space.
 *
 * It exists so that the formula `index * (item_h + gap)` is not copied into
 * every screen: copied, it eventually drifts away from the one in
 * nav_index_a_la_position(), and the list stops answering where it draws. The
 * test checks the index -> y -> index round trip across a whole list. */
static inline float nav_y_of_index(int index, float item_h, float gap)
{
    return (float)index * (item_h + gap);
}

/* Total height of a uniform list, to be stored in `contenu_h`.
 *
 * COUNTER-CASE - THE EXTRA GAP AT THE END. `nb * (item_h + gap)` counts a gap
 * AFTER the last item: `contenu_h` then exceeds the real content by `gap` pixels,
 * and the list happily scrolls that far into nothing. The last item lifts off the
 * bottom of the frame at the end of travel, which reads as a missing item. There
 * are `nb - 1` gaps, not `nb`. */
static inline float nav_content_h_uniform(int nb, float item_h, float gap)
{
    if (nb <= 0) return 0.0f;
    return (float)nb * item_h + (float)(nb - 1) * gap;
}

#endif /* UI_NAV_H */
