/* netpath - where the latency actually is: this machine, or the way out.
 *
 * === NET1 2026-10-03 — WHY SPLIT THE ROUND TRIP AT ALL ====================
 *
 * A session reports one number, the control channel's round trip, and that
 * number cannot answer the only question a person asks when a stream goes
 * soft: is it me, or is it them. 60 ms of which 45 are between the desk and
 * the router is a Wi-Fi problem, and 60 ms of which 3 are local is a problem
 * nobody at this end can fix. The official client draws exactly this, as
 * "+2 ms" to the box and "+18 ms" to Shadow.
 *
 * Only the LOCAL hop is measured. The rest is the session's own round trip
 * minus that, which is not a traceroute and does not pretend to be: every
 * intermediate hop stays lumped together under "the way out", because a
 * per-hop trace needs raw sockets, takes seconds, and answers a question
 * nobody asked.
 *
 * === WHAT THIS DOES NOT NEED =============================================
 *
 * No administrator, and no raw socket:
 *
 *   - on Windows, `IcmpSendEcho` from iphlpapi (already linked for
 *     `GetAdaptersAddresses`) is a documented unprivileged API;
 *   - on Linux, a `SOCK_DGRAM`/`IPPROTO_ICMP` socket is unprivileged when the
 *     caller's group is inside `net.ipv4.ping_group_range`, which most
 *     distributions now set to cover ordinary users.
 *
 * When neither works - a locked-down kernel, a gateway that drops ICMP, a VPN
 * with no gateway of its own - the call fails and says so. It does NOT fall
 * back to a guess. A made-up local hop would be worse than no split at all:
 * the whole value of the number is that it tells someone where to look.
 *
 * Every function here is BLOCKING and must not be called from a thread that
 * matters. A ping waits for a timeout it may never beat.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The default gateway's address, as text, into `out` (`cap` >= 46 for IPv6).
 * False when there is no default route, or none this platform can report. */
bool netpath_default_gateway(char *out, int cap);

/* One ICMP echo to `host`, waiting at most `timeout_ms`.
 *
 * Returns the round trip in microseconds, or -1 when the probe failed for any
 * reason (no reply, ICMP refused, no permission). The caller cannot tell those
 * apart on purpose: the display says "unknown" for all of them, and a UI that
 * distinguished "filtered" from "timed out" would be reporting on our own
 * plumbing rather than on the network. */
int64_t netpath_ping_us(const char *host, int timeout_ms);

/* === THE SPLIT ===========================================================
 *
 * `local_us` is the probe to the gateway; `total_us` is the session's own
 * round trip, passed in rather than measured here (this layer knows nothing
 * about a session). `remote_us` is what is left.
 *
 * `remote_us` is CLAMPED AT ZERO and `ordered` says whether it had to be. A
 * local hop larger than the total is not impossible noise, it is routine: the
 * two are measured at different moments by different mechanisms, and a 3 ms
 * gateway that answers slowly once will exceed a 2 ms total. Reporting a
 * negative remote hop would be nonsense; reporting it silently as zero would
 * be a lie by omission, so the flag lets the caller grey the figure out. */
typedef struct {
    int64_t local_us;    /* this machine to the default gateway */
    int64_t remote_us;   /* the gateway onward, by subtraction */
    int64_t total_us;    /* what was passed in */
    bool    have_local;  /* the gateway probe succeeded */
    bool    ordered;     /* local <= total, so remote_us means something */
    char    gateway[46];
} netpath_split;

/* The ARITHMETIC of the split, with no I/O: `local_us` < 0 means the gateway
 * probe failed or was never run. Split out from `netpath_measure` so the part
 * that has the edge cases - a clamp, an unknown hop, a total that is itself
 * unknown - can be exercised by the offline suite, which cannot ping. */
void netpath_split_compute(int64_t total_us, int64_t local_us, netpath_split *out);

/* Fills `out`. Returns false only when nothing at all could be measured, in
 * which case `total_us` is still set and the caller can show that alone. */
bool netpath_measure(int64_t total_us, int timeout_ms, netpath_split *out);

#ifdef __cplusplus
}
#endif
