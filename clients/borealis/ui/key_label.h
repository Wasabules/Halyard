/* === K21 2026-09-12 - NAME THE KEY IN OUR OWN FOOTER ===
 *
 * Reported on the Windows client: in the VM list, the bottom bar announced
 * "(B) Quitter  (Y) Reglages  (X) Actualiser  (A) Connecter" to someone with a
 * keyboard and no gamepad. Borealis' own hint bar had already been fixed for
 * exactly this (K20, `patches/borealis/hint.extrait`), but our screens do not
 * use it: they draw their OWN footer from a `std::vector<ui::Hint>` whose
 * button field is a hardcoded letter written at each call site. The K20 patch
 * could therefore never reach them, and fifteen call sites would each have had
 * to learn the mapping.
 *
 * So the mapping lives HERE, once, and the two footers that paint hints route
 * their button text through it. The call sites keep writing the LOGICAL button
 * ("A" = confirm, "B" = go back), which is also what the touch hit-test matches
 * on - translating at the call site would have broken that pairing.
 *
 * Pure on purpose - no Borealis, no NanoVG, no globals - so the table can be
 * pinned by `tests/test_key_label.c` with no GUI, no window and no console.
 * The caller supplies the two facts it cannot know: how many gamepads are
 * connected, and whether Borealis' A/B swap is on.
 *
 * The values MUST agree with the K20 patch in Borealis' `hint.cpp`: the two
 * bars sit on the same screen, and a footer saying "Esc" beside a Borealis hint
 * saying "B" is worse than either alone. When one changes, change both.
 */
#pragma once

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The desktop key name for a logical hint id, or NULL when the id must keep the
 * glyph it already carries.
 *
 * NULL is returned in three cases, and they are not the same thing:
 *   - a gamepad is connected, so the glyph is the honest answer;
 *   - the id is one whose letter IS the key (X, Y, L, R on Borealis' desktop
 *     mapping), so renaming it would only add noise;
 *   - the id is unknown to this table, which must degrade to "leave it alone"
 *     rather than guess.
 *
 * `controllers` is the count Borealis reports, `swap` its A/B swap setting. The
 * swap is read on the LOGICAL button, before any mapping: it is what the action
 * registered, and the platform layer decides from that same setting which key
 * feeds it. Applying it afterwards would swap Enter and Escape one time in two.
 */
/* The PlayStation face glyphs, for a console whose buttons are not letters.
 *
 * Reported from a real Vita, 2026-09-13: the footer read "LR / B / A". Those
 * are the LOGICAL ids, painted verbatim because `hintGlyph` only ever rewrote
 * them under `__GLFW__` - and a Vita has no glyph font for them either, so
 * nothing downstream turned them into anything.
 *
 * THE MAPPING IS BOREALIS', NOT A GUESS: `lib/platforms/psv/psv_input.cpp`
 * binds BUTTON_A to CROSS, B to CIRCLE, X to SQUARE, Y to TRIANGLE. Naming them
 * the other way round - by the Western "cross confirms" habit - would label the
 * button the user is NOT pressing.
 *
 * Returned as UTF-8 geometric shapes rather than the PlayStation symbols
 * themselves: the bundled font has these, and a missing glyph would draw a
 * blank box, which is worse than a letter. */
static inline const char *psv_label_for(const char *button)
{
    /* U+00D7, not U+2715: the font carries the multiplication sign and not
     * MULTIPLICATION X, which drew a tofu box on the console. The other three
     * shapes here were checked against the font's cmap and are present.
     * pad_label.hpp has the same table and the same fix - two sites, because
     * one names buttons in prose and the other paints the footer. */
    if (!strcmp(button, "A"))  return "\xC3\x97";            /* cross    */
    if (!strcmp(button, "B"))  return "\xE2\x97\xAF";        /* circle   */
    if (!strcmp(button, "X"))  return "\xE2\x96\xA1";        /* square   */
    if (!strcmp(button, "Y"))  return "\xE2\x96\xB3";        /* triangle */
    if (!strcmp(button, "L"))  return "L";
    if (!strcmp(button, "R"))  return "R";
    if (!strcmp(button, "LR")) return "L/R";
    if (!strcmp(button, "+"))  return "START";
    if (!strcmp(button, "-"))  return "SELECT";
    return NULL;
}

static inline const char *key_label_for(const char *button, int controllers, int swap)
{
    if (!button || !*button) return NULL;
    if (controllers > 0)     return NULL;

    if (!strcmp(button, "A")) return swap ? "Esc" : "Enter";
    if (!strcmp(button, "B")) return swap ? "Enter" : "Esc";

    /* "+" and "-" are the console's Start and Select. Borealis binds them to
     * F2 and F1 on desktop, and nothing on screen said so. */
    if (!strcmp(button, "+")) return "F2";
    if (!strcmp(button, "-")) return "F1";

    /* Our own composite id: one hint standing for the two shoulder buttons,
     * used by the settings screen's section rail. */
    if (!strcmp(button, "LR")) return "L/R";

    return NULL;
}

#ifdef __cplusplus
}
#endif
