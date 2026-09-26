/* HOLD-1 2026-09-12 - the hold that closes the three in-app testers.
 *
 * Counter-case: on the Windows client the link-quality page announced "hold B
 * to go back" and could not be left, because the block that measured the hold
 * was compiled only for `__SWITCH__`. Nothing could catch that - an `#ifdef`
 * that excludes code produces no warning and no failing test. This suite pins
 * the timing that now runs on every platform, so the next person to touch it
 * learns what the rules are instead of rediscovering them.
 *
 * The rule that costs the most if lost is the FIRST one below: releasing
 * cancels. Without it a short press followed by a pause exits on its own, and
 * the page reads as closing for no reason.
 */
#include <stdio.h>

#include "../clients/borealis/ui/hold_exit.h"

static int checks = 0;
static int failures = 0;

static void expect(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %s : attendu %d, obtenu %d\n", what, got, want);
    }
}

static void expect_near(const char *what, float got, float want)
{
    checks++;
    const float d = got - want;
    if (d > 0.001f || d < -0.001f) {
        failures++;
        printf("  FAIL %s : attendu %.3f, obtenu %.3f\n", what, (double)want, (double)got);
    }
}

int main(void)
{
    const double NEED = 0.8;   /* the value the three testers use */
    printf("=== test_hold_exit ===\n");

    /* --- Nothing pressed: nothing happens, ever --------------------------- */
    {
        hold_exit_t h = { 0, 0.0 };
        float p = 9.0f;
        for (int i = 0; i < 100; i++)
            expect("releasing does not exit", hold_exit_step(&h, 0, 1000.0 + i, NEED, &p), 0);
        expect_near("progression nulle", p, 0.0f);
    }

    /* --- A full hold: completes ONCE, at the threshold -------------------- */
    {
        hold_exit_t h = { 0, 0.0 };
        float p = 0.0f;
        expect("debut du maintien", hold_exit_step(&h, 1, 1000.0, NEED, &p), 0);
        expect_near("progression au debut", p, 0.0f);

        expect("a mi-parcours", hold_exit_step(&h, 1, 1000.4, NEED, &p), 0);
        expect_near("progression a mi-parcours", p, 0.5f);

        expect("just below the threshold", hold_exit_step(&h, 1, 1000.79, NEED, &p), 0);

        expect("at the threshold", hold_exit_step(&h, 1, 1000.8, NEED, &p), 1);
        expect_near("progression pleine", p, 1.0f);
    }

    /* --- Releasing CANCELS. The counter-case of the whole module. ---------- */
    {
        hold_exit_t h = { 0, 0.0 };
        float p = 0.0f;
        expect("appui bref", hold_exit_step(&h, 1, 1000.0, NEED, &p), 0);
        expect("appui bref suite", hold_exit_step(&h, 1, 1000.3, NEED, &p), 0);
        expect("relache", hold_exit_step(&h, 0, 1000.4, NEED, &p), 0);
        expect_near("progression remise a zero", p, 0.0f);

        /* A long pause, then a press. If the release had only PAUSED the
         * count, this single frame would exit immediately - which is the
         * defect this rule exists to prevent. */
        expect("reappui apres pause", hold_exit_step(&h, 1, 1010.0, NEED, &p), 0);
        expect_near("reparti de zero", p, 0.0f);
        expect("nouveau maintien complet", hold_exit_step(&h, 1, 1010.8, NEED, &p), 1);
    }

    /* --- Completing RESETS: reopening the page starts from zero ----------- */
    {
        hold_exit_t h = { 0, 0.0 };
        float p = 0.0f;
        expect("premier maintien", hold_exit_step(&h, 1, 1000.0, NEED, &p), 0);
        expect("premiere sortie",  hold_exit_step(&h, 1, 1000.9, NEED, &p), 1);
        /* Still held on the next frame: this must NOT exit a second time, it
         * must begin a new hold. */
        expect("still held after the exit",
               hold_exit_step(&h, 1, 1000.95, NEED, &p), 0);
        expect_near("progression repartie de zero", p, 0.0f);
        expect("seconde sortie", hold_exit_step(&h, 1, 1001.75, NEED, &p), 1);
    }

    /* --- Degenerate inputs must not crash or hang ------------------------- */
    {
        hold_exit_t h = { 0, 0.0 };
        float p = 0.0f;
        expect("duree nulle", hold_exit_step(&h, 1, 1000.0, 0.0, &p), 1);
        expect_near("duree nulle : plein", p, 1.0f);

        hold_exit_t h2 = { 0, 0.0 };
        expect("duree negative", hold_exit_step(&h2, 1, 1000.0, -1.0, &p), 1);

        /* A clock going backwards: the bar stays empty, the hold restarts,
         * nothing exits by surprise. */
        hold_exit_t h3 = { 0, 0.0 };
        expect("horloge en arriere : debut", hold_exit_step(&h3, 1, 1000.0, NEED, &p), 0);
        expect("horloge en arriere", hold_exit_step(&h3, 1, 990.0, NEED, &p), 0);
        expect_near("horloge en arriere : barre vide", p, 0.0f);

        expect("etat NULL", hold_exit_step(NULL, 1, 1000.0, NEED, &p), 0);
        expect_near("etat NULL : barre vide", p, 0.0f);

        /* No progress pointer at all - the testers that draw no bar. */
        hold_exit_t h4 = { 0, 0.0 };
        expect("sans pointeur : debut", hold_exit_step(&h4, 1, 1000.0, NEED, NULL), 0);
        expect("sans pointeur : sortie", hold_exit_step(&h4, 1, 1000.8, NEED, NULL), 1);
    }

    printf("%d verifications, %d echec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
