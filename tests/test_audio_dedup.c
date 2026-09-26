/* test_audio_dedup.c - the audio channel's anti-duplicate window.
 *
 * The server emits every Opus frame twice. Playing both doubles the duration
 * heard; throwing them all away gives silence. This suite pins both edges.
 *
 * Campaign S36 (2026-08-25): the sound was audible only on the very FIRST
 * session after launch, never afterwards. The cause is at the end of the file.
 *
 * Campaign ING-A1 (2026-09-11): the window had no way back from a numbering
 * discontinuity - one frame far ahead muted the rest of the stream. Its
 * counter-cases follow the S36 one, which keeps its demonstration on the old
 * rule (`no_resync`, what SHADOW_AUDIO_DEDUP_RESYNC=0 gives). Campaign ING-A4
 * (2026-09-11): the window's one door and its two counters, last.
 */
#include "../core/protocol/audio_dedup.h"
#include <stdio.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* The real stream: every frame arrives twice, with increasing numbers. */
static void flux_double(void)
{
    audio_dedup_t d = {0};
    int joues = 0;
    for (uint32_t seq = 1000; seq < 1100; seq++) {
        if (audio_dedup_accepte(&d, seq)) joues++;
        if (audio_dedup_accepte(&d, seq)) joues++;   /* le second exemplaire */
    }
    CHECK(joues == 100,
            "100 frames emitted twice must give 100 plays, not 200");
}

/* An A B A B alternation: that is what condemns a simple comparison against
 * the last one. */
static void alternating(void)
{
    audio_dedup_t d = {0};
    CHECK(audio_dedup_accepte(&d, 10), "first frame: played");
    CHECK(audio_dedup_accepte(&d, 11), "next frame: played");
    CHECK(!audio_dedup_accepte(&d, 10), "back to 10: already played");
    CHECK(!audio_dedup_accepte(&d, 11), "back to 11: already played");
    CHECK(audio_dedup_accepte(&d, 12), "12 is new: played");
}

/* Out of order: a late frame is played when its slot is free. */
static void out_of_order(void)
{
    audio_dedup_t d = {0};
    audio_dedup_accepte(&d, 500);
    audio_dedup_accepte(&d, 503);
    CHECK(audio_dedup_accepte(&d, 501),
            "501 arrives after 503 but has not been played yet: we play it");
    CHECK(!audio_dedup_accepte(&d, 501), "501 a second time: discarded");
    CHECK(!audio_dedup_accepte(&d, 400),
            "400 is outside the window (>64 behind): too late for its slot");
}

/* A forward jump larger than the window empties it cleanly. */
static void grand_saut(void)
{
    audio_dedup_t d = {0};
    audio_dedup_accepte(&d, 1);
    CHECK(audio_dedup_accepte(&d, 5000), "a big jump forward: played");
    CHECK(!audio_dedup_accepte(&d, 5000), "and only once");
    CHECK(audio_dedup_accepte(&d, 4990),
            "just behind the jump, slot free: played");
}

/* COUNTER-CASE S36 - SOUND ON ONE SESSION ONLY. */
static void counter_case_new_session(void)
{
    /* A complete session: the server numbered high. */
    audio_dedup_t d = {0};
    for (uint32_t seq = 1000000; seq < 1000100; seq++) audio_dedup_accepte(&d, seq);
    audio_dedup_t old_rule = d;      /* the same stale window, pre-ING-A1 rule */
    old_rule.no_resync = true;

    /* Next session: the server RENUMBERS low. With the state kept, every gap
     * was ~-1000000, hence "too late": a hundred percent of the frames dropped.
     * Console measurement: 3997 discarded out of 3997, against 1999 out of 3998
     * on the first session. */
    audio_dedup_t neuve = {0};   /* what the session context now does */
    int joues = 0;
    for (uint32_t seq = 0; seq < 100; seq++) {
        if (audio_dedup_accepte(&neuve, seq)) joues++;
        if (audio_dedup_accepte(&neuve, seq)) joues++;
    }
    CHECK(joues == 100,
            "COUNTER-CASE: a session restarting from LOW numbers must play its "
            "frames - with the window kept, it played none of them");

    /* And the direct proof that the retained state is indeed the culprit - on
     * the rule as it stood (the revert toggle still gives it). */
    int joues_avec_etat_perime = 0;
    for (uint32_t seq = 0; seq < 100; seq++)
        if (audio_dedup_accepte(&old_rule, seq)) joues_avec_etat_perime++;
    CHECK(joues_avec_etat_perime == 0,
            "COUNTER-CASE (demonstration): reusing the previous session's window "
            "under the pre-ING-A1 rule, NO frame goes through - that is the "
            "observed silence");

    /* ING-A1: the same stale window now recovers on the 8th packet. */
    int played = 0, first = -1;
    for (uint32_t seq = 0; seq < 100; seq++)
        if (audio_dedup_accepte(&d, seq)) { played++; if (first < 0) first = (int)seq; }
    CHECK(first == 7 && played == 93,
            "ING-A1: a stale window recovers on the 8th packet and plays the rest");
    CHECK(d.resyncs == 1, "ING-A1: exactly one resync, and it is counted");
}

/* COUNTER-CASE ING-A1 - a stream renumbered INSIDE a session: descriptor at
 * seq 0 then 1.., every frame twice. Never observed; the rule must not care
 * why the numbering jumped. */
static void renumbered_in_session(void)
{
    audio_dedup_t d = {0};
    for (uint32_t s = 0; s <= 30000; s++) { audio_dedup_accepte(&d, s); audio_dedup_accepte(&d, s); }
    int played = 0, twice = 0;
    for (uint32_t s = 0; s <= 3000; s++) {
        const bool a = audio_dedup_accepte(&d, s), b = audio_dedup_accepte(&d, s);
        played += (a || b); twice += (a && b);
    }
    CHECK(played >= 2997, "ING-A1: a renumbered stream plays again within 4 frames");
    CHECK(twice == 0, "ING-A1: and never plays a frame twice (AUD8)");
}

/* COUNTER-CASE ING-A1 - ONE frame far AHEAD, which is what a reassembly slot
 * left by the previous session delivers into a fresh window (ING-A2). */
static void stray_far_ahead(void)
{
    audio_dedup_t d = {0};
    for (uint32_t s = 1; s <= 1000; s++) { audio_dedup_accepte(&d, s); audio_dedup_accepte(&d, s); }
    audio_dedup_accepte(&d, 180000);
    int played = 0;
    for (uint32_t s = 1001; s <= 4000; s++) {
        played += audio_dedup_accepte(&d, s);
        played += audio_dedup_accepte(&d, s);
    }
    CHECK(played >= 2996, "ING-A1: one stray far-ahead frame no longer mutes the session");
}

/* The run must be CONSECUTIVE: 7 stragglers then one fresh packet do not
 * resync, and a straggler after every fresh frame never does. */
static void stragglers(void)
{
    audio_dedup_t d = {0};
    for (uint32_t s = 1000; s < 1100; s++) audio_dedup_accepte(&d, s);
    for (uint32_t s = 900; s < 907; s++)
        CHECK(!audio_dedup_accepte(&d, s), "7 stragglers beyond the window: still too late");
    CHECK(audio_dedup_accepte(&d, 1100), "a fresh frame: played, and it resets the run");
    CHECK(!audio_dedup_accepte(&d, 907), "the 8th straggler after a fresh one: too late, no resync");
    CHECK(d.resyncs == 0, "no resync on interleaved stragglers");

    audio_dedup_t e = {0};
    int played = 0;
    for (uint32_t s = 1000; s < 20000; s++) {
        played += audio_dedup_accepte(&e, s);
        played += audio_dedup_accepte(&e, s);
        played += audio_dedup_accepte(&e, s - 200);   /* a straggler 2 s late */
    }
    CHECK(played == 19000 && e.resyncs == 0,
            "one straggler per frame, forever: no resync, no extra play");
}

/* ING-A4 - THE ONE DOOR'S TWO COUNTERS. Every audio frame reaching the window
 * is counted BEFORE the decision, duplicates included, so arrived - refused is
 * exactly what was played, whatever path the frame came by. */
static void admission_counters(void)
{
    audio_dedup_t d = {0};
    uint32_t arrived = 0, refused = 0, played = 0;
    uint32_t old_arrived = 0, old_refused = 0;   /* the accounting before ING-A4 */
    uint8_t f[8] = { 0x12, 0, 0, 0, 0, 0xf4, 0xff, 0xfe };
    for (uint32_t seq = 1; seq <= 1000; seq++) {
        f[1] = (uint8_t)seq;         f[2] = (uint8_t)(seq >> 8);
        f[3] = (uint8_t)(seq >> 16); f[4] = (uint8_t)(seq >> 24);
        const bool split = (seq % 4) == 0;   /* this frame came through reassembly */
        for (int copy = 0; copy < 2; copy++) {
            const bool ok = audio_dedup_admit(&d, f, (int)sizeof f, &arrived, &refused);
            played += ok;
            /* Before: the single-packet path counted before the window, the
             * reassembly paths only what the window let through. */
            if (!split || ok) old_arrived++;
            if (!ok) old_refused++;
        }
    }
    CHECK(arrived == 2000 && refused == 1000 && played == 1000,
          "ING-A4: 1000 frames sent twice: 2000 arrived, 1000 refused, 1000 played");
    CHECK(arrived - refused == played,
          "ING-A4: arrived - refused is exactly the frames played");
    CHECK(old_arrived - old_refused == 750,
          "COUNTER-CASE: with split frames counted after the window, arrived - refused "
          "read 750 where 1000 frames were played");

    /* A frame too short to carry a number: counted and played, never compared. */
    audio_dedup_t e = {0};
    uint32_t a2 = 0, r2 = 0;
    const bool one = audio_dedup_admit(&e, f, 4, &a2, &r2);
    const bool two = audio_dedup_admit(&e, f, 4, &a2, &r2);
    CHECK(one && two && a2 == 2 && r2 == 0 && !e.amorcee,
          "a frame under 5 bytes is counted and played, and leaves the window alone");
}

int main(void)
{
    printf("== the audio channel's anti-duplicate window ==\n");
    flux_double();
    alternating();
    out_of_order();
    grand_saut();
    counter_case_new_session();
    renumbered_in_session();
    stray_far_ahead();
    stragglers();
    admission_counters();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
