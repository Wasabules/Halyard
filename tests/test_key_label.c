/* K21 2026-09-12 - the footer's button-to-key table.
 *
 * Counter-case: the Windows client's VM list announced "(B) Quitter (Y)
 * Reglages (X) Actualiser (A) Connecter" to a machine with a keyboard and no
 * gamepad. Nothing in the build could catch that - the letters were string
 * literals at fifteen call sites - so the table that replaces them is pinned
 * here.
 *
 * What this suite really guards is the PAIRING with Borealis' K20 patch
 * (`patches/borealis/hint.extrait`): the two bars share a screen, so A must
 * mean Enter in both and the A/B swap must move both. A change to one that
 * forgets the other shows up as a failure here.
 */
#include <stdio.h>
#include <string.h>

#include "../clients/borealis/ui/key_label.h"

static int checks = 0;
static int failures = 0;

static void expect_label(const char *what, const char *got, const char *want)
{
    checks++;
    const int ok = (want == NULL) ? (got == NULL)
                                  : (got != NULL && strcmp(got, want) == 0);
    if (!ok) {
        failures++;
        printf("  FAIL %s : attendu %s, obtenu %s\n", what,
               want ? want : "(glyphe)", got ? got : "(glyphe)");
    }
}

int main(void)
{
    printf("=== test_key_label ===\n");

    /* --- No gamepad: the bar names keys -------------------------------- */
    expect_label("A no pad",  key_label_for("A", 0, 0), "Enter");
    expect_label("B no pad",  key_label_for("B", 0, 0), "Esc");
    expect_label("+ no pad",  key_label_for("+", 0, 0), "F2");
    expect_label("- no pad",  key_label_for("-", 0, 0), "F1");
    expect_label("LR no pad", key_label_for("LR", 0, 0), "L/R");

    /* --- The A/B swap moves both, and only those two ------------------- */
    expect_label("A swapped", key_label_for("A", 0, 1), "Esc");
    expect_label("B swapped", key_label_for("B", 0, 1), "Enter");
    expect_label("+ swapped", key_label_for("+", 0, 1), "F2");
    expect_label("LR swapped", key_label_for("LR", 0, 1), "L/R");

    /* --- A gamepad is connected: the glyph is the honest answer --------
     * This is the case the platform test would have got wrong: a desktop WITH
     * a pad plugged in must keep its glyphs. */
    expect_label("A with a pad", key_label_for("A", 1, 0), NULL);
    expect_label("B with a pad", key_label_for("B", 1, 0), NULL);
    expect_label("+ with a pad", key_label_for("+", 1, 0), NULL);
    expect_label("LR with a pad", key_label_for("LR", 1, 0), NULL);
    expect_label("A two pads", key_label_for("A", 2, 1), NULL);

    /* --- Ids whose letter already IS the key --------------------------- */
    expect_label("X garde son glyphe", key_label_for("X", 0, 0), NULL);
    expect_label("Y garde son glyphe", key_label_for("Y", 0, 0), NULL);
    expect_label("L garde son glyphe", key_label_for("L", 0, 0), NULL);
    expect_label("R garde son glyphe", key_label_for("R", 0, 0), NULL);

    /* --- Degrade to "leave it alone", never to a guess ------------------ */
    expect_label("id inconnu",  key_label_for("ZL", 0, 0), NULL);
    expect_label("id vide",     key_label_for("", 0, 0), NULL);
    expect_label("id NULL",     key_label_for(NULL, 0, 0), NULL);
    expect_label("fleche",      key_label_for("\xe2\x86\x91", 0, 0), NULL);

    /* --- The table is a pure function: same input, same answer ---------- */
    for (int i = 0; i < 3; i++) {
        expect_label("A stable", key_label_for("A", 0, 0), "Enter");
        expect_label("B stable", key_label_for("B", 0, 1), "Enter");
    }

    printf("%d verifications, %d echec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
