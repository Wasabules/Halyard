/* sockets_compat.c - the portable implementation of the network shims. See the
 * .h for the API. */

#include "sockets_compat.h"

#if defined(_WIN32)

static int g_wsa_started = 0;

int shadow_sockets_init(void) {
    if (g_wsa_started) return 0;
    WSADATA d;
    int rc = WSAStartup(MAKEWORD(2, 2), &d);
    if (rc != 0) return -1;
    g_wsa_started = 1;
    return 0;
}

void shadow_sockets_shutdown(void) {
    if (!g_wsa_started) return;
    WSACleanup();
    g_wsa_started = 0;
}

int shadow_set_nonblocking(int sock, int enable) {
    u_long m = enable ? 1u : 0u;
    return ioctlsocket((SOCKET)sock, FIONBIO, &m) == 0 ? 0 : -1;
}

/* WIN1 2026-09-10 - see the .h: Winsock wants a DWORD of milliseconds, and a
 * `struct timeval` handed over through a cast would compile and lie. */
int shadow_set_sock_timeout(int sock, int which, int timeout_ms) {
    DWORD ms = (DWORD)(timeout_ms < 0 ? 0 : timeout_ms);
    return setsockopt((SOCKET)sock, SOL_SOCKET, which,
                      (const char *)&ms, (int)sizeof(ms)) == 0 ? 0 : -1;
}

#else /* POSIX */

#include <fcntl.h>

/* S81 - this module's category. See services/journal.h: it is declared here,
 * never inferred from the text of the messages. Defined for the WHOLE file: it
 * used to sit inside the Switch branch, which was fine while that was the only
 * branch with anything to say. The Vita's network init has plenty. */
#include "journal.h"
#define sklog(...) JOURNAL_INFO_(JOURNAL_CAT_NETWORK, __VA_ARGS__)
#define skdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_NETWORK, __VA_ARGS__)

#if defined(__SWITCH__)

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>

/* === S35 2026-08-25 - RAISING HOS'S UDP RECEIVE BUFFER ===
 *
 * Measured on console, every session: `SO_RCVBUF set=4194304 actual=42240`.
 * The N56 fix (a 4 MB receive buffer, which had removed the reassembly holes on
 * the desktop) is therefore INEFFECTIVE here: the `bsdsockets` driver refuses to
 * go beyond what its initialisation configuration reserved, and 42,240 is
 * exactly `0xA500`, libnx's default `udp_rx_buf_size`.
 *
 * Borealis initialises the driver in its `userAppInit` (run BEFORE `main`),
 * raising only `num_bsd_sessions` and `sb_efficiency`: the buffer sizes stay at
 * their defaults. Since the library is vendored as is, we do not modify it - we
 * close the driver and reopen it here, at the very start of `main`, before any
 * network I/O at all.
 *
 * Measured consequence of the ceiling: ~0.7 % real UDP loss
 * (`lost=78/11210` over 20 s), which triggers a key frame every ~500 ms
 * through G26. We treat the cause, not the symptom (KB.md §3.30).
 *
 * Caution: if the reopen fails (transfer memory unavailable) we retry with the
 * original configuration, then give up and leave the driver as it is - a
 * degraded network beats no network. `SHADOW_UDP_RXBUF=0` disables the whole
 * manoeuvre. */
int shadow_sockets_init(void) {
    const char *e = getenv("SHADOW_UDP_RXBUF");
    const long requested = e ? strtol(e, NULL, 0) : (1 << 20);   /* 1 MB by default */
    if (requested <= 0) {
        sklog("network: SHADOW_UDP_RXBUF=0 - the UDP buffer is left at the HOS default");
        return 0;
    }

    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    const u32 default_rx = cfg.udp_rx_buf_size;
    cfg.udp_rx_buf_size = (u32)requested;
    cfg.udp_tx_buf_size = 0x9000;   /* 36 KB: our sends are small but bursty */
    cfg.num_bsd_sessions = 12;      /* the same values as Borealis, not to be lost */
    cfg.sb_efficiency    = 8;

    socketExit();                   /* closes the one userAppInit opened */
    Result rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        /* Too greedy: we go back to exactly what Borealis had set. */
        SocketInitConfig secours = *socketGetDefaultInitConfig();
        secours.num_bsd_sessions = 12;
        secours.sb_efficiency    = 8;
        Result rc2 = socketInitialize(&secours);
        sklog("network: UDP buffer %ld B REFUSED (rc=0x%x) - falling back to the default "
                   "%u o (rc=0x%x)%s", requested, rc, default_rx, rc2,
                   R_FAILED(rc2) ? " - FAILED, the network may be unavailable" : "");
        return R_FAILED(rc2) ? -1 : 0;
    }
    sklog("network: the UDP receive buffer is raised to %ld bytes "
               "(HOS default %u) - see KB.md §3.30", requested, default_rx);
    return 0;
}

void shadow_sockets_shutdown(void) {}

#elif defined(__vita__) || defined(__psp2__)

/* === PS VITA: THE NETWORK DOES NOT EXIST UNTIL YOU ASK FOR IT ==============
 *
 * On Linux a socket call just works, and on the Switch libnx's `userAppInit`
 * has already brought the stack up - which is why this function was a `return
 * 0` for everything that was not a Switch. The Vita is neither: `sceNetInit`
 * has to be called, with a memory pool the CALLER owns, before any socket
 * exists at all. Without it the client shows its interface and then fails at
 * the first HTTP request, with an error that says nothing about the cause.
 *
 * Borealis does not do it either - checked, there is no `sceNetInit` anywhere
 * in its PSV platform - so this is the only place it can happen.
 *
 * THE POOL IS STATIC ON PURPOSE. It must outlive every socket, and the Vita has
 * 512 MB: a pool reserved once at startup is cheaper to reason about than a
 * heap allocation that must not be freed while a connection is open.
 * `SHADOW_VITA_NET_POOL` moves its size for a campaign without a rebuild -
 * and, unlike a guess baked into the binary, it makes the number visible in
 * the log.
 *
 * VALIDATED ON HARDWARE since 2026-09-13: the pool size was MEASURED against
 * the video loss rate, not picked. See the comment on `want`.
 */
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <stdlib.h>

static char *g_net_pool = NULL;

int shadow_sockets_init(void) {
    const char *e = getenv("SHADOW_VITA_NET_POOL");
    /* === 8 MB, AND THE NUMBER COMES FROM A MEASUREMENT ================
     *
     * 1 MB was "the size vita homebrew converges on", taken over without ever
     * being checked. Measured on console 2026-09-13, 1280x720 at 60 fps:
     *
     *     1 MB pool  ->  loss 5376/149962 = 3.6 %, a key frame every 1.2 s
     *     8 MB pool  ->  loss  359/64346  = 0.56 %, one every 2.6 s
     *
     * A factor of six. Loss triggers key-frame requests, and a key frame is a
     * burst that loses in its turn: the cycle feeds itself and shows on screen
     * as a periodic micro-freeze with artefacts -- exactly what was reported
     * from the hardware.
     *
     * Worth recording, because it closes a false lead: the per-socket ceiling
     * does NOT move (`SO_RCVBUF ... actual=124800` before and after). So it is
     * not one buffer's size that changes, it is the GLOBAL reserve SceNet
     * draws all its buffers from -- at 1 MB, the session's six sockets were
     * competing for it. */
    long want = e ? strtol(e, NULL, 0) : (8 << 20);   /* 8 MB */
    if (want < (64 * 1024)) want = 64 * 1024;

    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0)
        sklog("network: SCE_SYSMODULE_NET was already loaded, or refused");

    g_net_pool = (char *)malloc((size_t)want);
    if (!g_net_pool) {
        sklog("network: FAILED to reserve the %ld B net pool - no network", want);
        return -1;
    }

    SceNetInitParam p;
    p.memory = g_net_pool;
    p.size   = (int)want;
    p.flags  = 0;
    const int rc = sceNetInit(&p);
    if (rc < 0) {
        /* Already up is not a failure: something else in the process may have
         * brought it, and a second init returns an error we must not mistake
         * for "no network". The `netctl` call below settles it either way. */
        sklog("network: sceNetInit rc=0x%x (already up, or refused)", rc);
    }

    const int rc2 = sceNetCtlInit();
    if (rc2 < 0) {
        sklog("network: sceNetCtlInit FAILED rc=0x%x - the link state is unknown "
              "and connections will probably fail", rc2);
        return -1;
    }
    sklog("network: SceNet up, %ld B pool (SHADOW_VITA_NET_POOL moves it)", want);
    return 0;
}

void shadow_sockets_shutdown(void) {
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    free(g_net_pool);
    g_net_pool = NULL;
}

#else

int shadow_sockets_init(void) { return 0; }
void shadow_sockets_shutdown(void) {}

#endif /* platform */

int shadow_set_nonblocking(int sock, int enable) {
    int fl = fcntl(sock, F_GETFL, 0);
    if (fl < 0) return -1;
    if (enable) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
    return fcntl(sock, F_SETFL, fl) == 0 ? 0 : -1;
}

/* WIN1 2026-09-10 - the POSIX half. Byte for byte what the call sites in
 * `ctrl_input_tcp.c` used to write inline, so Linux and Switch keep exactly the
 * timeouts they had: 2 s on the input channel, 3 s on the probe. */
int shadow_set_sock_timeout(int sock, int which, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(sock, SOL_SOCKET, which, &tv, sizeof(tv)) == 0 ? 0 : -1;
}

#endif
