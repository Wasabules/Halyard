/* test_bitrate_ctl.c - G19 and the user's live bitrate choice (CFG-1),
 * core/protocol/bitrate_ctl.h.
 *
 * Until CFG-1, G19 and the UI wrote the same kUpdateSession f4 value and G19
 * never learned the user's choice: its cap was frozen at session start. Each
 * check names CFG-1 and its case letter. Compiled against the CURRENT G19 put
 * behind the same API (bench-only bitrate_ctl_current.h), the checks marked
 * COUNTER-CASE fail: that is what they are for.
 *
 * The harness plays the session, one step per second: the UI's message goes
 * out before G19's tick of the same second (the session loop serves the UI's
 * pending request first), and `wire` is the last value emitted by either writer.
 */
#include <stdio.h>
#include <stdint.h>
#ifndef BITRATE_CTL_HEADER
#define BITRATE_CTL_HEADER "../core/protocol/bitrate_ctl.h"
#endif
#include BITRATE_CTL_HEADER

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                 \
    if (!(cond)) { printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, what);   \
                   failures++; }                                              \
} while (0)

#define DUR_MAX 300
typedef struct { int t; uint32_t mbps; int bump; } ui_t;   /* bump: the generation moves */
typedef struct {
    uint32_t settings, env;      /* p->max_bitrate_mbps, SHADOW_BITRATE_MBPS (0 = unset) */
    int tcp;                     /* K15: no chunk counters, hence no loss signal at all */
    ui_t ui[3]; int n_ui;
    int loss_at[6]; int n_loss;  /* seconds whose window loses 1% */
    int dur;
} sc_t;
typedef struct {
    uint32_t wire[DUR_MAX + 1];
    uint32_t g19_min_after;      /* G19's own emissions after the first UI call */
    int g19_emits_after;
} run_t;

static int lossy(const sc_t *s, int sec)
{
    for (int i = 0; i < s->n_loss; i++) if (s->loss_at[i] == sec) return 1;
    return 0;
}

static void play(const sc_t *s, run_t *r)
{
    bitrate_ctl_t st;
    uint32_t user_mbps = 0; unsigned user_gen = 0;
    /* what the announcement carried (session_announce_channels) */
    uint32_t wire = s->env ? s->env : (s->settings ? s->settings : BITRATE_CLIENT_DEFAULT_MBPS);
    const int first_ui = s->n_ui ? s->ui[0].t : DUR_MAX + 1;
    bitrate_ctl_init(&st, bitrate_ctl_base_cap(s->settings, s->env));
    r->g19_min_after = 0xffffffffu; r->g19_emits_after = 0;
    for (int sec = 0; sec <= s->dur; sec++) {
        for (int i = 0; i < s->n_ui; i++) if (s->ui[i].t == sec) {
            if (s->ui[i].mbps) wire = s->ui[i].mbps;         /* the UI's own message */
            if (s->ui[i].bump) { user_mbps = s->ui[i].mbps; user_gen++; }
        }
        if (sec >= 3) {
            const double loss = s->tcp ? bitrate_ctl_loss(0, 0)
                                       : bitrate_ctl_loss(1000, lossy(s, sec) ? 10 : 0);
            const uint32_t out = bitrate_ctl_tick(&st, loss, user_mbps, user_gen);
            if (out) {
                wire = out;
                if (sec > first_ui) {
                    r->g19_emits_after++;
                    if (out < r->g19_min_after) r->g19_min_after = out;
                }
            }
        }
        r->wire[sec] = wire;
    }
}
static uint32_t max_after(const run_t *r, int from, int to)
{
    uint32_t m = 0;
    for (int s = from + 1; s <= to; s++) if (r->wire[s] > m) m = r->wire[s];
    return m;
}
static int secs_above(const run_t *r, int from, int to, uint32_t ref)
{
    int n = 0;
    for (int s = from + 1; s <= to; s++) n += r->wire[s] > ref;
    return n;
}

int main(void)
{
    run_t r;
    printf("== bitrate_ctl: G19 obeys the user's live choice (CFG-1) ==\n");

    /* The pieces. */
    CHECK(bitrate_ctl_base_cap(100, 25) == 25, "COUNTER-CASE CFG-1 (E): SHADOW_BITRATE_MBPS wins over the settings, as in the announcement");
    CHECK(bitrate_ctl_base_cap(100, 0) == 100, "CFG-1: without the env, the settings value");
    CHECK(bitrate_ctl_base_cap(0, 0) == BITRATE_CLIENT_DEFAULT_MBPS, "CFG-1: Auto resolves to the client default");
    CHECK(bitrate_ctl_loss(0, 0) == 0.0, "CFG-1 (T): no chunk expected reads as no loss");
    {
        bitrate_ctl_t st; bitrate_ctl_init(&st, 100);
        CHECK(bitrate_ctl_tick(&st, 0.0, 0, 0) == 25, "CFG-1: cautious start at 25 kept");
        CHECK(bitrate_ctl_tick(&st, 0.0, 60, 1) == 0, "CFG-1: adopting emits nothing - the UI message carries it");
        CHECK(st.cur == 60, "COUNTER-CASE CFG-1: the user's value is adopted as G19's current value");
    }

    /* (A) DEBIT-1's workaround: 100 breaks in play, 25 holds. The user drops
     * to 25 at 60 s on a clean link. Current tree: 70 on the wire at 62 s,
     * 100 by the end, while the screen says 25. */
    { sc_t s = { 100, 0, 0, {{60, 25, 1}}, 1, {0}, 0, 180 }; play(&s, &r);
      CHECK(r.wire[65] == 25, "COUNTER-CASE CFG-1 (A): 25 on the wire 5 s after the choice (current: 70)");
      CHECK(max_after(&r, 60, 180) <= 25, "COUNTER-CASE CFG-1 (A): never above the choice afterwards (current: 100)");
      CHECK(secs_above(&r, 60, 180, 25) == 0, "COUNTER-CASE CFG-1 (A): not one second above 25"); }

    /* (F) An fps-only call arrives with mbps == 0. Read literally, it would
     * clear the user's cap and let G19 climb back to the frozen one. Played
     * here by the WORST caller - one that moves the generation anyway. */
    { sc_t s = { 100, 0, 0, {{60, 25, 1}, {70, 0, 1}}, 2, {0}, 0, 180 }; play(&s, &r);
      CHECK(max_after(&r, 60, 180) <= 25, "COUNTER-CASE CFG-1 (F): an fps-only call leaves the cap at 25");
      CHECK(r.wire[180] == 25, "COUNTER-CASE CFG-1 (F): and 25 is still in force at the end"); }

    /* (R) The user RAISES 25 -> 60. "min(cur, cap) + emit" would send G19's
     * old 25 then climb from it (28 at 35 s): the choice undone upward. This
     * guards the fix, not the current tree. */
    { sc_t s = { 25, 0, 0, {{30, 60, 1}}, 1, {0}, 0, 120 }; play(&s, &r);
      CHECK(r.wire[35] == 60, "CFG-1 (R): a raise holds - 60 on the wire at 35 s");
      CHECK(r.g19_emits_after == 0 || r.g19_min_after >= 60, "CFG-1 (R): no G19 emission below 60 without loss"); }

    /* (A3) Auto, the user raises to 40, then 1% loss every 30 s. G19 must lower
     * FROM 40 and climb back TO 40. Current tree: 15, and never above 20 again. */
    { sc_t s = { 0, 0, 0, {{20, 40, 1}}, 1, {30, 60, 90}, 3, 120 }; play(&s, &r);
      CHECK(r.wire[31] == 30, "COUNTER-CASE CFG-1 (A3): a loss lowers from the user's 40 to 30 (current: 15)");
      CHECK(r.wire[50] == 40, "COUNTER-CASE CFG-1 (A3): and climbs back to 40 (current: 20)");
      CHECK(max_after(&r, 20, 120) == 40, "CFG-1 (A3): never above 40"); }

    /* (E) env.txt says 25, the settings say 100. The announcement and the
     * ready-time f13 obey the env; G19 climbed over it to 100. */
    { sc_t s = { 100, 25, 0, {{0, 0, 0}}, 0, {0}, 0, 180 }; play(&s, &r);
      CHECK(max_after(&r, 2, 180) <= 25, "COUNTER-CASE CFG-1 (E): SHADOW_BITRATE_MBPS=25 is never exceeded (current: 100)"); }

    /* (T) TCP video (K15): no loss signal, every window reads clean. */
    { sc_t s = { 100, 0, 1, {{60, 25, 1}}, 1, {0}, 0, 180 }; play(&s, &r);
      CHECK(r.wire[65] == 25, "COUNTER-CASE CFG-1 (T): over TCP the choice holds too");
      CHECK(max_after(&r, 60, 180) <= 25, "COUNTER-CASE CFG-1 (T): never above 25 over TCP"); }

    /* (L) G19 keeps its job: a loss after the choice still lowers - from 25. */
    { sc_t s = { 100, 0, 0, {{60, 25, 1}}, 1, {70}, 1, 180 }; play(&s, &r);
      CHECK(r.wire[70] == 18, "COUNTER-CASE CFG-1 (L): 1% loss after the choice lowers 25 to 18 (current: 54)");
      CHECK(max_after(&r, 60, 180) <= 25, "COUNTER-CASE CFG-1 (L): and the climb back stops at 25"); }

    /* (Late) G19 already at its cap (100 since 102 s): on a clean link the
     * choice survives even today - the override needs a loss, and then G19
     * sends 0.75 x 100 = 75. */
    { sc_t s = { 100, 0, 0, {{110, 25, 1}}, 1, {130}, 1, 200 }; play(&s, &r);
      CHECK(r.wire[125] == 25, "CFG-1 (Late): a choice made once G19 sits at its cap persists on a clean link");
      CHECK(r.wire[130] == 18, "COUNTER-CASE CFG-1 (Late): the first loss lowers from 25, not from 100 (current: 75)"); }

    /* (Lf) A choice below G19's floor of 8: a loss must not RAISE it to 8+. */
    { sc_t s = { 100, 0, 0, {{20, 5, 1}}, 1, {25}, 1, 60 }; play(&s, &r);
      CHECK(max_after(&r, 20, 60) <= 5, "COUNTER-CASE CFG-1 (Lf): 5 Mb/s stays 5, loss or not"); }

    /* (Auto) Choosing Auto mid-session is 20 on the wire (B1), and a cap. */
    { sc_t s = { 100, 0, 0, {{60, BITRATE_CLIENT_DEFAULT_MBPS, 1}}, 1, {0}, 0, 180 }; play(&s, &r);
      CHECK(max_after(&r, 60, 180) <= BITRATE_CLIENT_DEFAULT_MBPS, "COUNTER-CASE CFG-1 (Auto): Auto mid-session caps at 20"); }

    /* (S0) A choice made before G19's first tick (sec 3) is adopted: the
     * cautious start must not undo it. Design consequence of checking the
     * generation first. */
    { sc_t s = { 100, 0, 0, {{1, 60, 1}}, 1, {0}, 0, 60 }; play(&s, &r);
      CHECK(r.wire[3] == 60 && r.wire[60] == 60, "COUNTER-CASE CFG-1 (S0): a choice at 1 s is not replaced by the start at 25"); }

    /* (W) The optional wire feedback. Under the plain adopt rule the wire can
     * drift from G19's belief in two ways, and nothing brings it back. */
    {   /* W1: a choice at 1 s, then the ready-time f13 sends p->max at 3 s */
        bitrate_ctl_t st; bitrate_ctl_init(&st, 100);
        bitrate_ctl_on_wire(&st, 25);
        bitrate_ctl_on_wire(&st, 100);
        CHECK(bitrate_ctl_tick(&st, 0.0, 25, 1) == 25, "CFG-1 (W1): a choice overwritten by the ready-time f13 is re-sent");
        bitrate_ctl_on_wire(&st, 25);
        CHECK(bitrate_ctl_tick(&st, 0.0, 25, 1) == 0, "CFG-1 (W1): once, not on every tick");
    }
    {   /* W2: G19 read the old generation, then its 70 overwrote the UI's 25 in the shared slot */
        bitrate_ctl_t st; bitrate_ctl_init(&st, 100);
        st.cur = 67; st.good = 3; bitrate_ctl_on_wire(&st, 67);
        CHECK(bitrate_ctl_tick(&st, 0.0, 0, 0) == 70, "CFG-1 (W2): G19 climbs on the generation it read");
        bitrate_ctl_on_wire(&st, 70);
        CHECK(bitrate_ctl_tick(&st, 0.0, 25, 1) == 25, "CFG-1 (W2): the lost UI write is repaired at the next tick");
    }

    /* REGRESSION GUARD - nobody touches the menu: G19 is exactly G19
     * (G19 2026-08-22: start 25, +3 per 4 clean s, x0.75 above 0.8%). */
    { sc_t s = { 100, 0, 0, {{0, 0, 0}}, 0, {20}, 1, 250 }; play(&s, &r);
      CHECK(r.wire[3] == 25 && r.wire[6] == 28 && r.wire[18] == 37, "CFG-1 guard: the climb is unchanged");
      CHECK(r.wire[20] == 27, "CFG-1 guard: a loss still takes x0.75");
      CHECK(r.wire[120] == 100 && r.wire[250] == 100, "CFG-1 guard: and the frozen cap is still reached"); }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
