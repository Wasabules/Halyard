/* test_ui_nav.c - the mathematics of a list screen (clients/borealis/ui/nav.h).
 *
 * This module replaces Borealis's POINTER focus with an INDEX focus, after three
 * "Instruction Abort" crashes in a single day: a view destroyed while it held
 * the focus, and the next virtual call went into a freed vtable. An index cannot
 * be freed - but it can be OUT OF RANGE, and that is now the only way to make
 * the same bug again. Half of the checks below talk about nothing else.
 *
 * Each check names its COUNTER-CASE: the exact input that would produce a defect
 * visible on screen. A failure here therefore says what has just been undone.
 */
#include "../clients/borealis/ui/nav.h"
#include <stdio.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* The values handled are integer multiples of pixels; the epsilon only covers
 * the rounding of the sums, not an approximation of the rule. */
static bool proche(float a, float b)
{
    const float d = a - b;
    return (d < 0.0f ? -d : d) < 0.001f;
}

/* ── La liste empty ────────────────────────────────────────────────────────── */

static void empty_list(void)
{
    /* COUNTER-CASE - THE EMPTY LIST RETURNED AS A 0.
     * The virtual machine inventory arrives EMPTY until the REST request has
     * answered, and becomes empty again when the account has none left. A focus
     * of 0 is a perfectly plausible index there: the rendering would draw the
     * accent highlight on a nonexistent item and the A button would activate it.
     * That is exactly the class of bug we are fleeing, transposed from pointers
     * to indices. -1 looks like no valid index. */
    nav_t empty = { 0, 0, 0.0f, 400.0f, 0.0f };
    CHECK(nav_clamp_focus(&empty) == -1,
            "COUNTER-CASE: an empty list -> -1, NEVER 0");
    CHECK(nav_move(&empty, +1) == -1, "an empty list: moving down creates no item");
    CHECK(nav_move(&empty, -1) == -1, "an empty list: moving up creates no item");
    CHECK(nav_move(&empty, 0) == -1, "an empty list: a zero delta does not either");
    CHECK(nav_move_wrap(&empty, +1) == -1,
            "an empty list: wrapping cannot wrap around nothing");

    /* nav_move_wrap()'s modulo would divide by nb: the empty list must be
     * rejected BEFORE, otherwise it is a division by zero. */
    CHECK(nav_move_wrap(&empty, -1000) == -1,
            "an empty list: no division by zero in the wrap");

    CHECK(nav_index_at_position(&empty, 10.0f, 90.0f, 10.0f) == -1,
            "an empty list: a finger touches nothing");
    CHECK(proche(nav_scroll_clamp(&empty, 50.0f), 0.0f),
            "an empty list: nothing to scroll");

    /* A struct that was never initialised may carry a negative nb. It must read
     * as "empty", not as permission to index backwards. */
    nav_t corrompue = { -3, 2, 0.0f, 400.0f, 0.0f };
    CHECK(nav_clamp_focus(&corrompue) == -1, "a negative nb is treated as an empty list");
    CHECK(nav_move(&corrompue, +1) == -1, "a negative nb: no movement");
}

/* ── Le focus hors bornes ─────────────────────────────────────────────────── */

static void focus_corrompu(void)
{
    /* COUNTER-CASE - THE INDEX THAT OUTLIVES ITS LIST.
     * The inventory is refreshed while the screen is displayed: the list goes
     * from 12 machines to 5 while the focus is 9. Nobody is wrong, and yet the
     * next frame draws - and can activate - item 9 of an array that holds 5.
     * Clamping is the primitive that makes that scenario harmless, and it must
     * hold for EVERY function of the module. */
    nav_t after_refresh = { 5, 9, 0.0f, 400.0f, 1000.0f };
    CHECK(nav_clamp_focus(&after_refresh) == 4,
            "COUNTER-CASE: focus 9 over 5 items -> 4, not 9");
    CHECK(nav_move(&after_refresh, +1) == 4,
            "COUNTER-CASE: the corrupted index is repaired, not propagated to 10");
    CHECK(nav_move(&after_refresh, -1) == 3,
            "moving up from a corrupted index starts at the bound, not at 9");
    CHECK(nav_move_wrap(&after_refresh, +1) == 0,
            "the wrap also starts from the clamped index");

    nav_t negatif = { 5, -7, 0.0f, 400.0f, 1000.0f };
    CHECK(nav_clamp_focus(&negatif) == 0, "focus negatif -> 0");
    CHECK(nav_move(&negatif, -1) == 0, "a negative focus does not go any lower");
    CHECK(nav_move(&negatif, +2) == 2, "a negative focus restarts from 0");
    CHECK(nav_move_wrap(&negatif, -1) == 4,
            "wrapping from a negative focus: clamped to 0 then wraps to nb-1");
}

/* -- Movement that stops at the ends (the default) ------------------------ */

static void deplacement_bute(void)
{
    nav_t l = { 8, 3, 0.0f, 400.0f, 1000.0f };
    CHECK(nav_move(&l, +1) == 4, "one notch down");
    CHECK(nav_move(&l, -1) == 2, "one notch up");
    CHECK(nav_move(&l, 0) == 3, "a zero delta does not move");
    CHECK(nav_move(&l, +10) == 7, "next page: stops at the last one");
    CHECK(nav_move(&l, -10) == 0, "previous page: stops at the first one");

    /* COUNTER-CASE - THE ENORMOUS DELTA.
     * +1000 must return the last index, not 1003: an out-of-range index returned
     * here would be written as is into nav_t.focus by the caller, and the next
     * frame's rendering would read past the array. The clamping must happen
     * INSIDE the function, not in the caller that will forget it. */
    CHECK(nav_move(&l, +1000) == 7,
            "COUNTER-CASE: a delta of +1000 is clamped to the last index");
    CHECK(nav_move(&l, -1000) == 0, "COUNTER-CASE: a delta of -1000 is clamped to the first");

    /* The worst representable delta. `focus + delta` computed as an int would
     * OVERFLOW: a signed overflow is undefined, and the compiler is then free to
     * delete the bound that follows. The sum is done in long long. */
    CHECK(nav_move(&l, 2147483647) == 7, "COUNTER-CASE: a delta of INT_MAX clamps cleanly");
    CHECK(nav_move(&l, -2147483647 - 1) == 0, "COUNTER-CASE: a delta of INT_MIN clamps cleanly");

    /* A list of a single item: both bounds are the same index. */
    nav_t seule = { 1, 0, 0.0f, 400.0f, 90.0f };
    CHECK(nav_move(&seule, +1) == 0, "a single item: moving down changes nothing");
    CHECK(nav_move(&seule, -1) == 0, "a single item: moving up changes nothing");
}

/* -- Movement that wraps (the pause menu) --------------------------------- */

static void wrap_move(void)
{
    /* The pause menu fits entirely on screen: wrapping there is a convenience
     * (one more notch down comes back to "Continue") and disorients nobody, since
     * every entry stays visible. That is why it is a SEPARATE function and not
     * the default: on an inventory of 40 machines, the same wrap would jump from
     * the bottom to the top without warning. */
    nav_t menu = { 4, 3, 0.0f, 400.0f, 360.0f };
    CHECK(nav_move_wrap(&menu, +1) == 0, "last + 1 -> first");

    nav_t first = { 4, 0, 0.0f, 400.0f, 360.0f };
    CHECK(nav_move_wrap(&first, -1) == 3, "first - 1 -> last");
    CHECK(nav_move_wrap(&first, +2) == 2, "an ordinary move is unchanged");

    /* COUNTER-CASE - C'S NEGATIVE MODULO REMAINDER.
     * `(0 - 1) % 4` is -1 in C, not 3: a naively written wrap therefore returns a
     * NEGATIVE index on the very first press of "up" from the first entry -
     * precisely the invalid index the whole module forbids itself. */
    CHECK(nav_move_wrap(&first, -5) == 3,
            "COUNTER-CASE: a negative delta larger than nb stays within [0, nb-1]");
    CHECK(nav_move_wrap(&first, -1000) == 0,
            "COUNTER-CASE: -1000 over 4 entries stays within the bounds");
    CHECK(nav_move_wrap(&menu, +1001) == 0, "delta enorme : (3+1001) % 4 = 0");
    CHECK(nav_move_wrap(&menu, 2147483647) == 2, "a delta of INT_MAX: no overflow");

    int i, clamp_ok = 1;
    for (i = -2000; i <= 2000; i++) {
        const int r = nav_move_wrap(&menu, i);
        if (r < 0 || r >= menu.nb) clamp_ok = 0;
    }
    CHECK(clamp_ok, "4001 consecutive deltas all stay within [0, nb-1]");
}

/* ── Bornage du defilement ────────────────────────────────────────────────── */

static void scroll_clamped(void)
{
    /* COUNTER-CASE - CONTENT SHORTER THAN THE VIEW.
     * Two virtual machines in a frame that displays six: content_h - view_h is
     * NEGATIVE. Returned as is, it allows a negative scroll and the list FLOATS
     * above its frame, detached from its title - a defect one only sees on an
     * account with few machines, hence never during a run on the development
     * account. */
    nav_t court = { 2, 0, 0.0f, 400.0f, 190.0f };
    CHECK(proche(nav_scroll_max(&court), 0.0f),
            "COUNTER-CASE: content shorter than the view -> a zero max scroll");
    CHECK(proche(nav_scroll_clamp(&court, -50.0f), 0.0f),
            "COUNTER-CASE: never a negative scroll");
    CHECK(proche(nav_scroll_clamp(&court, 50.0f), 0.0f),
            "COUNTER-CASE: short content -> we do not scroll at all");

    nav_t longue = { 12, 0, 0.0f, 400.0f, 1000.0f };
    CHECK(proche(nav_scroll_max(&longue), 600.0f), "max scroll = what overflows");
    CHECK(proche(nav_scroll_clamp(&longue, 300.0f), 300.0f), "a valid value passes through intact");
    CHECK(proche(nav_scroll_clamp(&longue, 700.0f), 600.0f), "past the end: clamped");
    CHECK(proche(nav_scroll_clamp(&longue, -0.5f), 0.0f), "above the start: clamped");
    CHECK(proche(nav_scroll_clamp(&longue, 0.0f), 0.0f), "the top of the list is valid");
    CHECK(proche(nav_scroll_clamp(&longue, 600.0f), 600.0f), "the bottom of the list is valid");

    /* COUNTER-CASE - THE NaN THAT GOES THROUGH BOTH BOUNDS.
     * An animated scroll is (t - t0) / (t1 - t0); two frames with the same
     * timestamp - which happens when the applet regains control after sleep -
     * give 0/0 = NaN. And NaN fails BOTH `< 0` and `> max`: it would go through a
     * naive clamp intact, and a list whose scroll is NaN disappears entirely from
     * the screen, with no message. */
    const float zero = 0.0f;
    const float nan_ = zero / zero;
    CHECK(!(nan_ == nan_), "the reference NaN really is one");
    CHECK(proche(nav_scroll_clamp(&longue, nan_), 0.0f),
            "COUNTER-CASE: a NaN scroll comes back to the top of the list");

    const float inf = 1.0f / zero;
    CHECK(proche(nav_scroll_clamp(&longue, inf), 600.0f), "+infinity clamps to the maximum");
    CHECK(proche(nav_scroll_clamp(&longue, -inf), 0.0f), "-infinity clamps to zero");
}

/* ── Amener l'element focalise a l'ecran ──────────────────────────────────── */

static void defilement_minimal(void)
{
    const float H = 80.0f, M = 12.0f;

    /* COUNTER-CASE - THE JITTER.
     * An item already entirely visible, with its breathing room: the scroll must
     * NOT change by a single pixel. A version that systematically recentred the
     * focused item would slide the whole list on every press, including when the
     * next item is already in plain sight - unreadable on the console's screen,
     * and impossible to aim at with a finger. */
    nav_t l = { 12, 2, 100.0f, 400.0f, 1200.0f };
    CHECK(proche(nav_scroll_to_reveal(&l, 200.0f, H, M), 100.0f),
            "COUNTER-CASE: an already visible item -> the scroll is UNCHANGED");

    /* The same with no margin at all, flush against both edges: the function
     * must not invent a movement where everything already fits. */
    CHECK(proche(nav_scroll_to_reveal(&l, 100.0f, 400.0f, 0.0f), 100.0f),
            "an item exactly the size of the frame: no movement");

    /* Scrolling up: the strict minimum to clear the top with its margin. */
    CHECK(proche(nav_scroll_to_reveal(&l, 50.0f, H, M), 38.0f),
            "an item above: we scroll up exactly what is needed (50 - margin)");

    /* Moving down: the item's bottom plus its margin meets the frame's bottom. */
    CHECK(proche(nav_scroll_to_reveal(&l, 460.0f, H, M), 152.0f),
            "an item below: we scroll down exactly what is needed");

    /* And that minimal movement does make the item visible: a "minimal" rule
     * that stops one pixel short is useless. */
    nav_t after = l;
    after.scroll = nav_scroll_to_reveal(&l, 460.0f, H, M);
    CHECK(nav_is_visible(&after, 460.0f, H), "after scrolling, the item is visible");

    /* IDEMPOTENCE - the property that guarantees the absence of jitter:
     * reapplying the rule to its own result moves nothing. Without it, two calls
     * in the same frame (rendering then input handling) would fight. */
    CHECK(proche(nav_scroll_to_reveal(&after, 460.0f, H, M), after.scroll),
            "the rule is idempotent (second application: no movement)");

    /* The result stays clamped: the last item sacrifices its bottom margin
     * rather than scrolling past the end of the content. */
    nav_t end = { 12, 11, 0.0f, 400.0f, 1200.0f };
    CHECK(proche(nav_scroll_to_reveal(&end, 1120.0f, H, M), 800.0f),
            "last item: we do not scroll past the end of the content");

    /* An already corrupted scroll is repaired in passing rather than
     * propagated. */
    nav_t sale = { 12, 0, -400.0f, 400.0f, 1200.0f };
    CHECK(proche(nav_scroll_to_reveal(&sale, 0.0f, H, 0.0f), 0.0f),
            "an inherited negative scroll is repaired, not kept");
}

static void item_taller_than_frame(void)
{
    /* COUNTER-CASE - THE ITEM THAT DOES NOT FIT IN THE FRAME.
     * A virtual machine card with its description is 300 px in a 200 px frame in
     * handheld mode. No scroll satisfies both its edges, and both ways of failing
     * are visible to the eye:
     *   - handling the top then the bottom in turn makes the list OSCILLATE
     *     between two positions on every frame;
     *   - merely forbidding the downward move leaves an item located BELOW the
     *     frame off screen forever, while the focus is drawn on it: you navigate
     *     blind.
     * The chosen rule aligns the TOP in both directions. */
    const float H = 300.0f, M = 10.0f;
    nav_t l = { 6, 0, 0.0f, 200.0f, 2000.0f };

    const float from_top = nav_scroll_to_reveal(&l, 500.0f, H, M);
    CHECK(proche(from_top, 490.0f),
            "COUNTER-CASE: an over-tall item located BELOW the frame - we bring it in anyway");

    nav_t amene = l;
    amene.scroll = from_top;
    CHECK(proche(nav_scroll_to_reveal(&amene, 500.0f, H, M), from_top),
            "COUNTER-CASE: no oscillation - the result is a fixed point");

    /* From the middle of the item (the user had scrolled into it), we come back
     * to its start: it is defined, stable, and it is what we want when the focus
     * has just landed on it. */
    nav_t dedans = l;
    dedans.scroll = 600.0f;
    CHECK(proche(nav_scroll_to_reveal(&dedans, 500.0f, H, M), 490.0f),
            "an over-tall item: we show its start");

    /* The very first item, whose margin falls outside the content: clamped to 0
     * and stable. */
    nav_t start = { 6, 0, 0.0f, 200.0f, 2000.0f };
    CHECK(proche(nav_scroll_to_reveal(&start, 0.0f, H, M), 0.0f),
            "an over-tall first item: stays flush with the start of the content");
}

/* -- Consistency between the two visibility functions --------------------- */

static void visible_coherence_and_scrolling(void)
{
    /* Two functions that answer the same question must answer the same way:
     * with no margin, "do not move" and "is visible" are the SAME predicate. If
     * they diverged, the rendering would believe the item visible while the
     * scrolling rule moves it - that is, permanent jitter nobody could attribute.
     * Checked over the whole list. */
    const float H = 90.0f, GAP = 10.0f;
    nav_t l = { 10, 0, 137.0f, 400.0f, 0.0f };
    l.content_h = nav_content_h_uniform(l.nb, H, GAP);

    int i, coherent = 1;
    for (i = 0; i < l.nb; i++) {
        const float y = nav_y_of_index(i, H, GAP);
        const bool visible = nav_is_visible(&l, y, H);
        const bool immobile = proche(nav_scroll_to_reveal(&l, y, H, 0.0f), l.scroll);
        if (visible != immobile) coherent = 0;
    }
    CHECK(coherent, "with no margin, \u00ab visible \u00bb and \u00ab does not move \u00bb are the same predicate");

    /* And the chosen definition really is ENTIRELY visible: an item cut by an
     * edge must count as not visible, otherwise we never bring it back in
     * full. */
    nav_t up = { 10, 0, 50.0f, 400.0f, 990.0f };
    CHECK(!nav_is_visible(&up, 0.0f, H), "an item cut off at the top = NOT visible");
    CHECK(!nav_is_visible(&up, 400.0f, H), "an item cut off at the bottom = NOT visible");
    CHECK(nav_is_visible(&up, 50.0f, H), "an item flush with the top = visible");
    CHECK(nav_is_visible(&up, 360.0f, H), "an item flush with the bottom = visible");
}

/* ── Tactile ──────────────────────────────────────────────────────────────── */

static void position_tactile(void)
{
    const float H = 90.0f, GAP = 10.0f;      /* pas = 100 px */
    nav_t l = { 6, 0, 0.0f, 400.0f, 0.0f };
    l.content_h = nav_content_h_uniform(l.nb, H, GAP);

    CHECK(nav_index_at_position(&l, 0.0f, H, GAP) == 0, "at the very top: item 0");
    CHECK(nav_index_at_position(&l, 45.0f, H, GAP) == 0, "the middle of the first one");
    CHECK(nav_index_at_position(&l, 100.0f, H, GAP) == 1, "the top of the second");
    CHECK(nav_index_at_position(&l, 189.0f, H, GAP) == 1, "the bottom of the second");

    /* COUNTER-CASE - THE FINGER IN THE GAP BETWEEN TWO CARDS.
     * A `y / (item_h + gap)` returns an index for ANY value, including for a
     * finger placed exactly in the gap. The user then sees a virtual machine
     * start that they never touched: on this screen that is a heavy action
     * triggered by a few pixels of imprecision. */
    CHECK(nav_index_at_position(&l, 95.0f, H, GAP) == -1,
            "COUNTER-CASE: a finger in the gap between two cards -> -1");
    CHECK(nav_index_at_position(&l, 195.0f, H, GAP) == -1,
            "COUNTER-CASE: the same further down the list");

    /* Outside the frame: the touch event carries the coordinates of the whole
     * screen, and nothing guarantees it was aimed at this list. */
    CHECK(nav_index_at_position(&l, -1.0f, H, GAP) == -1, "above the frame -> -1");
    CHECK(nav_index_at_position(&l, 401.0f, H, GAP) == -1, "below the frame -> -1");

    /* COUNTER-CASE - THE FINGER BELOW THE LAST ITEM.
     * Six 100 px cards in a 1000 px frame: the empty area below the list is part
     * of the frame. An index returned there would be out of range. */
    nav_t aere = { 6, 0, 0.0f, 1000.0f, 0.0f };
    aere.content_h = nav_content_h_uniform(aere.nb, H, GAP);
    CHECK(nav_index_at_position(&aere, 700.0f, H, GAP) == -1,
            "COUNTER-CASE: a finger in the void below the last card -> -1");
    CHECK(nav_index_at_position(&aere, 500.0f, H, GAP) == 5, "the last card is touched");

    /* COUNTER-CASE - THE FORGOTTEN FRAME OF REFERENCE.
     * `y_local` is relative to the FRAME, not to the content: the scroll must be
     * added to it. Forgetting it gives a list that answers correctly as long as
     * nothing has been scrolled, then answers beside the point - off by as many
     * cards as have been scrolled, hence invisible during a run on a short
     * list. */
    nav_t defilee = l;
    defilee.scroll = 200.0f;
    CHECK(nav_index_at_position(&defilee, 0.0f, H, GAP) == 2,
            "COUNTER-CASE: a list scrolled by 200 px -> the top of the frame is item 2");
    CHECK(nav_index_at_position(&defilee, 100.0f, H, GAP) == 3,
            "COUNTER-CASE: the scroll does shift every index");

    /* Absurd inputs: better to touch nothing than to touch at random. */
    const float zero = 0.0f;
    CHECK(nav_index_at_position(&l, zero / zero, H, GAP) == -1, "y_local NaN -> -1");
    CHECK(nav_index_at_position(&l, 10.0f, 0.0f, GAP) == -1, "hauteur nulle -> -1");
    CHECK(nav_index_at_position(&l, 10.0f, H, -200.0f) == -1, "an absurd negative step -> -1");
}

/* ── Geometrie d'une liste uniforme ───────────────────────────────────────── */

static void geometrie_uniforme(void)
{
    const float H = 90.0f, GAP = 10.0f;

    /* COUNTER-CASE - THE EXTRA GAP AT THE END.
     * `nb * (item_h + gap)` counts a gap AFTER the last card: content_h then
     * exceeds the real content by `gap` pixels and the list agrees to scroll that
     * far into the void. At the end of the travel the last card lifts off the
     * bottom of the frame, which reads as a missing card. There are nb - 1 gaps,
     * not nb. */
    CHECK(proche(nav_content_h_uniform(6, H, GAP), 590.0f),
            "COUNTER-CASE: 6 cards = 590 px, not 600 (no gap after the last one)");
    CHECK(proche(nav_content_h_uniform(1, H, GAP), 90.0f), "a single card: no gap");
    CHECK(proche(nav_content_h_uniform(0, H, GAP), 0.0f), "an empty list: zero height");
    CHECK(proche(nav_content_h_uniform(-2, H, GAP), 0.0f), "nb negatif : hauteur nulle");

    /* A direct consequence: at the end of the travel the last card sits
     * EXACTLY at the bottom of the frame. That is what the counter-case above
     * breaks. */
    nav_t l = { 6, 5, 0.0f, 400.0f, 0.0f };
    l.content_h = nav_content_h_uniform(l.nb, H, GAP);
    l.scroll = nav_scroll_max(&l);
    CHECK(nav_is_visible(&l, nav_y_of_index(5, H, GAP), H),
            "at the end of the travel, the last card is entirely visible");

    /* A ROUND TRIP index -> y -> index. That is what stops the two formulas from
     * diverging: if a screen copied `i * (h + gap)` by hand and one of the two
     * were changed, the list would no longer answer where it draws. */
    nav_t wide = { 20, 0, 0.0f, 4000.0f, 0.0f };
    wide.content_h = nav_content_h_uniform(wide.nb, H, GAP);
    int i, aller_retour = 1;
    for (i = 0; i < wide.nb; i++) {
        const float y = nav_y_of_index(i, H, GAP);
        if (nav_index_at_position(&wide, y, H, GAP) != i) aller_retour = 0;
        if (nav_index_at_position(&wide, y + H - 1.0f, H, GAP) != i) aller_retour = 0;
    }
    CHECK(aller_retour, "an exact round trip index -> y -> index over 20 cards");

    CHECK(proche(nav_y_of_index(0, H, GAP), 0.0f), "the first card is at the origin");
    CHECK(proche(nav_y_of_index(3, H, GAP), 300.0f), "the fourth card is 3 steps down");
}

int main(void)
{
    printf("== list navigation: focus by index, scrolling, touch ==\n");
    empty_list();
    focus_corrompu();
    deplacement_bute();
    wrap_move();
    scroll_clamped();
    defilement_minimal();
    item_taller_than_frame();
    visible_coherence_and_scrolling();
    position_tactile();
    geometrie_uniforme();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
