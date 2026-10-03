/* test_qt_machine_state - the machine-state vocabulary (UI1).
 *
 * The pill's colour is a claim about the machine, so the table of words it is
 * read from is worth holding down. The bug this was written against: the
 * transitional test matched `start`, which also matches `started`, so a
 * RUNNING machine was painted amber and the "started" entry in the awake list
 * was dead code nobody could see was dead.
 */
#include <cstdio>
#include <cstring>

#include "../clients/qt/machine_state.hpp"

using halyard::pillClassFor;

static int checks = 0, failures = 0;

static void is(const char *state, const char *want)
{
    checks++;
    const char *got = pillClassFor(QString::fromUtf8(state));
    if (std::strcmp(got, want) != 0) {
        failures++;
        std::printf("  FAIL %-24s got \"%s\", expected \"%s\"\n", state, got, want);
    }
}

int main()
{
    std::printf("== Qt machine state pills (UI1 2026-10-03) ==\n");

    /* --- awake ----------------------------------------------------------- */
    is("running", "ok");
    is("RUNNING", "ok");            /* the comparison lowercases first */
    is("ready", "ok");
    is("active", "ok");
    is("online", "ok");
    /* The regression: "started" must be awake, not transitional. */
    is("started", "ok");
    is("Started", "ok");

    /* --- on its way ------------------------------------------------------ */
    is("starting", "busy");
    is("STARTING", "busy");
    is("booting", "busy");
    is("pending", "busy");
    is("creating", "busy");
    is("provisioning", "busy");
    is("resuming", "busy");
    is("waking", "busy");

    /* --- asleep, and everything unknown ---------------------------------- */
    is("stopped", "off");
    is("shutdown", "off");
    is("off", "off");
    is("", "off");
    is("terminated", "off");
    /* The conservative default is the whole point: a word nobody has seen
     * before must not come out green. */
    is("quiescent", "off");
    is("zzz", "off");
    is("   ", "off");

    /* A state that contains BOTH a transitional and an awake word is
     * transitional: it is the safer of the two claims. */
    is("starting-ready", "busy");

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
