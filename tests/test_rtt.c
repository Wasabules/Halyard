/* test_rtt.c - the round trip to the VM (core/protocol/rtt.h).
 *
 * This module exists because the client had NO network latency measurement at
 * all: of the report's twelve stages, one contains network and it measures
 * jitter, never delay. A link at a steady 200 ms read there exactly like a link
 * at 5 ms.
 *
 * The way a round-trip counter goes wrong is not by returning a bad number, it
 * is by returning one that never existed. So the counter-cases are refusals:
 *   - an unsolicited message from the server must not become a round trip of
 *     zero;
 *   - sequence number 0 must match nothing - a freshly zeroed table is full of
 *     zeros;
 *   - a duplicated reply must count once;
 *   - a clock that goes backwards must not produce a gigantic duration.
 */
#include "../core/protocol/rtt.h"

#include <stdio.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                   \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

static void the_nominal_round_trip(void)
{
    rtt_t r;
    rtt_reset(&r);
    CHECK(rtt_avg_us(&r) == 0 && r.count == 0, "[nominal] nothing measured at the start");

    rtt_sent(&r, 100, 1000000);
    CHECK(rtt_reply(&r, 100, 1030000) == 30000, "[nominal] 30 ms d'aller-retour");
    CHECK(r.last_us == 30000 && r.min_us == 30000 && r.max_us == 30000, "[nominal] les extremes");
    CHECK(rtt_avg_us(&r) == 30000, "[nominal] la moyenne");

    rtt_sent(&r, 101, 2000000);
    rtt_reply(&r, 101, 2010000);
    CHECK(r.min_us == 10000 && r.max_us == 30000, "[nominal] les extremes suivent");
    CHECK(rtt_avg_us(&r) == 20000, "[nominal] and so is the average");

    /* Several requests in flight at once: the heartbeat leaves every 500 ms
     * and does not wait for the previous reply. */
    rtt_reset(&r);
    rtt_sent(&r, 1, 1000); rtt_sent(&r, 2, 2000); rtt_sent(&r, 3, 3000);
    CHECK(rtt_reply(&r, 2, 12000) == 10000, "[nominal] we pair out of order");
    CHECK(rtt_reply(&r, 1, 21000) == 20000, "[nominal] and the oldest one after it");
    CHECK(r.count == 2, "[nominal] two samples, the third is still in flight");
}

static void it_refuses_what_is_not_a_reply(void)
{
    rtt_t r;
    rtt_reset(&r);
    rtt_sent(&r, 50, 1000);

    /* THE MAIN COUNTER-CASE. The server sends unsolicited messages - bitrate
     * events, periodic statistics. Counting those as replies would invent
     * round trips. */
    CHECK(rtt_reply(&r, 999, 5000) == 0, "[refusal] a sequence number that is not ours");
    CHECK(r.count == 0, "[refusal] and it enters no statistic");

    /* Number 0: a session's first replies carry none, and a freshly zeroed
     * table is full of zeros - matching them would produce a round trip for
     * every message. */
    CHECK(rtt_reply(&r, 0, 5000) == 0, "[refusal] sequence number zero never pairs");

    /* A duplicated reply. The first counts, the second does not: otherwise it
     * would produce a longer round trip that never happened. */
    CHECK(rtt_reply(&r, 50, 3000) == 2000, "[refusal] the real reply counts");
    CHECK(rtt_reply(&r, 50, 9000) == 0, "[refus] sa duplicata NON");
    CHECK(r.count == 1, "[refusal] a single sample");
    CHECK(r.max_us == 2000, "[refusal] and the maximum was not polluted");

    /* A mad clock. A time that goes backwards would give, unsigned, a
     * gigantic duration - the L21 family. */
    rtt_reset(&r);
    rtt_sent(&r, 7, 100000);
    CHECK(rtt_reply(&r, 7, 50000) == 0, "[refusal] a clock that goes backwards");
    rtt_sent(&r, 8, 100000);
    CHECK(rtt_reply(&r, 8, 100000) == 0, "[refusal] a zero round trip");
    rtt_sent(&r, 9, 0);
    CHECK(rtt_reply(&r, 9, 60LL * 1000000) == 0, "[refusal] sixty seconds is not a round trip");
    CHECK(r.count == 0, "[refusal] none of those three counted");

    CHECK(rtt_reply(NULL, 1, 1) == 0, "[bornes] NULL");
    rtt_sent(NULL, 1, 1);   /* must not crash */
    checks++;
}

static void percentiles_and_jitter(void)
{
    rtt_t r;
    int i;
    rtt_reset(&r);

    /* A hundred samples from 1 to 100 ms: the window keeps only the last 64,
     * so 37..100 ms. */
    for (i = 1; i <= 100; i++) { rtt_sent(&r, (uint32_t)i, 0); rtt_reply(&r, (uint32_t)i, i * 1000); }
    CHECK(r.n == RTT_KEEP, "[percentiles] the window is full");
    CHECK(rtt_pct_us(&r, 0) == 37000, "[percentiles] the smallest in the window");
    CHECK(rtt_pct_us(&r, 100) == 100000, "[centiles] le plus grand");
    CHECK(rtt_pct_us(&r, 50) > rtt_pct_us(&r, 0), "[percentiles] the median is in the middle");
    CHECK(rtt_pct_us(&r, 90) > rtt_pct_us(&r, 50), "[percentiles] the p90 is above it");
    /* The p90 is what matters: a good mean with a bad p90 FEELS bad, and the
     * mean alone hides it. */
    CHECK(rtt_pct_us(&r, 90) >= rtt_avg_us(&r), "[percentiles] and it exceeds the average");

    /* Bornes du centile. */
    CHECK(rtt_pct_us(&r, -10) == rtt_pct_us(&r, 0), "[percentiles] a negative p is clamped to 0");
    CHECK(rtt_pct_us(&r, 500) == rtt_pct_us(&r, 100), "[percentiles] and a p that is too large, to 100");
    { rtt_t q; rtt_reset(&q);
      CHECK(rtt_pct_us(&q, 50) == 0, "[centiles] sans echantillon, zero");
      CHECK(rtt_pct_us(NULL, 50) == 0, "[centiles] NULL"); }

    /* Jitter: a steady path has little, a swinging one has a lot. It is what
     * latency alone does not say. */
    rtt_reset(&r);
    for (i = 0; i < 10; i++) { rtt_sent(&r, (uint32_t)(i+1), 0); rtt_reply(&r, (uint32_t)(i+1), 30000); }
    CHECK(rtt_jitter_us(&r) == 0, "[gigue] un chemin parfaitement stable");
    rtt_reset(&r);
    for (i = 0; i < 10; i++) {
        rtt_sent(&r, (uint32_t)(i+1), 0);
        rtt_reply(&r, (uint32_t)(i+1), (i % 2) ? 90000 : 10000);
    }
    CHECK(rtt_jitter_us(&r) == 80000, "[jitter] a path that swings by 80 ms");
    { rtt_t q; rtt_reset(&q);
      CHECK(rtt_jitter_us(&q) == 0, "[gigue] sans echantillon");
      rtt_sent(&q, 1, 0); rtt_reply(&q, 1, 5000);
      CHECK(rtt_jitter_us(&q) == 0, "[jitter] a single sample is not a jitter"); }
}

int main(void)
{
    printf("test_rtt - the round trip to the VM\n");
    the_nominal_round_trip();
    it_refuses_what_is_not_a_reply();
    percentiles_and_jitter();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
