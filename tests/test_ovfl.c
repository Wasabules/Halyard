/* test_ovfl.c - ING-1, experiment E2: the SO_RXQ_OVFL accounting, offline.
 *
 * Pure C, no socket: it runs on Windows, where the counter itself cannot exist.
 *   Arm A = today's logic, verbatim from ctrl_session.c:336 and :528: a
 *           process-wide max, absent from the per-session reset (:2795-2806).
 *   Arm B = ovfl_accum.h: per-socket last_seen, per-session sum of deltas.
 *   Arm N = a naive "add every cmsg value", which counter-case (c) exists to
 *           catch in a future B.
 * Every report is fed to the three arms in turn: same inputs, same process.
 *
 * Exit code: 0 only if arm B passes every counter-case AND is exact on every
 * randomized session. Arm A's and N's results are printed, not asserted. */
#include "../core/protocol/ovfl_accum.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Arm A: verbatim ------------------------------------------------------ */
static uint32_t g_kernel_drops = 0;                                   /* :336 */
static void a_cmsg(uint32_t v) { if (v > g_kernel_drops) g_kernel_drops = v; } /* :528 */
static void a_session_reset(void) { /* :2795-2806 do not touch it */ }

/* ---- Arm B: proposed ------------------------------------------------------ */
#define MAXSOCK 8
static uint32_t b_last[MAXSOCK];
static uint32_t b_session;
static void b_socket_created(int s) { b_last[s] = 0; }
static void b_cmsg(int s, uint32_t v) { b_session += ovfl_accum(&b_last[s], v); }
static void b_session_reset(void) { b_session = 0; }

/* ---- Arm N: naive sum ----------------------------------------------------- */
static uint32_t n_session;
static void n_cmsg(uint32_t v) { n_session += v; }
static void n_session_reset(void) { n_session = 0; }

static void all_session_reset(void) { a_session_reset(); b_session_reset(); n_session_reset(); }
static void all_cmsg(int s, uint32_t v) { a_cmsg(v); b_cmsg(s, v); n_cmsg(v); }

static int b_fail = 0;
static void verdict(const char *name, uint32_t truth)
{
    printf("  %-44s truth=%-4u A=%-4u %-4s B=%-4u %-4s N=%-4u %s\n", name, truth,
           g_kernel_drops, g_kernel_drops == truth ? "ok" : "FAIL",
           b_session,      b_session == truth ? "ok" : "FAIL",
           n_session,      n_session == truth ? "ok" : "FAIL");
    if (b_session != truth) b_fail++;
}

static void fresh_process(void)
{
    g_kernel_drops = 0; memset(b_last, 0, sizeof b_last); b_session = 0; n_session = 0;
}

static void counter_cases(void)
{
    printf("counter-cases (value reported at the end of the session)\n");

    /* (a) session 1's socket reports 50; session 2's NEW socket reports 3. */
    fresh_process();
    all_session_reset(); b_socket_created(0);
    all_cmsg(0, 5); all_cmsg(0, 20); all_cmsg(0, 50);
    verdict("(a) session 1: socket reports 5,20,50", 50);
    all_session_reset(); b_socket_created(0);      /* reconnect: new socket */
    all_cmsg(0, 1); all_cmsg(0, 3);
    verdict("(a) session 2: NEW socket reports 1,3", 3);

    /* (b) video socket 10,40 and :base+30 socket 2,7, interleaved. */
    fresh_process();
    all_session_reset(); b_socket_created(0); b_socket_created(1);
    all_cmsg(0, 10); all_cmsg(1, 2); all_cmsg(0, 40); all_cmsg(1, 7);
    verdict("(b) video 10,40 + audio 2,7", 47);

    /* (c) repeated cumulative values: after 40, the reports 40 then 41 must
     * add 1 (total 41), not 81 (total 121). */
    fresh_process();
    all_session_reset(); b_socket_created(0);
    all_cmsg(0, 40); all_cmsg(0, 40); all_cmsg(0, 41);
    verdict("(c) one socket reports 40,40,41", 41);

    /* (d) a socket recreated WITHOUT zeroing last_seen (a forgotten reset,
     * e.g. a future S43-style resocket): B's regression branch still counts. */
    fresh_process();
    all_session_reset(); b_socket_created(0);
    all_cmsg(0, 50);
    all_cmsg(0, 3);                                  /* new socket, no reset */
    verdict("(d) 50 then a restarted counter at 3", 53);
}

/* ---- Randomized sessions -------------------------------------------------- */
static uint64_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }
static uint32_t rnd_in(uint32_t lo, uint32_t hi) { return lo + rnd() % (hi - lo + 1); }

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y);
}

typedef struct { double exact_a, exact_b, exact_n; uint32_t med_err_a, p99_err_a, max_err_a; } seed_res_t;

static seed_res_t run_seed(uint64_t seed, int scenarios)
{
    rng = seed * 0x9E3779B97F4A7C15ull + 1;
    int sessions = 0, ok_a = 0, ok_b = 0, ok_n = 0;
    uint32_t *err_a = malloc(sizeof(uint32_t) * (size_t)scenarios * 4);
    int ne = 0;
    for (int sc = 0; sc < scenarios; sc++) {
        fresh_process();                               /* one process launch */
        int nsess = (int)rnd_in(1, 4);
        for (int ss = 0; ss < nsess; ss++) {
            all_session_reset();
            int nsock = (int)rnd_in(1, 3);             /* video, :base+30, :base+13 */
            uint32_t cum[MAXSOCK] = {0}, truth = 0;
            int left[MAXSOCK];
            for (int k = 0; k < nsock; k++) { b_socket_created(k); left[k] = (int)rnd_in(1, 20); }
            int slot_of[MAXSOCK]; for (int k = 0; k < nsock; k++) slot_of[k] = k;
            int next_slot = nsock;
            int remaining = 0; for (int k = 0; k < nsock; k++) remaining += left[k];
            while (remaining > 0) {
                int k = (int)(rnd() % (uint32_t)nsock);
                if (left[k] == 0) continue;
                left[k]--; remaining--;
                /* 10 %: this socket is recreated mid-session (S43 resocket):
                 * its counter restarts at 0 and B is told so. */
                if (next_slot < MAXSOCK && rnd() % 10 == 0) {
                    truth += cum[k]; cum[k] = 0;
                    slot_of[k] = next_slot++; b_socket_created(slot_of[k]);
                }
                if (rnd() % 10 < 4) { /* repeated value */ }
                else cum[k] += rnd_in(1, 30);
                if (cum[k] != 0) all_cmsg(slot_of[k], cum[k]);  /* no cmsg while 0 */
            }
            for (int k = 0; k < nsock; k++) truth += cum[k];
            sessions++;
            ok_a += (g_kernel_drops == truth);
            ok_b += (b_session == truth);
            ok_n += (n_session == truth);
            if (b_session != truth) b_fail++;
            err_a[ne++] = g_kernel_drops > truth ? g_kernel_drops - truth : truth - g_kernel_drops;
        }
    }
    qsort(err_a, (size_t)ne, sizeof(uint32_t), cmp_u32);
    seed_res_t r;
    r.exact_a = 100.0 * ok_a / sessions; r.exact_b = 100.0 * ok_b / sessions; r.exact_n = 100.0 * ok_n / sessions;
    r.med_err_a = err_a[ne / 2]; r.p99_err_a = err_a[(ne * 99) / 100]; r.max_err_a = err_a[ne - 1];
    free(err_a);
    return r;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

int main(void)
{
    counter_cases();

    enum { SEEDS = 7, SCEN = 20000 };
    double ea[SEEDS], eb[SEEDS], en[SEEDS], ma[SEEDS];
    printf("\nrandomized sessions: %d seeds x %d process launches (1-4 sessions, 1-3 sockets,\n"
           "40 %% repeated values, 10 %% mid-session socket recreation)\n", SEEDS, SCEN);
    for (int s = 0; s < SEEDS; s++) {
        seed_res_t r = run_seed((uint64_t)s + 1, SCEN);
        ea[s] = r.exact_a; eb[s] = r.exact_b; en[s] = r.exact_n; ma[s] = r.med_err_a;
        printf("  seed %d: exact A=%.2f%% B=%.2f%% N=%.2f%% | |A-truth| median=%u p99=%u max=%u\n",
               s + 1, r.exact_a, r.exact_b, r.exact_n, r.med_err_a, r.p99_err_a, r.max_err_a);
    }
    qsort(ea, SEEDS, sizeof(double), cmp_d); qsort(eb, SEEDS, sizeof(double), cmp_d);
    qsort(en, SEEDS, sizeof(double), cmp_d); qsort(ma, SEEDS, sizeof(double), cmp_d);
    printf("  MEDIAN over seeds: exact A=%.2f%% [%.2f..%.2f]  B=%.2f%% [%.2f..%.2f]  N=%.2f%% [%.2f..%.2f]  "
           "|A-truth| median=%.0f [%.0f..%.0f]\n",
           ea[SEEDS / 2], ea[0], ea[SEEDS - 1], eb[SEEDS / 2], eb[0], eb[SEEDS - 1],
           en[SEEDS / 2], en[0], en[SEEDS - 1], ma[SEEDS / 2], ma[0], ma[SEEDS - 1]);

    printf("\nB failures: %d -> %s\n", b_fail, b_fail ? "FAIL" : "PASS");
    return b_fail ? 1 : 0;
}
