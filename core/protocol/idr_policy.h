/* idr_policy.h - when to ask the server for a key frame.
 *
 * PURE module: no dependency, no global state, testable offline
 * (tests/test_idr_policy.c). Same intent as vid_wire.c - isolate the rules
 * that each cost a measurement campaign, so nobody rediscovers them the hard
 * way.
 *
 * Why these rules exist: the Shadow server only sends SPS/PPS/IDR
 * spontaneously on the very first session. When we reconnect, it just carries
 * on with the GOP already in flight. The decoder, meanwhile, discards
 * everything until it sees a key frame. So if nobody asks for one, the screen
 * stays black indefinitely while the incoming bitrate looks perfectly normal.
 *
 * The two rules below decide WHEN to ask. Each has a precise, measured
 * counter-case documented on its function: that is what makes a regression
 * readable instead of an unexplained black screen.
 *
 * Created 2026-08-25 (campaign S34).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* --- Rule 1: rate-limit the requests ---------------------------------- */

/* Rate-limiter state. Reset it ON EVERY SESSION: the ticks it compares
 * against restart from zero too. */
typedef struct {
    uint16_t last_tick;   /* tick of the last request sent */
    bool     already_requested;   /* false = no request emitted yet */
} idr_rate_t;

/* Returns true when a request may go out at tick `tick`.
 *
 * `tick` is the video-feedback counter (~one every 50 ms), a u16 that restarts
 * from zero each session and wraps around every ~55 minutes.
 *
 * COUNTER-CASE (S34, 2026-08-25) - the signed form `tick - last >= n`
 * returned false forever as soon as `tick` went backwards: after a 24 s
 * session `last` was ~500, and the next session computed `0 - 500 = -500`.
 * No request was ever sent again. Measured: 18 sessions out of 20 with not a
 * single picture, at a perfectly normal incoming bitrate. Hence the MODULAR
 * 16-bit subtraction, plus the `deja_demande` flag that always lets the very
 * first request through. */
static inline bool idr_rate_allow(const idr_rate_t *r, uint16_t tick,
                                     uint16_t intervalle)
{
    if (!r->already_requested) return true;
    return (uint16_t)(tick - r->last_tick) >= intervalle;
}

/* Record that a request has just gone out at tick `tick`. */
static inline void idr_rate_mark(idr_rate_t *r, uint16_t tick)
{
    r->last_tick  = tick;
    r->already_requested  = true;
}

/* --- Rule 2: detect a decoder that has stopped producing -------------- */

/* Stall-detector state. Reset it ON EVERY SESSION. */
typedef struct {
    uint32_t last_output;  /* frames rendered at the last call */
    uint32_t stalled;         /* frames fed in a row with no output */
} idr_stall_t;

/* Call once per frame FED to the decoder. `rendues` is the count of frames the
 * decoder has actually produced.
 *
 * Returns true when a key frame should be requested.
 *
 * COUNTER-CASE (S34) - the strict equality `++calees == seuil` fired only
 * ONCE: past the threshold the counter kept climbing and the condition was
 * never true again. A single request is enough when the decoder has merely
 * lost its reference frame, but not when it has NEVER started - if that one
 * request is lost, or arrives before the server has registered us, the screen
 * stays black for the whole session. So we REPEAT every `seuil` feeds for as
 * long as nothing comes out. */
static inline bool idr_stall_request(idr_stall_t *s, uint32_t rendues,
                                     uint32_t threshold)
{
    if (rendues != s->last_output) {   /* the decoder is producing: all is well */
        s->last_output = rendues;
        s->stalled = 0;
        return false;
    }
    s->stalled++;
    return threshold != 0 && s->stalled >= threshold && (s->stalled % threshold) == 0;
}
