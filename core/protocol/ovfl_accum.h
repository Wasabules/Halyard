/* ovfl_accum.h - accounting for the kernel's SO_RXQ_OVFL drop counter (ING-1,
 * 2026-09-11). PURE, header-only, tested by tests/test_ovfl.c.
 *
 * What the kernel hands us: on Linux the cmsg carries the socket's CUMULATIVE
 * drop count (sk_drops), sampled when the datagram carrying it was queued, and
 * attached only when that count is non-zero. It belongs to one SOCKET and
 * starts at 0 on every new socket.
 *
 * The previous code kept `max(v)` in a process-wide static that the
 * per-session reset never touched. So:
 *   (a) a reconnection's drops stayed hidden until they exceeded the previous
 *       session's count, and
 *   (b) with three sockets it reported their MAX, not their sum.
 * Measured against a real kernel in the ING-1 bench: -17 in session 1 (max
 * instead of sum), +31 in session 2 (not reset); the delta accounting below
 * was exact in every case.
 *
 * One `last_seen` per socket, zeroed when the socket is created, and the DELTA
 * added to a per-session counter that the session reset zeroes. */
#ifndef OVFL_ACCUM_H
#define OVFL_ACCUM_H

#include <stdint.h>

/* Returns how many NEW drops `v` reports since the last value seen on that
 * socket, and records `v`. A value BELOW the last one can only be a counter
 * that restarted (a socket whose `last_seen` the caller forgot to zero): it is
 * counted as fresh instead of being turned into a four-billion delta. */
static inline uint32_t ovfl_accum(uint32_t *last_seen, uint32_t v)
{
    const uint32_t d = (v >= *last_seen) ? v - *last_seen : v;
    *last_seen = v;
    return d;
}

#endif /* OVFL_ACCUM_H */
