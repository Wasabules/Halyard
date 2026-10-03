/* test_qt_hud_theme - the HUD palette, held to its own contrast rule (HUD2).
 *
 * The flat redesign removed every border, so a thing is only distinguishable
 * from the thing behind it by FILL, and a value is only readable because its
 * ink was chosen for the fill under it. Both of those are arithmetic, and
 * both have already been got wrong once: the first pass shipped a muted grey
 * at about 4:1, which fails for the 10-11 px labels this HUD is made of.
 *
 * WCAG relative luminance and contrast ratio, implemented here rather than
 * taken on faith, so the numbers in the header are checked and not asserted.
 */
#include <cmath>
#include <cstdio>

#include "../clients/qt/hud_theme.hpp"

using namespace halyard::hud;

static int checks = 0, failures = 0;

/* WCAG 2.1, 1.4.3: the sRGB channel is linearised, then weighted. */
static double channel(double c)
{
    c /= 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(const QColor &c)
{
    return 0.2126 * channel(c.red())
         + 0.7152 * channel(c.green())
         + 0.0722 * channel(c.blue());
}

static double ratio(const QColor &a, const QColor &b)
{
    const double la = luminance(a), lb = luminance(b);
    const double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

static void atLeast(const QColor &ink, const QColor &on, double want,
                    const char *what)
{
    checks++;
    const double r = ratio(ink, on);
    if (r < want) {
        failures++;
        std::printf("  FAIL %-46s %.2f:1, needs %.1f:1\n", what, r, want);
    }
}

static void ok(bool cond, const char *what)
{
    checks++;
    if (!cond) { failures++; std::printf("  FAIL %s\n", what); }
}

int main()
{
    std::printf("== HUD palette contrast (HUD2 2026-10-03) ==\n");

    /* --- text on every surface it can land on ---------------------------- *
     *
     * 4.5:1 is the threshold for body text, and this HUD has no large text:
     * the biggest number is 26 px but the labels under it are 10.5, so every
     * pair is held to the SMALL-text bar. */
    atLeast(text(), bg(),    4.5, "text on the background");
    atLeast(text(), surf1(), 4.5, "text on a card");
    atLeast(text(), surf2(), 4.5, "text on a nested surface");
    atLeast(text(), surf3(), 4.5, "text on a control");

    atLeast(muted(), bg(),    4.5, "muted on the background");
    atLeast(muted(), surf1(), 4.5, "muted on a card");
    atLeast(muted(), surf2(), 4.5, "muted on a nested surface");
    atLeast(muted(), surf3(), 4.5, "muted on a control");

    /* --- the accents, used as INK on a dark surface ---------------------- */
    atLeast(primary(), surf1(), 4.5, "the accent as ink on a card");
    atLeast(good(),    surf1(), 4.5, "green as ink on a card");
    atLeast(warn(),    surf1(), 4.5, "amber as ink on a card");
    atLeast(bad(),     surf1(), 4.5, "red as ink on a card");
    atLeast(primary(), surf2(), 4.5, "the accent as ink, one level up");
    atLeast(good(),    surf2(), 4.5, "green as ink, one level up");

    /* --- the accents, used as a FILL with dark ink ----------------------- *
     *
     * This is the rule the redesign turns on. White on amber is about 2.5:1
     * and is what the first version of the banner did. */
    atLeast(inkOn(primary()), primary(), 4.5, "ink on the accent fill");
    atLeast(inkOn(good()),    good(),    4.5, "ink on the green fill");
    atLeast(inkOn(warn()),    warn(),    4.5, "ink on the amber fill");
    atLeast(inkOn(bad()),     bad(),     4.5, "ink on the red fill");

    /* And comfortably past it, because these carry the figures that matter -
     * a time remaining, a hop latency. */
    atLeast(inkOn(warn()), warn(), 7.0, "ink on amber clears AAA-small too");
    atLeast(inkOn(good()), good(), 7.0, "ink on green clears AAA-small too");

    /* The pairing must beat WHITE on the same fill, or picking a dark ink
     * bought nothing. */
    checks++;
    if (ratio(inkOn(warn()), warn()) <= ratio(QColor(255, 255, 255), warn())) {
        failures++;
        std::printf("  FAIL dark ink on amber is no better than white\n");
    }

    /* --- the surface ladder must actually be a ladder -------------------- *
     *
     * Without borders, a card is only visible because it is lighter than its
     * parent. Equal steps would make the whole thing one flat sheet. */
    ok(luminance(bg())    < luminance(surf1()), "a card is lighter than the ground");
    ok(luminance(surf1()) < luminance(surf2()), "a nested surface is lighter again");
    ok(luminance(surf2()) < luminance(surf3()), "and a control lighter still");

    /* Each step has to be big enough to SEE. 1.12 is about the smallest
     * luminance ratio that reads as a distinct surface on an LCD. */
    checks++;
    {
        const double s[4] = { luminance(bg()), luminance(surf1()),
                              luminance(surf2()), luminance(surf3()) };
        bool fine = true;
        for (int i = 0; i + 1 < 4; i++)
            if ((s[i + 1] + 0.05) / (s[i] + 0.05) < 1.12) fine = false;
        if (!fine) {
            failures++;
            std::printf("  FAIL two surfaces are too close to tell apart\n");
        }
    }

    /* --- the unknown-fill fallback --------------------------------------- */
    ok(inkOn(QColor(0xFF, 0xFF, 0xFF)).lightnessF() < 0.3,
       "a light fill we do not know gets dark ink");
    ok(inkOn(QColor(0x10, 0x10, 0x10)) == text(),
       "a dark fill we do not know gets the normal text colour");
    atLeast(inkOn(QColor(0xE0, 0xE0, 0xE0)), QColor(0xE0, 0xE0, 0xE0), 4.5,
            "the fallback still passes on a light grey");

    /* --- glass alpha ------------------------------------------------------ *
     *
     * The slider may not take the panel below the point where the muted ink
     * stops passing over a bright frame. The floor is the guard. */
    ok(glassAlpha(100) >= 150 && glassAlpha(100) <= 240, "full opacity is in range");
    ok(glassAlpha(20) >= 150, "the slider's minimum still respects the floor");
    ok(glassAlpha(0) >= 150, "and so does zero");
    ok(glassAlpha(100) > glassAlpha(60), "the slider still does something");

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
