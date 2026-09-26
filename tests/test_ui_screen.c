/* test_ui_screen.c - the arithmetic of a list screen WITH KINDS.
 *
 * What this file checks is the PURE part of clients/borealis/ui/screen.hpp: which entry
 * can take the focus, what height it occupies, where it starts, and which one a
 * finger touched. It is ordinary C, exactly like nav.h and anim.h - no console,
 * no GL context, no virtual machine.
 *
 * WHY THIS SUITE EXISTS. Kinds made the list NON-UNIFORM: a section separator is
 * not the height of a machine card, and a settings row without a description is
 * shorter than one with. `nav.h` only covers the uniform layout, so the
 * variable-height version had to be written - and a wrong list geometry does not
 * show: it just DRAWS correctly, and ANSWERS beside the point. You start a
 * virtual machine you never touched.
 *
 * Hence the check that structures this whole file: on a list made only of cards,
 * the new geometry must return EXACTLY what nav.h returns. The machine list
 * existed before kinds; if the two diverge by one pixel, it is the one that
 * breaks first.
 *
 * Each check names its COUNTER-CASE: the exact input that would produce a defect
 * visible in the hand. A failure here therefore says what has just been undone.
 */
#include "../clients/borealis/ui/screen.hpp"   /* its pure part is C: see its header */
#include "../clients/borealis/ui/nav.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

static bool proche(float a, float b) { return fabsf(a - b) < 0.001f; }

/* A minimal `nav_t`: only `nb`, `scroll` and `view_h` matter for the geometry
 * tested here. */
static nav_t view(int nb, float scroll, float view_h, float content_h)
{
    nav_t n;
    n.nb        = nb;
    n.focus     = 0;
    n.scroll    = scroll;
    n.view_h     = view_h;
    n.content_h = content_h;
    return n;
}

/* ===================================================================== */
/* Focalisabilite                                                        */
/* ===================================================================== */

static void focalisabilite(void)
{
    CHECK(ui_focusable(UI_KIND_CARD,   true),  "a card is focusable");
    CHECK(ui_focusable(UI_KIND_TOGGLE, true),  "a toggle is focusable");
    CHECK(ui_focusable(UI_KIND_CHOICE,   true),  "a choice is focusable");
    CHECK(ui_focusable(UI_KIND_ACTION,  true),  "an action is focusable");

    /* COUNTER-CASE - THE FOCUSABLE SECTION SEPARATOR.
     * `actionable` is true by DEFAULT in ui::Item: a caller adding a section
     * title does not think to set it to false. If the kind did not win, moving
     * down a settings page would stop on a title every other press, and the A
     * button would do nothing there - the screen would look broken. */
    CHECK(!ui_focusable(UI_KIND_TITLE, true),
            "COUNTER-CASE: a Title is NEVER focusable, even with the "
            "actionable flag true (which is its default)");
    CHECK(!ui_focusable(UI_KIND_TITLE, false), "... nor, obviously, without it");

    /* The "no machines" message: displayed, never selectable. */
    CHECK(!ui_focusable(UI_KIND_CARD, false),
            "a non-actionable entry does not take the focus");
}

/* ===================================================================== */
/* Hauteurs                                                              */
/* ===================================================================== */

static void heights(void)
{
    CHECK(proche(ui_entry_height(UI_KIND_TITLE, false), UI_H_TITLE),
            "a separator has its own height");
    CHECK(proche(ui_entry_height(UI_KIND_TITLE, true), UI_H_TITLE),
            "a separator ignores the description: it shows none");

    /* COUNTER-CASE - THE CARD THAT CHANGES HEIGHT WITH ITS SUBTITLE.
     * The machine inventory is reloaded periodically and the server does not
     * always fill the subtitle in. If the height followed that field, the WHOLE
     * list would change layout on every reload, under the user's fingers, and the
     * scroll in progress would aim at nothing. */
    CHECK(proche(ui_entry_height(UI_KIND_CARD, false), UI_H_CARD),
            "COUNTER-CASE: a Card WITHOUT a subtitle keeps the full height");
    CHECK(proche(ui_entry_height(UI_KIND_CARD, true), UI_H_CARD),
            "COUNTER-CASE: ... and the same one WITH it - the machine list never "
            "changes geometry on a reload");

    /* Settings, on the other hand, are packed: a page of twenty one-line
     * toggles must not take three screens of height. */
    CHECK(proche(ui_entry_height(UI_KIND_TOGGLE, false), UI_H_SETTING),
            "a toggle with no description fits on one line");
    CHECK(proche(ui_entry_height(UI_KIND_TOGGLE, true), UI_H_SETTING_DESC),
            "... and grows to carry its description");
    CHECK(proche(ui_entry_height(UI_KIND_CHOICE,  false), UI_H_SETTING),  "idem choix");
    CHECK(proche(ui_entry_height(UI_KIND_CHOICE,  true),  UI_H_SETTING_DESC), "idem choix");
    CHECK(proche(ui_entry_height(UI_KIND_ACTION, false), UI_H_SETTING),  "idem action");
    CHECK(proche(ui_entry_height(UI_KIND_ACTION, true),  UI_H_SETTING_DESC), "idem action");

    /* No kind returns a zero or negative height. An entry of zero height would
     * be INVISIBLE and would still receive the focus: the highlight would
     * disappear from the screen and the A button would trigger an entry nobody
     * can see. The loop also covers the kind we will add tomorrow - a kind
     * forgotten in the `switch` falls into the default branch, which must stay
     * positive. */
    {
        const ui_kind_t tous[5] = { UI_KIND_CARD, UI_KIND_TOGGLE,
                                     UI_KIND_CHOICE, UI_KIND_ACTION,
                                     UI_KIND_TITLE };
        int nulles = 0;
        for (int i = 0; i < 5; i++) {
            if (!(ui_entry_height(tous[i], false) > 0.0f)) nulles++;
            if (!(ui_entry_height(tous[i], true)  > 0.0f)) nulles++;
        }
        CHECK(nulles == 0,
                "every kind returns a STRICTLY positive height: an entry "
                "of zero height would be invisible AND focusable");
    }
}

/* ===================================================================== */
/* Hauteur totale du content                                             */
/* ===================================================================== */

static void content(void)
{
    const float h3[3] = { 84.0f, 60.0f, 52.0f };

    /* COUNTER-CASE - THE EXTRA GAP AT THE END.
     * `nb * (h + gap)` counts a gap AFTER the last entry: the list then agrees to
     * scroll `gap` pixels into the void and the last item lifts off the bottom of
     * the frame at the end of the travel, which reads as a missing item. There
     * are nb-1 gaps. */
    CHECK(proche(ui_content_h(h3, 3, 10.0f), 84.0f + 60.0f + 52.0f + 20.0f),
            "COUNTER-CASE: nb-1 gaps, not nb - otherwise the list scrolls into the void");

    CHECK(proche(ui_content_h(h3, 1, 10.0f), 84.0f),
            "a single entry: no gap");
    CHECK(proche(ui_content_h(h3, 0, 10.0f), 0.0f), "an empty list: zero height");
    CHECK(proche(ui_content_h(NULL, 3, 10.0f), 0.0f),
            "a null pointer: zero height, no dereference");
    CHECK(proche(ui_content_h(h3, -4, 10.0f), 0.0f),
            "a negative nb (a struct never initialised) reads as empty, "
            "not as an invitation to index backwards");

    /* THE EQUIVALENCE THAT PROTECTS THE MACHINE LIST. */
    float unif[8];
    for (int i = 0; i < 8; i++) unif[i] = UI_H_CARD;
    for (int nb = 0; nb <= 8; nb++)
        CHECK(proche(ui_content_h(unif, nb, UI_GAP),
                       nav_content_h_uniform(nb, UI_H_CARD, UI_GAP)),
                "a uniform list: the new geometry returns exactly what "
                "nav.h returns - two geometries that diverge give a list "
                "that answers beside what it draws");
}

/* ===================================================================== */
/* Position d'une entree                                                 */
/* ===================================================================== */

static void positions(void)
{
    const float h4[4] = { 52.0f, 60.0f, 84.0f, 60.0f };
    const float g = 10.0f;

    CHECK(proche(ui_y_of_index(h4, 4, 0, g), 0.0f), "the first entry starts at 0");
    CHECK(proche(ui_y_of_index(h4, 4, 1, g), 62.0f), "the 2nd starts after the 1st plus the gap");
    CHECK(proche(ui_y_of_index(h4, 4, 2, g), 132.0f), "then the 3rd");
    CHECK(proche(ui_y_of_index(h4, 4, 3, g), 226.0f), "then the 4th");

    /* COUNTER-CASE - THE STALE INDEX.
     * The list may have shrunk since the last move (a background reload) with
     * nobody being wrong. An out-of-range index must RETURN a position, not read
     * outside the array: it is the same discipline as nav_clamp_focus(), and it
     * is what makes an incorrect index harmless where a pointer would have
     * crashed. */
    CHECK(proche(ui_y_of_index(h4, 4, 99, g), ui_y_of_index(h4, 4, 4, g)),
            "COUNTER-CASE: a stale index is CLAMPED, it does not read outside the array");
    CHECK(proche(ui_y_of_index(h4, 4, -7, g), 0.0f),
            "COUNTER-CASE: a negative index comes back to the start, it does not index backwards");
    CHECK(proche(ui_y_of_index(NULL, 4, 2, g), 0.0f), "pointeur nul tolere");

    /* Internal consistency: the top of the last entry plus its height equals
     * exactly the content's height. Without that equality, the maximum scroll
     * does not show the end of the list. */
    CHECK(proche(ui_y_of_index(h4, 4, 3, g) + h4[3], ui_content_h(h4, 4, g)),
            "the last entry ends exactly at the bottom of the content");

    /* L'EQUIVALENCE, encore. */
    float unif[8];
    for (int i = 0; i < 8; i++) unif[i] = UI_H_CARD;
    for (int i = 0; i < 8; i++)
        CHECK(proche(ui_y_of_index(unif, 8, i, UI_GAP),
                       nav_y_of_index(i, UI_H_CARD, UI_GAP)),
                "a uniform list: the same position as nav_y_of_index");
}

/* ===================================================================== */
/* Tactile                                                              */
/* ===================================================================== */

static void tactile(void)
{
    /* A real settings page: a section title, two toggles, a choice with a
     * description. */
    const float h[4] = { UI_H_TITLE, UI_H_SETTING, UI_H_SETTING, UI_H_SETTING_DESC };
    const float g = UI_GAP;
    const float ch = ui_content_h(h, 4, g);
    nav_t n = view(4, 0.0f, 400.0f, ch);

    CHECK(ui_index_at_position(&n, 5.0f, h, g) == 0, "the top touches the 1st entry");
    CHECK(ui_index_at_position(&n, 51.0f, h, g) == 0, "its bottom too");
    CHECK(ui_index_at_position(&n, 70.0f, h, g) == 1, "then the 2nd");

    /* COUNTER-CASE - THE FINGER IN THE GAP.
     * A `y / step` returns an index for ANY value, including for a finger placed
     * exactly in the void between two cards. On this screen, "activate" starts a
     * virtual machine: an expensive action triggered by a few pixels of
     * imprecision. */
    CHECK(ui_index_at_position(&n, 57.0f, h, g) == -1,
            "COUNTER-CASE: the finger falls in the gap between two entries - nothing "
            "triggers");

    /* COUNTER-CASE - OUTSIDE THE FRAME.
     * A touch event arrives with the coordinates of the whole screen and nothing
     * guarantees the caller that it was aimed at this list. */
    CHECK(ui_index_at_position(&n, -3.0f, h, g) == -1, "COUNTER-CASE: above the frame");
    CHECK(ui_index_at_position(&n, 999.0f, h, g) == -1, "COUNTER-CASE: below the frame");
    CHECK(ui_index_at_position(&n, 300.0f, h, g) == -1,
            "inside the frame but below the end of the content: no entry");

    /* COUNTER-CASE - THE NaN.
     * It fails BOTH `< 0` and `> view_h`: it goes through a naive clamp intact.
     * These two checks hold the PROPERTY (a NaN designates no entry), not the
     * implementation: measured by mutation on 2026-08-27, removing screen.hpp's
     * explicit guards does not make them fail today, because every comparison
     * against NaN is false and the loop therefore exits on -1. That is an
     * accident of form - the same function written like
     * nav_index_at_position(), with a `(long long)(y / step)`, would convert the
     * NaN to an integer, which is undefined behaviour. These two lines are what
     * would notice that rewrite. */
    CHECK(ui_index_at_position(&n, NAN, h, g) == -1,
            "COUNTER-CASE: a NaN coordinate designates no entry");
    {
        nav_t nan_scroll = view(4, NAN, 400.0f, ch);
        CHECK(ui_index_at_position(&nan_scroll, 5.0f, h, g) == -1,
                "COUNTER-CASE: a NaN SCROLL does not either - it is the one "
                "produced by an animation whose two frames carry the same "
                "timestamp (an applet resuming after sleep)");
    }

    /* COUNTER-CASE - THE TWO FRAMES OF REFERENCE MIXED UP.
     * `y_local` is relative to the TOP OF THE FRAME, not to the content: the
     * function is the one that adds the scroll. Mixing the two gives a list that
     * is right until something is scrolled, then wrong - the easiest mistake to
     * make here. */
    {
        nav_t d = view(4, 62.0f, 400.0f, ch);   /* the 2nd entry is aligned at the top */
        CHECK(ui_index_at_position(&d, 0.0f, h, g) == 1,
                "COUNTER-CASE: after scrolling, the top of the FRAME touches the 2nd "
                "entry, not the 1st");
        CHECK(ui_index_at_position(&d, 80.0f, h, g) == 2, "and the rest follows");
    }

    CHECK(ui_index_at_position(NULL, 5.0f, h, g) == -1, "nav_t nul tolere");
    CHECK(ui_index_at_position(&n, 5.0f, NULL, g) == -1, "hauteurs nulles tolerees");

    /* A round trip on a NON-uniform list: the middle of each entry must return
     * that entry. Two formulas that diverge (position and touch) give a list that
     * answers beside what it draws. */
    for (int i = 0; i < 4; i++) {
        const float y = ui_y_of_index(h, 4, i, g) + h[i] * 0.5f;
        CHECK(ui_index_at_position(&n, y, h, g) == i,
                "a round trip index -> y -> index on a variable-height list");
    }

    /* THE EQUIVALENCE WITH nav.h, swept over the whole list and at several
     * scroll positions. This is the check that protects the machine list: it
     * existed before kinds, and if the two geometries diverge by one pixel it is
     * the one that breaks first. */
    {
        float unif[6];
        for (int i = 0; i < 6; i++) unif[i] = UI_H_CARD;
        const float ch6 = ui_content_h(unif, 6, UI_GAP);
        const float scrolls[4] = { 0.0f, 10.0f, 47.0f, 94.0f };
        int divergences = 0;

        for (int s = 0; s < 4; s++) {
            nav_t u = view(6, scrolls[s], 380.0f, ch6);
            for (int k = -20; k <= 1600; k++) {
                const float yl = (float)k * 0.25f;
                const int a = ui_index_at_position(&u, yl, unif, UI_GAP);
                const int b = nav_index_at_position(&u, yl, UI_H_CARD, UI_GAP);
                if (a != b) divergences++;
            }
        }
        CHECK(divergences == 0,
                "a uniform list: the variable-height geometry and nav.h's "
                "designate the SAME entry for every point and every "
                "scroll");
    }
}

/* ===================================================================== */
/* Choix du prochain focus                                               */
/* ===================================================================== */

/* A typical settings page:
 *   0 titre, 1 bascule, 2 bascule, 3 titre, 4 choix, 5 action */
static const unsigned char PAGE[6] = { 0, 1, 1, 0, 1, 1 };

static void prochain(void)
{
    CHECK(ui_next_focusable(PAGE, 6, 0, +1) == 1,
            "starting from the leading title, we move down to the first setting");
    CHECK(ui_next_focusable(PAGE, 6, 3, +1) == 4, "the middle title is skipped");
    CHECK(ui_next_focusable(PAGE, 6, 3, -1) == 2, "and skipped upwards too");
    CHECK(ui_next_focusable(PAGE, 6, 2, +1) == 2, "`from` is INCLUSIVE");

    /* COUNTER-CASE - THE PAGE WITH NOTHING FOCUSABLE.
     * A list made only of section titles, or the "no machines" message.
     * Returning 0 would draw a highlight on a decorative entry, which the A
     * button would then activate - exactly the class of defect nav_clamp_focus()
     * refuses on an empty list. */
    {
        const unsigned char que_des_titres[3] = { 0, 0, 0 };
        CHECK(ui_next_focusable(que_des_titres, 3, 0, +1) == -1,
                "COUNTER-CASE: nothing focusable returns -1, NOT 0");
        CHECK(ui_next_focusable(que_des_titres, 3, 2, -1) == -1, "... in both directions");
    }

    CHECK(ui_next_focusable(PAGE, 0, 0, +1) == -1, "an empty list: -1");
    CHECK(ui_next_focusable(NULL, 6, 0, +1) == -1, "pointeur nul : -1");

    /* An out-of-range `from` is CLAMPED, not refused: silently refusing would
     * suggest "nothing focusable left" while the page is full of it, and the
     * focus would disappear from the screen. */
    CHECK(ui_next_focusable(PAGE, 6, 99, -1) == 5,
            "a `from` that is too large restarts at the edge, it does not return -1");
    CHECK(ui_next_focusable(PAGE, 6, -9, +1) == 1,
            "a negative `from` restarts at the edge too");

    CHECK(ui_next_focusable(PAGE, 6, 0, 0) == 1,
            "a zero direction is treated as a descent, not as a loop "
            "forever in place");
}

/* ===================================================================== */
/* Deplacement du focus                                                  */
/* ===================================================================== */

static void deplacement(void)
{
    /* A full descent of the page: we must never stop on a title. */
    CHECK(ui_move_focus(PAGE, 6, 1, +1) == 2, "1 -> 2");
    CHECK(ui_move_focus(PAGE, 6, 2, +1) == 4,
            "2 -> 4: title 3 is crossed in a single press, not selected");
    CHECK(ui_move_focus(PAGE, 6, 4, +1) == 5, "4 -> 5");
    CHECK(ui_move_focus(PAGE, 6, 5, +1) == 5, "at the bottom we STOP (we do not wrap)");

    /* Remontee : symetrique. */
    CHECK(ui_move_focus(PAGE, 6, 4, -1) == 2, "4 -> 2, title 3 is crossed");
    CHECK(ui_move_focus(PAGE, 6, 1, -1) == 1,
            "at the top we stop on the first setting - title 0 is not a "
            "arret possible");

    /* COUNTER-CASE - THE TITLE AT THE END OF THE LIST.
     * `nav_move` stops at the last entry; if that is a separator, the jump "in
     * the direction of travel" finds nothing any more. Without the fallback in
     * the OPPOSITE direction, navigation would be stuck for good at the bottom of
     * the screen, the highlight sitting on an entry that does not answer. */
    {
        const unsigned char title_end[4] = { 1, 1, 1, 0 };
        CHECK(ui_move_focus(title_end, 4, 2, +1) == 2,
                "COUNTER-CASE: a title in LAST position does not block "
                "navigation - we stay on the last setting");
        CHECK(ui_move_focus(title_end, 4, 0, +9) == 2,
                "COUNTER-CASE: the same for a page jump that lands on it");
    }

    /* COUNTER-CASE - THE TITLE AT THE HEAD. Symmetrical, and it is the most
     * common layout: a settings page STARTS with a section title. */
    CHECK(ui_move_focus(PAGE, 6, 1, -9) == 1,
            "COUNTER-CASE: a page jump upwards lands on the leading "
            "title and comes back down to the first setting");

    /* COUNTER-CASE - THE ENORMOUS DELTA.
     * `focus + delta` computed as an `int` OVERFLOWS for a delta close to
     * INT_MAX, and a signed overflow is undefined behaviour: the compiler is free
     * to deduce that the following bound is always true. The computation goes
     * through nav_move(), which sums in `long long`. */
    CHECK(ui_move_focus(PAGE, 6, 5, INT_MAX) == 5,
            "COUNTER-CASE: a delta of INT_MAX - clamped at the end of the list, no "
            "debordement signe");
    CHECK(ui_move_focus(PAGE, 6, 1, INT_MIN) == 1,
            "COUNTER-CASE: a delta of INT_MIN - clamped at the head, then the title is skipped");

    /* Liste entierement decorative : aucun focus possible. */
    {
        const unsigned char que_des_titres[3] = { 0, 0, 0 };
        CHECK(ui_move_focus(que_des_titres, 3, 0, +1) == -1,
                "a page with nothing focusable never returns an index");
    }

    CHECK(ui_move_focus(PAGE, 0, 0, +1) == -1, "an empty list: -1");
    CHECK(ui_move_focus(NULL, 6, 0, +1) == -1, "pointeur nul : -1");

    /* A focus that is already out of range (a list shortened between two
     * frames) is REPAIRED in passing, not propagated. */
    CHECK(ui_move_focus(PAGE, 6, 42, -1) == 4,
            "a stale focus is clamped before the computation, it does not propagate");
    CHECK(ui_move_focus(PAGE, 6, -3, +1) == 1,
            "the same for a negative focus");

    /* A full round trip: going down to the end then back up must return
     * EXACTLY to the starting point. An asymmetric title jump (skipped on the way
     * down, not on the way back) would show here and nowhere else. */
    {
        int f = 1;
        for (int i = 0; i < 20; i++) f = ui_move_focus(PAGE, 6, f, +1);
        CHECK(f == 5, "twenty descents: we land on the last setting");
        for (int i = 0; i < 20; i++) f = ui_move_focus(PAGE, 6, f, -1);
        CHECK(f == 1, "twenty ascents: we come back to the first, never onto a title");
    }

    /* Every entry reached is focusable - the property that sums up the suite.
     * We check it from every starting point and for every common delta, rather
     * than on a chosen sample. */
    {
        const int deltas[7] = { -5, -2, -1, 0, 1, 2, 5 };
        int fautes = 0;
        for (int f = 0; f < 6; f++)
            for (int d = 0; d < 7; d++) {
                const int c = ui_move_focus(PAGE, 6, f, deltas[d]);
                if (c < 0 || c >= 6 || !PAGE[c]) fautes++;
            }
        CHECK(fautes == 0,
                "from ANY starting point and for every common delta, the "
                "focus lands on a focusable entry");
    }
}

/* === S79 - THE LIST'S BAND, ONCE THE RAIL IS REMOVED ===
 *
 * COUNTER-CASE - THE FINGER PLACED ON THE RAIL. The drawing and the hit test must
 * remove the SAME width. If they diverge, a finger placed on the section column
 * activates the settings row hidden behind it: the rail is drawn on top, so
 * nothing signals it, and the user sees a setting change by itself while
 * choosing a section. It is the horizontal version of the trap
 * ui_index_at_position() documents for the vertical axis.
 *
 * So we check the property that matters: a point on the rail is NEVER inside the
 * list's band, and the band covers exactly the rest. */
static void rail(void)
{
    const float PX = 48.0f, PW = 1184.0f;   /* page en mode dock, 1280 de large */

    /* With no rail, the band IS the page. That is what leaves the five other
     * screens - the ones with no sections - rigorously unchanged. */
    CHECK(proche(ui_list_x(PX, 0.0f), PX),
            "with no rail, the list starts at the page edge");
    CHECK(proche(ui_list_w(PW, 0.0f), PW),
            "with no rail, the list occupies the whole page");

    {
        const float lx = ui_list_x(PX, UI_RAIL_W);
        const float lw = ui_list_w(PW, UI_RAIL_W);

        CHECK(proche(lx, PX + UI_RAIL_W + UI_RAIL_SPACING),
                "with a rail, the list starts after the column and its gap");
        CHECK(proche(lx + lw, PX + PW),
                "the RIGHT edge does not move: the rail is taken off the left");

        /* The rightmost point of the rail - the one that will fall inside the
         * list's band at the first divergence. */
        CHECK(PX + UI_RAIL_W < lx,
                "the rail's right edge is STRICTLY before the start of the list");
        CHECK(!(PX + UI_RAIL_W - 0.5f >= lx),
                "a finger placed on the rail is not inside the list's band");
    }

    /* COUNTER-CASE - A WINDOW NARROWER THAN THE RAIL. The desktop resizes
     * freely. A negative width propagated into the text measurements no longer
     * cuts anything: the labels overflow the frame instead of being truncated,
     * and the list draws outside its area. */
    CHECK(ui_list_w(100.0f, UI_RAIL_W) >= 0.0f,
            "a page narrower than the rail: the list width stays >= 0");
    CHECK(proche(ui_list_w(UI_RAIL_W + UI_RAIL_SPACING, UI_RAIL_W), 0.0f),
            "a page exactly the size of the rail: nothing is left for the list");
}

int main(void)
{
    printf("== list screen with kinds (geometry and focus, pure functions) ==\n");
    focalisabilite();
    heights();
    content();
    positions();
    tactile();
    prochain();
    deplacement();
    rail();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
