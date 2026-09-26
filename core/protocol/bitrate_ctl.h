/* bitrate_ctl.h - G19, the adaptive bitrate controller, as a PURE function.
 * No state of its own, no I/O, no getenv, no clock: the session owns the struct
 * and calls bitrate_ctl_tick() once per second from the G19 block of the
 * session loop.
 *
 * === WHY (CFG-1, 2026-09-10) ===
 * G19 and the UI wrote the SAME kUpdateSession f4 value through the same setter,
 * and G19 never learned about the user's choice: its cap was the one frozen at
 * session start (`p->max_bitrate_mbps`, which ignores SHADOW_BITRATE_MBPS too).
 * Measured on the offline replay: cap 100, user drops to 25 at 60 s on a clean
 * link -> the wire is back at 70 at 62 s and at 100 by the end. That undoes the
 * DEBIT-1 workaround (100 breaks in play, 25 holds) while the screen says 25.
 *
 * The rule is now: the user's latest live choice IS the cap. G19 ADOPTS it
 * (cur = cap, no emission - the UI's own message already carries it), may still
 * lower it on loss, and never climbs above it.
 */
#ifndef SHADOW_BITRATE_CTL_H
#define SHADOW_BITRATE_CTL_H

#include <stdint.h>
#include "bitrate.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BITRATE_CTL_START_MAX_MBPS 25u    /* cautious start, unchanged from G19 */
#define BITRATE_CTL_FLOOR_MBPS      8u
#define BITRATE_CTL_LOSS_DOWN   0.008     /* above: x0.75 */
#define BITRATE_CTL_LOSS_CLEAN  0.0015    /* below: a clean window */
#define BITRATE_CTL_CLEAN_WINDOWS   4
#define BITRATE_CTL_STEP_UP_MBPS    3u

typedef enum {
    BITRATE_CTL_EV_NONE = 0,
    BITRATE_CTL_EV_START,       /* first tick: min(cap, 25) */
    BITRATE_CTL_EV_DOWN,        /* "[G19] perte ... -> debit abaisse a" */
    BITRATE_CTL_EV_UP,          /* "[G19] stable -> debit remonte a" */
    BITRATE_CTL_EV_ADOPT,       /* the user chose: cur = cap, nothing emitted */
    BITRATE_CTL_EV_RESYNC       /* the reported wire disagreed with cur: re-sent */
} bitrate_ctl_event_t;

typedef struct {
    uint32_t cur;        /* current request, Mbps; 0 = not started yet */
    int      good;       /* consecutive clean 1-s windows */
    uint32_t base_cap;   /* resolved like the announcement: settings, then SHADOW_BITRATE_MBPS */
    uint32_t user_mbps;  /* last live choice this session, 0 = none */
    unsigned seen_gen;   /* last UI generation adopted */
    uint32_t wire;       /* last value the session REPORTED on the wire, 0 = never reported */
    bitrate_ctl_event_t event;   /* what the last tick did, for the log line */
} bitrate_ctl_t;

/* The session cap, resolved exactly like the announcement
 * (session_announce_channels): the settings value, then SHADOW_BITRATE_MBPS on top, else the
 * client default. G19 used `p->max_bitrate_mbps` alone, so an env.txt cap was
 * obeyed by the announcement and the ready-time f13, then climbed over by G19
 * from the fourth second on (CFG-1, case E). */
static inline uint32_t bitrate_ctl_base_cap(uint32_t settings_mbps, uint32_t env_mbps)
{
    if (env_mbps) return env_mbps;
    return settings_mbps ? settings_mbps : BITRATE_CLIENT_DEFAULT_MBPS;
}

/* `base_cap_mbps` is `vparams.max_bitrate_bps / 1000000u`: the value the
 * announcement actually carried. 0 falls back on the client default. */
static inline void bitrate_ctl_init(bitrate_ctl_t *st, uint32_t base_cap_mbps)
{
    st->cur = 0; st->good = 0;
    st->base_cap = base_cap_mbps ? base_cap_mbps : BITRATE_CLIENT_DEFAULT_MBPS;
    st->user_mbps = 0; st->seen_gen = 0; st->wire = 0;
    st->event = BITRATE_CTL_EV_NONE;
}

/* OPTIONAL: the session reports every f13 it actually sent (the UI's pending
 * request and the ready-time f13). Once it does, G19 re-sends
 * its value whenever the wire disagrees with it. That closes the two ways the
 * wire drifts from G19's belief under the plain adopt rule: the ready-time f13
 * landing AFTER a choice made in the first 3 s, and a UI write landing INSIDE
 * G19's tick when both share one pending slot. Never called = plain adopt. */
static inline void bitrate_ctl_on_wire(bitrate_ctl_t *st, uint32_t mbps)
{
    st->wire = mbps;
}

/* The loss of one window. No chunk expected (TCP video, K15: no SUFP chunks at
 * all) is NOT a loss - and it is not a licence to climb above the user either. */
static inline double bitrate_ctl_loss(uint32_t d_exp, uint32_t d_mis)
{
    return d_exp ? (double)d_mis / (double)d_exp : 0.0;
}

static inline uint32_t bitrate_ctl_cap(const bitrate_ctl_t *st)
{
    return st->user_mbps ? st->user_mbps : st->base_cap;
}

/* One 1-s window. Returns the Mbps to emit, or 0 for "emit nothing".
 * `user_mbps`/`user_gen` are what ctrl_session_set_video_config stored: the
 * generation moves only when the UI sent a bitrate (mbps != 0). A generation
 * change carrying 0 is read as "fps only" and leaves the cap alone - checked
 * here as well, so a caller that forgets the guard cannot reopen CFG-1 (F). */
static inline uint32_t bitrate_ctl_tick(bitrate_ctl_t *st, double loss,
                                        uint32_t user_mbps, unsigned user_gen)
{
    uint32_t cap, emit = 0;
    st->event = BITRATE_CTL_EV_NONE;

    if (user_gen != st->seen_gen) {
        st->seen_gen = user_gen;
        if (user_mbps) {
            st->user_mbps = user_mbps;
            st->cur  = user_mbps;     /* ADOPT, up or down */
            st->good = 0;
            st->event = BITRATE_CTL_EV_ADOPT;
            /* the UI's pending message carries it - unless the session
             * reported a wire that says otherwise */
            return (st->wire && st->wire != st->cur) ? st->cur : 0;
        }
    }

    cap = bitrate_ctl_cap(st);
    if (st->cur == 0) {
        st->cur = cap < BITRATE_CTL_START_MAX_MBPS ? cap : BITRATE_CTL_START_MAX_MBPS;
        emit = st->cur;
        st->event = BITRATE_CTL_EV_START;
    }
    if (loss > BITRATE_CTL_LOSS_DOWN && st->cur > BITRATE_CTL_FLOOR_MBPS) {
        uint32_t n = (uint32_t)(st->cur * 0.75);
        st->cur = n < BITRATE_CTL_FLOOR_MBPS ? BITRATE_CTL_FLOOR_MBPS : n;
        st->good = 0;
        emit = st->cur;
        st->event = BITRATE_CTL_EV_DOWN;
    } else if (loss < BITRATE_CTL_LOSS_CLEAN) {
        if (++st->good >= BITRATE_CTL_CLEAN_WINDOWS && st->cur < cap) {
            uint32_t n = st->cur + BITRATE_CTL_STEP_UP_MBPS;
            st->good = 0;
            st->cur = n > cap ? cap : n;
            emit = st->cur;
            st->event = BITRATE_CTL_EV_UP;
        }
    } else {
        st->good = 0;
    }
    if (!emit && st->wire && st->cur && st->wire != st->cur) {
        emit = st->cur;
        st->event = BITRATE_CTL_EV_RESYNC;
    }
    return emit;
}

#ifdef __cplusplus
}
#endif

#endif /* SHADOW_BITRATE_CTL_H */
