/* rtt - the REAL round trip to the VM, measured on traffic we already send.
 *
 * === WHY THERE WAS NONE ===
 *
 * The latency report counts twelve stages, and **exactly one contains network**:
 * the spread of a picture's UDP burst, which measures JITTER and loss, never
 * propagation delay. A link at a steady 200 ms reads there exactly like a link
 * at 5 ms. The other eleven stages are local - decode queue, decode, upload,
 * draw rate.
 *
 * Put plainly: this client had never been able to say "your latency to the VM is
 * N ms", which is the first question anyone asks while playing.
 *
 * === WHAT IS MEASURED, AND WHY IT IS HONEST ===
 *
 * The control channel `:base+11` is a request/response channel, and **the server
 * echoes the request's sequence number** in field 1 of its reply (established
 * over 170 replies, KB §3.49). We already send a heartbeat there every 500 ms.
 * So it is enough to timestamp the send and match the reply:
 *
 *   - no invented traffic. This repo is byte-exact by discipline, and sending on
 *     a channel what the server does not expect can kill it (KB §9, S57);
 *   - the real path, the real VM, the real TLS;
 *   - one measurement every 500 ms, for free.
 *
 * === WHAT THIS NUMBER IS NOT ===
 *
 * It is an **application-level TCP+TLS** round trip to the VM. It is NOT the
 * latency of the video path, which is UDP and may behave differently - different
 * queue, different prioritisation. It is the best measurement available without
 * inventing traffic, and the display must say WHICH of the two it is showing
 * rather than writing "latency" full stop.
 */
#ifndef SHADOW_RTT_H
#define SHADOW_RTT_H

#include <stdint.h>
#include <string.h>

/* Enough to cover several seconds of 500 ms heartbeats without the oldest entry
 * being overwritten before its reply arrives. A reply later than that is
 * unusable anyway. */
#define RTT_PENDING 16
#define RTT_KEEP    64   /* samples kept for the percentiles */

typedef struct {
    uint32_t seq[RTT_PENDING];
    int64_t  sent_us[RTT_PENDING];
    int      head;

    uint32_t samples[RTT_KEEP];   /* en microsecondes */
    int      n, next;

    uint32_t last_us, min_us, max_us;
    uint64_t sum_us;
    uint32_t count;
    /* Cadence d'envoi : voir rtt_send_jitter_us. */
    uint32_t send_gap_us[RTT_KEEP];
    int      i_send;
    int      n_send;
    int64_t  last_send_us;
} rtt_t;

static inline void rtt_reset(rtt_t *r)
{
    if (r) memset(r, 0, sizeof *r);
}

/* === OUR OWN CADENCE, AND WHY IT IS MEASURED HERE ======================
 *
 * RTT jitter reads 17 ms on PS Vita against 1 on desktop, and ING-2 already
 * established that the LOOP manufactures part of it: a heartbeat sent late
 * pairs with a reply, and the delay counts as network. The two cannot be
 * separated by asking the server -- there is no common clock.
 *
 * But what is entirely on our side can be measured: the INTERVAL between two
 * sends. It is supposed to be 500 ms every time. If it jitters as much as the
 * RTT, the loop is the cause; if it stays flat while the RTT moves, the
 * network path is. One number that decides between two hypotheses beats two
 * numbers that illustrate them. */
static inline uint32_t rtt_send_jitter_us(const rtt_t *r)
{
    uint64_t s = 0;
    int i, k = 0;
    if (!r || r->n_send < 2) return 0;
    for (i = 1; i < r->n_send; i++) {
        const int64_t d = (int64_t)r->send_gap_us[i] - (int64_t)r->send_gap_us[i - 1];
        s += (uint64_t)(d < 0 ? -d : d);
        k++;
    }
    return k ? (uint32_t)(s / (uint64_t)k) : 0;
}

static inline uint32_t rtt_send_gap_avg_us(const rtt_t *r)
{
    uint64_t s = 0;
    int i;
    if (!r || r->n_send == 0) return 0;
    for (i = 0; i < r->n_send; i++) s += r->send_gap_us[i];
    return (uint32_t)(s / (uint64_t)r->n_send);
}

/* Records the send of a request carrying `seq`. */
static inline void rtt_sent(rtt_t *r, uint32_t seq, int64_t now_us)
{
    if (!r) return;
    /* The interval since the previous send, taken before anything is overwritten. */
    if (r->last_send_us > 0) {
        const int64_t gap = now_us - r->last_send_us;
        if (gap > 0 && gap < 60000000) {          /* resuming a session is not an interval */
            r->send_gap_us[r->i_send] = (uint32_t)gap;
            r->i_send = (r->i_send + 1) % RTT_KEEP;
            if (r->n_send < RTT_KEEP) r->n_send++;
        }
    }
    r->last_send_us = now_us;
    r->seq[r->head]     = seq;
    r->sent_us[r->head] = now_us;
    r->head = (r->head + 1) % RTT_PENDING;
}

/* Matches a reply. Returns the round trip in microseconds, or 0 when that number
 * is not one of ours - which is the normal case: the server also sends
 * unsolicited messages, and counting those as replies would invent round trips
 * of zero. */
static inline uint32_t rtt_reply(rtt_t *r, uint32_t seq, int64_t now_us)
{
    int i;
    if (!r || seq == 0) return 0;
    for (i = 0; i < RTT_PENDING; i++) {
        if (r->seq[i] != seq) continue;
        {
            const int64_t dt = now_us - r->sent_us[i];
            /* A matched entry is cleared: without that, a duplicated reply
              * would produce a second, longer round trip that does not
              * exist. */
            r->seq[i] = 0;
            if (dt <= 0 || dt > 30 * 1000 * 1000) return 0;   /* horloge folle */
            {
                const uint32_t us = (uint32_t)dt;
                r->last_us = us;
                if (!r->count || us < r->min_us) r->min_us = us;
                if (us > r->max_us) r->max_us = us;
                r->sum_us += us;
                r->count++;
                r->samples[r->next] = us;
                r->next = (r->next + 1) % RTT_KEEP;
                if (r->n < RTT_KEEP) r->n++;
                return us;
            }
        }
    }
    return 0;
}

static inline uint32_t rtt_avg_us(const rtt_t *r)
{
    return (r && r->count) ? (uint32_t)(r->sum_us / r->count) : 0;
}

/* A percentile over the kept window. `p` runs from 0 to 100.
 *
 * The p90 matters more than the mean: a good mean with a p90 at 200 ms FEELS
 * bad, and the mean alone hides it - the lesson the latency report already
 * learned (KB, L5). */
static inline uint32_t rtt_pct_us(const rtt_t *r, int p)
{
    uint32_t tmp[RTT_KEEP];
    int i, j, idx;
    if (!r || r->n == 0) return 0;
    memcpy(tmp, r->samples, sizeof(uint32_t) * (size_t)r->n);
    for (i = 1; i < r->n; i++) {           /* tri par insertion, n <= 64 */
        const uint32_t v = tmp[i];
        for (j = i - 1; j >= 0 && tmp[j] > v; j--) tmp[j + 1] = tmp[j];
        tmp[j + 1] = v;
    }
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    idx = (r->n - 1) * p / 100;
    return tmp[idx];
}

/* Jitter: the mean gap between consecutive samples in the window. It says what
 * latency alone does not - a path at a steady 30 ms is playable, a path swinging
 * between 10 and 90 ms is not. */
static inline uint32_t rtt_jitter_us(const rtt_t *r)
{
    uint64_t s = 0;
    int i, k = 0;
    if (!r || r->n < 2) return 0;
    for (i = 1; i < r->n; i++) {
        const int64_t d = (int64_t)r->samples[i] - (int64_t)r->samples[i - 1];
        s += (uint64_t)(d < 0 ? -d : d);
        k++;
    }
    return k ? (uint32_t)(s / (uint64_t)k) : 0;
}

#endif /* SHADOW_RTT_H */
