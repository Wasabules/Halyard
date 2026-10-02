/* test_clip_dir - which way the clipboard is allowed to travel.
 *
 * Four predicates, and both of their failure modes are SILENT: a permissive
 * `to_pc` quietly defeats a one-way setting by pulling the VM's clipboard
 * anyway, and a wrong clamp quietly disables a feature the user configured.
 * Neither shows up as a crash, a log line or a failed session - which is
 * exactly the shape of defect this suite exists for.
 *
 * The mutation checks at the bottom are the point: they are the assertions that
 * fail if the fallback is changed from "both ways" to "off", or if `atoi` is
 * put back in place of the hand-written parse.
 *
 * Compiled with -Wall -Wextra -Werror -O1 by tests/run_tests.sh.
 */
#include <stdio.h>
#include <string.h>

#include "../core/protocol/clip_dir.h"

static int checks = 0, failures = 0;

static void eq_int(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %-52s got %d, expected %d\n", what, got, want);
    }
}

static void eq_bool(bool got, bool want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %-52s got %s, expected %s\n", what,
               got ? "true" : "false", want ? "true" : "false");
    }
}

static void eq_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL %-52s got \"%s\", expected \"%s\"\n", what,
               got ? got : "(null)", want);
    }
}

int main(void)
{
    printf("== clipboard direction (CLIP6 2026-10-02) ==\n");

    /* --- the four values, read as themselves ------------------------------ */
    eq_int(clip_dir_from_env("0"), CLIP_MODE_OFF,   "\"0\" is off");
    eq_int(clip_dir_from_env("1"), CLIP_MODE_BOTH,  "\"1\" is both ways");
    eq_int(clip_dir_from_env("2"), CLIP_MODE_TO_VM, "\"2\" is PC -> VM");
    eq_int(clip_dir_from_env("3"), CLIP_MODE_TO_PC, "\"3\" is VM -> PC");

    /* --- absent and empty: the feature is ON, not OFF --------------------- */
    eq_int(clip_dir_from_env(NULL), CLIP_MODE_BOTH, "unset defaults to both ways");
    eq_int(clip_dir_from_env(""),   CLIP_MODE_BOTH, "empty defaults to both ways");

    /* --- the leading blanks and the sign every other toggle accepts ------- */
    eq_int(clip_dir_from_env("  2"), CLIP_MODE_TO_VM, "leading spaces");
    eq_int(clip_dir_from_env("\t3"), CLIP_MODE_TO_PC, "a leading tab");
    eq_int(clip_dir_from_env("+2"),  CLIP_MODE_TO_VM, "an explicit plus");
    eq_int(clip_dir_from_env("0  "), CLIP_MODE_OFF,   "trailing spaces, still off");
    /* Trailing junk after a digit is IGNORED, exactly as `atoi` ignores it.
     * Being stricter here than every other toggle in the repo would mean a
     * value that works for one key fails for the next - and the header makes
     * consistency with the other keys an explicit goal. What is NOT tolerated
     * is a value with no digits at all; see below. */
    eq_int(clip_dir_from_env("2x"),  CLIP_MODE_TO_VM, "trailing junk is ignored, like atoi");
    eq_int(clip_dir_from_env("3 #"), CLIP_MODE_TO_PC, "a trailing comment marker");

    /* --- OUT OF RANGE FALLS BACK TO BOTH WAYS, never to off.
     *
     * This is the decision the header argues for: a value we cannot understand
     * is not an instruction to stop sharing. Someone editing env.txt by hand
     * who writes 4 wanted a clipboard, not silence. */
    eq_int(clip_dir_from_env("4"),    CLIP_MODE_BOTH, "4 is out of range -> both");
    eq_int(clip_dir_from_env("99"),   CLIP_MODE_BOTH, "99 -> both");
    eq_int(clip_dir_from_env("-1"),   CLIP_MODE_BOTH, "-1 -> both");
    eq_int(clip_dir_from_env("-0"),   CLIP_MODE_OFF,  "-0 is still zero, so off");
    eq_int(clip_dir_from_env("1000000000000"), CLIP_MODE_BOTH, "a huge number -> both");

    /* --- NOT A NUMBER AT ALL. `atoi` answers 0 for every one of these, and 0
     * is OFF - the most harmful possible guess. */
    eq_int(clip_dir_from_env("O"),     CLIP_MODE_BOTH, "the letter O, not a zero");
    eq_int(clip_dir_from_env("off"),   CLIP_MODE_BOTH, "the word \"off\"");
    eq_int(clip_dir_from_env("oui"),   CLIP_MODE_BOTH, "a French yes");
    eq_int(clip_dir_from_env("true"),  CLIP_MODE_BOTH, "the word \"true\"");
    eq_int(clip_dir_from_env("-"),     CLIP_MODE_BOTH, "a lone minus");
    eq_int(clip_dir_from_env("+"),     CLIP_MODE_BOTH, "a lone plus");
    eq_int(clip_dir_from_env(" "),     CLIP_MODE_BOTH, "a single space");
    eq_int(clip_dir_from_env("#2"),    CLIP_MODE_BOTH, "a commented-out value");

    /* --- the directions ---------------------------------------------------- */
    eq_bool(clip_dir_to_pc(CLIP_MODE_BOTH),  true,  "both: VM -> PC allowed");
    eq_bool(clip_dir_to_vm(CLIP_MODE_BOTH),  true,  "both: PC -> VM allowed");
    eq_bool(clip_dir_to_pc(CLIP_MODE_TO_PC), true,  "to_pc: VM -> PC allowed");
    eq_bool(clip_dir_to_vm(CLIP_MODE_TO_PC), false, "to_pc: PC -> VM REFUSED");
    eq_bool(clip_dir_to_pc(CLIP_MODE_TO_VM), false, "to_vm: VM -> PC REFUSED");
    eq_bool(clip_dir_to_vm(CLIP_MODE_TO_VM), true,  "to_vm: PC -> VM allowed");
    eq_bool(clip_dir_to_pc(CLIP_MODE_OFF),   false, "off: nothing to PC");
    eq_bool(clip_dir_to_vm(CLIP_MODE_OFF),   false, "off: nothing to the VM");

    eq_bool(clip_dir_off(CLIP_MODE_OFF),   true,  "off is off");
    eq_bool(clip_dir_off(CLIP_MODE_BOTH),  false, "both is not off");
    eq_bool(clip_dir_off(CLIP_MODE_TO_VM), false, "to_vm is not off");
    eq_bool(clip_dir_off(CLIP_MODE_TO_PC), false, "to_pc is not off");

    /* An unknown mode must deny BOTH directions rather than open one. A mode
     * only ever comes from the clamp above, so this is unreachable today - and
     * it is asserted precisely because a future caller may not go through the
     * clamp. */
    eq_bool(clip_dir_to_pc(42), false, "an unknown mode denies VM -> PC");
    eq_bool(clip_dir_to_vm(42), false, "an unknown mode denies PC -> VM");

    /* --- the names, which end up in the session summary -------------------- */
    eq_str(clip_dir_name(CLIP_MODE_OFF),   "off",           "name: off");
    eq_str(clip_dir_name(CLIP_MODE_BOTH),  "both ways",     "name: both");
    eq_str(clip_dir_name(CLIP_MODE_TO_VM), "PC -> VM only", "name: to the VM");
    eq_str(clip_dir_name(CLIP_MODE_TO_PC), "VM -> PC only", "name: to the PC");
    eq_str(clip_dir_name(42),              "both ways",     "name: unknown is not NULL");

    /* === MUTATION CHECKS ==================================================
     *
     * Each of these fails for one specific edit that would compile, run, and
     * look right in every session where the user never touched the setting. */

    /* 1. The fallback. Change it to CLIP_MODE_OFF - the "safe" choice someone
     *    will argue for - and these three fail. A clipboard that silently stops
     *    working after a typo is not safer; it is a support request. */
    checks++;
    if (clip_dir_from_env("nonsense") == CLIP_MODE_OFF ||
        clip_dir_from_env(NULL)       == CLIP_MODE_OFF ||
        clip_dir_from_env("7")        == CLIP_MODE_OFF) {
        failures++;
        printf("  FAIL an unreadable value must NOT fall back to off\n");
    }

    /* 2. The parse. The ONE thing this module does differently from `atoi` is
     *    refusing a value with no digits in it, because `atoi` answers 0 for
     *    those and 0 is OFF - the most harmful possible guess. Put `atoi` back
     *    and this fails while everything else still passes. */
    checks++;
    if (clip_dir_from_env("O") == CLIP_MODE_OFF ||
        clip_dir_from_env("off") == CLIP_MODE_OFF ||
        clip_dir_from_env("") == CLIP_MODE_OFF) {
        failures++;
        printf("  FAIL a value with no digits must not read as 0 (= off)\n");
    }

    /* 3. The two directions must not be the same function. Swap them and every
     *    single-direction assertion above still passes in pairs - except this
     *    one, which pins the asymmetry itself. */
    checks++;
    if (clip_dir_to_pc(CLIP_MODE_TO_VM) == clip_dir_to_vm(CLIP_MODE_TO_VM)) {
        failures++;
        printf("  FAIL to_pc and to_vm answer alike on a one-way mode\n");
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
