/* test_path_probe.c - the path capacity estimate
 * (core/protocol/path_probe.h).
 *
 * This module answers "what bitrate does your link carry?", and the way it can
 * go wrong is not by returning a bad number: it is by returning a number at all
 * when it has measured nothing. A tester showing "17 Mb/s" after three samples
 * looks exactly as sure of itself as after a hundred, and the user will set
 * their bitrate from it.
 *
 * So the counter-cases are mostly refusals:
 *   - too few samples: do NOT conclude;
 *   - no loss at all: do NOT conclude a ceiling;
 *   - zero-bitrate samples: ignored, or the advised ceiling becomes zero.
 */
#include "../core/protocol/path_probe.h"

#include <stdio.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                   \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* ── Les refus ────────────────────────────────────────────────────────────── */

static void it_refuses_to_guess(void)
{
    path_probe_t p;
    int i;

    path_probe_reset(&p);
    CHECK(path_probe_verdict(&p) == PROBE_INSUFFICIENT, "[refusal] nothing measured, nothing to say");
    CHECK(path_probe_suggest_mbps(&p) == 0.0f, "[refusal] and no advice either");

    /* Three samples, one of them lossy: tempting, and not enough. */
    path_probe_add(&p, 15.0f, 0.0f);
    path_probe_add(&p, 17.0f, 0.02f);
    path_probe_add(&p, 14.0f, 0.0f);
    CHECK(path_probe_verdict(&p) == PROBE_INSUFFICIENT,
          "[refusal] three samples do not make a measurement");
    CHECK(path_probe_suggest_mbps(&p) == 0.0f, "[refusal] still no advice");

    /* Enough samples, but NO loss. The verdict is not "no ceiling at
     * 20 Mb/s" but "none found up to 20": the difference is everything,
     * because the first wording invites you to climb. */
    path_probe_reset(&p);
    for (i = 0; i < 20; i++) path_probe_add(&p, 20.0f, 0.0f);
    CHECK(path_probe_verdict(&p) == PROBE_NO_CEILING, "[refusal] with no loss, no ceiling is found");
    CHECK(path_probe_suggest_mbps(&p) == 0.0f,
          "[refusal] and we do NOT advise a figure - that would be extrapolating");
    CHECK(p.clean_max_mbps == 20.0f, "[refusal] but we know it carries at least 20");

    /* A single loss spike does not make a ceiling: it can come from anything
     * else. */
    path_probe_reset(&p);
    for (i = 0; i < 20; i++) path_probe_add(&p, 15.0f, 0.0f);
    path_probe_add(&p, 16.0f, 0.03f);
    CHECK(path_probe_verdict(&p) == PROBE_NO_CEILING,
          "[refusal] an isolated spike does not make a ceiling");
}

/* ── La session pausee ────────────────────────────────────────────────────── */

static void a_paused_session(void)
{
    path_probe_t p;
    int i;
    path_probe_reset(&p);

    /* COUNTER-CASE: a stopped stream gives zero-bitrate samples. Counting
     * them would make a "lowest lossy bitrate" of 0, hence an advised ceiling
     * of zero - the opposite of a measurement, and the user would end up with
     * a postage-stamp picture. */
    for (i = 0; i < 10; i++) path_probe_add(&p, 0.0f, 0.0f);
    CHECK(p.samples == 0, "[pause] a zero throughput is not a sample");
    for (i = 0; i < 10; i++) path_probe_add(&p, 0.05f, 0.5f);
    CHECK(p.samples == 0, "[pause] nor is a residual throughput with loss");
    CHECK(p.lossy_min_mbps == 0.0f, "[pause] so no floor at zero");
}

/* ── La vraie session ─────────────────────────────────────────────────────── */

static void the_measured_session(void)
{
    /* The observed pattern: climb to 17, lose, fall back to 13, climb again. */
    static const struct { float mbps, loss; } S[] = {
        {13.0f,0.000f},{15.5f,0.000f},{16.8f,0.000f},{16.4f,0.034f},
        {13.6f,0.000f},{15.7f,0.000f},{17.2f,0.000f},{17.2f,0.015f},
        {12.0f,0.000f},{15.4f,0.000f},{17.2f,0.000f},{16.5f,0.006f},
        {16.2f,0.000f},{17.4f,0.000f},{15.8f,0.013f},{13.8f,0.000f},
    };
    path_probe_t p;
    size_t i;
    path_probe_reset(&p);
    for (i = 0; i < sizeof S / sizeof S[0]; i++) path_probe_add(&p, S[i].mbps, S[i].loss);

    CHECK(p.samples == 16, "[session] every sample counted");
    CHECK(p.lossy == 4, "[session] quatre en perte");
    CHECK(path_probe_verdict(&p) == PROBE_CEILING_FOUND, "[session] a ceiling is visible");

    /* The advice is the highest bitrate seen CLEAN. Not the mean - that would
     * advise a bitrate at which half the samples were already losing. Nor the
     * lowest seen lossy - that would advise the exact point where it
     * breaks. */
    CHECK(path_probe_suggest_mbps(&p) > 17.0f && path_probe_suggest_mbps(&p) < 17.5f,
          "[session] the advice is the highest clean throughput, 17.4");
    CHECK(p.lossy_min_mbps > 15.7f && p.lossy_min_mbps < 15.9f,
          "[session] and the lowest seen with loss, 15.8");

    /* The signature measured on the console: loss appears higher than clean.
     * The day that stops being true, bitrate is not what is saturating. */
    CHECK(path_probe_lossy_avg(&p) > path_probe_clean_avg(&p),
          "[session] the loss arrives at HIGH throughputs - that really is saturation");

    /* Bornes. */
    CHECK(path_probe_verdict(NULL) == PROBE_INSUFFICIENT, "[bornes] NULL");
    CHECK(path_probe_suggest_mbps(NULL) == 0.0f, "[bounds] NULL advises nothing");
    CHECK(path_probe_clean_avg(NULL) == 0.0f, "[bounds] the average of a NULL");
    { path_probe_t q; path_probe_reset(&q);
      CHECK(path_probe_clean_avg(&q) == 0.0f, "[bounds] an average with no sample");
      CHECK(path_probe_lossy_avg(&q) == 0.0f, "[bounds] an average with no loss");
      path_probe_add(NULL, 10.0f, 0.0f);
      CHECK(q.samples == 0, "[bounds] adding into NULL does not crash"); }

    /* The threshold: 0.3 % separates background noise from real saturation.
     * Just below counts as clean, just above as lossy. */
    path_probe_reset(&p);
    path_probe_add(&p, 10.0f, 0.002f);
    CHECK(p.lossy == 0 && p.clean_max_mbps == 10.0f, "[threshold] 0.2 % is noise");
    path_probe_add(&p, 11.0f, 0.004f);
    CHECK(p.lossy == 1, "[threshold] 0.4 % is loss");
}

int main(void)
{
    printf("test_path_probe - what the link really carries\n");
    it_refuses_to_guess();
    a_paused_session();
    the_measured_session();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
