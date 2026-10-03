/* test_netpath - the latency split, me -> my router -> Shadow (NET1).
 *
 * Only the arithmetic is reachable offline: a ping needs a network and a
 * gateway, and a suite that pings would fail on a build machine rather than
 * on a defect. `netpath_split_compute` is the part with the edge cases, and
 * they are all cases a real session produces - a gateway that drops ICMP, a
 * local hop measured larger than the total, a session with no round trip yet.
 */
#include <stdio.h>
#include <string.h>

#include "../core/services/netpath.h"

static int checks = 0, failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL %s\n", what); }
}

static void eqi(long long got, long long want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %-46s got %lld, expected %lld\n", what, got, want);
    }
}

int main(void)
{
    netpath_split s;

    printf("== netpath latency split (NET1 2026-10-03) ==\n");

    /* --- the ordinary case ----------------------------------------------- */
    memset(&s, 0, sizeof s);
    netpath_split_compute(20000, 2000, &s);     /* 20 ms total, 2 ms local */
    eqi(s.local_us, 2000, "the local hop is what was measured");
    eqi(s.remote_us, 18000, "the remote hop is the remainder");
    eqi(s.total_us, 20000, "the total is kept");
    ok(s.have_local, "the local hop is marked known");
    ok(s.ordered, "local <= total, so the split means something");

    /* --- the gateway did not answer -------------------------------------- */
    memset(&s, 0, sizeof s);
    netpath_split_compute(20000, -1, &s);
    eqi(s.local_us, -1, "an unmeasured local hop stays unknown");
    ok(!s.have_local, "and is marked unknown");
    /* NOT -1: the whole round trip did happen out there, and showing it as
     * one unlabelled figure is honest where a dash is merely unhelpful. */
    eqi(s.remote_us, 20000, "the remote hop becomes the whole total");
    ok(!s.ordered, "with nothing to order it against");

    /* --- no session round trip yet --------------------------------------- */
    memset(&s, 0, sizeof s);
    netpath_split_compute(0, 2000, &s);
    eqi(s.total_us, -1, "a zero total reads as unknown, not as zero");
    eqi(s.local_us, 2000, "the local hop is still reported");
    ok(s.have_local, "and still marked known");
    eqi(s.remote_us, -1, "the remote hop cannot be derived");
    ok(!s.ordered, "and is not ordered");

    memset(&s, 0, sizeof s);
    netpath_split_compute(-1, -1, &s);
    eqi(s.total_us, -1, "neither known: total unknown");
    eqi(s.remote_us, -1, "neither known: remote unknown");
    ok(!s.have_local, "neither known: local unknown");

    /* --- the local hop exceeds the total --------------------------------- *
     *
     * Routine rather than impossible: the two are measured at different
     * moments by different mechanisms, so a gateway that answers slowly once
     * beats a fast session round trip. A negative remote hop would be
     * nonsense; a silent zero would be a lie, hence the flag. */
    memset(&s, 0, sizeof s);
    netpath_split_compute(5000, 9000, &s);
    eqi(s.remote_us, 0, "an over-large local hop clamps the remote to zero");
    ok(!s.ordered, "and the clamp is reported, not hidden");
    eqi(s.local_us, 9000, "the local hop is still reported as measured");

    /* Exactly equal is NOT a clamp: everything was local, which is a real
     * reading and must not be greyed out. */
    memset(&s, 0, sizeof s);
    netpath_split_compute(5000, 5000, &s);
    eqi(s.remote_us, 0, "an equal local hop leaves nothing remote");
    ok(s.ordered, "and equal is ordered, not clamped");

    /* --- a sub-millisecond LAN ------------------------------------------- *
     *
     * Windows' IcmpSendEcho reports whole milliseconds, so a fast gateway
     * comes back as 0 - which is a SUCCESS, not a failure. Zero must survive
     * as a measured value or every wired LAN would read as "unknown". */
    memset(&s, 0, sizeof s);
    netpath_split_compute(20000, 0, &s);
    ok(s.have_local, "a zero local hop is measured, not missing");
    eqi(s.local_us, 0, "and stays zero");
    eqi(s.remote_us, 20000, "with the whole total remote");
    ok(s.ordered, "and is ordered");

    /* --- the gateway name is not touched by the arithmetic --------------- *
     *
     * `netpath_measure` fills it before computing, so a compute that cleared
     * the struct would wipe the one piece of context the display shows. */
    memset(&s, 0, sizeof s);
    /* RFC 5737 TEST-NET-1, not a real address off this machine: a test
     * that hardcodes the author's own gateway puts a detail of their home
     * network into the repository for no reason. */
    strcpy(s.gateway, "192.0.2.1");
    netpath_split_compute(20000, 2000, &s);
    ok(strcmp(s.gateway, "192.0.2.1") == 0,
       "the gateway name survives the computation");

    /* --- a null out must not crash --------------------------------------- */
    netpath_split_compute(20000, 2000, NULL);
    ok(1, "a null destination is ignored rather than dereferenced");

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
