/* ctrl_session.c - implementation. See ctrl_session.h. */

#include <errno.h>
/* pthread.h BEFORE any wolfSSL header: `wolfssl/options.h` does
 * `#undef _POSIX_THREADS`, and newlib gates half of pthread.h behind that macro
 * (mutex, create, join - the rwlocks stay visible, hence an error message that
 * unhelpfully suggests `pthread_rwlock_unlock`). `ctrl_audio_dtls.h` below pulls
 * wolfSSL in, so an `#include <pthread.h>` placed after it builds on Linux and
 * on Windows and FAILS only for the Switch - which is where it sat until
 * 2026-09-12, because no loop builds the console. Same note as `ctrl_tcp.c`,
 * `ctrl_video_tcp.c` and `ctrl_input_tcp.c`; this is the fourth file to pay it. */
#include <pthread.h>
#include "ctrl_session.h"
#include "bitrate.h"    /* B1: the one bitrate ladder and its defaults */
#include "bitrate_ctl.h"   /* CFG-1: G19 as a pure function, and the user's cap */
#include "gamepad_wire.h"
#include "rumble_state.h"
#include "ctrl_tcp.h"
#include "ctrl_video_tcp.h"
#include "ctrl_input_tcp.h"  /* I1 2026-05-18 */
#include "ctrl_audio_dtls.h" /* I2 2026-05-18 */
#include "ctrl_comchan.h"   /* V15 2026-05-16 — :base+14 lifecycle bus + focus events */
#include "ctrl_gamepad.h"   /* the gamepad on :base+13 (KB §3.25) */
#include "native_input.h"    /* I1 phase 3 2026-05-18 */
#include "ctrl_msgs.h"
#include "session_caps.h"   /* INT1: the public grant snapshot */
#include "vid_uplink.h"   /* SRV1/SRV3: the uplink rules, pure and tested */
#include "../services/filetransfer.h"   /* FT2: the SFTP self-test */
#include "proto.h"                       /* FT2: reply-shape diagnostic */
#include "encryption.h"
#include "sufp.h"
#include "smoke_test.h"   /* jwt_instance */
#include "../common/log.h"
#include "../common/stats.h"
#include "../services/sockets_compat.h"

#ifndef _WIN32
#include <poll.h>   /* L6: wait on the packets rather than on the clock */
#endif
/* WIN1 2026-09-10 - on Windows `poll` comes from sockets_compat.h just above
 * (WSAPoll under its POSIX name). There is no <poll.h> to include. */
#include "ctrl_session_int.h"   /* types de contexte partages */
#include "vid_reasm.h"
#include "ann_reply.h"
#include "rtt.h"
#include "latency.h"   /* L5: the per-stage latency report */
#include "ovfl_accum.h"   /* ING-1: kernel drop accounting, per socket */
#include "session_host.h"   /* DNS1: one lookup of the VM name per session */
#include "../services/log_mask.h"   /* SEC2: secrets in the log, start and end only */
#include "../services/local_clipboard.h"   /* CLIP3: the clipboard of this machine */
#include "../services/config.h"   /* FT3: SHADOW_DATA_DIR */
#include "ft_uri.h"   /* FT4: the SFTP session as a clickable URI */
#include "clip_dir.h"   /* CLIP6: which way the clipboard may travel */
#include "idr_policy.h"
#include "cursor_wire.h"
#include "audio_route.h"   /* DEC-1: which :base+30 plaintexts are audio */
#include "aud_reasm.h"     /* ING-A2: split audio frames, and their session state */
#include "cursor_state.h"          /* reassemblage video (extrait 2026-08-25) */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* S81 - the category is DECLARED here, not inferred from the message text.
 * `clog` stays at INFO: the existing calls do not disappear. `cdbg` is there
 * for the bulky lines, which move over to it one at a time. */
#define clog(...) JOURNAL_INFO_(JOURNAL_CAT_SESSION, __VA_ARGS__)
#define cdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SESSION, __VA_ARGS__)
/* ============================================================================
 * Helpers
 * ==========================================================================*/

static void emit_progress(const ctrl_session_params *p,
                            const char *step, const char *detail) {
    clog("[ctrl_session] %s — %s", step, detail ? detail : "");
    if (p->on_progress) p->on_progress(step, detail, p->user);
}

/* gen_uuid_v4 lives in smoke_test.c (a static helper, duplicated here because
 * it is not exposed in a header). Staying DRY would mean extracting it into a
 * shared helper, but the copy is tiny here. */
static bool gen_uuid_v4(char out[37]) {
    static uint32_t seed = 0;
    if (!seed) {
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
        seed = (uint32_t)(t.tv_nsec ^ t.tv_sec);
        srand(seed);
    }
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)(rand() & 0xFF);
    b[6] = (b[6] & 0x0F) | 0x40;       /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80;       /* variant 10 */
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
             b[10],b[11],b[12],b[13],b[14],b[15]);
    return true;
}

/* Setup UDP socket connected to vm_host:port. Sends a 25B "register" packet
 * `[0x41 0x01 0x00 0x14 0x00][hash 20B]` for NAT pinhole + server-side
 * client identification. Sets non-blocking mode for recv loop. Returns -1
 * on any failure. */
/* D1 2026-08-21 - handle on the TCP video channel, so the developer menu bar
 * can request a picture refresh (the 0x64 byte the official client sends on
 * resize, see KB §3.21). */
static ctrl_video_tcp_t *g_active_vst = NULL;
/* AF8 2026-09-10 - guards the READ-then-CALL in `ctrl_session_request_refresh`
 * against the teardown clearing the pointer and freeing the block. The call it
 * protects only raises a flag, so no I/O ever runs under this lock (rule 3). */
static pthread_mutex_t   g_active_vst_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Bitrate requested from the UI, waiting to be sent. 0 = nothing to do.
 *
 * We do NOT send it from the calling thread: the control channel is a numbered
 * stream, and emitting outside the loop would desynchronise the sequence. So we
 * drop the value here and the loop ships it with the right number. That is what
 * the official client does: it applies the bitrate live (kUpdateSession f13)
 * without restarting the session - see KB §3.24. */
/* True as long as the session loop is running: the UI uses it to say whether a
 * setting actually goes anywhere, rather than letting the user believe it was
 * applied while nothing is listening. */
static volatile int g_session_active = 0;

/* Fingerprint of the last audio frames played, to check whether the second copy
 * of a frame really is identical to the first. */
static struct { uint32_t seq; int len; uint32_t sum; } g_aud_seen[16];

bool ctrl_session_active(void) { return g_session_active != 0; }

/* The UI's live bitrate, Mbps, waiting for the session loop. The setter already
 * resolved SHADOW_VIDEO_CFG_DEFAULT into the client default; 0 = none pending. */
static volatile uint32_t g_pending_bitrate_mbps = 0;
static volatile int      g_pending_video_cfg    = 0;
/* === CFG-1 2026-09-11 - THE USER'S LIVE CHOICE, FOR G19 ===
 * One packed word, (generation << 16) | Mbps, written by the UI thread and read
 * once a second by the session: two separate volatiles could be read torn. The
 * generation moves only when a bitrate is actually chosen. G19 used to write
 * the very same value through the very same setter, and never learned what the
 * user had picked - see bitrate_ctl.h. */
static volatile uint32_t g_user_word = 0;
/* CFG-1 - the cap the announcement carried (settings, then SHADOW_BITRATE_MBPS),
 * set by session_announce_channels() before the receive loop starts. G19 used
 * `p->max_bitrate_mbps` alone, so an env.txt cap was obeyed by the
 * announcement, then climbed over by G19 from the fourth second on. */
static uint32_t g_announced_cap_mbps = 0;

/* INT1 2026-10-02 - the public grant snapshot (session_caps.h) and the
 * file-transfer credential kept beside it, never inside it. Declared here,
 * with the other module state, because the Capabilities reply writes the
 * server version into it during the handshake - earlier in this file than
 * the functions that fill the rest. */
static shadow_session_caps g_caps;
static char                g_ft_secret[1024];
static size_t              g_ft_secret_len;
/* FT4: the VM address this session is talking to. The snapshot carries the
 * port but not the host, because every channel in core already had it on hand
 * from `ctrl_session_params`; a caller OUTSIDE core does not, and a URI needs
 * it. Kept beside the secret rather than in the snapshot for the same reason
 * the secret is: INT1's rule is that the public struct holds nothing a crash
 * dump should not. (A host name is not a credential - but it is the half that
 * makes one usable, and the two now have one lifetime.) */
static char                g_ft_host[256];

void ctrl_session_set_video_config(uint32_t mbps)
{
    /* B1: the sentinel is resolved HERE, not stored, so the consumer below has
     * only two cases to read instead of three. CFG-4: 0 now means "nothing to
     * send" - the frame-rate-only calls it served have no field on the wire. */
    if (mbps == SHADOW_VIDEO_CFG_DEFAULT) mbps = BITRATE_CLIENT_DEFAULT_MBPS;
    if (!mbps) return;
    g_pending_bitrate_mbps = mbps;
    g_pending_video_cfg = 1;
    const uint32_t gen = ((g_user_word >> 16) + 1u) & 0xFFFFu;
    g_user_word = (gen << 16) | (mbps & 0xFFFFu);

    /* === CFG-5 2026-09-12 - THE REQUEST IS LOGGED, NOT ONLY THE SEND ===
     * The session loop already logs `[UI] video demandee ... src=ui` when the
     * message actually leaves, which is the right place for the wire. But it
     * says nothing when there is no loop to ship it, and nothing at all when
     * this function is never reached.
     *
     * That gap cost a diagnosis: asked to check the reported "the video
     * settings do not seem to apply properly", two live logs held `[UI] video
     * demandee` lines that were ALL `src=g19` - and there was no way to tell
     * whether the user had pressed and we dropped it, or never pressed at all
     * in those sessions. A press now leaves a trace of its own, before the loop
     * is involved. */
    clog("[UI] bitrate chosen by the user: %u Mbps, waiting for the loop", mbps);
}


/* G13 - key frame request from outside. The only reliable signal that one is
 * needed is the decoder itself: when it stops producing pictures while we keep
 * feeding it, it has lost its reference frame and only a key frame will restart
 * it. */
void ctrl_session_request_idr(void)
{
    g_idr_needed = 1;
}

void ctrl_session_set_bitrate(uint32_t mbps)
{
    /* The UI's alias. G19 no longer goes through here (CFG-1): its emissions
     * are its own, and must not count as the user's choice. */
    ctrl_session_set_video_config(mbps);
}

void ctrl_session_request_refresh(void) {
    /* AF8 - reached from the UI thread ("Rafraichir l'image" in the pause
     * menu, the dev menu), while the stream view stays on screen for the whole
     * teardown. */
    pthread_mutex_lock(&g_active_vst_mtx);
    if (g_active_vst) ctrl_video_tcp_request_refresh(g_active_vst);
    pthread_mutex_unlock(&g_active_vst_mtx);
}

/* Set by the UI before the session; see ctrl_session.h. */
static int g_reg_input_pushed = 0;
void ctrl_session_set_udp_register_input(int on) { g_reg_input_pushed = on ? 1 : 0; }

/* SHADOW_FORCE_IPV4=1 - use IPv4 only, like the Switch does.
 *
 * The console has no usable IPv6 (the HOS resolver is unreliable on AAAA),
 * while a Linux desktop picks IPv6 first with AF_UNSPEC. The two platforms
 * therefore do NOT take the same network path, and a defect that only happens
 * on one of them used to be impossible to reproduce on the other. This toggle
 * makes the experiment possible without a console. */
static int shadow_force_ipv4(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("SHADOW_FORCE_IPV4"); on = e ? atoi(e) : 0; }
    return on;
}

/* === S44 2026-08-25 - A CONNECTED SOCKET DISCARDS WHAT COMES FROM ELSEWHERE ===
 *
 * `connect()` on a UDP socket filters: the kernel delivers ONLY the datagrams
 * whose source address AND port match the peer exactly. Everything else is
 * dropped without a word - no error, no counter, no log line.
 *
 * That is the only mechanism consistent with everything we measured on the
 * `:base+30` channel: our eight announcements are byte-for-byte identical to
 * the official client's (audio included), our bootstrap can be made identical,
 * our traffic on that port is the same, the socket was recreated on a fresh
 * source port - and still nothing arrives. If the server sends that stream from
 * a source port other than `:base+30`, we cannot see it.
 *
 * A `disassociate()` call (connect with AF_UNSPEC) dissolves the association:
 * the socket then accepts any source. We keep the peer around so we can still
 * send with `sendto`. Applied to the cursor/audio channel only - video works,
 * we leave it alone.
 *
 * `SHADOW_CURSOR_ANYSRC=0` goes back to the connected socket. */
static struct sockaddr_storage g_cursor_peer;
static socklen_t               g_cursor_peer_len = 0;

/* A `disassociate()` function (connect with AF_UNSPEC) used to live here: HOS
 * refuses it with errno 106. So we never associate at all, rather than undoing
 * an association afterwards. */

/* Sends on the cursor channel, whether the socket is associated or not. */
static ssize_t cursor_send(int s, const void *buf, size_t n)
{
    if (g_cursor_peer_len > 0)
        return sendto(s, (const char *)buf, n, 0,
                      (struct sockaddr *)&g_cursor_peer, g_cursor_peer_len);
    return send(s, (const char *)buf, n, 0);
}

/* === ING-1 2026-09-11 - WHERE THE KERNEL DROP COUNTER EXISTS AT ALL ===
 *
 * SO_RXQ_OVFL is Linux-only: Windows has no such option, and libnx's socket
 * headers do not define it (checked over its 413 files). So `kernel=` - printed
 * in every stats line and painted green by the link page - was a structural 0
 * on the console and on Windows, and KB entries cited "kernel=0" from console
 * sessions as evidence. On Linux it was wrong another way: the MAX of three
 * sockets' cumulative counts, never reset between sessions. Now it is counted
 * only where it can be, per socket as a delta, summed per session; elsewhere
 * the stats line prints kernel=n/a and the link page hides the row. */
#if defined(SO_RXQ_OVFL) && !defined(_WIN32)
#  define KDROPS_MEASURED 1
#else
#  define KDROPS_MEASURED 0
#endif
static int g_kdrops_ok = 0;   /* this platform counts them: a capability, not session state */

/* === VI1 2026-09-11 - SHADOW_DIAG_VIDEO_MUTE_AT_S / _MS: AN OUTAGE ON DEMAND ===
 * DIAGNOSTIC, OFF by default. From AT seconds after the receive loop starts
 * and for MS milliseconds (default 8000), video datagrams are read and thrown
 * away before reassembly. The session then sees exactly what a §3.34 outage
 * looks like - D4, S41c, the frozen-picture banner, the silent [L5] stages -
 * without waiting for a real one, and on console through env.txt. It can only
 * REMOVE packets, never add one; UDP video only (TCP video is untouched).
 * Process configuration, read once. */
static int      g_diag_mute_at_s = -2;   /* -2 unread, <= 0 off */
static int      g_diag_mute_ms   = 8000;
static unsigned g_diag_muted     = 0;    /* datagrams thrown away */

static void diag_discard_packet(const uint8_t *pkt, size_t len, void *user)
{
    (void)pkt; (void)len; (void)user;
    g_diag_muted++;
}

/* `associate` at 0: the socket is NOT connected, so it accepts datagrams from
 * any source and sends with `sendto`. Needed for the cursor/audio channel - see
 * S44. On HOS, undoing an association after the fact is refused (errno 106), so
 * the only option is never to make one. */
static int udp_register_ex(const char *vm_host, int port, const uint8_t hash[20],
                           int associer);

static int udp_register(const char *vm_host, int port, const uint8_t hash[20]) {
    return udp_register_ex(vm_host, port, hash, 1);
}

static int udp_register_ex(const char *vm_host, int port, const uint8_t hash[20],
                           int associer) {
    if (port < 1 || port > 65535) {
        clog("udp_register: port %d hors plage", port);
        return -1;
    }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = shadow_force_ipv4() ? AF_INET : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    /* 16 bytes: a %d on an int can write 11 of them ("-2147483648") even though
     * a valid port is 5 at most - the bound above guarantees that, and the
     * buffer no longer has to depend on it. */
    char port_s[16]; snprintf(port_s, sizeof(port_s), "%d", port);
    int gai = session_getaddrinfo(vm_host, port_s, &hints, &res);   /* DNS1 */
    if (gai != 0 || !res) {
        clog("udp_register: getaddrinfo(%s:%d) FAIL gai=%d", vm_host, port, gai);
        return -1;
    }
    int s = socket(res->ai_family, SOCK_DGRAM, 0);
    if (s < 0) {
        clog("udp_register: socket :%d FAIL errno=%d", port, shadow_sock_errno());
        freeaddrinfo(res); return -1;
    }
    /* N56 2026-05-18 - raise the UDP socket's SO_RCVBUF to absorb bursts.
     * Root cause of `first_hole_idx=0` in 58% of cases: the Linux kernel default
     * rmem is 208 KB, and at 1080p / 1.2 MB/s the buffer fills in 170 ms. If the
     * recv thread is blocked (libavcodec decode, GUI render), packets are
     * dropped in FIFO order -> the first chunks of the burst are lost = chunk 0
     * systematically missing.
     * 4 MB gives ~3 seconds of buffer before dropping. Linux silently clips to
     * `net.core.rmem_max` if the value is too large - check with getsockopt.
     * Override with the `SHADOW_UDP_RCVBUF` env var (in bytes). */
    {
        int rcvbuf = 4 * 1024 * 1024;
        const char *e = getenv("SHADOW_UDP_RCVBUF");
        if (e) { int v = atoi(e); if (v > 0) rcvbuf = v; }
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
#if KDROPS_MEASURED
        /* Ask the kernel to TELL us how many packets it drops for lack of room
         * in the receive queue. Without that counter, a local loss is
         * indistinguishable from a network loss - and we have spent hours
         * hunting a protocol defect for what may be nothing but a loop that is
         * too slow. */
        int ovfl_on = 1;
        if (setsockopt(s, SOL_SOCKET, SO_RXQ_OVFL, (const char *)&ovfl_on, sizeof(ovfl_on)) == 0)
            g_kdrops_ok = 1;   /* ING-1 */
#endif
        int actual = 0; socklen_t alen = sizeof(actual);
        if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&actual, &alen) == 0) {
            /* The value READ BACK is the only one that counts, and it does not
             * read the same way on every platform: Linux returns TWICE what was
             * asked for (internal accounting), HOS returns the real value and
             * silently caps it to whatever the socket driver reserved. That is
             * how S35 was found: 4 MB requested, 42,240 obtained on the console
             * (KB.md §3.30). Never assume the request succeeded - read it back,
             * and say so. */
#if defined(__SWITCH__)
            const char *reading = "real value, capped by the socket driver";
#elif defined(_WIN32)
            const char *reading = "valeur exacte (winsock)";   /* ING-1 */
#else
            const char *reading = "linux returns twice what was asked for";
#endif
            clog("udp_register: :%d SO_RCVBUF set=%d actual=%d (%s)",
                 port, rcvbuf, actual, reading);
        }
    }
#if defined(_WIN32)
    /* Windows winsock quirk: if we send() to a port that is not yet ready
     * server-side, Windows receives an ICMP "port unreachable" and BLOCKS future
     * recv() calls with WSAECONNRESET. SIO_UDP_CONNRESET=FALSE disables that
     * behaviour (the socket ignores ICMP unreach, like Linux). Without it -> 0
     * video frames on Windows.
     * The define is not always exposed on MinGW depending on WIN32_WINNT, so we
     * use the magic value directly (= _WSAIOW(IOC_VENDOR,12)). */
#  ifndef SIO_UDP_CONNRESET
#    define SIO_UDP_CONNRESET 0x9800000C
#  endif
    BOOL false_val = FALSE;
    DWORD bytes_returned = 0;
    if (WSAIoctl((SOCKET)s, SIO_UDP_CONNRESET, &false_val, sizeof(false_val),
                  NULL, 0, &bytes_returned, NULL, NULL) == SOCKET_ERROR) {
        clog("udp_register: SIO_UDP_CONNRESET :%d FAIL errno=%d (non-fatal)",
             port, shadow_sock_errno());
    }
#endif
    /* S44: we keep the cursor channel's peer around so we can send with
     * `sendto` once the socket has been disassociated. */
    if (port % 1000 == 30 && res->ai_addrlen <= sizeof(g_cursor_peer)) {
        memcpy(&g_cursor_peer, res->ai_addr, res->ai_addrlen);
        g_cursor_peer_len = res->ai_addrlen;
    }
    if (associer && connect(s, res->ai_addr, res->ai_addrlen) < 0) {
        clog("udp_register: connect :%d FAIL errno=%d", port, shadow_sock_errno());
        shadow_closesocket(s); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    /* K12: `hash == NULL` => we only want the socket, with no cleartext
     * registration. The official client sends one on NO port other than
     * :base+10 and :base+30; on :base+13 it only emits encrypted packets. */
    if (!hash) {
        shadow_set_nonblocking(s, 1);
        clog("udp_register: :%d socket only (no plaintext register)", port);
        return s;
    }
    uint8_t reg[32];
    int reg_len = ctrl_build_udp_register(reg, sizeof(reg), hash);
    if (reg_len < 0) { shadow_closesocket(s); return -1; }
    ssize_t sent = associer ? send(s, reg, reg_len, 0)
                            : sendto(s, (const char *)reg, (size_t)reg_len, 0,
                                     (struct sockaddr *)&g_cursor_peer, g_cursor_peer_len);
    if (sent != reg_len) {
        clog("udp_register: send :%d FAIL sent=%zd want=%d errno=%d",
             port, sent, reg_len, shadow_sock_errno());
        shadow_closesocket(s); return -1;
    }
    shadow_set_nonblocking(s, 1);
    clog("udp_register: :%d OK reg=%dB sent=%zd", port, reg_len, sent);
    return s;
}

/* Drains a non-blocking UDP socket: calls handle() for every packet received
 * until exhaustion. Returns the number of packets drained. */
/* Number of packets the KERNEL dropped for lack of room, as it reports them
 * itself. Filled in by udp_drain through SO_RXQ_OVFL. */
static uint32_t g_kernel_drops = 0;
/* L20 - the highest frame rate seen over ONE second since the session started.
 * It lives HERE and not in a function `static`: this is SESSION state, the
 * defect family that has already produced four failures in this repo. It is
 * reset along with everything else when a session starts. */
static double   g_crete_img_s = 0.0;
static uint32_t g_crete_img   = 0;   /* L21: peak milestones, SESSION state */
static int      g_crete_sec   = -1;

/* === G53 2026-08-28 - THE GAMEPAD'S DOWNSTREAM PATH, READ AT LAST ===
 *
 * `udp_input` (:base+13) had ONLY `send()` calls. Yet the server answers on it,
 * and its answers carry exactly what cost two campaigns (G49 then G50):
 *
 *   - `04 00 08 03 ... [11]=01` = kReply to our kPlug -> the announcement
 *     SUCCEEDED, and the VM assigned a device id. The official telemetry shows
 *     the transition `gamepad 1 - 255 - UNDEFINED -> PLUGGING` then
 *     `gamepad 1 - 0 - PLUGGING -> PLUGGED`: 255 means "not assigned yet", and
 *     ONLY the reply resolves it. Without reading it, our gamepad stays
 *     indefinitely in the equivalent of PLUGGING.
 *   - `04 00 08 04 ... [11]=00` = kReply to a kUnplug.
 *   - `04 00 07 ...` = kVibration, which we cannot drive yet.
 *
 * The server sends EACH reply three times, ~10.5 ms apart: reading it must
 * therefore be idempotent, and here we only log.
 *
 * The envelope is bare - `[ct 14][nonce 12][tag 16]`, 42 bytes, with NO SUFP
 * header, unlike video and audio.
 *
 * We decrypt with the variant that has NO replay protection: the cipher is
 * SHARED with video, and `:base+13` has its own nonce space - turning replay
 * protection on would make it reject video frames. */
/* === G53c 2026-08-28 - THESE COUNTERS ARE SESSION STATE ===
 * They used to live in function `static`s. The second session of the same
 * launch would therefore have found them at their cap, and the gamepad channel
 * would have looked SILENT while it was talking - precisely the family of
 * defects this repo has paid for four times (black screen from the 3rd session
 * on, sound audible only once, silent hardware detectors). Reset per session. */
/* D4: bridge to device_mode.cpp (C++). The headless binary does not carry that
 * layer: it provides a stub in main_test_stubs.c. */
int shadow_link_info(int *docked, int *strength);

/* K15 - is video requested over TCP? One single read point: the toggle drives
 * both the REQUEST (field f3 of the announcement, ctrl_msgs.c) and the RECEIVE
 * side (here). Splitting them would give a session with no picture. */
/* K15n: arms a key frame request to be sent on the CONTROL channel. In TCP mode
 * that is the only route: `iP` goes out on the video UDP socket, which no
 * longer exists. Without it the server only sends non-IDR slices and the
 * decoder never starts - measured: 1725 frames fed, zero output, `idr_t=0`. */
static volatile int g_flush_tcp_needed = 0;

/* === B4 2026-09-02 - RESOLVED ONCE PER SESSION, NOT ONCE PER PROCESS ===
 *
 * This was a function `static` filled on the first call, so the value was frozen
 * for the life of the application. That was invisible while the toggle existed
 * only as an environment variable - `env.txt` is read before `main` and never
 * changes. It stops being invisible the moment a SETTING drives it: change the
 * transport, reconnect, and the second session would silently keep the first
 * one's. That is the defect family CLAUDE.md names first, and `ctrl_session.c`
 * already documents the identical fix three hundred lines below for
 * `SHADOW_UDP_REG_13`.
 *
 * Per SESSION is the right granularity, not per call: the transport is decided
 * in the channel announcement at bootstrap and cannot change while the session
 * runs. `session_resolve_transport` is called once, at the top of
 * `ctrl_session_run`. */
static int g_video_tcp = 0;

static void session_resolve_transport(const ctrl_session_params *p)
{
    /* The env var keeps priority over the setting, as every toggle in this repo
     * does: `env.txt` serves the A/B runs. */
    const char *e = getenv("SHADOW_VIDEO_NET_TCP");
    g_video_tcp = e ? (atoi(e) != 0) : (p && p->video_tcp);
    /* The request side is told the SAME value, from here, so the two halves of
     * one decision cannot drift apart. */
    ctrl_msgs_set_video_tcp(g_video_tcp);
    if (g_video_tcp)
        clog("[K15] video transport: TCP (STFP+TLS) - the server's adaptive "
             "server is disabled in this mode");
}

static int video_en_tcp(void) { return g_video_tcp; }

static uint32_t g_gp_brut = 0, g_gp_vus = 0, g_gp_ko = 0;

/* G55: called by ctrl_gamepad_detach. A summary that only prints when something
 * arrives proves nothing when nothing arrives. */
void ctrl_session_log_gamepad_rx(void)
{
    clog("[G53] :base+13 summary - raw=%u seen=%u decrypt_failed=%u",
         g_gp_brut, g_gp_vus, g_gp_ko);
}

static void on_gamepad_packet(const uint8_t *pkt, size_t n, void *user)
{
    /* === G53b 2026-08-28 - COUNT THE RAW ARRIVAL BEFORE ANY FILTER ===
     * First measurement: zero [G53] lines over a 60 s session in which 2479
     * gamepad messages went out. A callback that says nothing does not
     * distinguish "the server is not answering" from "we are dropping its
     * answer". So we count BEFORE the sentinel, before the size bound, before
     * decryption - the K17b lesson, applied up front this time. */
    {
        if (++g_gp_brut <= 8 || (g_gp_brut % 100) == 0) {
            char hx[3 * 24 + 1]; int o = 0;
            const size_t m = n < 24 ? n : 24;
            for (size_t i = 0; i < m; i++)
                o += snprintf(hx + o, sizeof(hx) - o, "%02x ", pkt[i]);
            clog("[G53] RAW ARRIVAL #%u on :base+13: len=%zu: %s", g_gp_brut, n, hx);
        }
    }

    session_ctx_t *ctx = (session_ctx_t *)user;
    if (!ctx || ctx->magic != SESSION_CTX_MAGIC || !ctx->cipher) return;
    if (n < 28 + 1) return;
    const int ct_len = (int)n - 28;
    if (ct_len <= 0 || ct_len > 64) return;

    uint8_t clair[64];
    memcpy(clair, pkt, ct_len);
    const uint8_t *nonce = pkt + n - 28;
    const uint8_t *tag   = pkt + n - 16;
    if (!shadow_cipher_decrypt_unsafe(ctx->cipher, clair, ct_len, nonce, tag)) {
        if (++g_gp_ko <= 3)
            clog("[G53] manette : dechiffrement KO (len=%zu)", n);
        return;
    }

    /* === G57 2026-08-28 - FORCE FEEDBACK, FINALLY PUT TO USE ===
     * The format is parsed by a PURE module (`gamepad_wire.c`, tested offline)
     * and stored in shared state (`rumble_state.c`) that the UI thread reads
     * once per frame. We very deliberately do NOT touch the hardware here: this
     * thread also drains video and cursor, and Borealis's rumble primitive
     * starts with a `padUpdate()` on the same PadState as the UI thread -
     * calling it from here would be a concurrent write on the input state, and
     * would eat button edges without a single error. */
    {
        gamepad_wire_vibration_t vb;
        if (gamepad_wire_parse_vibration(clair, (size_t)ct_len, &vb))
            rumble_state_set(vb.id, vb.low, vb.high);
    }

    /* Log the RAW bytes before interpreting them: an unexpected reply has to be
     * visible as it is, not forced into a category. */
    const uint8_t type = (ct_len > 2) ? clair[2] : 0xFF;
    if (++g_gp_vus <= 12 || (g_gp_vus % 50) == 0) {
        char hx[3 * 16 + 1]; int ho = 0;
        for (int i = 0; i < ct_len && i < 16; i++)
            ho += snprintf(hx + ho, sizeof(hx) - ho, "%02x ", clair[i]);
        const char *name = (type == 7) ? "kVibration"
                        : (type == 8) ? "kReply"
                        : (type == 5) ? "kPing" : "?";
        clog("[G53] manette RX #%u type=%u (%s) len=%d : %s",
             g_gp_vus, type, name, ct_len, hx);
    }
}

static int udp_drain(int fd, uint8_t *buf, size_t cap,
                       void (*handle)(const uint8_t *pkt, size_t len, void *user),
                       void *user, uint32_t *ovfl_last) {
    (void)ovfl_last;   /* ING-1: only read where the kernel counts drops */
    /* === K16e 2026-08-29 - AN ABSENT SOCKET IS NOT A FAILURE ===
     *
     * In TCP video mode there is NO video UDP socket (K15i): `udp_video` is -1,
     * and the loop still called `recv(-1, ...)` on every pass. Observed on the
     * console in the 2026-08-29 session: `udp_drain fd=-1 recv FAIL errno=9`
     * (EBADF) every six milliseconds.
     *
     * Two costs, one of them not obvious: a wasted syscall in the hottest loop
     * of the session, and above all an ERROR logged permanently for a perfectly
     * normal situation. A log that shouts about the normal is a log people stop
     * reading - and that is exactly what let K16b go unnoticed for eight
     * sessions.
     *
     * The test is here, inside the function, not at its three call sites: a
     * fourth caller would forget it. */
    if (fd < 0) return 0;

    int count = 0;
    while (1) {
#if KDROPS_MEASURED
        /* recvmsg rather than recv: it is the only way to retrieve the overflow
         * counter, which comes through as an ancillary message. */
        struct iovec iov = { buf, cap };
        union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(uint32_t))]; } ctl;
        struct msghdr msg = {0};
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = ctl.b; msg.msg_controllen = sizeof(ctl.b);
        ssize_t n = recvmsg(fd, &msg, 0);
        if (n > 0) {
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
                    uint32_t v; memcpy(&v, CMSG_DATA(c), sizeof(v));
                    /* ING-1 - the cmsg carries the socket's CUMULATIVE
                     * count: add only what is new on THIS socket. */
                    if (ovfl_last) g_kernel_drops += ovfl_accum(ovfl_last, v);
                }
            }
        }
#else
        ssize_t n = recv(fd, buf, cap, 0);
#endif
        if (n <= 0) {
            /* Log the real errors (anything other than "no data ready"). */
            if (n < 0) {
                int e = shadow_sock_errno();
                if (e != EAGAIN && e != EWOULDBLOCK) {
                    static int s_logged = 0;
                    if (s_logged < 5) {
                        clog("udp_drain fd=%d recv FAIL errno=%d", fd, e);
                        s_logged++;
                    }
                }
            }
            break;
        }
        if (handle) handle(buf, (size_t)n, user);
        count++;
        if (count > 500) break;  /* safety */
    }
    return count;
}

/* ============================================================================
 * Frame state - used in callbacks
 * ==========================================================================*/

/* Buffer for the SUFP output (= complete reassembled frame, still encrypted).
 * 8 MiB for heavy keyframes (1080p high profile). On the heap, because a
 * Borealis thread stack is smaller than 8 MiB. */
#define SUFP_OUT_CAP (8 * 1024 * 1024)

/* An on_video_full_frame callback used to live here: the video SUFP
 * reassembler fired it when a frame was complete. Video no longer goes through
 * SUFP since vid_reasm.c parses the header inline, so that callback was never
 * called again - while DUPLICATING the chacha20 decryption and the VideoFrame
 * header parsing that vid_reasm.c actually does. Deleted on 2026-08-25: dead
 * code that mirrors live code eventually gets read as the reference. */


/* RE11 2026-05-18 - VST callback wired to h264_decoder via on_video.
 * RE6 finding C95 : VST :base+20 4142B = H.264 NAL Annex-B IDR retransmits.
 * Forward directly to caller's on_video (= h264_decoder_feed_annexb downstream).
 * pts_ms = monotonic ms (consistent with the UDP video path, which passes ts/90). */
/* === S54 2026-08-26 - CURSOR IMAGES ARRIVE HERE ===
 *
 * `:base+20` is the `Cursor` channel (KB §3.37). Its type 0x12 frames carry the
 * remote pointer's image. They had been arriving here all along, and the
 * `[RE11]` guard threw them away every session, complaining that an H.264 start
 * code was missing - which was true, and beside the point.
 *
 * Here we decode the image header (pure module `cursor_wire.h`, which validates
 * the dimensions BEFORE a single pixel is read: these bytes come off the
 * network) and hand the image over to `cursor_state`. */
/* K15k - a video frame that arrived over the STFP channel. The payload is one
 * whole frame in the clear: `VideoFrame` header then Annex-B. We hand it to the
 * UDP path's emission code, which carries four campaigns' worth of fixes (G40,
 * G43, deduplication, access-unit aggregation) and would be absurd to
 * duplicate. */
static void vtcp_video_callback(void *udata, const uint8_t *charge, size_t len,
                                uint32_t frame_id, uint8_t flag, int is_key)
{
    (void)frame_id; (void)flag; (void)is_key;
    if (!udata || !charge || len == 0) return;
    /* K15l - feed the volume counters, otherwise any future measurement on this
     * path would be wrong: `avg_kbps` reported 0.0 here because only the UDP
     * bytes were counted. That is exactly the defect family V12 has just purged
     * - a counter that is displayed but never written. */
    session_ctx_t *sc = (session_ctx_t *)udata;
    if (sc && sc->magic == SESSION_CTX_MAGIC && sc->stats) {
        sc->stats->udp_video_pkts++;
        sc->stats->udp_video_bytes += (uint64_t)len;
    }

    /* K15o - SEE what we feed, rather than infer it. The decoder stalls ("1725
     * frames fed with no output", idr_t=0): we need to know whether the sequence
     * parameter sets arrive, and in what shape. The first sixteen bytes are
     * enough - VideoFrame header, then start code and NAL type. */
    static uint32_t n = 0;
    if (++n <= 8 || (n % 500) == 0) {
        char hx[3 * 16 + 1]; int o = 0;
        const size_t m = len < 16 ? len : 16;
        for (size_t i = 0; i < m; i++)
            o += snprintf(hx + o, sizeof(hx) - o, "%02x ", charge[i]);
        clog("[K15k] TCP video frame #%u: %zu bytes: %s", n, len, hx);
    }
    vid_reasm_emit_payload(udata, (uint8_t *)charge, (int)len);
}

static void vst_cursor_callback(void *udata, const uint8_t *charge, size_t len,
                                uint8_t type, uint32_t id)
{
    (void)type; (void)id;
    session_ctx_t *ctx = (session_ctx_t *)udata;
    if (!ctx || ctx->magic != SESSION_CTX_MAGIC || !charge || len == 0) return;

    cursor_wire_image_t im;
    if (!cursor_wire_parse_image(charge, len, &im)) {
        static uint32_t rejets = 0;
        if (++rejets <= 3)
            clog("[S54] unreadable cursor image (%zu bytes) - ignored", len);
        return;
    }

    static uint32_t received = 0;
    received++;
    if (received <= 3 || (received % 50) == 0)
        clog("[S54] cursor #%u: %s%ux%u format=%u stride=%u hotspot=(%u,%u)",
             received, im.hidden ? "MASQUE " : "", im.width, im.height,
             im.format, im.stride, im.hot_x, im.hot_y);

    cursor_state_feed_image(im.hidden ? NULL : charge + im.offset_pixels,
                            im.image_size, im.width, im.height,
                            im.stride, im.format, im.hot_x, im.hot_y);
}

void vst_to_h264_callback(void *udata, const uint8_t *nal_bytes, size_t len,
                            uint32_t frame_id, uint8_t flag, int is_keyframe) {
    (void)frame_id; (void)flag;
    session_ctx_t *ctx = (session_ctx_t *)udata;
    if (!ctx || ctx->magic != SESSION_CTX_MAGIC) {
        /* S37: this is not a session context. Rather than dereference whatever
         * it happens to be, we SAY so - once, then stay quiet. */
        static int dit = 0;
        if (!dit) { dit = 1; clog("[S37] vst: contexte invalide (magic=0x%x) — rappel ignore",
                                  ctx ? ctx->magic : 0u); }
        return;
    }
    if (!ctx->p || !ctx->p->on_video || !nal_bytes || len == 0) return;

    /* The Annex-B start code must be there.
     *
     * S54: this guard logged "VST frame missing Annex-B start code" every
     * session - accurate, and misleading: the rejected frames were CURSOR
     * images, and this channel has never carried video for us. They now go to
     * `vst_cursor_callback` before reaching here. What is left: a genuinely
     * malformed video frame, or an unknown variant. */
    if (len < 5 || nal_bytes[0] != 0 || nal_bytes[1] != 0 ||
        nal_bytes[2] != 0 || nal_bytes[3] != 1) {
        static int g_warn_nostart = 0;
        if (g_warn_nostart < 3) {
            clog("[RE11] :base+20 frame with no Annex-B code, byte0=%02x len=%zu "
                 "— ni video ni curseur, ignoree", nal_bytes[0], len);
            g_warn_nostart++;
        }
        return;
    }

    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t pts_ms = (uint64_t)t.tv_sec * 1000ULL + t.tv_nsec / 1000000ULL;

    static int g_log = 0;
    if (g_log < 5) {
        uint8_t nt = nal_bytes[4] & 0x1f;
        clog("[RE11] VST → h264 feed nal_type=%u len=%zu key=%d pts=%llu",
             nt, len, is_keyframe, (unsigned long long)pts_ms);
        g_log++;
    }

    ctx->p->on_video(nal_bytes, len, pts_ms, (bool)is_keyframe, ctx->p->user);
}

/* SUFP cursor callback - complete reassembled frame, decrypted then handed up. */
/* AUD10 - see the callback below. Not `static`: the statistics line lives in
 * another function of this same file. */
uint32_t g_reasm_audio = 0, g_reasm_cursor = 0;

/* === AUD12 - ACCUMULATOR FOR SPLIT AUDIO FRAMES ===
 *
 * A FLAC frame cut in two is worth ~1259 bytes of plaintext; we take a generous
 * margin without taking too much, and REFUSE anything beyond rather than
 * overflow. The slot logic moved to aud_reasm.h on 2026-09-11, where
 * tests/test_aud_reasm.c pins it.
 *
 * ING-A2 2026-09-11 - THIS IS SESSION STATE, AND FILE SCOPE DOES NOT MAKE IT
 * SAFE. This comment used to say that a FILE-scope `static`, unlike a function
 * one, let a restarting session find clean slots. It does not: a file-scope
 * `static` lives exactly as long as a function one. What gives a new session
 * clean slots is ctrl_session_aud_reasm_reset(), called at session start next
 * to vid_reasm_reset_session() - and until 2026-09-11 nothing called it (it came
 * with AUD12, 959e1a38, and was never wired in). A slot stays active when the
 * last copy of a split frame loses its final chunk: there is no timeout. In the
 * next session, a lost chunk 0 let the chunk 1 of the first split frame with the
 * same byte 1 complete the OLD frame, whose sequence number belongs to the
 * previous stream; the server renumbers from 1 on every stream (KB 3.31), so the
 * anti-duplicate window jumped ahead and refused every later frame as too late,
 * for about as long as the previous session had lasted. Offline replay on real
 * FLAC frames: after a 10-minute session and one lost datagram, 406 of 30000
 * frames played, ~296 s mute. Monte Carlo: 0.60 / 3.30 / 12.25 % of FLAC-to-FLAC
 * reconnects muted at 0.5 / 1.66 / 5 % loss when the two copies are sent in
 * sequence - the order a live FLAC session showed (`decoupees: ok=` is half of
 * `rejets: multi=`). With the reset: 0 in 200 000 trials, 2.19 ns per session.
 * AUD16 cannot see such a mute: packets keep arriving.
 * The 4 x 8 KiB of slots stay out of session_ctx_t on purpose: it is a stack
 * local of ctrl_session_run, and 32 KiB more on a thread stack is a risk on
 * Switch. */
static aud_reasm_state_t g_aud_reasm;

/* === AUD-LC-3 2026-09-11 - THE :base+30 CENSUS IS SESSION STATE TOO ===
 * The [AUD2] census, its "first packet" mark, the budget of the [AUD2] frame
 * dumps and the one of the [AUD11] rejects were function statics of
 * on_cursor_packet. In a process that runs several sessions (autotest,
 * auto-reconnect, a new stream from the VM list), sessions 2..N never logged
 * their first packet nor their first three frames, and their census went on
 * from the earlier sessions' counts (harness: `<100B=6996 audio=3498`
 * inherited, `<100B=1998 audio=999` once reset; live, 2026-09-11: the 8th
 * session's first census read `audio=8475`). The defect family KB 3.28 names.
 * They live here, next to the slots, and ctrl_session_aud_reasm_reset() clears
 * them with the slots in one memset - out of session_ctx_t, a stack local that
 * already carries ~8.4 KB of NACK queue (d_ptype alone is 1 KB). The fields
 * keep the old variable names, so a grep through the history still finds them.
 * AUD11 also gets its own budget: it used to share `d_hdr_ko`, which the AUD12
 * path bumps on every split packet, so in a FLAC session its six lines were
 * spent within a few packets without printing anything. */
static struct {
    uint32_t d_hdr_ko, d_dec_ko, d_cursor, d_audio;        /* sorting */
    uint32_t d_ko_type, d_ko_multi, d_ko_flag, d_ko_court;  /* why refused */
    uint32_t d_ptype[256];                                 /* first plaintext byte */
    uint32_t d_w47, d_wsmall, d_waudio, d_wbig;            /* sizes on the wire */
    int64_t  d_last_ms;                                    /* 0 = no packet yet */
    int      aud_dump;                                     /* [AUD2] frame dumps: 3 */
    uint32_t aud11_logged;                                 /* [AUD11] lines: 6 */
} g_aud30;

/* Session start: clears the split-frame slots and every counter of the
 * :base+30 census, and returns how many slots still held a partial frame. Each
 * one could have muted the new session, so the caller logs the count - a live
 * census of stale slots at each session start. Called on the session thread
 * before the first drain of :base+30: no packet of the new session can be in a
 * slot yet, and nothing else touches this state. */
static unsigned ctrl_session_aud_reasm_reset(void)
{
    const unsigned stale = aud_reasm_reset(&g_aud_reasm);
    memset(&g_aud30, 0, sizeof g_aud30);
    memset(g_aud_seen, 0, sizeof g_aud_seen);
    g_reasm_audio  = 0;
    g_reasm_cursor = 0;
    return stale;
}

/* === ING-A1 2026-09-11 - SHADOW_DIAG_AUDIO_SEQ_JUMP_AT_S: A FAR-AHEAD NUMBER
 *     ON DEMAND ===
 * DIAGNOSTIC, OFF by default (the VI1 pattern). AT seconds after the receive
 * loop starts, the next audio frame's number plus 1 000 000 is shown to the
 * anti-duplicate window ONCE per session: nothing is delivered and nothing is
 * sent. That is exactly what a stale reassembly slot or a renumbered stream
 * does to the window. With the default rule, one [ING-A1] line follows within
 * ~1 s and the `dup=` delta of the 5 s stats line returns to about half the
 * `audio=` delta; with SHADOW_AUDIO_DEDUP_RESYNC=0 every later frame is refused
 * for the rest of the session. Process configuration, read once; the per-session
 * state is ctx->aud_diag_jump. */
static int g_diag_aud_jump_at_s = -2;   /* -2 unread, <= 0 off */

/* === AUD-DEDUP-3 2026-09-11 - AUDIO LOSS, COUNTED WHERE THE NUMBERS ARE ===
 * The window computed every gap in the numbering and threw it away: no stat,
 * no key, no panel row carried audio loss, and losing BOTH copies of a run of
 * frames - a burst, the ~300 ms console freeze - left no trace. The accountant
 * (audio_loss.h) is fed ONLY the numbers the window accepted, so what is played
 * stays bit-identical (0 differing decisions on ~1.2 M packets, offline). It
 * also keeps the arrival time of each accepted frame for the D4 companion
 * line: during a video outage, the longest arrival silence says whether the
 * link DELAYED the packets (no loss, a long silence) or LOST them. Counting
 * only - no toggle, like S117's loss publication. Nothing is sent. */
static void aud_loss_accepted(session_ctx_t *ctx, uint32_t seq)
{
    const int64_t now = vid_now_ms();
    /* 0 = no frame yet: the first one reports no gap (an interval counter at 0
     * against a monotonic clock would read the whole uptime). */
    const int64_t arrival = ctx->aud_last_accept_ms > 0 ? now - ctx->aud_last_accept_ms : 0;
    ctx->aud_last_accept_ms = now;
    if (arrival > ctx->aud_gap_max_ms) ctx->aud_gap_max_ms = arrival;
    const uint32_t gap = audio_loss_note(&ctx->aud_loss, seq);
    /* A gap of 5 frames or more gets its line - 50 ms of sound, 1-2 s of a
     * silent VM's cadence - 40 per session. The number is PROVISIONAL: a late
     * frame may still fill part of it; aud_lost= only counts what cannot. */
    if (gap >= 5 && ctx->aud17_logged < 40) {
        ctx->aud17_logged++;
        clog("[AUD17] trou audio : %u trames (t=%ds, arrivee +%lld ms)",
             (unsigned)gap, ctx->stats->session_seconds, (long long)arrival);
    }
    ctx->stats->audio_frames_lost = ctx->aud_loss.lost;
    ctx->stats->audio_holes       = ctx->aud_loss.holes;
    ctx->stats->audio_hole_max    = ctx->aud_loss.hole_max;
    ctx->stats->audio_renum       = ctx->aud_loss.renum;
}

/* === ING-A1 / ING-A4 2026-09-11 - THE ANTI-DUPLICATE WINDOW HAS ONE DOOR ===
 * The three audio paths - single packet, reassembly by byte 1, SUFP fallback -
 * each called the window themselves. They all go through here now, which:
 *  - counts the frame in `udp_audio_pkts` BEFORE the decision, duplicates
 *    included, and a refusal in `audio_dup_skipped` (audio_dedup_admit). Two of
 *    the three paths used to count AFTER the window: with split FLAC frames in
 *    the stream (real sound only), `udp_audio_pkts - audio_dup_skipped` - the
 *    panel's count of audio frames played - read 1517 where 1993 had been
 *    played (-24 %, live). The single-packet path, the only one a silent VM
 *    uses, already counted before, so older logs stay comparable. No toggle:
 *    an instrument, like AUD-LC-3;
 *  - logs one line when the window re-primes itself (ING-A1's witness), capped
 *    at 8 per session by a ctx field;
 *  - hosts SHADOW_DIAG_AUDIO_SEQ_JUMP_AT_S;
 *  - AUD-DEDUP-3: notes every ACCEPTED 0x12 frame in the loss accountant
 *    (aud_loss_accepted), and sorts a refusal that was too old - 64 or more
 *    behind, the S36 class - into audio_stale; audio_dup_skipped still counts
 *    every refusal.
 * Nothing here emits on :base+30 (S57). */
static bool aud_accept(session_ctx_t *ctx, const uint8_t *plain, int len)
{
    if (ctx->aud_diag_jump == 1 && len >= 5) {
        ctx->aud_diag_jump = 2;
        (void)audio_dedup_accepte(&ctx->aud_dedup, audio_dedup_seq(plain) + 1000000u);
        clog("[DIAG] numero audio decale volontairement (SHADOW_DIAG_AUDIO_SEQ_JUMP_AT_S)");
    }
    const uint32_t resyncs = ctx->aud_dedup.resyncs;
    const bool ok = audio_dedup_admit(&ctx->aud_dedup, plain, len,
                                      &ctx->stats->udp_audio_pkts,
                                      &ctx->stats->audio_dup_skipped);
    if (ctx->aud_dedup.resyncs != resyncs && ctx->aud_resync_logged < 8) {
        ctx->aud_resync_logged++;
        clog("[ING-A1] fenetre anti-doublon resynchronisee seq=%u (#%u)",
             (unsigned)audio_dedup_seq(plain), (unsigned)ctx->aud_dedup.resyncs);
    }
    /* AUD-DEDUP-3 - audio frames only: the 0x02 stream descriptor shares seq 0
     * (and only reaches here with SHADOW_AUDIO_TYPE_FILTER=0). A refusal leaves
     * the window untouched, so plus_haut is the value the decision used. */
    if (len >= 5 && aud_plain_is_audio(plain, len)) {
        const uint32_t seq = audio_dedup_seq(plain);
        if (ok)
            aud_loss_accepted(ctx, seq);
        else if (audio_loss_is_stale(ctx->aud_dedup.plus_haut, seq))
            ctx->stats->audio_stale++;
    }
    return ok;
}

/* Delivers a COMPLETE audio frame: same filtering and same anti-duplicate
 * window as the single-packet path. Every frame arrives twice, split ones
 * included. */
static void aud_livrer(session_ctx_t *ctx, const uint8_t *plain, int len)
{
    if (!aud_plain_is_audio(plain, len) || !ctx->p->on_audio) return;
    if (!aud_accept(ctx, plain, len)) return;   /* ING-A4: counted there, before the window */
    g_reasm_audio++;
    ctx->stats->udp_audio_bytes += (uint64_t)len;
    ctx->p->on_audio(plain, (size_t)len, 0, ctx->p->user);
}



static void on_cursor_full_frame(uint8_t subchan, uint32_t frame_id,
                                   const uint8_t *buf, size_t len, void *user) {
    (void)subchan; (void)frame_id;
    session_ctx_t *ctx = (session_ctx_t *)user;
    if (!ctx || ctx->magic != SESSION_CTX_MAGIC || !buf || len <= 28) return;
    int ct_len = (int)(len - 28);
    if (ct_len <= 0) return;
    /* Reuses plain_buf - cursor frames are small (< 1 KiB). */
    memcpy(ctx->plain_buf, buf, ct_len);
    const uint8_t *nonce = buf + ct_len;
    const uint8_t *tag   = buf + ct_len + 12;
    if (!shadow_cipher_decrypt_unsafe(ctx->cipher, ctx->plain_buf, ct_len,
                                        nonce, tag)) return;

    /* === AUD10 2026-08-27 - MULTI-PACKET FRAMES ARE AUDIO TOO ===
     *
     * This callback delivered EVERYTHING to the cursor. That is a leftover from
     * when we believed `:base+30` carried the cursor; S55 established that this
     * channel is AudioOut and nothing else, but only the SINGLE-PACKET path was
     * fixed. The reassembly path kept sending its frames to the cursor, where
     * they vanished.
     *
     * Measured consequence in FLAC: the decoder only received 83 frames per
     * second where it needs 100. The 17 missing ones are exactly the frames too
     * large for a single packet - and FLAC, being lossless, produces far more of
     * them than Opus (595 to 1216 bytes against 80 to 160). Hence sound that
     * "glitches a bit" without any error counter moving: those frames were not
     * rejected, they were delivered to the wrong recipient.
     *
     * So we apply the SAME sorting as the single-packet path: `0x12` is audio,
     * everything else goes to the cursor. */
    static int g_cur30_r = -1;
    if (g_cur30_r < 0) {
        const char *e = getenv("SHADOW_CURSOR_ON_30");
        g_cur30_r = e ? atoi(e) : 0;
    }
    if (!g_cur30_r && aud_plain_is_audio(ctx->plain_buf, ct_len) && ctx->p->on_audio) {
        /* Same anti-duplicate window as the single-packet path: the server
         * sends every frame TWICE, and both halves of a split frame arrive in
         * duplicate too. Without the window we would play those frames at
         * double speed. ING-A4 2026-09-11: through aud_accept(), which counts
         * the frame BEFORE the window, like the single-packet path (this path
         * used to count it after). */
        if (!aud_accept(ctx, ctx->plain_buf, ct_len)) return;
        g_reasm_audio++;
        ctx->stats->udp_audio_bytes += (uint64_t)ct_len;
        ctx->p->on_audio(ctx->plain_buf, (size_t)ct_len, 0, ctx->p->user);
        return;
    }

    g_reasm_cursor++;
    if (ctx->p->on_cursor)
        ctx->p->on_cursor(ctx->plain_buf, (size_t)ct_len, ctx->p->user);
}

static void on_cursor_packet(const uint8_t *pkt, size_t n, void *user) {
    session_ctx_t *ctx = (session_ctx_t *)user;
    if (!ctx || ctx->magic != SESSION_CTX_MAGIC) return;   /* S37 */
    /* S55: this counter is called "cursor" but it counts ALL packets on
     * `:base+30`, which is AudioOut (KB §3.37). It is what made us believe in a
     * "cursor channel active 13 times out of 20" and falsified hours of
     * intermittency measurement: it said nothing about the cursor, only that the
     * AUDIO channel was alive. The field name is kept - it is published in the
     * stats the UI reads - but it must no longer be read as cursor traffic. */
    ctx->stats->udp_cursor_pkts++;

    /* AUD2 diagnostic - the audio counter stayed at zero while the channel
     * receives 155 packets/s. Rather than guess, we count what becomes of EVERY
     * packet: rejected by the header, decryption failed, or sorted by the first
     * plaintext byte. Only one of those three lines can explain the zero, and it
     * will say so.
     * AUD10 - the `reassemble:` part counts the frames that come out of
     * REASSEMBLY, the ones that do not fit in a single packet. Without it there
     * is no way to know whether the fix works: the frame rate alone proves
     * nothing, it depends on what the VM is playing.
     * AUD-LC-3 2026-09-11 - every counter of this census lives in g_aud30,
     * reset at session start: see there. */
    /* Sizes ON THE WIRE. This is the direct comparison with the official
     * client's capture: 47 B = cursor, 129-246 B = audio. If our packets are all
     * 47 B, the server is not sending us sound and the problem is upstream; if
     * they are large, it is on our side. */
    if (n == 47)        g_aud30.d_w47++;
    else if (n < 100)   g_aud30.d_wsmall++;
    else if (n <= 260)  g_aud30.d_waudio++;
    else                g_aud30.d_wbig++;
    {
        struct timespec dts;
        clock_gettime(CLOCK_MONOTONIC, &dts);
        int64_t now = (int64_t)dts.tv_sec * 1000 + dts.tv_nsec / 1000000;
        /* First packet: say it right away. Whether the channel is alive (or
         * not) is the first question, and waiting 10 s to find out wastes a
         * run. AUD-LC-3: into the journal too, same text - stderr.log is
         * pulled by no tool and reaches no mirror, and only the journal
         * timestamps a line, which is the arrival time the S57/A7 analyses
         * need. */
        if (g_aud30.d_last_ms == 0) {
            g_aud30.d_last_ms = now;
            fprintf(stderr, "[AUD2] first packet on :base+30 (%lu B on the wire)\n",
                        (unsigned long)n);   /* not the journal: no %z rewrite here */
            clog("[AUD2] first packet on :base+30 (%u B on the wire)", (unsigned)n);
        }
        if (now - g_aud30.d_last_ms >= 10000) {
            g_aud30.d_last_ms = now;
            char top[160]; int to = 0; top[0] = 0;
            for (int v = 0; v < 256 && to < (int)sizeof(top) - 16; v++)
                if (g_aud30.d_ptype[v])
                    to += snprintf(top + to, sizeof(top) - to, "%s0x%02x=%u",
                                   to ? " " : "", v, g_aud30.d_ptype[v]);
            /* Sized for the WHOLE line, `top`'s 159 bytes of per-type
             * histogram included: at 320 the compiler could prove the last
             * conversion would be cut, which on a diagnostic line means the
             * part that was added last - the histogram - is the part that
             * silently disappears. */
            char line[768];
            snprintf(line, sizeof(line),
                 "[AUD2] :+30 fil: 47B=%u <100B=%u 100-260B=%u >260B=%u "
                 "| sort: en-tete_ko=%u dechiffrement_ko=%u curseur=%u audio=%u "
                 "| reassemble: audio=%u curseur=%u "
                 "| rejets: type=%u multi=%u flag=%u court=%u "
                 "| decoupees: ok=%u desordre=%u "
                 "| first plaintext byte: %s",
                 g_aud30.d_w47, g_aud30.d_wsmall, g_aud30.d_waudio, g_aud30.d_wbig,
                 g_aud30.d_hdr_ko, g_aud30.d_dec_ko, g_aud30.d_cursor, g_aud30.d_audio,
                 g_reasm_audio, g_reasm_cursor,
                 g_aud30.d_ko_type, g_aud30.d_ko_multi, g_aud30.d_ko_flag, g_aud30.d_ko_court,
                 g_aud_reasm.ok, g_aud_reasm.out_of_order, top);
            clog("%s", line);
            fprintf(stderr, "%s\n", line);
        }
    }

    /* CUR1 2026-05-18: cursor packets are single-packet (= max_chunks=0,
     * self-contained), not multi-chunk reassembly like video. Bypass sufp, then
     * decrypt + call back directly.
     * Format observed in the V16 capture:
     *   byte0 = 0x13 (v1) OR 0x23 (v2)  <- cursor uses both versions
     *   byte1 = subchan
     *   byte2-5 = 0 (= chunk_idx=0, max_chunks=0 = self-contained sentinel)
     *   byte6-9 = u32 frame_id
     *   byte10 = flag (= 0x01 chacha20 encrypted)
     *   [ct][nonce 12][tag 16] = payload encrypted same way as video
     */
    if (n >= 11) {
        uint8_t  byte0 = pkt[0];
        uint8_t  type  = byte0 & 0x0f;
        uint16_t max_chunks = (uint16_t)pkt[4] | ((uint16_t)pkt[5] << 8);
        uint8_t  flag10 = pkt[10];
        if (type == 0x3 && max_chunks == 0 && flag10 == 0x01 && n > 11 + 28) {
            /* Self-contained chacha20-encrypted cursor frame */
            int ct_len = (int)n - 11 - 28;
            if (ct_len > 0 && ct_len <= 4096 && ctx->cipher) {
                static uint8_t plain[4096];
                memcpy(plain, pkt + 11, ct_len);
                const uint8_t *nonce = pkt + n - 28;
                const uint8_t *tag   = pkt + n - 16;
                if (shadow_cipher_decrypt_unsafe(ctx->cipher, plain, ct_len,
                                                    nonce, tag)) {
                    /* === AUD2 2026-08-21 - THE AUDIO IS HERE ===
                     *
                     * Guided capture alternating silence and sound: `:base+30`
                     * goes from 19.8 to 170 packets/s as soon as the remote
                     * desktop plays sound, with VARIABLE sizes (129-246 B) where
                     * the cursor is a fixed 47 B. Sound therefore shares the
                     * cursor's channel, with the same header and the same
                     * encryption - which explains why no extra socket ever
                     * opened, and why the earlier captures, all of them silent,
                     * could not show it.
                     *
                     * We split on the type declared at the head of the
                     * plaintext: 0x02 bitmap and 0x12 position are cursor
                     * (cursor_state.c), the rest is an Opus frame. Those frames
                     * had always been arriving here - they ended up in the
                     * cursor parser's "unknown type" counter. */
                    /* === SORTING BY TYPE, RE-READ ===
                     *
                     * The 8-byte frames I took for cursor positions
                     * (`12 01 00 00 00 f4 ff fe`) are in fact VERY SHORT audio
                     * frames: `0x12`, then a 4-byte sequence number, then
                     * `f4 ff fe` - a minimal Opus packet, the one an encoder
                     * emits during silence. The header is the same as on the
                     * large frames.
                     *
                     * So `0x12` = audio, whatever the length. The rest goes to
                     * the cursor as before. */
                    /* === S55 2026-08-26 - THIS CHANNEL IS AUDIO, AND NOTHING ELSE ===
                     *
                     * The sorting above rested on the channel map, which turned
                     * out to be wrong. The official client's telemetry NAMES its
                     * sockets with their port: `:base+30` is called `AudioOut`,
                     * and the cursor is on `:base+20` (KB §3.37, verified on two
                     * different port bases).
                     *
                     * So what was going to `cursor_state.c` was not cursor data:
                     * an analysis decrypted 16,551 packets from this channel and
                     * found 16,549 of audio, the two remaining ones being a
                     * stream descriptor (271 bytes, `10 80 bb` = 16 bits /
                     * 48,000 Hz) - not an image.
                     *
                     * The worst part was not the mis-routing but its COUNTER:
                     * `udp_cursor_pkts` increments for EVERY packet on that
                     * port, which made us believe for hours in a "cursor channel
                     * active" that did not exist, and falsified all our
                     * intermittency measurements.
                     *
                     * `SHADOW_CURSOR_ON_30=1` restores the old routing
                     * (diagnostic only). */
                    static int g_cur30 = -1;
                    if (g_cur30 < 0) {
                        const char *e = getenv("SHADOW_CURSOR_ON_30");
                        g_cur30 = e ? atoi(e) : 0;
                    }
                    const uint8_t ptype = plain[0];
                    const bool is_cursor = g_cur30 && (ptype != 0x12);

                    /* The first audio frames in hex: it remains to be seen
                     * whether the Opus starts at byte 0 or after a small
                     * header. */
                    if (!is_cursor) {
                        /* AUD-LC-3: three per SESSION now (g_aud30). */
                        if (g_aud30.aud_dump < 3) {
                            g_aud30.aud_dump++;
                            char hx[3 * 24 + 1]; int ho = 0;
                            for (int i = 0; i < ct_len && i < 24; i++)
                                ho += snprintf(hx + ho, sizeof(hx) - ho, "%02x ", plain[i]);
                            clog("[AUD2] audio frame #%d len=%d: %s", g_aud30.aud_dump, ct_len, hx);
                            fprintf(stderr, "[AUD2] audio frame #%d len=%d: %s\n",
                                    g_aud30.aud_dump, ct_len, hx);
                        }
                    }
                    g_aud30.d_ptype[ptype]++;

                    /* === DEC-1 / AUD-DEDUP-1 2026-09-11 - THE STREAM DESCRIPTOR
                     *     IS NOT AUDIO ===
                     * Every stream opens with a `0x02` descriptor, sent twice
                     * with seq 0: 271 B in an Opus session, 13 B in a FLAC one
                     * (streaming/audio_route.h). This path handed it to the
                     * decoder, where from the second session of a process on it
                     * classified the session as Opus: a FLAC session after an
                     * Opus one decoded nothing, and so did every FLAC session
                     * from the 8th of a process on (bench: 0 of 200 frames, 7
                     * repetitions out of 7; live 2026-09-11: the 8th session, in
                     * two series out of two). Each Opus session after the first
                     * also counted one false decode error - the red row of the
                     * panel. It is dropped HERE: after the census and the [AUD2]
                     * dump, so that `trame audio #1` still shows it, and by
                     * RETURNING - falling through to on_cursor would bring back
                     * the "cursor traffic on :base+30" that S55 removed. The two
                     * reassembly paths already filtered on 0x12. Instruments that
                     * change meaning: the census `audio=` and the stats `audio=`
                     * lose the descriptor's two copies per session, and the first
                     * [AUD8] check reports seq=1. Bench (variant Bg, with
                     * audio.c's promotion): 100 % bit-exact FLAC in every order,
                     * Opus sessions 2+ from erreurs=1 to 0; the first Opus
                     * session of a process loses 7 frames instead of 6, since the
                     * descriptor no longer counts toward the offset probe.
                     * SHADOW_AUDIO_TYPE_FILTER=0 lets every type through, as
                     * before. The descriptor is logged once per session instead;
                     * its byte 7 read 1 in Opus sessions and 2 in FLAC ones - the
                     * codec, probably, so it is logged raw until confirmed. */
                    static int g_aud_type_filter = -1;
                    if (g_aud_type_filter < 0) {
                        const char *e = getenv("SHADOW_AUDIO_TYPE_FILTER");
                        g_aud_type_filter = e ? atoi(e) : 1;
                    }
                    if (!is_cursor && g_aud_type_filter && !aud_plain_is_audio(plain, ct_len)) {
                        if (ptype == 0x02 && ct_len >= 13 && ctx->aud_desc_logged < 1) {
                            ctx->aud_desc_logged++;
                            clog("[AUD2] descripteur de flux : %u bits %u Hz %u voies octet7=%u",
                                 (unsigned)plain[9],
                                 (unsigned)plain[10] | ((unsigned)plain[11] << 8),
                                 (unsigned)plain[12], (unsigned)plain[7]);
                        }
                        return;
                    }

                    if (!is_cursor && ctx->p->on_audio) {
                        /* === AUD8 - ANTI-DUPLICATE WINDOW ===
                         *
                         * Measured: 200 frames per second while each lasts
                         * 10 ms, which is exactly TWICE real time. Every frame
                         * reaches us in duplicate - reasonable redundancy on a
                         * UDP transport with no retransmission, but it doubles
                         * the playback duration if decoded as is.
                         *
                         * The header carries a 4-byte sequence number; we keep a
                         * sliding window of 64 already-seen values. It also
                         * absorbs reordering: a late packet is accepted if it
                         * has not been played yet, and ignored otherwise.
                         * Without the window, a plain `seq != last` would let
                         * every other frame through on an A B A B alternation. */
                        g_aud30.d_audio++;

                        /* S36 2026-08-25: the window now lives in the SESSION
                         * context. As a `static` it kept the previous stream's
                         * numbers, and since the server renumbers on every
                         * session, all subsequent frames were thrown away as
                         * "too old": 3997 rejected out of 3997, not one sample
                         * played. Sound only came through on the very first
                         * stream. The rule and its counter-case are in
                         * audio_dedup.h.
                         * ING-A1 / ING-A4 2026-09-11: through aud_accept(), the
                         * window's one door, which also counts the frame in
                         * udp_audio_pkts before the decision - here, as before. */
                        {
                            if (!aud_accept(ctx, plain, ct_len)) {
                                /* Is the second copy really the same? If it
                                 * differed, it would not be redundancy but a
                                 * second piece of information - error
                                 * correction, for instance - and discarding it
                                 * would cost quality. We compare the first few
                                 * cases. A refusal implies ct_len >= 5. */
                                if (ctx->aud_dup_verifies < 8) {
                                    ctx->aud_dup_verifies++;
                                    const uint32_t seq = audio_dedup_seq(plain);
                                    int idx = (int)(seq % 16);
                                    if (g_aud_seen[idx].seq == seq) {
                                        uint32_t sum = 0;
                                        for (int q = 0; q < ct_len; q++)
                                            sum = sum * 31u + plain[q];
                                        const bool same =
                                            (g_aud_seen[idx].len == ct_len
                                             && g_aud_seen[idx].sum == sum);
                                        clog("[AUD8] doublon seq=%u : %s (len %d vs %d)",
                                             seq,
                                             same ? "IDENTIQUE"
                                                  : "DIFFERENT - this is not a mere repetition",
                                             g_aud_seen[idx].len, ct_len);
                                    }
                                }
                                return;
                            }
                        }

                        if (ct_len >= 5) {
                            const uint32_t sq = audio_dedup_seq(plain);
                            uint32_t sum = 0;
                            for (int q = 0; q < ct_len; q++) sum = sum * 31u + plain[q];
                            int idx = (int)(sq % 16);
                            g_aud_seen[idx].seq = sq;
                            g_aud_seen[idx].len = ct_len;
                            g_aud_seen[idx].sum = sum;
                        }
                        ctx->stats->udp_audio_bytes += (uint64_t)ct_len;
                        ctx->p->on_audio(plain, (size_t)ct_len, 0, ctx->p->user);
                        return;
                    }
                    g_aud30.d_cursor++;
                    if (ctx->p->on_cursor) {
                        ctx->p->on_cursor(plain, (size_t)ct_len, ctx->p->user);
                    }
                    return;
                }
                g_aud30.d_dec_ko++;
            }
        } else if (type == 0x3 && flag10 == 0x01 && max_chunks > 0
                    && n > 11 + 28 && ctx->cipher) {
            /* === AUD12 2026-08-27 - SPLIT AUDIO FRAMES ===
             *
             * Measured: 100% of the packets refused by the single-packet path
             * have `max_chunks > 0` (2152 out of 2152). A FLAC frame is 595 to
             * 1216 bytes and exceeds what fits in one datagram alongside the
             * header and the encryption overhead: the server cuts it into TWO
             * chunks, of ~1241 and ~18 bytes of plaintext.
             *
             * TWO PROPERTIES, each verified on the hex dump, and each the
             * opposite of what video does:
             *
             *  1. BOTH CHUNKS CARRY `flag10 = 0x01`, so each is encrypted
             *     INDEPENDENTLY and decrypts on its own. There is nothing to
             *     assemble before decryption - it is the PLAINTEXTS that must be
             *     concatenated. On video, the second chunk would be a cleartext
             *     continuation (`flag10 = 0`).
             *
             *  2. BYTES 6-9 DIFFER BETWEEN THE TWO CHUNKS (`08 1c 04 18` then
             *     `18 1c 04 18`). On this channel that field is therefore NOT a
             *     frame id but a send timestamp. And the SUFP reassembler groups
             *     by frame id: it could STRUCTURALLY never pair these chunks,
             *     and its completed-frame counter stayed at zero. That is what
             *     made the previous fix inert.
             *
             * The link between the chunks is byte 1. MIND THE NAME: it is NOT a
             * subchannel here, unlike video. Analysis of the official captures
             * shows it - over 936 packets, the deltas of that byte between
             * consecutive packets are 0 (the duplicate copy) or +1, never
             * anything else: it is a FRAME COUNTER modulo 256. The grouping is
             * correct, but calling it a "subchannel" would send someone looking
             * for a multiplexing scheme that does not exist.
             *
             * Every frame arrives TWICE: the second copy restarts at index 0,
             * which cleanly resets the accumulator, and the downstream
             * anti-duplicate window discards the replayed frame. */
            const int ct_len = (int)n - 11 - 28;
            if (ct_len > 0 && ct_len <= AUD_REASM_MAX) {
                static uint8_t clair[AUD_REASM_MAX];
                memcpy(clair, pkt + 11, ct_len);
                const uint8_t *nonce = pkt + n - 28;
                const uint8_t *tag   = pkt + n - 16;
                if (shadow_cipher_decrypt_unsafe(ctx->cipher, clair, ct_len,
                                                    nonce, tag)) {
                    const uint8_t sub = pkt[1];
                    const uint16_t idx = (uint16_t)pkt[2] | ((uint16_t)pkt[3] << 8);
                    /* ING-A2: the slot logic is aud_reasm.h's; its state,
                     * g_aud_reasm, is reset at every session start. The frame
                     * is handed on before the next chunk is fed. */
                    const uint8_t *whole = NULL;
                    const int whole_len = aud_reasm_chunk(&g_aud_reasm, sub, idx,
                                                          max_chunks, clair, ct_len,
                                                          &whole);
                    if (whole_len > 0) aud_livrer(ctx, whole, whole_len);
                } else {
                    g_aud30.d_dec_ko++;
                }
            }
            g_aud30.d_hdr_ko++;   /* keeps the comparison with the history */
            g_aud30.d_ko_multi++;
        } else {
            /* AUD11 - WHY the packet is refused. The global counter said "3035
             * rejects" without saying which of the four tests failed, and I
             * inferred a different, wrong cause twice in a row. The detail costs
             * four counters.
             * AUD-LC-3 2026-09-11: its six lines are a per-session budget of
             * their own. They used to share `d_hdr_ko`, which the split path
             * above bumps on every packet, so a FLAC session spent them within a
             * few packets without printing one. The number after `#` is this
             * reject's rank in the session - what the first Opus session of a
             * process used to print. */
            g_aud30.d_hdr_ko++;
            if      (type != 0x3)        g_aud30.d_ko_type++;
            else if (max_chunks != 0)    g_aud30.d_ko_multi++;
            else if (flag10 != 0x01)     g_aud30.d_ko_flag++;
            else                         g_aud30.d_ko_court++;
            if (g_aud30.aud11_logged < 6) {
                g_aud30.aud11_logged++;
                char hx[3 * 16 + 1]; int ho = 0;
                for (int i = 0; i < (int)n && i < 16; i++)
                    ho += snprintf(hx + ho, sizeof(hx) - ho, "%02x ", pkt[i]);
                clog("[AUD11] rejet #%u : type=%u max=%u flag10=%02x n=%zu : %s",
                     (unsigned)g_aud30.aud11_logged, type, max_chunks, flag10, n, hx);
            }
        }
    }

    /* Fallback: try sufp reassembly (= if this is the multi-chunk format).
     * ING-A3 2026-09-11: only when SHADOW_AUD_SUFP=1 created the reassembler -
     * see its creation site for why it is off by default. */
    if (ctx->sufp_cursor) sufp_feed(ctx->sufp_cursor, pkt, n);
}

/* ============================================================================
 * Public entry point
 * ==========================================================================*/

/* D4 2026-08-20 - backoff between two channel-open attempts.
 * Reviewed 2026-08-21: sliced into 50 ms pieces with an abort_flag test. The
 * first version did a single 250 ms nanosleep (up to 2 s cumulative), violating
 * KB rule §7.3 - every long-running thread must see abort_flag within 100 ms,
 * otherwise HOS leaks handles and the console must be rebooted. The SSE fix in
 * the same batch quoted that very rule by name.
 * EINTR is handled too: nanosleep was not restarted, so the backoff could be
 * truncated arbitrarily. */
static void chan_backoff_sleep(int ms, const volatile int *abort_flag) {
    while (ms > 0) {
        if (abort_flag && *abort_flag) return;
        int slice = ms > 50 ? 50 : ms;
        struct timespec ts = {0, (long)slice * 1000000L}, rem;
        while (nanosleep(&ts, &rem) == -1 && errno == EINTR) ts = rem;
        ms -= slice;
    }
}

/* The bootstrap handshake: steps 2 to 6 of the sequence documented in
 * CLAUDE.md - session ids, Capabilities, Authentication, Encryption, Heartbeat
 * seq=3, RegisterSession. Deviate from that order and the server refuses the
 * binding.
 *
 * Extracted from ctrl_session_run on 2026-08-25: it produces only THREE values
 * - the 20 B authentication hash, the Encryption reply (which carries the server
 * key) and our own client key - which makes it the cleanest cut available in
 * that 1719-line function. The eight failure paths return `false` where the
 * original body did `goto cleanup`, and `stats->exit_reason` is filled in as
 * before. */
/* L2 2026-08-25 - stopwatch on the bootstrap round trips.
 *
 * Every handshake step is a send followed by a SYNCHRONOUS receive on the
 * control channel: this is therefore the only place in the client where we
 * measure a real server round trip without changing anything in the protocol.
 * Capabilities is the most revealing - the server does almost nothing for it, so
 * its time approaches the pure network path (plus TLS encryption).
 * The others include server-side work: session creation, key derivation,
 * channel allocation. */
static long long hs_now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool session_handshake(const ctrl_session_params *p,
                              ctrl_tcp_session *tcp,
                              ctrl_session_stats *stats,
                              int width, int height,
                              shadow_auth_reply *out_auth,
                              shadow_encryption_reply *out_enc,
                              uint8_t out_client_key[32])
{
    /* Step 2: sessionId / connectionId */
    char session_id[37], connection_id[37];
    if (!gen_uuid_v4(session_id) || !gen_uuid_v4(connection_id)) {
        emit_progress(p, "M9.fail", "uuid gen FAIL"); stats->exit_reason = 1; return false;
    }

    /* Step 3: Capabilities + reply */
    emit_progress(p, "M9.cap", "send Capabilities");
    long long t_M9 = hs_now_ms();
    uint8_t buf[8192]; size_t reply_len = 0;
    int n = ctrl_build_capabilities(buf, sizeof(buf),
        "12.3.3", "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3");
    if (n < 0 || !ctrl_tcp_send_cleartext(tcp, buf, (size_t)n)
        || !ctrl_tcp_recv_cleartext(tcp, buf, sizeof(buf), &reply_len, 8000)) {
        emit_progress(p, "M9.fail", "Capabilities flow FAIL");
        stats->exit_reason = 1; return false;
    }
    clog("[L2] aller-retour Capabilities     = %lld ms", hs_now_ms() - t_M9);
    /* SRV8: the reply carries the ShadowStreamer version. Every byte-exact
     * decision in this repo is dated against ONE server build, so the build is
     * worth one line per session - it is the first thing anyone will ask for
     * the day a VM stops behaving. */
    {
        unsigned smaj = 0, smin = 0, spat = 0;
        if (ctrl_parse_capabilities_reply(buf, reply_len, &smaj, &smin, &spat)) {
            clog("[SRV8] ShadowStreamer side serveur : %u.%u.%u", smaj, smin, spat);
            /* INT1: into the snapshot too, so a client can gate on the build
             * instead of asking. This runs BEFORE caps_begin_session(), which
             * is why that function carries these three fields across its
             * memset instead of zeroing them. */
            g_caps.srv_major = smaj; g_caps.srv_minor = smin; g_caps.srv_patch = spat;
        }
        else
            /* No %z: the Vita's newlib does not consume the argument for it
             * (tools/check-z-formats.py). */
            clog("[SRV8] version serveur illisible dans la reponse Capabilities "
                 "(%u octets) - format change, or not 6.3.1", (unsigned)reply_len);
    }

    /* Step 4: Authentication + reply (extracts the 20 B hash) */
    emit_progress(p, "M10.auth", "send Authentication");
    long long t_M10 = hs_now_ms();
    /* S4 diagnostic 2026-08-21 - dump the Authentication message so it can be
     * diffed against the desktop client
     * (SHADOW_DUMP_AUTH=1 -> ./halyard-data/our_auth.bin). */
    {
        /* S4 2026-08-21: SHADOW_AUTH_PERMS allows going back to the old partial
         * mask (250) should 0x1fe ever regress. */
        static int g_perms = -1;
        if (g_perms < 0) {
            const char *e = getenv("SHADOW_AUTH_PERMS");
            g_perms = e ? (int)strtol(e, NULL, 0) : SHADOW_PERM_ALL;
        }
        n = ctrl_build_authentication(buf, sizeof(buf), false, (uint32_t)g_perms,
            p->streaming_token, p->client_id, session_id, connection_id);
    }
    if (n > 0 && getenv("SHADOW_DUMP_AUTH")) {
        FILE *af = fopen("./halyard-data/our_auth.bin", "wb");
        if (af) { fwrite(buf, 1, (size_t)n, af); fclose(af); }
    }
    if (n < 0 || !ctrl_tcp_send_cleartext(tcp, buf, (size_t)n)
        || !ctrl_tcp_recv_cleartext(tcp, buf, sizeof(buf), &reply_len, 8000)) {
        emit_progress(p, "M10.fail", "Auth flow FAIL");
        stats->exit_reason = 1; return false;
    }
    clog("[L2] aller-retour Authentication   = %lld ms", hs_now_ms() - t_M10);
    shadow_auth_reply auth_reply = {0};
    if (getenv("SHADOW_DUMP_AUTH")) {
        FILE *rf = fopen("./halyard-data/our_auth_reply.bin", "wb");
        if (rf) { fwrite(buf, 1, reply_len, rf); fclose(rf); }
        clog("[S17] reponse Authentication : %zu B dumpee",
             reply_len);
    }
    if (!ctrl_parse_authentication_reply_v2(buf, reply_len, &auth_reply)
        || !auth_reply.has_hash) {
        emit_progress(p, "M10.fail", "Auth hash 20B not found");
        stats->exit_reason = 1; return false;
    }

    /* Step 5: Encryption + reply (extracts the 32 B key) */
    emit_progress(p, "M11.enc", "send Encryption");
    long long t_M11 = hs_now_ms();
    uint32_t algos[] = {SHADOW_ALG_NONE, 2, 4, SHADOW_ALG_CHACHA20_POLY1305};
    uint8_t client_key[32] = {0};
    n = ctrl_build_encryption_request(buf, sizeof(buf), algos, 4, client_key);
    if (n < 0 || !ctrl_tcp_send_cleartext(tcp, buf, (size_t)n)
        || !ctrl_tcp_recv_cleartext(tcp, buf, sizeof(buf), &reply_len, 8000)) {
        emit_progress(p, "M11.fail", "Encryption flow FAIL");
        stats->exit_reason = 1; return false;
    }
    clog("[L2] aller-retour Encryption       = %lld ms", hs_now_ms() - t_M11);
    shadow_encryption_reply enc_reply = {0};
    if (!ctrl_parse_encryption_reply(buf, reply_len, &enc_reply) || !enc_reply.has_key) {
        emit_progress(p, "M11.fail", "Encryption key not extracted");
        stats->exit_reason = 1; return false;
    }

    /* === S42 2026-08-25 - TWO MESSAGES THE OFFICIAL CLIENT NEVER SENDS ===
     *
     * Diff of the `captures_audio_20260821_223137` capture (the only one with
     * sound) against our own bootstrap, payload sizes:
     *
     *   desktop: 87  245  127            115  99  99  103  95  97  103  99  89
     *   us     : 87  245  127   89  499  115  99  99  103  95  97  103  99  89
     *                           ^^^^^^^^
     *
     * The whole tail matches to the byte. But we insert a Heartbeat (seq 3) and
     * a RegisterSession (seq 4) that the official client NEVER emits at that
     * point - and since every message carries its sequence number, those two
     * intruders SHIFT the whole numbering: our channel announcements go out as
     * 5..12 where the official client numbers them 3..10.
     *
     * And those announcements are precisely what declares the channels,
     * including audio - which, for us, never starts (`:base+30` silent, 3.29).
     *
     * The original comment justified the heartbeat with "without it the server
     * falls back to top-slice-only degraded mode". That observation dates from
     * 2026-05-09, BEFORE G4: the "top slice only" was in fact our own index bug
     * (we dropped the last chunk of every frame). The justification is therefore
     * void, but we do not flip a default without a measurement:
     * `SHADOW_BOOTSTRAP_EXACT=1` removes both messages AND renumbers the
     * announcements to 3..10, to match the capture. To be compared on the
     * console through `env.txt`. */
    static int g_boot_exact = -1;
    if (g_boot_exact < 0) {
        const char *e = getenv("SHADOW_BOOTSTRAP_EXACT");
        g_boot_exact = e ? atoi(e) : 0;
    }

    if (!g_boot_exact) {
        /* Step 5b: Heartbeat seq=3 (= between Encryption seq=2 and Register seq=4). */
        emit_progress(p, "M11b.hb", "send Heartbeat seq=3");
        n = ctrl_build_heartbeat(buf, sizeof(buf), 3);
        if (n < 0 || !ctrl_tcp_send_cleartext(tcp, buf, (size_t)n)) {
            emit_progress(p, "M11b.fail", "Heartbeat seq=3 FAIL");
            stats->exit_reason = 1; return false;
        }

        /* Step 6: RegisterSession (= seq=4) */
        emit_progress(p, "M12.reg", "send RegisterSession");
        long long t_M12 = hs_now_ms();
        n = ctrl_build_register_session(buf, sizeof(buf), (uint32_t)width, (uint32_t)height);
        if (n < 0 || !ctrl_tcp_send_cleartext(tcp, buf, (size_t)n)
            || !ctrl_tcp_recv_cleartext(tcp, buf, sizeof(buf), &reply_len, 8000)) {
            emit_progress(p, "M12.fail", "RegisterSession FAIL");
            stats->exit_reason = 1; return false;
        }
        clog("[L2] aller-retour RegisterSession  = %lld ms", hs_now_ms() - t_M12);
    } else {
        clog("[S42] amorcage byte-exact : Heartbeat seq=3 et RegisterSession "
             "removed, announcements renumbered 3..10 (like the desktop capture)");
    }

    *out_auth = auth_reply;
    *out_enc  = enc_reply;
    memcpy(out_client_key, client_key, 32);   /* K11 : sert de clef Tx */
    return true;
}

/* Bootstrap step 7: the eight channel announcements (f1 = 5..12), sent in this
 * order after RegisterSession. Their bodies are byte-exact captures of the
 * official client (ctrl_msgs.c); only the video channel's body is parameterised,
 * to carry the requested resolution, frame rate and bitrate.
 *
 * Extracted from ctrl_session_run on 2026-08-25. It produces NOTHING: everything
 * it builds dies with it, which makes it a risk-free cut. */
/* === INT1 2026-10-02 - the public snapshot of what the server granted =======
 *
 * See `session_caps.h` for the contract and for why the file-transfer secret is
 * NOT a field of it. Module state, not session state in the CLAUDE.md sense:
 * it describes the session that is running and is republished wholesale at each
 * bootstrap, with `generation` so a caller can tell a stale copy. The secret
 * lives beside it rather than inside it, and is wiped when a new session
 * publishes.
 *
 * Written only during the bootstrap, before anything can observe the session as
 * active, and read-only afterwards - which is what makes the lockless read in
 * `ctrl_session_caps()` sound. */

static void caps_begin_session(int port_base)
{
    const uint32_t gen = g_caps.generation + 1u;
    /* The Capabilities reply arrives BEFORE this point in the bootstrap, so the
     * server version is already in the snapshot and a plain memset would erase
     * it. Carried across, like `generation`. (The first version of this left a
     * comment saying the wipe would happen and did nothing about it.) */
    const unsigned maj = g_caps.srv_major, min_ = g_caps.srv_minor,
                   pat = g_caps.srv_patch;
    /* Wipe the previous session's secret before anything else: a snapshot that
     * outlived its session must not carry a live credential. */
    for (size_t z = 0; z < sizeof g_ft_secret; z++)
        ((volatile char *)g_ft_secret)[z] = 0;
    g_ft_secret_len = 0;
    g_ft_host[0] = '\0';   /* FT4: the host dies with the secret it completes */
    memset(&g_caps, 0, sizeof g_caps);
    g_caps.generation = gen;
    g_caps.port_base  = port_base;
    g_caps.srv_major  = maj;
    g_caps.srv_minor  = min_;
    g_caps.srv_patch  = pat;
}

static void caps_note_grant(const ann_reply_t *ar, int port_base)
{
    const int bi = shadow_chan_idx_from_ann(ar->channel);
    if (bi < 0) return;
    shadow_chan_caps *c = &g_caps.chan[bi];
    if (!c->granted) g_caps.n_granted++;
    c->granted = true;
    c->tcp     = ar->tcp ? true : false;
    /* The absolute port, resolved ONCE here. The reply's own field reads
     * 7000+offset, which is not the live base - the trap that sent the
     * file-transfer self-test to port 7015 while the channel listened on
     * 14015. */
    c->port    = (uint16_t)(port_base + ann_reply_port_offset(ar));
    c->handle  = ar->have_session ? ar->handle : 0;

    if (ar->channel == ANN_CHAN_VIDEO && ar->have_mode) {
        g_caps.video_width       = ar->width;
        g_caps.video_height      = ar->height;
        g_caps.video_fps         = ar->fps;
        g_caps.video_bitrate_bps = ar->bitrate_bps;
        g_caps.video_codec       = ar->codec;
    }
    if (ar->have_audio && ar->channel == ANN_CHAN_AUDIO) {
        g_caps.audio_sample_rate = ar->sample_rate;
        g_caps.audio_bits        = ar->bits;
        g_caps.audio_codec       = ar->audio_codec;
    }
}

bool ctrl_session_caps(shadow_session_caps *out)
{
    if (!out) return false;
    *out = g_caps;
    return g_caps.generation != 0 && g_caps.n_granted > 0;
}

bool ctrl_session_file_transfer_secret(char *out, size_t cap, size_t *n)
{
    if (n) *n = 0;
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (g_ft_secret_len == 0) return false;
    if (!g_caps.chan[SHADOW_CHAN_IDX_FILETRANSFER].granted) return false;
    /* Refuse rather than truncate: half a credential is a confusing failure,
     * and a caller that sized its buffer wrongly needs to know. */
    if (g_ft_secret_len + 1 > cap) return false;
    memcpy(out, g_ft_secret, g_ft_secret_len);
    out[g_ft_secret_len] = '\0';
    if (n) *n = g_ft_secret_len;
    return true;
}

bool ctrl_session_file_transfer_uri(char *out, size_t cap, size_t *n)
{
    if (n) *n = 0;
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (g_ft_secret_len == 0 || !g_ft_host[0]) return false;
    const shadow_chan_caps *cc = &g_caps.chan[SHADOW_CHAN_IDX_FILETRANSFER];
    if (!cc->granted) return false;

    /* `ft_uri_build` refuses rather than truncates, so a short buffer gives an
     * empty string and false - exactly what this function promises. */
    const size_t w = ft_uri_build(out, cap, g_ft_host, (int)cc->port,
                                  "shadow", g_ft_secret, g_ft_secret_len);
    if (w == 0) { out[0] = '\0'; return false; }
    if (n) *n = w;
    return true;
}

/* === FT2 2026-10-02 - the file-transfer self-test ===========================
 *
 * The announcement reply is the ONE place the SSH password for `:base+15` is in
 * hand, so it is the only place the self-test can run without storing the
 * credential anywhere. Off unless SHADOW_FT_SELFTEST=1, and compiled to nothing
 * unless the build asked for -DSHADOW_FILETRANSFER=ON.
 *
 * The secret lives in a stack buffer for the duration of the call and is zeroed
 * on the way out: never written to disk, never logged, never handed to a
 * separate harness that would have to be given it. */
/* INT1: keep the file-transfer secret for `ctrl_session_file_transfer_secret()`.
 * This is the one place the reply is in hand. Stored beside the snapshot and
 * never inside it - a struct callers copy and serialise is where a credential
 * would leak. Wiped by `caps_begin_session` on the next bootstrap. */
static void caps_note_ft_host(const char *host)
{
    if (!host || !host[0]) { g_ft_host[0] = '\0'; return; }
    size_t i = 0;
    while (host[i] && i + 1 < sizeof g_ft_host) { g_ft_host[i] = host[i]; i++; }
    g_ft_host[i] = '\0';
}

static void caps_note_ft_secret(const uint8_t *reply, size_t reply_len)
{
    char tmp[sizeof g_ft_secret]; size_t tn = 0;
    if (shadow_ft_secret_from_reply(reply, reply_len, tmp, sizeof tmp, &tn)
        && tn > 0 && tn < sizeof g_ft_secret) {
        memcpy(g_ft_secret, tmp, tn);
        g_ft_secret_len = tn;
        clog("[INT1] file-transfer credential held for this session (%u bytes, "
             "never logged); ctrl_session_file_transfer_secret() hands it over",
             (unsigned)tn);
    }
    for (size_t z = 0; z < sizeof tmp; z++) ((volatile char *)tmp)[z] = 0;
}

static void session_ft_selftest(const ctrl_session_params *p,
                                const uint8_t *reply, size_t reply_len,
                                const ann_reply_t *ar)
{
    static int g_ft_selftest = -1;
    if (g_ft_selftest < 0) {
        const char *e = getenv("SHADOW_FT_SELFTEST");
        g_ft_selftest = e ? atoi(e) : 0;
    }
    if (!g_ft_selftest) return;

    char secret[1024]; size_t sn = 0;
    if (shadow_ft_secret_from_reply(reply, reply_len, secret, sizeof secret, &sn)) {
        /* === FT2 2026-10-02 - THE BASE IS NOT 7000 =========================
         * First version hardcoded `7000 + offset` from the §3.37 note that the
         * reply's port field reads `7000 + offset`. But that is the number the
         * SERVER puts in the field, not the port to dial: the live base on this
         * VM is 9000 (vm.port 2000 + 7000), so the self-test knocked on 7015
         * and got "TCP or SSH transport failed" on a channel that was granted
         * and listening. `p->port_base` is the base every other channel in this
         * client already uses. */
        const int ftport = p->port_base + ann_reply_port_offset(ar);
        clog("[FT2] running the SFTP self-test on :%d (secret %u bytes, never logged)",
             ftport, (unsigned)sn);
        shadow_ft_selftest(p->vm_host, (uint16_t)ftport, secret, sn);
    } else {
        clog("[FT2] the file-transfer reply carries no secret field "
             "(%u bytes) - nothing to test against", (unsigned)reply_len);
    }
    for (size_t z = 0; z < sizeof secret; z++) ((volatile char *)secret)[z] = 0;
}

/* === FT3 2026-10-02 - HANDING THE SFTP CREDENTIAL TO A THIRD-PARTY CLIENT ===
 *
 * The VM's file transfer is plain SFTP on `:base+15`, password auth, and the
 * password is the 395-byte field of the file-transfer announcement reply. Which
 * means WinSCP, FileZilla or `sftp` can drive it directly - there is nothing
 * proprietary left once you have host, port and password.
 *
 * What stopped that being usable is that we deliberately never wrote the
 * password ANYWHERE: `ctrl_session_file_transfer_secret()` hands it to a caller
 * in memory, and nothing in `clients/` calls it. So the capability existed and
 * was out of reach - the same family as the six settings corrected on
 * 2026-08-27.
 *
 * WHY A FILE AND NOT THE LOG. The log is the thing that gets pasted into an
 * issue, shipped to a PC by `devlink`, and read by `journal`'s redaction rules.
 * A credential that grants READ AND WRITE to the VM's entire filesystem (the
 * server does not confine SFTP paths, KB §3.50) has no business in it. A
 * separate file the user goes and opens deliberately is a different act from
 * reading a log, and that difference is the whole safeguard.
 *
 * SHADOW_FT_REVEAL=1 arms it; it is OFF by default and the log says only that
 * the file was written, never what is in it. The file is REMOVED at the end of
 * the session, because the secret dies with the session anyway and a stale one
 * invites a confusing failure rather than a breach. A crash leaves it behind -
 * stated here rather than pretended otherwise. */

#define FT_REVEAL_PATH SHADOW_DATA_DIR "sftp.txt"
/* FT5 2026-10-02 - the credential ON ITS OWN, byte for byte.
 *
 * The first version wrote `password = <395 bytes>` into the file above. That
 * file is `key = value` lines, and the credential CONTAINS NEWLINES - so
 * reading "the password line" yielded `-----BEGIN OPENSSH PRIVATE KEY-----`
 * and nothing more. Pasted into WinSCP that is 35 bytes of a 395-byte secret,
 * which is exactly the "Erreur d'authentification" that was reported. A format
 * that cannot represent its own content is the defect, not the client.
 *
 * So the bytes get a file of their own, with nothing else in it: no header, no
 * label, no trailing anything. `wc -c` on it must say 395. */
#define FT_SECRET_PATH SHADOW_DATA_DIR "sftp_password.bin"

static void session_ft_reveal(const ctrl_session_params *p,
                              const uint8_t *reply, size_t reply_len,
                              const ann_reply_t *ar)
{
    static int g_reveal = -1;
    if (g_reveal < 0) {
        const char *e = getenv("SHADOW_FT_REVEAL");
        g_reveal = e ? atoi(e) : 0;
    }
    if (!g_reveal) return;

    char secret[1024]; size_t sn = 0;
    if (!shadow_ft_secret_from_reply(reply, reply_len, secret, sizeof secret, &sn)
        || sn == 0) {
        clog("[FT3] SHADOW_FT_REVEAL=1 but the file-transfer reply carries no "
             "secret - nothing written");
        return;
    }

    const int port = p->port_base + ann_reply_port_offset(ar);

    /* The credential first, alone, byte for byte - see FT_SECRET_PATH. Binary
     * mode so no CRLF translation happens on Windows: the server refuses the
     * CRLF form (measured), so a text-mode write would produce a file that
     * cannot authenticate. */
    int wrote_secret = 0;
    {
        FILE *k = fopen(FT_SECRET_PATH, "wb");
        if (k) {
            wrote_secret = fwrite(secret, 1, sn, k) == sn;
            fclose(k);
        }
    }

    FILE *f = fopen(FT_REVEAL_PATH, "wb");
    if (f) {
        /* Describes the access and POINTS AT the credential; it no longer tries
         * to contain it. What this file is for is being read by a person. */
        fprintf(f,
            "# Halyard - SFTP access to this Shadow VM, for THIS session only.\n"
            "#\n"
            "# WARNING: the credential grants READ AND WRITE on the WHOLE\n"
            "# filesystem of the VM - the server confines no SFTP path. It is\n"
            "# regenerated every session and is useless once this one ends.\n"
            "# Never paste it into an issue, a log or a chat.\n"
            "#\n"
            "# Written because SHADOW_FT_REVEAL=1. Both files are removed when\n"
            "# the session ends normally; delete them by hand after a crash.\n"
            "\n"
            "host     = %s\n"
            "port     = %d\n"
            "user     = shadow\n"
            "password = see %s  (%u bytes, NOT reproduced here)\n"
            "\n"
            "# === WHAT THIS CREDENTIAL IS, AND WHAT CANNOT USE IT ============\n"
            "#\n"
            "# It is an OpenSSH PRIVATE KEY in PEM form, used as a PASSWORD.\n"
            "# The server is libssh_0.11.0 and offers `password` only - there is\n"
            "# no publickey method to offer the key to. Any user name works; the\n"
            "# server looks only at the password.\n"
            "#\n"
            "# It must be passed BYTE-EXACT: %u bytes, two embedded newlines,\n"
            "# and a TRAILING newline. Measured against the live server on\n"
            "# 2026-10-02 - dropping the trailing newline is refused, and so is\n"
            "# every single-line rewriting of it (newlines as spaces, newlines\n"
            "# removed, newlines as a literal backslash-n, the base64 body\n"
            "# alone, the body plus a newline).\n"
            "#\n"
            "# CONSEQUENCE: a GUI password box is one line, so WinSCP and\n"
            "# FileZilla CANNOT take this. Nor can a URI - curl calls such a URI\n"
            "# malformed - and curl/libssh2 refuses the credential even passed\n"
            "# as an argument, with the same bytes libssh accepts.\n"
            "#\n"
            "# What works is a library call: `ssh_userauth_password(s, NULL,\n"
            "# <the %u bytes>)` with libssh, which is what Halyard's own\n"
            "# core/services/filetransfer.c does. Script against that, or\n"
            "# against any client that can read a password from a FILE.\n"
            "#\n"
            "# There is no host key to trust - it is generated per session too.\n",
            p->vm_host ? p->vm_host : "?", port,
            wrote_secret ? FT_SECRET_PATH : "(COULD NOT BE WRITTEN)",
            (unsigned)sn, (unsigned)sn, (unsigned)sn);
        fclose(f);
        clog("[FT3] SFTP access for :%d described in %s, credential (%u bytes, "
             "byte-exact) in %s - both grant full read/write on the VM and are "
             "removed when the session ends",
             port, FT_REVEAL_PATH, (unsigned)sn, FT_SECRET_PATH);
    } else {
        clog("[FT3] could not write %s", FT_REVEAL_PATH);
    }
    for (size_t z = 0; z < sizeof secret; z++) ((volatile char *)secret)[z] = 0;
}

static void session_ft_reveal_clear(void)
{
    /* FT5: the credential file goes first - it is the one that matters. */
    (void)remove(FT_SECRET_PATH);
    /* Unconditional: the toggle may have been turned off since, and a file left
     * from an earlier armed session is exactly what should not survive. remove()
     * failing because there is nothing there is the normal case, hence no log. */
    (void)remove(FT_REVEAL_PATH);
}

static bool session_announce_channels(const ctrl_session_params *p,
                                      ctrl_tcp_session *tcp,
                                      ctrl_session_stats *stats,
                                      session_ctx_t *ctx,
                                      int width, int height,
                                      uint8_t *buf, size_t buf_cap)
{
    /* CAREFUL: `buf` is a POINTER here, not the caller's array. `sizeof(buf)`
     * is 8 in this scope, not 8192 - that is exactly the regression introduced
     * when this function was extracted on 2026-08-25, and it made all eight
     * announcements fail INSTANTLY (capacity 8 bytes). A measurement session
     * caught it; no test covers this path. Use `buf_cap`, never `sizeof(buf)`. */
    size_t reply_len = 0;
    int n = 0;
    caps_begin_session(p->port_base);   /* INT1: publish a fresh snapshot */
    /* Step 7: 8 channel announcements (f1=5..12).
     * Q1 2026-05-18: for chan_idx=0 (= video, seq=5), max_bitrate/fps/resolution
     * can be overridden through the custom params OR the env vars. */
    emit_progress(p, "M13.ann", "send 8 channel announcements");
    ctrl_video_params_t vparams = {
        .width           = (uint32_t)width,
        .height          = (uint32_t)height,
        /* F16 REVERT 2026-05-22 23h15: back to 143.85f. A long run (60 s @
         * 144 fps) shows better quality over time than 60 fps. The initial
         * "green blocks" are a first-IDR problem, to be fixed separately with
         * wait-for-clean-IDR (F17). Override with SHADOW_FPS=60 if a compromise
         * is needed. */
        .fps             = FPS_CLIENT_DEFAULT,
        .max_bitrate_bps = BITRATE_CLIENT_DEFAULT_MBPS * 1000000U,  /* B1: one place */
        .codec           = 2,              /* desktop hardcoded (= presumed H.264) */
        .profile         = 1,              /* desktop hardcoded (= speed/low-latency) */
        /* RE3 2026-05-18 - defaults false (= V14 byte-exact baseline, no wire effect) */
        .cursor_merged       = false,
        .high_color_fidelity = false,
        .hdr_enabled         = false,
        .vr_enabled          = false,
        /* F20 REVERT 2026-05-23 00h05: F5=1 as the default broke the GUI
         * ("Waiting for video..." indefinitely) even though test-cli worked.
         * Reason unknown (probably some timing/handshake specific to F5 mode).
         * F5 stays OPT-IN through SHADOW_REG_F5=1 for anyone who wants to try. */
        .re8_f4  = false,
        .re8_f5  = false,
        .re8_f6  = false,
        .re8_f8  = false,
        .re8_f10 = false,
    };
    /* params struct overrides */
    if (p->target_fps > 0) vparams.fps = p->target_fps;
    if (p->max_bitrate_mbps > 0) vparams.max_bitrate_bps = p->max_bitrate_mbps * 1000000U;
    /* env var overrides (highest priority) */
    {
        const char *e;
        if ((e = getenv("SHADOW_BITRATE_MBPS")) && atoi(e) > 0)
            vparams.max_bitrate_bps = (uint32_t)atoi(e) * 1000000U;
        if ((e = getenv("SHADOW_FPS")) && atof(e) > 0)
            vparams.fps = (float)atof(e);
        g_announced_cap_mbps = vparams.max_bitrate_bps / 1000000u;   /* CFG-1: G19's base cap */
        /* === K14 2026-08-27 - AUDIO CODEC: OPUS OR FLAC ===
         * The official client's "High fidelity" is not a bitrate, it is a
         * CODEC: lossless FLAC instead of Opus. A single byte of the audio
         * announcement decides it, and the account already allows it
         * (`capabilities ... codecs=opus,flac`). The receiving decoder
         * recognises it from its sync word and sticks to it for the session. */
        {
            const char *ea = getenv("SHADOW_AUDIO_CODEC");
            const uint32_t ac = ea ? (uint32_t)atoi(ea) : 1u;
            ctrl_msgs_set_audio_codec(ac);
            /* K18b - SAY WHAT WE ASK FOR. Without this line, "FLAC does not
             * work" is indistinguishable from "FLAC was never requested" - and
             * that is exactly the ambiguity we just spent ten minutes resolving
             * by hand. */
            clog("[K14] audio codec requested: %s (SHADOW_AUDIO_CODEC=%s)",
                 ac == 2 ? "FLAC" : "Opus", ea ? ea : "absent");
        }

        /* === K13 2026-08-27 - SHADOW_CODEC WROTE INTO THE VOID ===
         * It fed `vparams.codec`, a field the guided capture shows to be
         * INVARIANT between an H.264 request and an H.265 request from the
         * official client. The Quality screen's "Codec" setting therefore had no
         * effect at all, while offering H.265 and AV1.
         * It now drives `codec_wire`, field 3 at the video level, with the WIRE
         * values: 0 = H.264, 1 = H.265, 2 = AV1.
         * `SHADOW_CODEC_LEGACY` remains available to write the old field, in
         * case its (unknown) meaning is ever found. */
        if ((e = getenv("SHADOW_CODEC_LEGACY")) && atoi(e) > 0)
            vparams.codec = (uint32_t)atoi(e);
        if ((e = getenv("SHADOW_CODEC")) && atoi(e) >= 0)
            vparams.codec_wire = (uint32_t)atoi(e);
        if ((e = getenv("SHADOW_PROFILE_ID")) && atoi(e) > 0)
            vparams.profile = (uint32_t)atoi(e);
        /* RE3 2026-05-18 - the 4 missing bools (no-op on the wire, kept for API compat) */
        if ((e = getenv("SHADOW_CURSOR_MERGED")) && atoi(e) == 1)
            vparams.cursor_merged = true;
        if ((e = getenv("SHADOW_HIGH_COLOR_FIDELITY")) && atoi(e) == 1)
            vparams.high_color_fidelity = true;
        if ((e = getenv("SHADOW_HDR")) && atoi(e) == 1)
            vparams.hdr_enabled = true;
        if ((e = getenv("SHADOW_VR")) && atoi(e) == 1)
            vparams.vr_enabled = true;
        /* RE8 2026-05-18 - 5 bool fields, C95 wire confidence.
         * Each can be enabled individually to A/B the multi-NAL trigger
         * hypothesis. */
        if ((e = getenv("SHADOW_REG_F4"))  && atoi(e) == 1) vparams.re8_f4  = true;
        if ((e = getenv("SHADOW_REG_F5"))  && atoi(e) == 1) vparams.re8_f5  = true;
        if ((e = getenv("SHADOW_REG_F6"))  && atoi(e) == 1) vparams.re8_f6  = true;
        if ((e = getenv("SHADOW_REG_F8"))  && atoi(e) == 1) vparams.re8_f8  = true;
        if ((e = getenv("SHADOW_REG_F10")) && atoi(e) == 1) vparams.re8_f10 = true;
    }
    clog("[Q1] channel video params: %ux%u @ %.1f fps @ %.1f Mbps codec=%u profile=%u re8=[%d%d%d%d%d]",
         vparams.width, vparams.height, (double)vparams.fps,
         (double)vparams.max_bitrate_bps / 1e6, vparams.codec_wire, vparams.profile,
         vparams.re8_f4, vparams.re8_f5, vparams.re8_f6, vparams.re8_f8, vparams.re8_f10);

    /* S42: the official client numbers its eight announcements 3..10; we use
     * 5..12, because two extra messages (Heartbeat, RegisterSession) consumed 3
     * and 4. When they are removed, the numbering must follow - otherwise we
     * keep the very offset we were trying to correct. */
    static int g_boot_exact_ann = -1;
    if (g_boot_exact_ann < 0) {
        const char *e = getenv("SHADOW_BOOTSTRAP_EXACT");
        g_boot_exact_ann = e ? atoi(e) : 0;
    }
    const int seq0 = g_boot_exact_ann ? 3 : 5;
    for (int seq = seq0; seq <= seq0 + 7; seq++) {
        int chan_idx = seq - seq0;
        const ctrl_video_params_t *vp = (chan_idx == 0) ? &vparams : NULL;
        n = ctrl_build_channel_announcement_ex(buf, buf_cap, seq, chan_idx, vp);
        if (n < 0 || !ctrl_tcp_send_cleartext(tcp, buf, (size_t)n)) {
            emit_progress(p, "M13.fail", "channel announcement FAIL");
            stats->exit_reason = 1; return false;
        }
    }
    /* Drain the reply (the server answers once after the batch).
     * H1 V8 - hex dump of the reply (probably the server-side encoder profile
     * that was chosen). See tools/ida/out/H1_V8_unexplored.md */
    if (ctrl_tcp_recv_cleartext(tcp, buf, buf_cap, &reply_len, 3000)) {
        /* === SEC1 2026-08-28 - THIS DUMP EXPOSED A PRIVATE KEY ===
         *
         * The reply to the batch of eight announcements contains, for the SFTP
         * channel `:base+15`, an **ed25519 OpenSSH private key IN CLEARTEXT**:
         * it is the session material the server entrusts to us. This block
         * UNCONDITIONALLY dumped its first 512 bytes, in hex THEN in ascii, on
         * every session. Every log produced so far contains it, twice - and a
         * log gets forwarded, archived, pasted into a bug report.
         *
         * The dump existed to explore the reply (H1.V8, May 2026); its content
         * has since been fully decoded - supported modes (FPS1), port and
         * transport granted per channel (K15b). It teaches nothing any more.
         *
         * So we bound it to 24 bytes, which covers the field tag, the
         * `sessionId` and the port - everything we still read by eye - and we
         * REFUSE any body of 400 bytes or more, the size below which the key
         * cannot fit. `SHADOW_DUMP_M13=1` restores the full dump for a
         * reverse-engineering session, knowingly.
         *
         * The programmatic analysis below (mode extraction) is untouched: it
         * reads `buf` directly and writes nothing to the log. */
        static int g_dump_m13 = -1;
        if (g_dump_m13 < 0) {
            const char *e = getenv("SHADOW_DUMP_M13");
            g_dump_m13 = e ? atoi(e) : 0;
        }
        const int porte_cle = (reply_len >= 400);
        size_t dump_len = g_dump_m13 ? (reply_len < 512 ? reply_len : 512)
                                     : (porte_cle ? 0
                                        : (reply_len < 24 ? reply_len : 24));
        if (dump_len == 0) {
            clog("[H1.V8] M13 reply len=%zu — vidage SUPPRIME (>=400 o : ce corps "
                 "carries the SFTP channel's private key, cf. SEC1)", reply_len);
        } else {
            char hex[2048]; int ho = 0;
            for (size_t i = 0; i < dump_len && ho < (int)sizeof(hex) - 4; i++) {
                ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x ", buf[i]);
            }
            clog("[H1.V8] M13 reply len=%zu hex(%zu premiers)=%s",
                 reply_len, dump_len, hex);
        }
        /* FPS1 2026-05-18: extract the supported modes from the reply.
         * Pattern: `22 0B 08 <varint W> 10 <varint H> 1D <fixed32 fps>`
         * = field 4 wire 2 len 11 -> mode { f1=w, f2=h, f3=fps fixed32 }.
         * === L18 2026-08-29 - "THE 1st IS THE CURRENT MODE" IS FALSE ===
         * That annotation dated from FPS1 (2026-05-18) and had never been
         * checked. The session itself refutes it:
         *   requested: 1280x720 @ 120 fps       (`[Q1]`)
         *   decoded  : 1280x720                 (`h264: frame`)
         *   mode #1  : 2560x1440 @ 59.94        (this list)
         * We ask for and receive 720p, and the first entry announces 1440p. So
         * it is neither what was accepted nor what the VM renders - the list in
         * fact contains four 1280x720 entries, at positions 2, 6, 27 and 67. It
         * is a list of SUPPORTED modes, in no established order.
         *
         * The annotation cost us: it made me conclude that the VM rendered in
         * 1440p at 59.94 and that the frame rate was capped there at 60 - two
         * false claims, the second refuted by a session averaging 68.9 fps. An
         * unverified comment reads like a measurement.
         *
         * What is established: these 74 entries are what we MAY ask for. How the
         * server chooses, and where to read its choice, remains open. */
        int mode_count = 0;
        for (size_t i = 0; i + 13 <= reply_len; i++) {
            if (buf[i] != 0x22 || buf[i+1] != 0x0B || buf[i+2] != 0x08) continue;
            size_t j = i + 3;
            uint32_t w = 0; int shift = 0;
            while (j < reply_len && shift < 28) {
                w |= (uint32_t)(buf[j] & 0x7F) << shift;
                if (!(buf[j] & 0x80)) { j++; break; }
                shift += 7; j++;
            }
            if (j >= reply_len || buf[j] != 0x10) continue;
            j++;
            uint32_t h = 0; shift = 0;
            while (j < reply_len && shift < 28) {
                h |= (uint32_t)(buf[j] & 0x7F) << shift;
                if (!(buf[j] & 0x80)) { j++; break; }
                shift += 7; j++;
            }
            if (j + 5 > reply_len || buf[j] != 0x1D) continue;
            float fps;
            memcpy(&fps, buf + j + 1, 4);
            if (w >= 256 && w <= 8192 && h >= 144 && h <= 4320
                && fps > 1.0f && fps < 500.0f) {
                clog("[FPS1] server mode supported #%d: %ux%u @ %.2f fps",
                     mode_count + 1, w, h, (double)fps);
                mode_count++;
            }
            i = j + 4;
        }
        clog("[FPS1] %d modes supported by the server (none is marked as "
             "courant — voir L18)", mode_count);
    }

    /* === K12 2026-08-27 - THE SERVER ANSWERS EIGHT TIMES, NOT ONCE ===
     *
     * The comment above claims "the server answers once after the batch". That
     * is FALSE: it answers EACH of the eight announcements, and we threw the
     * other seven away. They went out with the drain, truncated to four bytes by
     * a capped log line.
     *
     * What they contain is not anecdotal: from them, the official client prints
     * one line per channel of the form
     *
     *     control chan video session granted : 1280x720@59.912 - H264 - UDP - SUFP
     *     control chan cursor session granted : TCP - SUFP
     *
     * - that is, channel by channel, the CODEC, the TRANSPORT and the PROTOCOL
     * granted. In other words the server always told us that video was over UDP
     * and that the cursor was on a separate channel: the campaign that corrected
     * the channel map would have been free had we read these bytes.
     *
     * It is also the only place where the "reliability" mode's port will be
     * readable the day we capture it, and what will settle the audio codec
     * question (Opus or FLAC) without a capture of the official client.
     *
     * TIME BOUND: the extra replies are already in flight, so we wait 150 ms for
     * each and stop at the first one missing. Worst case adds 150 ms to startup,
     * no more - waiting 3 s per reply as we do for the first would cost 21. */
    {
        static int g_ann_replies = -1;
        if (g_ann_replies < 0) {
            const char *e = getenv("SHADOW_ANN_REPLIES");
            g_ann_replies = e ? atoi(e) : 1;
        }
        if (g_ann_replies) {
            /* Names aligned on the bootstrap's field-to-channel mapping:
             * f1 Video, f2 Audio, f3 Input, f4 Cursor, f5 Micro, f6 Controller,
             * f7 Clipboard, f8 FileTransfer. */
            static const char *NOMS[8] = {
                "video", "audio", "input", "curseur",
                "micro", "manette", "presse-papier", "transfert"
            };
            /* === K12b 2026-09-03 - DECODE, NE PLUS VIDER ===
             *
             * This loop used to log the first 160 bytes of every reply in hex
             * AND in ASCII. The eighth reply - FileTransfer - carries an
             * OpenSSH ed25519 PRIVATE KEY in cleartext starting at offset 33
             * of the body, so every completed bootstrap wrote a live private
             * key into `halyard.log`, and `tools/switch-logsink.sh`
             * mirrored that line over plain TCP to the development machine.
             *
             * It also threw away everything the reply says. The server states,
             * per channel, the granted transport, port offset and session
             * handle - and for video the resolution, frame rate, codec and
             * bitrate cap, for audio the codec it actually granted. We were
             * deriving the channel map from a REST port and assuming our own
             * audio request had been honoured, while the answer arrived every
             * session in a message we read and discarded.
             *
             * `ann_reply.c` reads the fields we want and NEVER copies the key
             * field. What follows logs facts, never bytes. */
            /* === FT2 2026-10-02 - THE FIRST REPLY WAS NEVER PARSED =========
             *
             * This loop starts at r=1 because the first reply is read before
             * it, for the mode extraction. But `ann_reply_parse` was therefore
             * never run on that first reply, so one of the eight grants has
             * always been invisible to everything downstream - which is how the
             * file-transfer self-test came up empty on a session that received
             * "8 of 8". Parse it here, with the length it was actually read
             * with (`reply_len`), before the loop touches `buf`. */
            {
                ann_reply_t ar0;
                if (!ann_reply_parse(buf, reply_len, &ar0)) {
                    /* === FT2 2026-10-02 - AND WHAT THIS REPLY ACTUALLY IS ====
                     *
                     * Measured: `f3.10, 1097 bytes`. Field 10 is DisplayConfig
                     * (the request this client still calls `register_session`),
                     * so the reply read BEFORE the loop is that request's
                     * acknowledgement - not a channel grant at all. A first
                     * reading of this called it "a lost grant"; that was wrong,
                     * and failing to parse it here is correct behaviour.
                     *
                     * What it does expose is the TALLY below: `received`
                     * starts at 1 for this reply, so a session that collects
                     * the DisplayConfig ack plus six grants plus one
                     * unrecognised answer reports a reassuring "8 of 8" while
                     * two channels were never granted. Structure only, never
                     * content: a real FileTransfer grant carries the SSH
                     * password. */
                    clog("[FT2] pre-loop reply is not an announcement "
                         "(%u bytes, f3.%u - f3.10 is the DisplayConfig ack, "
                         "which is expected here)",
                         (unsigned)reply_len, ar0.response_field);
                }
                if (ann_reply_parse(buf, reply_len, &ar0)) {
                    clog("[K12] accord %s (1re reponse) : %s, offset %+d",
                         ann_reply_channel_name(ar0.channel),
                         ar0.tcp ? "TCP" : "UDP", ann_reply_port_offset(&ar0));
                    if (ctx && ar0.have_session) {
                        const int bi0 = shadow_chan_idx_from_ann(ar0.channel);
                        if (bi0 >= 0) {
                            ctx->chan_handle[bi0]    = ar0.handle;
                            ctx->chan_handle_ok[bi0] = 1;
                        }
                    }
                    caps_note_grant(&ar0, p->port_base);   /* INT1 */
                    if (ar0.channel == ANN_CHAN_FILEXFER) {
                        caps_note_ft_host(p->vm_host);   /* FT4 */
                        caps_note_ft_secret(buf, reply_len);
                        session_ft_reveal(p, buf, reply_len, &ar0);   /* FT3 */
                        session_ft_selftest(p, buf, reply_len, &ar0);
                    }
                }
            }
            int received = 1;   /* the first reply is already read above */
            int granted_seen = 0;   /* FT2: real channel grants, not messages */
            /* === FT2 2026-10-02 - READ UNTIL EIGHT GRANTS, NOT EIGHT MESSAGES
             *
             * This loop read exactly seven more messages and counted whatever
             * arrived. The server INTERLEAVES its own traffic with the grants -
             * measured: one 36-byte message shaped `f2:len7 f4:len25`, i.e. a
             * Request from the SERVER (field 2), with no field 3 at all, so not
             * a reply to anything. Each such message consumed one of the seven
             * slots, and the grants it displaced were simply never read: the
             * session reported "8 of 8" while two channels (micro and file
             * transfer) had no grant recorded, and the file-transfer self-test
             * found nothing to connect to.
             *
             * So the budget is now in GRANTS, with a generous ceiling on reads
             * so an interleaving server cannot starve us, and the loop stops
             * early the moment all eight are in. A read timeout still breaks
             * out, which is what ends it when the server really does grant
             * fewer than eight. */
            for (int r = 1; r < 24 && granted_seen < 8; r++) {
                size_t rl = 0;
                ann_reply_t ar;
                if (!ctrl_tcp_recv_cleartext(tcp, buf, buf_cap, &rl, 150)) break;
                received++;

                if (!ann_reply_parse(buf, rl, &ar)) {
                    /* Not a channel announcement, or not one we understand.
                     * Its LENGTH only - never its content: this is the branch
                     * an unrecognised file-transfer reply would fall into. */
                    /* Named by its response-type field, not by its length: 3 is
                     * capabilities or periodic stats, 10 a display config, 13 a
                     * bitrate acknowledgement. The server interleaves those with
                     * the announcements, so one landing here is normal - and
                     * saying WHICH removes the guesswork. Never its content. */
                    clog("[K12] grant #%d: not an announcement (reply f3.%u, %zu bytes)",
                         r, ar.response_field, rl);
                    /* === FT2 2026-10-02 - WHAT IS THIS REPLY, THEN? =========
                     *
                     * This VM grants 6 channels out of 8 (no micro, no file
                     * transfer) and answers the other two with something this
                     * parser does not recognise. "f3.0" does not mean field 0
                     * - it means no field 3 was found at all, so the reply is
                     * not shaped like a channel grant. Dump its STRUCTURE -
                     * field numbers, wire types and lengths, never content - so
                     * the refusal can be named instead of guessed at. A real
                     * grant for this channel would carry the SSH password,
                     * which is exactly why only the shape is logged. */
                    {
                        char shape[192]; int so = 0; int off2 = 0;
                        while ((size_t)off2 < rl && so < (int)sizeof shape - 24) {
                            uint32_t fn = 0, wt = 0;
                            const int nxt = pb_read_tag(buf, rl, off2, &fn, &wt);
                            if (nxt < 0) break;
                            if (wt == 2) {
                                const uint8_t *sb = NULL; size_t sl2 = 0;
                                const int a2 = pb_read_lendelim(buf, rl, nxt, &sb, &sl2);
                                so += snprintf(shape + so, sizeof shape - so,
                                               "f%u:len%u ", fn, (unsigned)sl2);
                                if (a2 < 0) break;
                                off2 = a2;
                            } else {
                                uint64_t v = 0;
                                const int a2 = (wt == 0) ? pb_read_varint(buf, rl, nxt, &v)
                                                         : pb_skip_field(buf, rl, nxt, wt);
                                if (wt == 0)
                                    so += snprintf(shape + so, sizeof shape - so,
                                                   "f%u=%llu ", fn,
                                                   (unsigned long long)v);
                                else
                                    so += snprintf(shape + so, sizeof shape - so,
                                                   "f%u:w%u ", fn, wt);
                                if (a2 < 0) break;
                                off2 = a2;
                            }
                        }
                        clog("[FT2] unrecognised reply #%d shape: %s", r, shape);
                    }
                    continue;
                }

                /* The channel comes from the field NUMBER inside the reply, so
                 * it no longer depends on the order the replies arrive in -
                 * which the old code could only guess at, and said so. */
                const int off = ann_reply_port_offset(&ar);
                if (ar.channel == ANN_CHAN_VIDEO && ar.have_mode) {
                    clog("[K12] accord %s : %ux%u @ %.2f fps, codec=%u, "
                         "debit max %u Mb/s, %s, offset %+d",
                         ann_reply_channel_name(ar.channel),
                         ar.width, ar.height, (double)ar.fps, ar.codec,
                         ar.bitrate_bps / 1000000u, ar.tcp ? "TCP" : "UDP", off);
                    /* CFG-3 2026-09-11 - the reassembly now takes its NAL grammar
                     * from each picture's header, but the DECODER is still built
                     * from SHADOW_CODEC: a grant that differs from the request is
                     * a session the decoder cannot read. This is the one place
                     * where both values are in hand. */
                    if (ar.codec != vparams.codec_wire)
                        clog("[K12] codec granted %u != requested %u",
                             ar.codec, vparams.codec_wire);
                } else if (ar.have_audio) {
                    clog("[K12] accord %s : %u Hz %u bits, codec %s, %s, offset %+d",
                         ann_reply_channel_name(ar.channel),
                         ar.sample_rate, ar.bits,
                         ar.audio_codec == 2 ? "FLAC" : "Opus",
                         ar.tcp ? "TCP" : "UDP", off);
                } else {
                    clog("[K12] accord %s : %s, offset %+d",
                         ann_reply_channel_name(ar.channel),
                         ar.tcp ? "TCP" : "UDP", off);
                }
                /* SRV5: keep the granted handle - it is the stream identifier
                 * the unregister request needs, and without it one channel
                 * cannot be re-announced. Stored by OUR body index, not by the
                 * server's channel number: those are two different orders (see
                 * shadow_chan_idx_from_ann) and mixing them would re-announce the
                 * wrong channel. */
                if (ctx && ar.have_session) {
                    const int bi = shadow_chan_idx_from_ann(ar.channel);
                    if (bi >= 0) {
                        if (!ctx->chan_handle_ok[bi]) granted_seen++;
                        ctx->chan_handle[bi]    = ar.handle;
                        ctx->chan_handle_ok[bi] = 1;
                    }
                }
                caps_note_grant(&ar, p->port_base);   /* INT1 */
                if (ar.channel == ANN_CHAN_FILEXFER) {
                    caps_note_ft_host(p->vm_host);   /* FT4 */
                    caps_note_ft_secret(buf, rl);
                    session_ft_reveal(p, buf, rl, &ar);   /* FT3 */
                    session_ft_selftest(p, buf, rl, &ar);
                }
                (void)NOMS;
            }
            /* === FT2 2026-10-02 - "8 of 8" WAS COUNTING REPLIES, NOT GRANTS
             *
             * `received` is incremented for every reply read, and it starts at
             * 1 for the pre-loop DisplayConfig acknowledgement. So a session
             * that got six real channel grants, that ack and one unrecognised
             * answer printed "8 of 8" - and the two channels the server never
             * granted (measured on this VM: micro and file transfer) were
             * invisible. The reply count is still worth printing, but the
             * number that matters is how many CHANNELS were granted. */
            unsigned granted = 0;
            if (ctx) for (int i = 0; i < 8; i++) if (ctx->chan_handle_ok[i]) granted++;
            clog("[K12] replies read: %d | CHANNELS GRANTED: %u of 8%s",
                 received, granted,
                 granted == 8 ? "" : "  <- the server did not grant them all");
        }
    }

    return true;
}

/* === SRV5 2026-10-02 — RE-ANNOUNCE ONE CHANNEL ==============================
 *
 * THE ONLY RECOVERY THE SERVER ALLOWS for a stream client it has invalidated.
 * Read off ShadowStreamer 6.3.1 (details in
 * halyard-lab/notes/findings/server-vs-halyard.md §2):
 *
 *   - `AClient::SetInvalid` @0x140c01640 clears one byte (+218) and calls its
 *     virtual hook, which is `nullsub_833` in EVERY client class: the client is
 *     never removed from the channel.
 *   - the channel's demux (`DtlsChannel::WaitForInputData` @0x140c24570) picks a
 *     client only when it is valid AND its stored IP prefixes the datagram's
 *     source; otherwise it logs `Failed to find matching client` and DISCARDS
 *     the datagram.
 *
 * So after an invalidation, every byte we send on that channel is thrown away
 * before any dispatcher runs - whatever source port we send it from, since the
 * match is on the IP. That is why AUD5, AUD16 and SHADOW_VIDEO_REREG, which all
 * re-send the `A` registration DATAGRAM, were measured doing nothing: "900 sends
 * in 30 min, 807 of them with no line" (§3.38), "twelve attempts with no answer"
 * (AUD16b). They were never capable of working.
 *
 * `AChannel::AddNewClient` @0x140c22240 is what inserts a client, it is reached
 * only from the control channel's registration path, and it has no duplicate
 * guard. A fresh client is valid, so the demux breaks on it and skips the stale
 * one. Hence: request field 9 (unregister this stream) then field 8 (announce it
 * again), on `:base+11`.
 *
 * Measured to choose the default: nothing live yet. This is read off the server
 * binary, and the mechanism it replaces is proven incapable - that is the whole
 * argument for making it the default. The unregister half is the uncertain one
 * (we send the handle the server granted, but no capture shows an unregister
 * used as a RECOVERY rather than at shutdown), so it has its own toggle.
 *
 * `SHADOW_REANN=0` restores the previous behaviour exactly: the callers then
 * fall back to their `A` datagram resend.
 * Returns true if both messages went out. */
/* === CLIP3 2026-10-02 - THE CLIPBOARD, JOINED TO THE SESSION ===============
 *
 * `clip_wire` decodes the messages, `clip_chan` reassembles them and `clip_tcp`
 * carries them; all three are blind to what a clipboard is. This is the piece
 * that joins them to the clipboard of the machine the client runs ON, in both
 * directions:
 *   VM -> here   the receive thread STAGES the text below, and the service loop
 *                writes it to the local clipboard;
 *   here -> VM   the service loop polls the local change counter and sends a
 *                REPLY when it moves.
 *
 * WHY THE PASTE IS NOT APPLIED ON THE RECEIVE THREAD. `OpenClipboard` fails
 * while another process holds the clipboard, so `local_clipboard.c` retries for
 * up to 100 ms. A receive thread blocked that long stops polling its abort flag
 * within the 100 ms the Switch demands (CLAUDE.md), and `clip_tcp.h` says in so
 * many words that a callback must not block. The thread therefore copies and
 * returns, and the loop does the Win32 work.
 *
 * WHY A FILE STATIC AND NOT `session_ctx_t`. The staging slot needs a mutex, and
 * `session_ctx_t` is also compiled by `tests/test_vid_reasm.c`, which has no
 * business growing a pthread dependency. So this follows `g_caps` above: a
 * static that is reset WHOLESALE when the channel opens, explicitly, rather than
 * relying on `ctx = {0}`.
 *
 * === CLIP4 2026-10-02 - THE DIRECTION IS A SETTING, NOT A CONSTANT =========
 *
 * `SHADOW_CLIPBOARD` carries FOUR values, not a boolean, so that one key says
 * both whether the clipboard is shared and which way:
 *
 *     0  off          nothing is opened
 *     1  both ways    the default
 *     2  PC -> VM     what you copy here lands in the VM; the VM's never comes
 *     3  VM -> PC     what you copy in the VM lands here; yours never leaves
 *
 * `0` keeps exactly the meaning it had, which is why the key was extended
 * rather than joined by a second one: two keys would allow "off, but one way",
 * a state with no meaning that someone would eventually have to resolve.
 *
 * ONE-WAY IS ENFORCED AT THE PULL, NOT AT DELIVERY. With VM -> PC off we stop
 * ASKING (`clip_tcp_set_auto_request`): dropping the text after it arrived
 * would look identical from here and send whatever the user copied in the VM
 * across the network anyway, which is the one thing the setting is chosen to
 * prevent. Likewise PC -> VM off means we neither poll the local clipboard nor
 * answer the VM when it asks for it.
 *
 * The direction is re-read at every poll, so changing it takes effect without
 * reconnecting. Turning it from `0` to anything else does NOT, because at `0`
 * no channel was opened - the setting's own description says so.
 *
 * Default: both ways wherever there IS a local clipboard. The server grants the
 * channel on every session, it costs one TLS socket plus one 32-bit counter
 * read every 300 ms, and it carries nothing at all until somebody copies
 * something. On console `local_clipboard_available()` is false and the channel
 * is never opened - which is why there is no `#ifdef _WIN32` anywhere in here
 * (device_caps.h exists because subtractive platform conditions have broken the
 * Vita port twice). */

/* CLIP6 2026-10-02 - the four predicates moved to `clip_dir.h`, so that the one
 * testable piece of this feature stops being the one piece with no test. They
 * were static functions in this file, and no test can include this file: it
 * pulls wolfSSL, pthreads, sockets and the journal. `tests/test_clip_dir.c`
 * now covers them with 47 checks, three of them mutation checks on the
 * fallback, the parse and the asymmetry of the two directions. */
static int clip_mode_now(void)
{
    return clip_dir_from_env(getenv("SHADOW_CLIPBOARD"));
}

/* The largest local copy we will forward. `local_clipboard_get` REFUSES rather
 * than truncates, so a bigger copy is simply not forwarded - half a pasted
 * document is worse than none. 1 MiB is a quarter of `clip_chan`'s own cap and
 * about 500 pages of text; it lives on the heap because the service loop's
 * thread stack is already tight (see the 32 KiB note further up). */
#define CLIP_LOCAL_MAX (1024u * 1024u)

/* How often we look at the local clipboard. 300 ms is below the point at which
 * a copy-then-paste feels delayed, and `GetClipboardSequenceNumber` costs one
 * call with no lock and no conversion, so being wrong here is cheap either way.
 * SHADOW_CLIPBOARD_POLL_MS overrides it. */
#define CLIP_POLL_MS 300

static struct {
    pthread_mutex_t lock;
    char     *in;          /* staged paste from the VM, malloc'd; NULL = none */
    size_t    in_len;
    char     *buf;         /* the local read buffer, CLIP_LOCAL_MAX, kept open */
    uint64_t  token;       /* local change token; see local_clipboard.h */
    long long t_poll_ms;
    int       mode;        /* CLIP4: the direction last seen, for the summary */
    /* Set by the receive thread when the VM ASKS for our clipboard, cleared by
     * the loop that answers. One int written by one thread and cleared by the
     * other: a lost race costs one unanswered request, which the VM retries. */
    volatile int asked;
    unsigned  applied, pushed, push_fail, staged_drop, not_text;
} g_clip = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, NULL, 0, 0, CLIP_MODE_BOTH, 0,
             0, 0, 0, 0, 0 };

/* Runs on the clip receive thread. Copies and returns; see the block above. */
static void session_clip_text_cb(const uint8_t *text, size_t n, void *user)
{
    (void)user;
    if (!text || n == 0) return;
    char *copy = (char *)malloc(n);
    if (!copy) return;
    memcpy(copy, text, n);
    pthread_mutex_lock(&g_clip.lock);
    /* ONE slot, newest wins. A queue would make the user's clipboard replay a
     * history they have already moved past. */
    if (g_clip.in) { free(g_clip.in); g_clip.staged_drop++; }
    g_clip.in     = copy;
    g_clip.in_len = n;
    pthread_mutex_unlock(&g_clip.lock);
}

/* Runs on the clip receive thread: notes that the VM asked and returns. The
 * answer needs the local clipboard, and reading it can block for 100 ms. */
static void session_clip_request_cb(void *user)
{
    (void)user;
    g_clip.asked = 1;
}

static void session_clipboard_open(const ctrl_session_params *p, session_ctx_t *ctx)
{
    /* Read fresh every session, not cached in a static: the settings screen
     * writes this key, and a cache would make the choice take a restart of the
     * application rather than of the session. */
    const int mode = clip_mode_now();
    if (mode == CLIP_MODE_OFF) return;

    if (!local_clipboard_available()) {
        clog("[CLIP3] no local clipboard on this platform - channel not opened");
        return;
    }

    const shadow_chan_caps *cc = &g_caps.chan[SHADOW_CHAN_IDX_CLIPBOARD];
    if (!cc->granted) {
        clog("[CLIP3] the server did not grant the clipboard channel - not opened");
        return;
    }
    if (!cc->tcp) {
        clog("[CLIP3] the clipboard was granted as UDP, which this client cannot "
             "speak - not opened");
        return;
    }

    /* Wholesale reset. A previous session can have left a staged paste and a
     * stale change token behind, and a stale token makes the first poll report a
     * change that never happened. */
    pthread_mutex_lock(&g_clip.lock);
    free(g_clip.in); g_clip.in = NULL; g_clip.in_len = 0;
    pthread_mutex_unlock(&g_clip.lock);
    g_clip.applied = g_clip.pushed = g_clip.push_fail = 0;
    g_clip.staged_drop = g_clip.not_text = 0;
    g_clip.t_poll_ms = 0;
    g_clip.asked = 0;
    if (!g_clip.buf) {
        g_clip.buf = (char *)malloc(CLIP_LOCAL_MAX);
        if (!g_clip.buf) { clog("[CLIP3] out of memory for the clipboard buffer"); return; }
    }
    /* Seeded with what the clipboard reads NOW: otherwise the first poll sends
     * whatever the user had copied before starting the stream to the VM,
     * unasked. */
    g_clip.token = local_clipboard_token();

    /* The ABSOLUTE port from the grant, not `port_base + 14` recomputed here.
     * That is the INT1 rule and the lesson FT2 paid for. */
    if (clip_tcp_open(&ctx->clip, p->vm_host, (int)cc->port,
                      session_clip_text_cb, NULL, p->abort_flag) != 0) {
        clog("[CLIP3] clipboard open FAILED on :%u - continuing without it",
             (unsigned)cc->port);
        ctx->clip = NULL;
        return;
    }
    /* CLIP4: the direction, applied to the channel itself. `set_auto_request`
     * is what makes "VM -> PC off" mean the text is never asked for, rather
     * than asked for and discarded. */
    clip_tcp_set_auto_request(ctx->clip, clip_dir_to_pc(mode) ? true : false);
    clip_tcp_set_request_cb(ctx->clip, session_clip_request_cb, NULL);
    g_clip.mode = mode;
    clog("[CLIP3] clipboard wired on :%u, %s (SHADOW_CLIPBOARD=0/1/2/3)",
         (unsigned)cc->port, clip_dir_name(mode));
    /* No REQUEST here on purpose: it would pull the VM's current clipboard and
     * overwrite the user's local one the moment the stream starts, which nobody
     * asked for. The VM announces an UPDATE as soon as it copies anything. */
}

static void session_clipboard_tick(session_ctx_t *ctx, long long now_ms)
{
    if (!ctx->clip) return;

    /* Take the staged paste under the lock, apply it OUTSIDE: `local_clipboard_set`
     * can block for 100 ms, and holding a mutex across blocking I/O is the one
     * thing CLAUDE.md forbids outright. */
    char *pending = NULL; size_t pending_len = 0;
    pthread_mutex_lock(&g_clip.lock);
    pending = g_clip.in; pending_len = g_clip.in_len;
    g_clip.in = NULL; g_clip.in_len = 0;
    pthread_mutex_unlock(&g_clip.lock);

    /* CLIP4: the direction, re-read here so a change applies without a
     * reconnection. `clip_tcp` is told again only when it has actually moved -
     * the call is cheap, but a log line per pass would not be. */
    const int mode = clip_mode_now();
    if (mode != g_clip.mode) {
        clip_tcp_set_auto_request(ctx->clip, clip_dir_to_pc(mode) ? true : false);
        clog("[CLIP3] direction changed: %s -> %s",
             clip_dir_name(g_clip.mode), clip_dir_name(mode));
        g_clip.mode = mode;
    }

    /* A paste staged just before VM -> PC was turned off, or while it was off
     * because the channel was opened both ways and the user has since changed
     * their mind. Taken out of the slot above and dropped here rather than left
     * to apply later, which would paste out of nowhere. */
    if (pending && !clip_dir_to_pc(mode)) {
        free(pending);
        pending = NULL;
    }

    if (pending) {
        uint64_t tok = 0;
        if (local_clipboard_set(pending, pending_len, &tok)) {
            /* Adopt our own write's token, so the poll below does not read it
             * back as "the user copied something" and bounce it to the VM. The
             * VM side has the mirror guard; this is the near end of it. */
            g_clip.token = tok;
            g_clip.applied++;
            clog("[CLIP3] VM -> PC: %u bytes onto the local clipboard",
                 (unsigned)pending_len);
        } else {
            clog("[CLIP3] VM -> PC: the local clipboard refused %u bytes",
                 (unsigned)pending_len);
        }
        free(pending);
    }

    if (!clip_dir_to_vm(mode)) {
        /* PC -> VM is off. The change token still has to FOLLOW the clipboard,
         * or turning the direction back on would send whatever happens to be
         * there as if it had just been copied. */
        g_clip.asked = 0;
        (void)local_clipboard_changed(&g_clip.token);
        return;
    }

    static int g_poll_ms = -1;
    if (g_poll_ms < 0) {
        const char *e = getenv("SHADOW_CLIPBOARD_POLL_MS");
        g_poll_ms = (e && atoi(e) > 0) ? atoi(e) : (int)CLIP_POLL_MS;
    }
    if (now_ms - g_clip.t_poll_ms < g_poll_ms) return;
    g_clip.t_poll_ms = now_ms;

    /* The VM asking counts as a reason to send even when nothing changed: it
     * asked because it has nothing, and `local_clipboard_changed` would say no.
     * Cleared before the send, so a request arriving during it is not lost. */
    const int asked = g_clip.asked;
    g_clip.asked = 0;
    if (!asked && !local_clipboard_changed(&g_clip.token)) return;

    size_t n = 0;
    if (!local_clipboard_get(g_clip.buf, CLIP_LOCAL_MAX, &n) || n == 0) {
        /* Ordinary: the clipboard holds an image, or a file list, or more than
         * CLIP_LOCAL_MAX, or another process had it open. Counted, not reported:
         * the token has already moved, so this is not retried. */
        g_clip.not_text++;
        return;
    }
    if (clip_tcp_send_text(ctx->clip, (const uint8_t *)g_clip.buf, n) == 0) {
        g_clip.pushed++;
        clog("[CLIP3] PC -> VM: %u bytes sent to the VM clipboard", (unsigned)n);
    } else {
        g_clip.push_fail++;
    }
}

static void session_clipboard_close(session_ctx_t *ctx)
{
    if (ctx->clip) {
        clip_tcp_stats_t st;
        clip_tcp_get_stats(ctx->clip, &st);
        clog("[CLIP3] clipboard summary (%s) - VM->PC applied=%u (updates=%u "
             "ignored=%u texts=%u stale=%u) | PC->VM sent=%u failed=%u "
             "not-text=%u asked-by-VM=%u | staged-dropped=%u",
             clip_dir_name(g_clip.mode),
             g_clip.applied, st.rx_updates, st.rx_updates_ignored, st.rx_texts,
             st.rx_stale, g_clip.pushed, g_clip.push_fail, g_clip.not_text,
             st.rx_asked, g_clip.staged_drop);
        clip_tcp_close(ctx->clip);
        ctx->clip = NULL;
    }
    /* The staged paste, but NOT `g_clip.buf`: that one is reused by the next
     * session, and a 1 MiB allocation per session is a cost with no purpose. */
    pthread_mutex_lock(&g_clip.lock);
    free(g_clip.in); g_clip.in = NULL; g_clip.in_len = 0;
    pthread_mutex_unlock(&g_clip.lock);
}

static bool session_reannounce_channel(ctrl_tcp_session *tcp,
                                       session_ctx_t *ctx,
                                       uint32_t *hb_seq,
                                       int chan_idx,
                                       const char *why)
{
    if (!tcp || !hb_seq || chan_idx < 0 || chan_idx > 7) return false;

    static int g_reann = -1;
    if (g_reann < 0) {
        const char *e = getenv("SHADOW_REANN");
        g_reann = e ? atoi(e) : 1;
    }
    if (!g_reann) return false;

    /* The unregister needs the handle the server granted for THIS stream. With
     * no handle we skip it rather than guess an identifier: unregistering the
     * wrong stream would take down a channel that works. */
    static int g_reann_unreg = -1;
    if (g_reann_unreg < 0) {
        const char *e = getenv("SHADOW_REANN_UNREG");
        g_reann_unreg = e ? atoi(e) : 1;
    }
    uint8_t buf[512];
    if (g_reann_unreg && ctx && ctx->chan_handle_ok[chan_idx]) {
        /* field 9: stream_id = the granted handle, truncated to the 32 bits the
         * wire field carries. The server prints it as `sessionId %u`. */
        const int n = ctrl_build_unregister_stream(
            buf, sizeof buf, (*hb_seq)++,
            (int64_t)(uint32_t)ctx->chan_handle[chan_idx], 0);
        if (n > 0 && !ctrl_tcp_send_cleartext(tcp, buf, (size_t)n)) {
            clog("[SRV5] channel idx=%d: unregister send FAILED (%s)", chan_idx, why);
            return false;
        }
    }

    const int n = ctrl_build_channel_announcement_ex(buf, sizeof buf,
                                                     (*hb_seq)++, chan_idx, NULL);
    if (n <= 0 || !ctrl_tcp_send_cleartext(tcp, buf, (size_t)n)) {
        clog("[SRV5] channel idx=%d: re-announcement send FAILED (%s)", chan_idx, why);
        return false;
    }
    clog("[SRV5] channel idx=%d RE-ANNOUNCED (%s, handle=%s) - the server can only "
         "revive an invalidated stream client this way",
         chan_idx, why,
         (ctx && ctx->chan_handle_ok[chan_idx]) ? "known" : "unknown, unregister skipped");
    return true;
}

/* Bootstrap step 8: chacha20 cipher, UDP sockets, registration with the server,
 * then opening the side channels (video TCP, input TCP, audio DTLS, ComChan).
 * This is the last phase before the service loop.
 *
 * Extracted from ctrl_session_run on 2026-08-25. The side channels already live
 * in `ctx`, so only four resources come back out: the three UDP sockets and the
 * cipher. They are copied to the caller at the `done` label, INCLUDING WHEN THE
 * FUNCTION FAILS - otherwise a partial open would leak, the parent's cleanup not
 * seeing what had already been allocated.
 *
 * The two server replies are passed by value: they are small structs, and it
 * leaves the original body unchanged. */
static bool session_open_media(const ctrl_session_params *p,
                               session_ctx_t *ctx,
                               ctrl_session_stats *stats,
                               shadow_auth_reply auth_reply,
                               shadow_encryption_reply enc_reply,
                               const uint8_t client_key[32],
                               int port_base_used,
                               int *out_udp_video, int *out_udp_cursor,
                               int *out_udp_input, shadow_cipher **out_cipher)
{
    bool ok = false;
    int udp_video = -1, udp_cursor = -1, udp_input = -1;
    shadow_cipher *cipher = NULL;

    /* Step 8: set up the chacha20 cipher + UDP register */
    {
        /* === SEC2 2026-09-11 - SECRETS IN THE LOG: THE START AND THE END ONLY ===
         * These lines wrote the session's chacha20 key, the authentication hash
         * (the UDP register identifier) and the nonce IN FULL, at INFO level -
         * so into the network mirror too. F36 wanted them for offline crypto
         * derivation tests; the tools that parsed DEBUG_KEY are the FEC-era
         * brute-forcers (tools/fec_bruteforce*.py, find_parity_key.py), dead
         * since F31. Whoever held such a log could decrypt that session's
         * captured video, audio and gamepad traffic. Masked by
         * shadow/log_mask.h: two logs of one session still match, nothing
         * usable is left. The labels stay, so the lines are still found. */
        char m[48];
        clog("DEBUG_KEY: chacha20_key=%s", log_mask_bytes(enc_reply.key, 32, m, sizeof m));
        clog("DEBUG_HASH: auth_hash=%s", log_mask_bytes(auth_reply.hash, 20, m, sizeof m));
        if (p->streaming_token)
            clog("DEBUG_TOKEN: streaming_token=%s", log_mask_text(p->streaming_token, m, sizeof m));
        clog("DEBUG_NONCE: nonce_extra (%zu B)=%s nonce_counter=%llu",
             enc_reply.nonce_extra_len,
             log_mask_bytes(enc_reply.nonce_extra,
                            enc_reply.nonce_extra_len < 32 ? enc_reply.nonce_extra_len : 32,
                            m, sizeof m),
             (unsigned long long)enc_reply.nonce_counter);
    }
    /* K11: Tx = client key (our Encryption request), Rx = server key (its
     * reply). We used to pass the server key on both sides, so everything we
     * encrypted was unreadable to the server. */
    cipher = shadow_cipher_create(client_key, enc_reply.key);
    if (!cipher) {
        emit_progress(p, "M14.fail", "cipher create FAIL");
        stats->exit_reason = 1; goto fin;
    }
    ctx->cipher = cipher;

    /* === K15i 2026-08-29 - IN TCP MODE, THE VIDEO UDP SOCKET MUST NOT EXIST ===
     *
     * Measured on the first capture in the `reliability` profile: the official
     * client opens NO video UDP socket - `CONNECT :11010 type=TCP` and nothing
     * else. The server, for its part, stops feeding UDP.
     *
     * Creating one here would be wrong twice over: it would send a registration
     * the server no longer expects, and its failure would ABORT the bootstrap a
     * few lines below (`if (udp_video < 0) goto done`) while video would be
     * arriving perfectly over TCP.
     *
     * `SHADOW_VIDEO_NET_TCP` is the same toggle that ASKS for TCP in the
     * channel announcement (K15): asking and receiving have to go together,
     * otherwise you get a session with no picture - which is exactly what
     * happened while only the request side existed. */
    if (!video_en_tcp()) {
        udp_video  = udp_register(p->vm_host, port_base_used + 10, auth_reply.hash);
    } else {
        clog("[K15i] video requested over TCP: no UDP socket on :%d",
             port_base_used + 10);
    }
    {
        /* S44: the cursor/audio channel is NOT associated, so that it accepts
         * datagrams from any source. Undoing the association afterwards is
         * refused by HOS (errno 106): the only option is never to make one. */
        /* === S44 REFUTED AND HARMFUL - default put back to 0 on 2026-08-25 ===
         *
         * The idea was that a connected socket filters the source and could be
         * hiding a stream sent from another port. Measured on Linux, same VM,
         * same code: without `connect()` the socket no longer has a local
         * address of its own (`ss` shows `UNCONN *:*` where video and input are
         * `ESTAB` over IPv6) and the channel drops to ZERO packets. With
         * `connect()`, the same test gives `cursor=5989 audio=5983` and the
         * sound plays at 1.00x real time.
         *
         * So it was a regression of ours, not a lead. The default goes back to
         * the connected socket; `SHADOW_CURSOR_ANYSRC=1` restores the experiment
         * (which now only serves to document it). */
        static int g_anysrc = -1;
        if (g_anysrc < 0) {
            const char *e = getenv("SHADOW_CURSOR_ANYSRC");
            g_anysrc = e ? atoi(e) : 0;
        }
        udp_cursor = udp_register_ex(p->vm_host, port_base_used + 30,
                                     auth_reply.hash, g_anysrc ? 0 : 1);
        if (!g_anysrc) g_cursor_peer_len = 0;   /* we keep send() */
        else if (udp_cursor >= 0)
            clog("[S44] :base+30 opened UNCONNECTED - accepts anything "
                 "quelle source (emission par sendto)");
    }
    /* K9 2026-08-21 - the cleartext registration on :base+13 is OUR OWN
     * addition: the official client NEVER sends that 25 B packet. On that port
     * it only emits encrypted 42 B packets, every ~7 s (`[ct 14][nonce 12]
     * [tag 16]`, chacha20 with a key that is not the control channel's).
     * Hypothesis: our unexpected packet invalidates the input channel
     * server-side. SHADOW_UDP_REG_13=1 to send it again. */
    {
        /* No static cache here: the UI can change this setting between two
         * connections of the SAME process, and a static would keep the first
         * session's value. That is exactly what happened on 2026-08-25 - the
         * setting was 1 and the log still said "socket only", because the
         * application's first session had frozen it at 0. The env var still
         * takes priority. */
        const char *e13 = getenv("SHADOW_UDP_REG_13");
        const int g_reg13 = e13 ? atoi(e13) : g_reg_input_pushed;
        udp_input = udp_register(p->vm_host, port_base_used + 13,
                                  g_reg13 ? auth_reply.hash : NULL);
    }
    /* G1 2026-08-21 - GAMEPAD. For us the :base+13 channel carried only a
     * keepalive; the guided capture showed that it also carries the whole
     * gamepad protocol (KB §3.25). We attach the sender to it, which first sends
     * the plug announcement so the VM creates its virtual gamepad.
     * The local evdev reader is opt-in: SHADOW_GAMEPAD=1. */
    if (udp_input >= 0 && cipher) {
        ctrl_gamepad_attach(udp_input, cipher);
        ctrl_gamepad_start_local_reader((volatile int *)p->abort_flag);
        ctrl_gamepad_selftest();
        ctrl_gamepad_axis_probe();
    }

    /* V13 2026-05-15: open VideoSslTcpChannel :base+20 (= TCP+TLS).
     * RE'd 2026-05-15, documented in tools/ida/out/VIDEO_SSL_TCP_CHANNEL_RE.md.
     * The server spontaneously pushes VideoTcpFrames (= H.264 NAL directly, not
     * SUFP), at ~3 fps; it serves as correction / IDR retransmit.
     * Phase 1 (log only): no callback, we just observe what the server pushes.
     * SHADOW_VIDEO_TCP=0 disables it. */
    static int g_video_tcp = -1;
    if (g_video_tcp < 0) {
        const char *e = getenv("SHADOW_VIDEO_TCP");
        g_video_tcp = e ? atoi(e) : 1;
    }
    /* RE11 2026-05-18 - VST callback -> h264_decoder through on_video.
     * RE6 finding C95: VST frames of 4142 B = H.264 NAL Annex-B IDR retransmits
     * at 3 fps. Wiring them to the decoder is a potential fix for the 2% taskbar
     * bug, through resilience to UDP loss. */
    /* D4 2026-08-20 - retry with backoff on the side channels.
     * Matrix collected over 4 sessions (2 GUI + 2 headless, 2 different VMs):
     *   vst 3/4 OK, input-tcp 2/4 OK, comchan 0/4, audio-dtls 0/4.
     * The failures are err=-308 (RST) occurring AFTER a successful TCP connect
     * -> the server accepts the socket then cuts at the TLS handshake, which
     * points to a race with its own binding: we knock too early, once, and the
     * channel stays dead for the whole session ("continuing without").
     * A spaced-out retry settles it: if it is a race it succeeds on the 2nd
     * attempt; if the port is never served (comchan/audio) it fails the same way
     * and we know.
     * SHADOW_CHAN_RETRIES=0 restores the historical behaviour (1 attempt). */
    static int g_chan_retries = -1, g_chan_retry_ms = -1;
    if (g_chan_retries < 0) {
        const char *e = getenv("SHADOW_CHAN_RETRIES");
        g_chan_retries  = e ? atoi(e) : 4;
        if (g_chan_retries < 0) g_chan_retries = 0;   /* revue : -1 sert de
            "not initialised" sentinel, so SHADOW_CHAN_RETRIES=-1 made the
            `att <= -1` loop never run - no attempt at all. */
        e = getenv("SHADOW_CHAN_RETRY_MS");
        g_chan_retry_ms = e ? atoi(e) : 250;
    }

    if (g_video_tcp) {
        extern void vst_to_h264_callback(void *udata, const uint8_t *nal_bytes,
                                          size_t len, uint32_t frame_id,
                                          uint8_t flag, int is_keyframe);
        int rc = -1;
        for (int att = 0; att <= g_chan_retries; att++) {
            if (att > 0) {
                chan_backoff_sleep(g_chan_retry_ms, p->abort_flag);
                clog("[D4] vst: retry %d/%d (backoff %d ms)", att, g_chan_retries,
                     g_chan_retry_ms);
            }
            rc = ctrl_video_tcp_open(&ctx->vst, p->vm_host, (uint16_t)port_base_used,
                                      auth_reply.hash, p->bearer_jwt,
                                      p->streaming_token,
                                      /* S37 2026-08-25: `ctx`, NOT `&ctx`.
                                       * Here `ctx` is already a pointer - this
                                       * function was extracted from
                                       * `ctrl_session_run`, where `ctx` was a
                                       * stack VALUE and `&ctx` therefore
                                       * correct. The `&` survived the
                                       * extraction: the callback received the
                                       * address of a dead stack slot, read a
                                       * nonsense `p` from it (0x70 in the trace)
                                       * and crashed on the first dereference.
                                       * Invisible for entire sessions because
                                       * this channel only carries key frame
                                       * retransmits: the callback almost never
                                       * fires, and crashes as soon as it does. */
                                      vst_to_h264_callback, ctx);
            pthread_mutex_lock(&g_active_vst_mtx);   /* AF8 */
            g_active_vst = ctx->vst;
            pthread_mutex_unlock(&g_active_vst_mtx);
            /* S54: cursor images go to cursor_state, not to H.264. */
            if (ctx->vst) ctrl_video_tcp_set_cursor_cb(ctx->vst, vst_cursor_callback);
            if (rc == 0) { if (att) clog("[D4] vst: OK on attempt %d", att + 1); break; }
        }
        if (rc != 0) {
            clog("vst: open FAIL — continuing without TCP video channel");
            ctx->vst = NULL;
        } else {
            clog("vst: ctrl_video_tcp_open OK on :%d (RE11 callback to h264 active)",
                 port_base_used + 20);
        }
    }

    /* === K15k 2026-08-29 - THE VIDEO CHANNEL OVER TCP ===
     *
     * Same STFP client as the cursor, on `:base+10` instead of `:base+20`, and
     * wrapped in TLS like it (K15e: the stream starts with a ServerHello).
     *
     * Each frame's payload is passed AS IS to the UDP emission path: the first
     * capture in the `reliability` profile shows that it carries the SAME
     * `VideoFrame` header (K15d), the STFP header stacking on top of it instead
     * of replacing it. That observation is what reduced the port from ~500 lines
     * to this block.
     *
     * What disappears from this path, and must not be recreated: the SUFP
     * header, the per-subchannel tracking, chunk reassembly, application-level
     * decryption (TLS handles that) and the NACK queue - TCP retransmits by
     * itself. The corresponding toggles become inert; that is written down in
     * CLAUDE.md. */
    if (video_en_tcp()) {
        const int rcv = ctrl_video_tcp_open_port(&ctx->vtcp, p->vm_host,
                                                 (uint16_t)port_base_used, 10,
                                                 CTRL_VST_ROLE_VIDEO,
                                                 auth_reply.hash, p->bearer_jwt,
                                                 p->streaming_token,
                                                 vtcp_video_callback, ctx);
        if (rcv != 0) {
            clog("[K15k] TCP video channel :%d - open FAILED, the session will have NO PICTURE",
                 port_base_used + 10);
            ctx->vtcp = NULL;
        } else {
            clog("[K15k] TCP video channel opened on :%d (STFP + TLS)",
                 port_base_used + 10);
        }
    }

    /* V15 2026-05-16 (TIER 8 U4 breakthrough): ComChan :base+14 lifecycle bus
     * + focus event broadcast triplet type=1/2/4. Hypothesis at C70 that type=2
     * (WINDOW_STATE_CHANGED) is the server-side multi-NAL gate. Coexists with
     * ctrl_input_tcp on the same :base+14 port - 2 separate TCP connections.
     * SHADOW_COMCHAN=0 disables it. */
    static int g_comchan = -1;
    if (g_comchan < 0) {
        /* D11 2026-08-21 - default OFF. Two reasons together:
         *   1. The hypothesis that justified this channel (V15: the
         *      FOCUS/WINDOW_STATE/ACTIVE_APP triplet being the multi-NAL gate)
         *      is REFUTED - 0 measured effect (see RETHINK_360_2026-05-16).
         *   2. It NEVER connects: 5 sessions out of 5 with err=-308, after a
         *      successful TCP connect. It opens a 2nd TLS connection on
         *      :base+14, a port already held by ctrl_input_tcp - the server
         *      serves only one.
         * Net result: a misleading error line every session, for a channel with
         * no use. SHADOW_COMCHAN=1 to reopen it if future RE gives it a role
         * again. */
        const char *e = getenv("SHADOW_COMCHAN");
        /* S8 2026-08-21 - default RESTORED to ON, and the open moved BEFORE the
         * input channel. The official client's capture shows it opens TWO TLS
         * connections on :base+14 and writes on the ComChan first (t=873.070),
         * then sends its input Connect (t=873.077). We only opened one, and ours
         * was the input: if the server assigns roles by connection order, our
         * input channel was taken for the ComChan - which explains why it
         * accepts TLS, accepts our frames (byte-exact, verified) and NEVER sends
         * a byte back, where the desktop receives ~15 messages of 104 B per
         * second.
         * D11 had turned this channel OFF after seeing it fail 5 times out of 5
         * with err=-308 - but back then it opened SECOND, on a port already
         * taken by our input. The order was the cause, not the channel. */
        /* Default OFF, but the port conflict is gone: since S11 the input is on
         * :base+12 (DTLS), so ComChan can coexist on :base+14 - verified, it
         * opens cleanly with SHADOW_COMCHAN=1.
         * We still leave it OFF: tested on 2026-08-21 as a lead for the keyboard
         * not being applied (the focus hypothesis), with no effect whatsoever. */
        g_comchan = e ? atoi(e) : 0;
    }
    if (g_comchan) {
        int rc = ctrl_comchan_open(&ctx->comchan, p->vm_host,
                                    (uint16_t)port_base_used, "Halyard");
        if (rc != 0) {
            clog("comchan: open FAIL — continuing without :base+14 lifecycle channel");
            ctx->comchan = NULL;
        } else {
            clog("comchan: opened on :%d BEFORE the input channel (S8)",
                 port_base_used + 14);
        }
    }

    /* I1 2026-05-18: open InputSslTcpFlatBuffersChannel :base+14 (= TCP+TLS).
     * RE'd from the V16 capture's plaintext SSL_WRITE on ssl=0x3781b750. Wire
     * format = length-prefixed FlatBuffer messages, Connect=96 B + mouse/
     * keyboard events of 144 B each. SHADOW_INPUT_TCP=0 disables it. */
    static int g_input_tcp = -1;
    if (g_input_tcp < 0) {
        const char *e = getenv("SHADOW_INPUT_TCP");
        g_input_tcp = e ? atoi(e) : 1;
    }
    if (g_input_tcp) {
        int rc = -1;
        for (int att = 0; att <= g_chan_retries; att++) {   /* D4 */
            if (att > 0) {
                chan_backoff_sleep(g_chan_retry_ms, p->abort_flag);
                clog("[D4] input-tcp: retry %d/%d (backoff %d ms)", att,
                     g_chan_retries, g_chan_retry_ms);
            }
            {
                /* Primer = the same 25 B registration frame we send on :base+20
                 * (`41 01 00 14 00 [hash 20B]`), in case the port requires it
                 * before it will talk. */
                uint8_t regf[64];
                int regn = ctrl_build_udp_register(regf, sizeof(regf), auth_reply.hash);
                ctrl_input_tcp_probe_ports(p->vm_host, (uint16_t)port_base_used,
                                            regn > 0 ? regf : NULL,
                                            regn > 0 ? (size_t)regn : 0);
            }
            ctrl_input_tcp_scan(p->vm_host, (uint16_t)port_base_used);
            rc = ctrl_input_tcp_open(&ctx->itc, p->vm_host, (uint16_t)port_base_used);
            if (rc == 0) { if (att) clog("[D4] input-tcp: OK on attempt %d", att + 1); break; }
        }
        if (rc != 0) {
            clog("itc: open FAIL — continuing without TCP input channel");
            ctx->itc = NULL;
        } else {
            clog("itc: input channel opened (DTLS :%d unless overridden)", port_base_used + 12);
            /* I1 phase 3 2026-05-18: route the GUI inputs to the native TCP
             * channel. */
            native_input_set(ctx->itc);

            /* AUD1 2026-08-21 - sound was never wired up because we do not know
             * which way it comes: none of our captures contains any, the remote
             * desktop having stayed silent throughout every reverse-engineering
             * session. Only three UDP channels receive anything (video, cursor,
             * and this DTLS channel), hence the hypothesis that it is
             * multiplexed here. So we hook the Opus decoder onto whatever is not
             * an input echo, and the channel reports the sizes it receives:
             * playing sound on the remote desktop will settle it. */
            static int g_aud_from_input = -1;
            if (g_aud_from_input < 0) {
                /* REFUTED by the 2026-08-21 measurement: with a video playing on
                 * the remote desktop, the report gives 1146 packets received on
                 * this channel, ALL of them 104 B input echoes, and none of any
                 * other size. Audio does not come through here.
                 * Default 0; the size report stays active. */
                const char *e = getenv("SHADOW_AUDIO_FROM_INPUT");
                g_aud_from_input = e ? atoi(e) : 0;
            }
            if (g_aud_from_input && p->on_audio) {
                ctrl_input_tcp_set_audio_cb(ctx->itc, p->on_audio, p->user);
                clog("[AUD1] audio decoder wired to channel :%d",
                     port_base_used + 12);
            }
        }
    }

    /* I2 2026-05-18 phase 1: open AudioUdpChannel :base+12 (= DTLS 1.2).
     * V16 captures of UDP_SENDMSG on :base+12: ClientHello `16 fe ff` then
     * post-handshake Opus frames of 141 B. Phase 1 = handshake validation + RX
     * counter, phase 2 = wire audio_decoder up to actually play sound.
     * SHADOW_AUDIO_DTLS=0 disables it. */
    static int g_audio_dtls = -1;
    if (g_audio_dtls < 0) {
        /* S11 2026-08-21 - default switched to OFF. `:base+12` is not audio:
         * the `captures_inputport_20260821_135207/` capture proves it carries
         * the INPUT channel (KB §3.21). This channel has in fact NEVER received a
         * single packet - `idle Ns (no audio packets)` every session, which we
         * blamed on an audio problem. Leaving it open now steals the port from
         * the input channel, which needs it.
         * Where the audio really is remains to be determined.
         * SHADOW_AUDIO_DTLS=1 to reopen it. */
        const char *e = getenv("SHADOW_AUDIO_DTLS");
        g_audio_dtls = e ? atoi(e) : 0;
    }
    if (g_audio_dtls) {
        /* I3 2026-05-18: forward received Opus payloads to the caller through
         * the on_audio callback. Without that, audio_rx_thread reads the packets
         * and drops them -> total silence. */
        int rc = ctrl_audio_dtls_open(&ctx->aud, p->vm_host, (uint16_t)port_base_used,
                                       p->on_audio, p->user);
        if (rc != 0) {
            clog("aud: open FAIL — continuing without audio channel");
            ctx->aud = NULL;
        } else {
            clog("aud: ctrl_audio_dtls_open OK on :%d on_audio=%p",
                 port_base_used + 12, (void *)p->on_audio);
        }
    }

    /* CLIP3 2026-10-02 - the clipboard, last of the side channels. Opened here
     * and not earlier because it reads the grant snapshot, which
     * `session_announce_channels` fills: the absolute port comes from the
     * server, never from an offset recomputed on the spot. A failure is not
     * fatal - a stream without copy/paste is still a stream. */
    session_clipboard_open(p, ctx);

    if (udp_video < 0 && !video_en_tcp()) {
        emit_progress(p, "M14.fail", "UDP video register FAIL");
        stats->exit_reason = 1; goto fin;
    }

    stats->bootstrap_ok = true;

    ok = true;
fin:
    *out_udp_video  = udp_video;
    *out_udp_cursor = udp_cursor;
    *out_udp_input  = udp_input;
    *out_cipher     = cipher;
    return ok;
}

/* Video feedback towards the server, every 50 ms.
 *
 * Four findings live here, all born of the comparison with the official client:
 *   G14 - the field we took for a VOLUME is a RATE;
 *   G15 - the retransmission request must go out RIGHT NOW, not on the next
 *         pass: 50 ms more is already one lost frame;
 *   G17 - targeted repair is not what the official client does;
 *   K12 - this channel's real keepalive is encrypted, not a bare byte.
 *
 * Moved out of the service loop on 2026-08-25: 264 lines, NO escaping stream, no
 * variable that outlives it - its sixteen counters are internal.
 *
 * The deadline and the sequence counter are passed by POINTER rather than kept
 * in statics: a static would survive from one session to the next inside the
 * same process, which would show up on the second connection from the UI. The
 * statistics and the cipher come from the context, which already carries the
 * same objects - no need to pass them again. */
/* === S34 2026-08-25 - THE VIDEO FEEDBACK STATE BELONGS TO THE SESSION ===
 *
 * These fields used to be `static` inside `session_video_feedback_tick`, so they
 * survived from one session to the next while being compared against counters
 * that do restart at zero (`ip_counter`, `bytes_rx`). Measured over a run of
 * twenty sessions: the picture never came back from the THIRD one on.
 *
 * The exact chain: `last_evt_idr_tick` kept the value reached at the end of the
 * previous session (~500 ticks for 24 s). The next session restarted at
 * `ip_counter = 0`, and the rate limit `ip_counter - last_evt_idr_tick >= 10`
 * computed `0 - 500 = -500`: NO key frame request could be emitted any more. And
 * the server does not restart a new client at the beginning of a GOP - it
 * continues its own. With no request, never another SPS/PPS/IDR (measured: NAL
 * type 1 exclusively from the 3rd session on), the decoder's gate never opened,
 * and the screen stayed black while the incoming bitrate was perfectly normal.
 * Reported symptom: "I no longer get video decoding but I can see incoming video
 * traffic".
 *
 * The log counters were part of it: capped once and for all, they went silent
 * from the 2nd session on - which is what kept the failure invisible for several
 * campaigns. So they are reset too.
 *
 * `ctrl_session_run` declares one zeroed instance: everything in here restarts
 * cleanly on every session. Add ONLY session state to it - an env var cache
 * stays `static`. */
typedef struct {
    long long t_last_ip_ms;      /* derniere emission du retour (horloge ms) */
    uint16_t  ip_counter;        /* feedback tick, ~1 every 50 ms */

    uint32_t  gE_last_bytes_rx;  /* G14: volume at the last gE, for the delta */
    long long gE_last_ms;
    uint32_t  gE_lisse;          /* debit lisse annonce au serveur */

    int64_t   delay_last_us;     /* G15: the last delay measurement */
    int       delay_have;

    uint8_t   nack_nonce;
    int       nack_ce_sec;       /* NACKs sent within the current second */
    int       nack_sec;          /* current second (-1 = not yet) */

    uint16_t  ifr_counter;       /* the IFR message counter (starts at 1) */
    idr_rate_t idr;              /* rate limit on key-frame requests */

    /* SRV1 2026-10-02: origin of the microsecond clock reported in gE field 3,
     * SESSION-relative on purpose (vid_uplink.h). `armed` is separate from the
     * value: overloading 0 as "not set" collides with a clock that reads 0. */
    int64_t   gE_t0_us;
    int       gE_t0_armed;

    /* SRV3 2026-10-02: the 20-byte registration hash, copied so the feedback
     * tick can re-register on its own. The tick does not see `auth_reply`, and
     * passing one more argument through a function this repo already calls with
     * seven would be the worse of the two. */
    uint8_t   reg_hash[20];
    int       srv3_said;         /* SRV3: the "gate lost" line, once per session */
    /* HID1 2026-10-02 - the lock-key probe goes out ONCE per session. Here and
     * not in a function static: that is the defect family CLAUDE.md names
     * first, and a probe that fires only on the first session of a process
     * answers nothing on the second. `fb` is memset per session. */
    int       hid_probe_sent;
    uint32_t  hid_probe_seq;     /* HID1: so the REPLY can be recognised */
    int       hid_reply_logged;

    long long t_ka13;            /* the last keepalive frame on :base+13 */

    /* === SRV-OBS 2026-10-02 — THE THREE 2026-10-02 UPLINK FIXES WERE BLIND ===
     *
     * SRV1 (gE field 3 is a timestamp), SRV2 (one liveness byte instead of
     * three) and SRV3 (the key-frame counter never reaching the server's
     * 0xFFFE sentinel) all changed what leaves this function, and NONE of them
     * left a trace: after a session one could not say whether the new field 3
     * was even emitted, let alone whether it stayed monotonic. The periodic
     * `[SRV-OBS]` line at the end of this function reports these, and they are
     * SESSION state for the reason the S34 block above gives at length - a
     * `static` here would carry the first session's totals into the second and
     * make every A/B wrong, which is the defect family this file keeps paying
     * for. */
    long long obs_t0_ms;         /* session origin, for the `t=` of the line */
    long long obs_log_ms;        /* last summary printed */
    int       obs_armed;         /* separate from obs_t0_ms, for the reason
                                  * gE_t0_armed above is separate from its
                                  * value: a clock that legitimately reads 0
                                  * must not re-arm the origin on every pass */
    uint32_t  obs_ge;            /* SRV1: gE packets actually on the wire */
    uint32_t  obs_f3_last;       /* SRV1: last field-3 value sent, in us */
    uint32_t  obs_f3_back;       /* SRV1: samples NOT strictly ahead of the
                                  * previous one (the server differences
                                  * consecutive samples: see below) */
    uint32_t  obs_ping50;        /* SRV2: liveness messages sent on :base+10 */
    int       obs_ping_len;      /* SRV2: 1 byte (the fix) or 3 (SHADOW_PI3=1) */
    uint32_t  obs_ifr;           /* SRV3: key-frame requests issued */

    int       nack_log, ifr_log, ka13_log;  /* per-session log budgets */
} video_feedback_t;

/* === SRV1 2026-10-02 — THE TIMESTAMP gE FIELD 3 MUST CARRY =================
 *
 * The server reads field 3 as a MICROSECOND instant in the same domain as its
 * own steady clock, and it is the sole input to the RTT it attributes to us:
 *   sub_140C041E0  : now_us = clock(); if (now_us > f3) estimator(f1, f2, now_us - f3)
 *   sub_140BFA710  : sliding-window MEAN of those ages, published at est+96
 *   sub_140BFA270  : returns *(int*)(est+96) / 1000.0  -> the RTT in ms
 * (ShadowStreamer 6.3.1; the whole chain is read in
 * halyard-lab/notes/findings/server-vs-halyard.md §1.)
 *
 * Two consequences fix the shape of this function:
 *
 * 1. We used to put `g_last_frame_id` here - a small counter. `now_us - fid` is
 *    then the VM's uptime in microseconds, so the server believed our RTT was
 *    hundreds of thousands of milliseconds. KB §3.27 had ALREADY written
 *    "field3: a timestamp in microseconds"; only the code disagreed.
 *
 * 2. The origin must be the SESSION, not the process and not the epoch. The
 *    guard is `now_us > f3`, and ShadowStreamer has been running since the VM
 *    booted: a timestamp larger than its clock makes it DROP the whole sample
 *    silently. A session-relative microsecond count is always far below the
 *    server's uptime, so the sample is always accepted.
 *
 * Measured to choose the default: nothing live yet - this is read off the
 * server binary, and the old value is provably not a timestamp. SHADOW_GE_TS=0
 * restores the frame id so the A/B is exact. */
static uint32_t ge_horodatage_us(video_feedback_t *fb)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    const int64_t us = (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
    /* The rule itself is in vid_uplink.h, pure and covered by
     * tests/test_vid_uplink.c. Only the clock read stays here. */
    return vid_uplink_ge_f3(us, &fb->gE_t0_us, &fb->gE_t0_armed);
}

/* Flushes ONE retransmission request. Called either on every turn of the
 * receive loop (the default, V10) or on the 50 ms tick (SHADOW_NACK_TICK=1, the
 * old behaviour). The cap of 20 sends per second is the real brake either
 * way. */
static void nack_vidange(session_ctx_t *ctx, video_feedback_t *fb,
                         long long now_ms, int udp_video)
{
    const int sec_courante = (int)(now_ms / 1000);
    if (sec_courante != fb->nack_sec) {
        fb->nack_sec = sec_courante;
        fb->nack_ce_sec = 0;
    }
    if (ctx->nack_head != ctx->nack_tail && fb->nack_ce_sec < 20) {
        fb->nack_ce_sec++;
        nack_request_t *req = &ctx->nack_queue[ctx->nack_head];
        if (req->count > 0 && req->count <= NACK_MAX_INDICES) {
            uint8_t nack_pkt[6 + NACK_MAX_INDICES * 2];
            nack_pkt[0] = 0x72;
            nack_pkt[1] = 0x47;
            nack_pkt[2] = 0x01;
            nack_pkt[3] = fb->nack_nonce++;
            /* The count fits in ONE byte on the wire. NACK_MAX_INDICES is
             * 128, so it fits - but the conversion stays explicit: a future
             * raise of the cap would otherwise truncate in silence, which is
             * this repo's signature failure. */
            nack_pkt[4] = (uint8_t)req->count;
            nack_pkt[5] = 0x00;
            for (int i = 0; i < (int)req->count; i++) {
                nack_pkt[6 + i*2]     = (uint8_t)(req->indices[i] & 0xFF);
                nack_pkt[6 + i*2 + 1] = (uint8_t)((req->indices[i] >> 8) & 0xFF);
            }
            const int nack_len = 6 + (int)req->count * 2;
            send(udp_video, nack_pkt, (size_t)nack_len, 0);
            ctx->stats->nack_sent++;   /* the log is capped at 10 lines:
                                        * with no counter, one read the
                                        * cap as the number of sends. */
            if (fb->nack_log < 10) {
                clog("[F18] NACK sent count=%d indices=[%u,%u,...] frame_id=0x%08x",
                     (int)req->count,
                     req->count > 0 ? req->indices[0] : 0,
                     req->count > 1 ? req->indices[1] : 0,
                     req->frame_id);
                fb->nack_log++;
            }
        }
        ctx->nack_head = (ctx->nack_head + 1) % NACK_QUEUE_CAP;
    }
}

static void session_video_feedback_tick(session_ctx_t *ctx,
                                        video_feedback_t *fb,
                                        long long now_ms, int udp_video, int udp_input,
                                        int port_base_used, int ctrl_strict)
{
    /* === V10 2026-08-28 - THE REQUEST WENT OUT TWO FRAMES TOO LATE ===
     *
     * G15 had already established that the `rG` packet carries no frame id, so
     * its indices designate the CURRENT frame server-side, and that waiting
     * makes the request useless. The flush had nevertheless stayed INSIDE this
     * congestion feedback's 50 ms tick: at 45 fps, 50 ms is two whole frames,
     * and the server had moved on.
     *
     * So it moves out of the gate. This function is called on every pass of the
     * receive loop (~200/s), so the request goes out within ~5 ms of the hole
     * being detected. The safeguard remains the cap of 20 sends per second,
     * which is the real brake - not the tick.
     *
     * SHADOW_NACK_TICK=1 restores the flush on the 50 ms tick. */
    static int g_nack_tick = -1;
    if (g_nack_tick < 0) {
        const char *e = getenv("SHADOW_NACK_TICK");
        g_nack_tick = e ? atoi(e) : 0;
    }
    if (!g_nack_tick && udp_video >= 0) nack_vidange(ctx, fb, now_ms, udp_video);

    /* === K15p 2026-08-29 - THIS BLOCK WAS DEAD IN TCP MODE ===
     * The whole feedback loop (congestion, key-frame request, NACK flush) was
     * guarded by `udp_video >= 0`. In TCP that socket no longer exists (K15i),
     * so NOTHING could ask for a key frame - and the server was sending nothing
     * but non-IDR slices. Measured symptom: `decodeur cale, 1725 images
     * fournies sans sortie` - the emitted string stays French because KB.md §9
     * quotes it verbatim - with `idr_t=0`, and zero requests emitted.
     * The `gE` feedback and the NACKs do stay reserved for UDP: they make no
     * sense on a transport that retransmits by itself. */
    if (now_ms - fb->t_last_ip_ms >= 50 && (udp_video >= 0 || video_en_tcp())) {
        /* V10b: a toggle must RESTORE the old behaviour, not switch the feature
         * off. The first measurement showed `nack=0` across all three reference
         * sessions while 130 chunks were missing: `SHADOW_NACK_TICK=1` did not
         * put the flush back on the tick, it removed it - the block had been
         * MOVED, not duplicated. A revert toggle that does not return to the
         * previous behaviour makes every A/B wrong, and that is exactly the
         * mistake this repo has been documenting since G20. */
        if (g_nack_tick) nack_vidange(ctx, fb, now_ms, udp_video);
        /* SRV1: field 3 is a session-relative microsecond timestamp, not a
         * frame id - see ge_horodatage_us(). SHADOW_GE_TS=0 puts the frame id
         * back. */
        /* SRV1 RETRACTED 2026-10-02: the default is the ECHO of the server's
         * own send timestamp (bytes 6-9 of the chunk header), which is what
         * this client always sent and what `now_us - f3` is built to consume.
         * `SHADOW_GE_TS=1` selects the session-relative clock, which measured
         * as a regression - see vid_uplink.h §SRV1. */
        static int g_ge_ts = -1;
        if (g_ge_ts < 0) {
            const char *e = getenv("SHADOW_GE_TS");
            g_ge_ts = e ? atoi(e) : 0;
        }
        uint32_t ge_f3 = g_ge_ts ? ge_horodatage_us(fb) : g_last_frame_id;
        /* V14: compute bytes_rx since the last gE (= delta from stats), then x4. */
        /* === G14 2026-08-22 - THE FIELD IS A RATE, NOT A VOLUME ===
         *
         * Determined from the official client's capture: in steady state it
         * announces 146,564 for 10,610 bytes received in 70 ms, i.e.
         * 151,571 bytes per second. Field 1 is therefore a smoothed estimate
         * of the receive rate in BYTES PER SECOND.
         *
         * We were sending `bytes_received x 4`. Over a 50 ms interval, bytes
         * per second are `bytes x 20`: so we announced FIVE TIMES LESS than
         * what we are actually taking in. A control loop fed such an
         * underestimate keeps the encoder scraping the floor - hence an
         * over-compressed picture and blocky motion, while the link carries
         * five times more.
         *
         * We measure the real interval rather than assuming 50 ms: the loop is
         * not metronomic, and dividing by a wrong duration would bring back the
         * very error we are fixing. */
        uint32_t cur_bytes = (uint32_t)ctx->stats->udp_video_bytes;
        uint32_t delta_bytes = cur_bytes - fb->gE_last_bytes_rx;
        long long delta_ms = (fb->gE_last_ms > 0) ? (now_ms - fb->gE_last_ms) : 50;
        if (delta_ms < 1) delta_ms = 1;
        fb->gE_last_bytes_rx = cur_bytes;
        fb->gE_last_ms = now_ms;
        uint32_t bytes_field = (uint32_t)((uint64_t)delta_bytes * 1000u
                                          / (uint64_t)delta_ms);
        /* Smoothing: the official client does not follow the jitter from one
         * interval to the next, and a value that oscillates would make the
         * encoder oscillate with it. */
        fb->gE_lisse = fb->gE_lisse ? (uint32_t)((fb->gE_lisse * 7u + bytes_field) / 8u)
                                : bytes_field;
        bytes_field = fb->gE_lisse;
        /* Change in the offset since the last send: positive when packets are
         * falling behind (a queue filling up), negative when it drains.
         * Bounded to stay in the order of magnitude the official client emits
         * - a wild value at startup would make the server take an absurd
         * decision. */
        int32_t delta_us = 0;
        if (g_delay_valid) {
            if (fb->delay_have) {
                int64_t d = g_delay_offset_us - fb->delay_last_us;
                if (d >  1000000) d =  1000000;
                if (d < -1000000) d = -1000000;
                delta_us = (int32_t)d;
            }
            fb->delay_last_us = g_delay_offset_us;
            fb->delay_have = 1;
        }
        uint8_t ge[15] = {
            0x67, 0x45, 0x01,
            (uint8_t)(bytes_field & 0xFF),
            (uint8_t)((bytes_field >> 8) & 0xFF),
            (uint8_t)((bytes_field >> 16) & 0xFF),
            (uint8_t)((bytes_field >> 24) & 0xFF),
            (uint8_t)(delta_us & 0xFF),
            (uint8_t)((delta_us >> 8) & 0xFF),
            (uint8_t)((delta_us >> 16) & 0xFF),
            (uint8_t)((delta_us >> 24) & 0xFF),
            (uint8_t)(ge_f3 & 0xFF),
            (uint8_t)((ge_f3 >> 8) & 0xFF),
            (uint8_t)((ge_f3 >> 16) & 0xFF),
            (uint8_t)((ge_f3 >> 24) & 0xFF)
        };
        const ssize_t ge_n = send(udp_video, ge, 15, 0);
        /* === SRV-OBS 2026-10-02 — WHAT WE ACTUALLY PUT IN FIELD 3 ===========
         *
         * Counted only when the datagram really left: in TCP mode `udp_video`
         * is -1 (K15i) and this send fails every 50 ms, so counting attempts
         * would report a gE flow that never existed, which is the kind of
         * figure one then reasons from.
         *
         * The monotonicity test is MODULAR, not `<`: field 3 is a u32 of
         * microseconds and wraps about every 71 min (vid_uplink.h), and a wrap
         * is forward motion - the server only differences consecutive samples.
         * So the step is taken as an unsigned difference: a step of 0 means our
         * clock did not advance between two samples, and a step at or past 2^31
         * means the value is BEHIND the previous one. Either makes the server's
         * `now_us - f3` age meaningless, and SHADOW_GE_TS=0 (the frame id we
         * used to send) makes this counter climb at once - which is exactly the
         * counter-case this line exists to show. */
        if (ge_n == 15) {
            const uint32_t step = ge_f3 - fb->obs_f3_last;
            if (fb->obs_ge && (step == 0u || step >= 0x80000000u))
                fb->obs_f3_back++;
            fb->obs_f3_last = ge_f3;
            fb->obs_ge++;
        }
        fb->ip_counter++;
        if ((fb->ip_counter % 14) == 0) {
            /* === SRV2 2026-10-02 — THESE BYTES ARE NOT ONE MESSAGE ===========
             *
             * The server parses a stream channel as a BYTE STREAM, and only the
             * first byte of each message selects a handler
             * (ACommonSfpClient::DealWithInput @0x140c12a20, ShadowStreamer
             * 6.3.1):
             *   0x50 'P' -> liveness, consumes exactly ONE byte
             *   0x41 'A' -> registration, consumes 5 + u16@3
             *   0x78 'x' / 0x77 'w' -> 3 bytes, and a WRONG magic in bytes 1-2
             *                          calls SetInvalid: the channel dies
             *   anything else -> consumed one byte at a time, silently
             *
             * So `50 49 01` was read as a ping plus two junk message starts
             * ('I', then 0x01). Harmless today, because neither 0x49 nor 0x01
             * selects a handler on this channel - but it is the exact shape that
             * kills a channel the day a stray byte lands on 0x77/0x78, which is
             * the mechanism behind §3.34/§3.38. One byte is what the protocol
             * reads; we send one byte. SHADOW_PI3=1 restores the three.
             *
             * 0x70 'p' is in the video dispatcher's explicit ignore list
             * (`C b e f l p s y`), so it refreshes NOTHING server-side. Kept
             * because the official client sends it and byte-exactness on this
             * channel has been worth more than one campaign; do not count it as
             * a keep-alive. */
            static int g_pi3 = -1;
            if (g_pi3 < 0) {
                const char *e = getenv("SHADOW_PI3");
                g_pi3 = e ? atoi(e) : 0;
            }
            /* SRV-OBS 2026-10-02: count the liveness messages and record the
             * SHAPE we sent (1 byte or 3). The change was invisible otherwise:
             * nothing in the log distinguished a session run with the fix from
             * one run with SHADOW_PI3=1, so neither half of the A/B could be
             * identified after the fact. The length is recorded rather than the
             * toggle so the line reports what went on the wire. */
            if (g_pi3) {
                uint8_t pi[3] = {0x50, 0x49, 0x01};
                if (send(udp_video, pi, 3, 0) == 3) {
                    fb->obs_ping50++;
                    fb->obs_ping_len = 3;
                }
            } else {
                uint8_t pi[1] = {0x50};
                if (send(udp_video, pi, 1, 0) == 1) {
                    fb->obs_ping50++;
                    fb->obs_ping_len = 1;
                }
            }
            uint8_t p[1] = {0x70};
            send(udp_video, p, 1, 0);
        }
        /* iP RequestIFrame every ~2 s (= 28 ticks at 70 ms).
         * Forces the server to send a new FULL IDR -> complete refresh of
         * every MB (= bottom slice). RE 2026-05-09 FUN_00bec180: proto v2
         * packet = `69 50 00 02 [u16 request_counter_LE]`.
         *
         * H1 V9 F2 fix (see tools/ida/out/H1_V9_server_discrimination.md):
         * sub_BEC180.c:246-265 confirms bytes 4-5 are an incremental REQUEST
         * COUNTER (u16 at a1+194), NOT an output_id. We always sent 0 -> the
         * server deduplicated every IDR refresh request (probably the root
         * cause of the rare bottom slice). Increment it now. */
        /* V12 2026-05-15: IFR period 28 -> 14 (twice as frequent) to reduce
         * the CABAC drift time between IDR refreshes. Without it, the bottom
         * P-frame slices accumulate errors over ~2 s before the IDR refresh
         * -> visible on the taskbar (= highly variable areas).
         * SHADOW_IFR_PERIOD=N to override (default 14, min 4). */
        static int g_ifr_period = -1;
        if (g_ifr_period < 0) {
            const char *e = getenv("SHADOW_IFR_PERIOD");
            /* G5 FIX 2026-06-02: default OFF (was 14, i.e. a forced keyframe
             * every ~700 ms). The incremental IFR forced an IDR roughly every
             * 700 ms (the server does not deduplicate an increasing counter) ->
             * coarse IDR then refinement = the blurry/sharp "autofocus" effect.
             * It was a pre-G4 workaround to refresh the missing bottom slice;
             * obsolete since G4.
             * SHADOW_IFR_PERIOD=N (>=4) to re-enable. 0 = OFF. */
            g_ifr_period = e ? atoi(e) : 0;
            if (g_ifr_period > 0 && g_ifr_period < 4) g_ifr_period = 4;
        }

        /* === G17 2026-08-22 - TARGETED REPAIR IS NO LONGER SUBORDINATE TO
         * THE KEY FRAME REQUEST ===
         *
         * This block used to be nested INSIDE the one that emits the IFR: we
         * therefore only asked for a retransmission when we were already
         * asking for a full refresh - that is, when it is useless, the key
         * frame making targeted repair pointless. Measured: `nack=0` on a run
         * without the periodic IFR, `nack=27` on the same content with it.
         * Repair was never attempted in the one case where it could have
         * avoided the key frame.
         *
         * Lifted one level: it is now evaluated on every 50 ms tick,
         * independently of the IFR. */
        /* === G15 2026-08-22 - THE RETRANSMISSION MUST GO OUT RIGHT NOW ===
         *
         * The `rG` packet carries NO frame id: the format has none, so the
         * indices it lists can only designate the frame currently being sent
         * server-side. Waiting 100 ms amounts to asking for chunks of a frame
         * five frames old at 45 fps - the server has forgotten them, and the
         * request repairs nothing.
         *
         * And it is the only targeted repair we have. Failing that, a
         * truncated frame breaks the reference chain, the decoder stops, and
         * we ask for a whole key frame: the measurement gives 45 truncated
         * frames for 14 stalls in 50 s, each followed by a key frame that
         * costs quality (G12).
         *
         * So we send without waiting. The safeguard remains a per-second cap,
         * so as not to flood the channel if the loss becomes persistent. */
        /* V10: the NACK flush has been LIFTED out of the 50 ms tick - see the
         * head of this function. */

        /* G6 2026-06-02: IFR either (a) periodic IF re-enabled
         * (g_ifr_period>0), OR (b) EVENT-DRIVEN when a loss has been detected
         * (g_idr_needed), rate-limited to ~1 s (20 ticks x 50 ms). Recovers
         * quickly after a loss WITHOUT forcing a periodic IDR (= no autofocus
         * on a clean stream). */
        /* G26: rate limit brought down from ~1 s (20) to ~500 ms (10 ticks of
         * ~50 ms) to shorten the corruption window after a loss, without
         * spamming key frames (our losses are about one every 2.7 s). */
        /* The rate limit uses MODULAR 16-bit arithmetic, and is disarmed until
         * a first request has been made. The previous signed subtraction went
         * negative as soon as `ip_counter` restarted from zero (new session) or
         * wrapped (65,536 ticks, ~55 min): it then blocked EVERY request until
         * the process ended - see S34. */
        /* G26: ~500 ms between two requests (10 ticks of ~50 ms). The rule and
         * its counter-case are in idr_policy.h. */
        int evt_idr = g_idr_needed && idr_rate_allow(&fb->idr, fb->ip_counter, 10);
        int periodic_idr = (!ctrl_strict) && (g_ifr_period > 0
                            && (fb->ip_counter % g_ifr_period) == (g_ifr_period / 2));
        if (!ctrl_strict && (periodic_idr || evt_idr)) {
            if (evt_idr) { g_idr_needed = 0; idr_rate_mark(&fb->idr, fb->ip_counter); }
            /* === SRV3 2026-10-02 — THE COUNTER MUST NEVER GO BACKWARDS =======
             *
             * The server's gate (sub_140C12F70 @0x140c12f70) is
             *     if (N > stored || stored == 0xFFFF) { stored = N; }
             * so a request whose counter is NOT GREATER than the last one is
             * discarded with no log on either side. The only reset is the
             * 0xFFFF sentinel, which a fresh client gets from its constructor
             * (`*(_DWORD*)(this+520) = -1`) and which an `A` registration
             * restores. 0xFFFE must therefore never be our counter: it is the
             * "served" value the `A` path writes at +522.
             *
             * We start at 1 and increment once per request, so the wrap is
             * 65,535 requests away - hours at our rate, but reachable on a long
             * session, and past it EVERY request would be silently ignored for
             * the rest of the session. Re-register instead: `A` resets the gate
             * to 0xFFFF *and* forces a reference frame, which is exactly what we
             * want at that moment anyway. SHADOW_IFR_WRAP=0 goes back to
             * letting it wrap.
             * Source: halyard-lab/notes/findings/server-vs-halyard.md §3. */
            static int g_ifr_wrap = -1;
            if (g_ifr_wrap < 0) {
                const char *e = getenv("SHADOW_IFR_WRAP");
                g_ifr_wrap = e ? atoi(e) : 1;
            }
            /* Only on the UDP path: in TCP mode the key-frame request leaves
             * through the control channel and `ctrl_video_tcp` keeps its own
             * counter, so this one is never on the wire and must not be
             * "repaired" - the first version of this guard fired on every
             * request past 0xFFFE in TCP mode, logging in a loop. */
            if (g_ifr_wrap && udp_video >= 0
                && vid_uplink_ifr_needs_rereg(fb->ifr_counter)) {
                uint8_t reg[25];
                const int rl = ctrl_build_udp_register(reg, sizeof(reg),
                                                       fb->reg_hash);
                if (rl > 0 && send(udp_video, reg, (size_t)rl, 0) == rl) {
                    clog("[SRV3] IFR counter at %u - registration re-sent to reset "
                         "the server gate, counter back to 1", fb->ifr_counter);
                    fb->ifr_counter = 1;
                } else if (!fb->srv3_said) {
                    /* Said ONCE. Holding at 0xFFFD means we come back here on
                     * every subsequent request, and a line each time would bury
                     * the log at the request rate. */
                    fb->srv3_said = 1;
                    clog("[SRV3] IFR counter at %u and re-registration failed "
                         "(errno=%d) - key-frame requests will be ignored by the "
                         "server for the rest of this session",
                         fb->ifr_counter, shadow_sock_errno());
                }
                if (vid_uplink_ifr_needs_rereg(fb->ifr_counter))
                    fb->ifr_counter = VID_UPLINK_IFR_HOLD;
            }
            uint8_t ifr[6] = {0x69, 0x50, 0x00, 0x02,
                               (uint8_t)(fb->ifr_counter & 0xFF),
                               (uint8_t)((fb->ifr_counter >> 8) & 0xFF)};
            /* === K15n 2026-08-29 - OVER TCP, THE KEY FRAME REQUEST GOES ELSEWHERE ===
             *
             * `iP` is sent on the video UDP socket, which no longer exists in
             * TCP mode (K15i). Our requests therefore fell into the void, and
             * the symptom was exactly that of a decoder that never starts:
             * "decoder stalled (1725 frames fed with no output)" with
             * `idr_t=0`. The decoder received slices without ever receiving an
             * SPS, a PPS or an IDR - it could produce nothing, and nothing
             * complained anywhere but in that counter.
             *
             * The STFP equivalent already existed and was serving the cursor:
             * the `0x64` message of `ctrl_video_tcp_request_refresh`, which S54
             * had identified as a refresh. That answers question Q6 of the K15
             * report ("how do you ask for a key frame on an STFP channel?"),
             * open until now. */
            if (video_en_tcp()) {
                /* === K15n 2026-08-29 - THE REQUEST GOES OVER THE CONTROL CHANNEL
                 *
                 * The STFP `0x64` is not enough: measured over three sessions,
                 * `idr_t=0`, and the dump of the first bytes shows that ALL the
                 * frames are NON-IDR slices (`00 00 00 01 61`, type 1). The
                 * decoder therefore receives slices without ever receiving an
                 * SPS, a PPS or an IDR - hence "decoder stalled, 1725 frames fed
                 * with no output", without an error anywhere else.
                 *
                 * `Request_Flush` (N40, case 7 of the oneof) goes out on
                 * `:base+11`, which exists in BOTH modes. It is the only
                 * transport-independent key frame request we have. */
                /* The control channel is not in this function's scope: we ARM
                 * here, the main loop sends - the same pattern as
                 * `g_idr_needed`. */
                g_flush_tcp_needed = 1;
                if (ctx->vtcp) ctrl_video_tcp_request_keyframe(ctx->vtcp);
            } else {
                send(udp_video, ifr, 6, 0);
            }
            /* === S59 2026-08-27 - NO MORE CLEARTEXT IFR ON THE GAMEPAD CHANNEL ===
             *
             * The original comment said "also on the input port (= maybe the
             * server listens for it there)". That was a guess, made when we
             * believed `:base+13` carried the input. §3.37 established
             * that it is the GAMEPAD channel, and the official client sends
             * ONLY encrypted 42-byte packets on it - 1922 in one capture, zero
             * in cleartext. Its single IFR goes out on `:base+10`, the video.
             *
             * Moreover the official binary logs `gamepad: Invalid encrypted
             * message size: {} < {}` and `gamepad: Decrypt failed`: our 6
             * cleartext bytes fall below the minimum size of an AEAD envelope
             * (28 bytes).
             *
             * `SHADOW_IFR_ON_13=1` restores it, to redo the A/B. */
            static int g_ifr13 = -1;
            if (g_ifr13 < 0) {
                const char *e = getenv("SHADOW_IFR_ON_13");
                g_ifr13 = e ? atoi(e) : 0;
            }
            if (g_ifr13 && udp_input >= 0) send(udp_input, ifr, 6, 0);
            if (fb->ifr_log < 8) {
                clog("[G6] IFR request counter=%u (evt=%d periodic=%d)",
                     fb->ifr_counter, evt_idr, periodic_idr);
                fb->ifr_log++;
            }
            /* SRV-OBS 2026-10-02: the running total. `[G6]` above logs the
             * first eight requests only, so on a long session nothing said how
             * many were issued, and `ifr_counter` alone cannot answer it - a
             * re-registration resets it to 1 (SRV3). Both are reported, which
             * is what makes a reset visible as such rather than as a session
             * that stopped asking. Counted on both paths: in TCP mode the
             * request leaves over the control channel and this counter is not
             * on the wire, but a request was still issued. */
            fb->obs_ifr++;
            fb->ifr_counter++;

            /* F18 NACK 2026-05-22 23h30: drain nack_queue + send the rG packet.
             * Real limit: at most 20 NACKs per second (counter fb->nack_this_sec).
             * The old "1 per 100 ms" scheme left a dead variable behind, flagged
             * by the compiler once -Wall was turned on.
             * Wire format (RE'd from an LD_PRELOAD desktop capture):
             *   byte 0:    0x72 ('r')
             *   byte 1:    0x47 ('G')
             *   byte 2:    0x01 (proto version)
             *   byte 3:    nonce/counter (incremental)
             *   byte 4:    count (= number of u16 chunk_idx)
             *   byte 5:    0x00 (padding)
             *   bytes 6+:  count x u16 LE chunk_idx
             * Total len = 6 + count*2 bytes. SHADOW_NACK=0 disables it. */

            /* N29 rG NACK DISABLED: it made the UDP rate collapse from 392 to
             * 66 pps (= server stop). Incorrect format. See
             * [[project-video-540-clamp-workaround]]. */
            /* === K12 2026-08-21 - THE REAL ENCRYPTED KEEPALIVE ON :base+13 ===
             * The old code did send 42 B, but it was NOISE: a frozen header
             * copied from an old capture and a random body, with the comment
             * "will fail decrypt". We now know what it is, having decrypted the
             * 2026-08-21 capture:
             *   chacha20-poly1305, `[ct 14][nonce 12][tag 16]`,
             *   constant payload = 04 00 05 00 00 00 00 00 00 00 00 00 00 00
             * emitted every ~7 s, only the nonce counter changing.
             * The key is the CLIENT -> SERVER one, that is the 32 bytes of OUR
             * Encryption request (K11), not those of the reply - Shadow's
             * encryption uses a different key per direction.
             * SHADOW_INPUT_KEEPALIVE13_MS=0 disables it. */
            static int g_ka13 = -1;
            if (g_ka13 < 0) {
                const char *e = getenv("SHADOW_INPUT_KEEPALIVE13_MS");
                g_ka13 = e ? atoi(e) : 7000;
            }
            if (g_ka13 > 0 && udp_input >= 0 && ctx->cipher
                && now_ms - fb->t_ka13 >= g_ka13) {
                static const uint8_t KA13[14] = {
                    0x04, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                };
                uint8_t pkt[14 + SHADOW_AEAD_OVERHEAD];
                memcpy(pkt, KA13, sizeof(KA13));
                int total = shadow_cipher_encrypt(ctx->cipher, pkt, (int)sizeof(KA13));
                if (total == (int)sizeof(pkt)) {
                    send(udp_input, pkt, (size_t)total, 0);
                    if (fb->ka13_log < 3) {
                        clog("[K12] keepalive :%d chiffre, %d B (nonce %02x%02x…)",
                             port_base_used + 13, total, pkt[14], pkt[15]);
                        fb->ka13_log++;
                    }
                } else {
                    clog("[K12] chiffrement du keepalive :%d KO (rc=%d)",
                         port_base_used + 13, total);
                }
                fb->t_ka13 = now_ms;
            }
        }
        fb->t_last_ip_ms = now_ms;
    }

    /* === SRV-OBS 2026-10-02 — ONE LINE THAT SAYS WHETHER SRV1/2/3 WORK ======
     *
     * The three uplink fixes of 2026-10-02 changed what this function emits and
     * logged nothing, so a session could not be judged: field 3 could have been
     * stuck, the liveness byte could have stopped going out, and the key-frame
     * counter could have been silently reset, all without a line. Each fact
     * here is one a reader needs to decide whether a change is doing its job -
     * not a dump, which is what `[G53]`/`[AUD2]` taught this repo to avoid:
     *   gE=        gE packets that really left (0 in TCP mode, by design)
     *   f3=        the LAST field-3 value sent, the server's sole RTT input
     *   f3_back=   samples not strictly ahead of the previous one; must stay 0,
     *              and climbs immediately under SHADOW_GE_TS=0 (the frame id)
     *   ping50=    liveness messages on :base+10, with the shape sent (SRV2:
     *              1 byte, or 3 under SHADOW_PI3=1)
     *   ifr=/ifr_ctr= requests issued, and the counter now on the wire; the two
     *              differ exactly when SRV3 re-registered and reset it to 1
     *   nack=      `rG` retransmission requests, read from the session stats
     *              that nack_vidange already maintains - a second counter for
     *              the same sends would be one more thing to keep in step.
     *
     * Chosen default: ON. One line every 10 s against ~20 feedback packets a
     * second is noise-free (the 5 s `stats —` line is twice as frequent and far
     * longer), and these three changes have no other witness; SHADOW_SRV_OBS=0
     * silences it for a capture where the log size itself is being measured.
     * The period is checked on EVERY pass and not inside the 50 ms gate above:
     * the gate is what we are observing, and an observer that stops when its
     * subject does reports nothing at the moment it matters. */
    static int g_srv_obs = -1;   /* env cache: `static` is right here, S34 */
    if (g_srv_obs < 0) {
        const char *e = getenv("SHADOW_SRV_OBS");
        g_srv_obs = e ? atoi(e) : 1;
    }
    if (g_srv_obs) {
        if (!fb->obs_armed) {
            /* `obs_armed` and not `obs_t0_ms == 0`: a monotonic clock reading 0
             * is unlikely rather than impossible, and a sentinel that collides
             * with a valid value is the SRV1 defect vid_uplink.h documents. */
            fb->obs_armed  = 1;
            fb->obs_t0_ms  = now_ms;
            fb->obs_log_ms = now_ms;   /* first line at t=10s, not an empty one */
        } else if (now_ms - fb->obs_log_ms >= 10000) {
            fb->obs_log_ms = now_ms;
            /* No %z anywhere: the vitasdk's newlib neither prints it nor
             * consumes its argument, so every later conversion on this line
             * would read the wrong slot (tools/check-z-formats.py). */
            clog("[SRV-OBS] t=%llds gE=%u f3=%uus f3_back=%u ping50=%u(%dB) "
                 "ifr=%u ifr_ctr=%u nack=%u",
                 (long long)((now_ms - fb->obs_t0_ms) / 1000),
                 fb->obs_ge, fb->obs_f3_last, fb->obs_f3_back,
                 fb->obs_ping50, fb->obs_ping_len,
                 fb->obs_ifr, (unsigned)fb->ifr_counter,
                 ctx->stats->nack_sent);
        }
    }
}

/* CFG-1 2026-09-11 - one sender for the live bitrate (kUpdateSession f13),
 * shared by the UI's request and G19: both log the same line, with their
 * source. Returns whether the message went out. */
static bool send_bitrate(ctrl_tcp_session *tcp, uint32_t *hb_seq, uint32_t mbps,
                         const char *src)
{
    uint8_t vbuf[256];
    const int vn = ctrl_build_video_encoding_config_ex(vbuf, sizeof(vbuf), *hb_seq,
                                                       mbps * 1000000u);
    if (vn <= 0 || !ctrl_tcp_send_cleartext(tcp, vbuf, (size_t)vn)) return false;
    clog("[UI] video demandee : %u Mbps (seq=%u) src=%s", mbps, *hb_seq, src);
    (*hb_seq)++;
    return true;
}

bool ctrl_session_run(const ctrl_session_params *p, ctrl_session_stats *out) {
    /* CFG-1 / CFG-4 2026-09-11 - a bitrate chosen with no session running must
     * not leak into this one as a pre-Ready f13 (it bypassed the per-link
     * value); the announcement already carries the settings. */
    g_pending_bitrate_mbps = 0;
    g_pending_video_cfg    = 0;
    g_user_word            = 0;
    g_announced_cap_mbps   = 0;
    session_host_reset();   /* DNS1: a VM's address is never carried over */
    static ctrl_session_stats local_stats;
    ctrl_session_stats *stats = out ? out : &local_stats;
    memset(stats, 0, sizeof(*stats));

    /* B4 - one read, at the top: the transport cannot change while the session
     * runs, and reading it per call would freeze the FIRST session's value. */
    session_resolve_transport(p);

    if (!p || !p->vm_host || !p->streaming_token || !p->client_id || !p->bearer_jwt) {
        emit_progress(p, "error", "missing required params");
        stats->exit_reason = 1;
        return false;
    }
    int width  = p->display_width  > 0 ? p->display_width  : 1920;
    int height = p->display_height > 0 ? p->display_height : 1080;
    /* N26: allows an override through the SHADOW_DISPLAY_HEIGHT env var, to test
     * whether the server adapts its SPS when we ask for 540 (= half height). */
    {
        const char *eh = getenv("SHADOW_DISPLAY_HEIGHT");
        if (eh) {
            int h2 = atoi(eh);
            if (h2 > 0 && h2 <= 4096) {
                clog("[N26] override height %d → %d (env SHADOW_DISPLAY_HEIGHT)", height, h2);
                height = h2;
            }
        }
    }

    bool any_video = false;
    ctrl_tcp_session *tcp = NULL;
    g_gp_brut = g_gp_vus = g_gp_ko = 0;   /* G53c: SESSION state */
    int udp_video = -1, udp_cursor = -1, udp_input = -1;
    shadow_cipher *cipher = NULL;
    session_ctx_t ctx = {0};
    ctx.magic = SESSION_CTX_MAGIC;   /* S37: see ctrl_session_int.h */
    ctx.p = p;
    ctx.stats = stats;
    /* === ING-A1 2026-09-11 - THE ANTI-DUPLICATE WINDOW RE-PRIMES ITSELF ===
     * `ctx = {0}` gives the new rule: 8 consecutive packets beyond the window
     * re-prime it (audio_dedup.h). Bench: 0 differing decisions in 658 018 calls
     * and identical decoded PCM in 150/150 runs in normal operation; a stale-slot
     * session went from 174 [47..3246] to 5996 of 6000 frames, a renumbered
     * stream or a stray far-ahead frame from 0 % to 99.8-99.9 %; +0.15 ns per
     * call; 30/30 unit checks on MinGW and on Linux ASan+UBSan.
     * SHADOW_AUDIO_DEDUP_RESYNC=0 restores the previous rule exactly: the window
     * then never moves back. */
    {
        static int g_dedup_resync = -1;
        if (g_dedup_resync < 0) {
            const char *e = getenv("SHADOW_AUDIO_DEDUP_RESYNC");
            g_dedup_resync = e ? atoi(e) : 1;
        }
        ctx.aud_dedup.no_resync = !g_dedup_resync;
    }
    ctx.plain_buf = (uint8_t *)malloc(SUFP_OUT_CAP);
    if (!ctx.plain_buf) {
        emit_progress(p, "error", "plain_buf malloc FAIL");
        stats->exit_reason = 1;
        return false;
    }
    /* A single SUFP reassembler, for the CURSOR (`:base+30`). Video no longer
     * uses one since vid_reasm.c parses the header inline - yet a second
     * reassembler was still created here and destroyed without ever receiving a
     * byte: 3.07 MiB per session (1.07 of struct + 2.00 of output buffer)
     * allocated for nothing, which matters on Switch. Removed on 2026-08-25.
     * The previous comment also claimed `sufp_create` did not allocate its
     * buffer, and spoke of 8 MiB: both were wrong, it does allocate
     * SUFP_MAX_FRAME_BYTES (2 MiB). */
    /* === ING-A3 2026-09-11 - THE :base+30 FALLBACK COULD NEVER PRODUCE A FRAME ===
     *
     * on_cursor_packet() handed this reassembler every packet it had not
     * already consumed: the AUD12 split-audio chunks, every decrypt failure,
     * every AUD11 reject. None of them can complete a frame. The two chunks of
     * one split frame carry DIFFERENT bytes 6-9 on this channel (AUD12: a send
     * time, not a frame id), so SUFP's frame-id grouping never pairs them - and
     * if two ever did pair, the result would be two independently sealed
     * [ct][nonce][tag] blobs checked under the last tag, which Poly1305
     * rejects. A decrypt failure completes at once, is decrypted again from
     * the same bytes and fails again. sufp_feed refuses the 11-byte datagrams.
     * Bench (the real sufp.c fed the FLAC split pattern, 10^6 packets): 0
     * frames completed, one `sufp: window full` INFO line per 100 packets (per
     * 115 with identical copies), 1.2-1.4 us per packet. In a FLAC session with
     * sound that is ~0.7 INFO lines/s in the VIDEO category - the line that
     * sent AUD13 down a false lead - plus 3.07 MiB allocated per session, and
     * an allocation failure that ended the session for a component that
     * produces nothing. Audio (AUD10/AUD12) goes through the direct and
     * aud_reasm paths, which never used it.
     * SHADOW_AUD_SUFP=1 restores the fallback exactly; sufp.c and
     * tests/test_sufp.c stay. sufp_destroy below already takes a NULL. */
    static int g_aud_sufp = -1;
    if (g_aud_sufp < 0) {
        const char *e = getenv("SHADOW_AUD_SUFP");
        g_aud_sufp = e ? atoi(e) : 0;
    }
    if (g_aud_sufp) {
        ctx.sufp_cursor = sufp_create(on_cursor_full_frame, &ctx);
        if (!ctx.sufp_cursor) {
            emit_progress(p, "error", "sufp_create FAIL");
            stats->exit_reason = 1;
            goto cleanup;
        }
    }

    /* Step 1: connect the TCP control channel on port_base + 11.
     * port_base is computed by the caller from the /vm/ip response (= vm.port +
     * 7000 per the observed samples). If the caller does not provide it -> fail
     * cleanly. */
    int port_base_used = p->port_base;
    if (port_base_used <= 0) {
        emit_progress(p, "M8.fail", "port_base non fourni (caller doit calculer vm.port + 7000)");
        stats->exit_reason = 1;
        goto cleanup;
    }
    int control_port = port_base_used + 11;
    char detail[80]; snprintf(detail, sizeof(detail), "ctrl_tcp :%d (port_base=%d)", control_port, port_base_used);
    emit_progress(p, "M8.connect", detail);
    /* === A7 2026-08-25 - A SLOW BOOTSTRAP COSTS THE AUDIO ===
     *
     * Measured over five sessions: the `:base+30` channel (cursor AND sound)
     * does NOT start when the bootstrap drags.
     *
     *     bootstrap 8410 ms -> silent channel
     *     bootstrap 4158 / 4661 / 5363 / 6039 ms -> active channel
     *
     * On Switch the bootstrap is systematically slow, for a precise reason: the
     * FIRST attempt to connect to the control channel goes unanswered - the VM
     * has not opened its port yet - and we wait the full 5 s timeout before
     * retrying. Every session: 5 s lost, then a 2 s pause; the audio channel
     * never gets its chance. That is the explanation for "no sound on the
     * console" and for the missing cursor, and it is in no way Switch-specific:
     * the slow desktop session failed in exactly the same way.
     *
     * So we shorten the FIRST attempt to 1.2 s and the pause to 400 ms. A port
     * that answers does so within a few tens of milliseconds on this link
     * (measured round trip: 19 ms); waiting 5 s only means finding out later
     * that it was not answering. Subsequent attempts go back to the long
     * timeout, in case the server still reserves the port after a disconnect
     * (~60 s, hence the 15 attempts). */
    for (int attempt = 1; attempt <= 15; attempt++) {
        const int delai  = (attempt <= 3) ? 1200 : 5000;
        const int repos  = (attempt <= 3) ?  400 : 2000;
        tcp = ctrl_tcp_open_port_abortable(p->vm_host, control_port, delai,
                                           p->abort_flag);
        if (tcp) break;
        if (p->abort_flag && *p->abort_flag) { stats->exit_reason = 3; goto cleanup; }
        char wait[80];
        snprintf(wait, sizeof(wait), "retry %d/15 in %d ms on :%d",
                 attempt, repos, control_port);
        emit_progress(p, "M8.retry", wait);
        chan_backoff_sleep(repos, p->abort_flag);
    }
    if (!tcp) {
        {
            /* Says WHICH STAGE failed instead of asserting a cause. The old
             * text blamed "zombie VM or a different port base" for every
             * NULL, including the case where the socket had connected and
             * only TLS refused - which is what the console showed on
             * 2026-09-13, two lines under its own `connected ... NODELAY=on`. */
            char why[160];
            snprintf(why, sizeof why,
                     "open FAILED after 15 attempts on :%d - last stage: %s",
                     control_port, ctrl_tcp_last_failure());
            emit_progress(p, "M8.fail", why);
        }
        stats->exit_reason = 1;
        goto cleanup;
    }
    emit_progress(p, "M8.found", detail);
    ctrl_tcp_set_bearer(tcp, p->bearer_jwt);
    int instance = jwt_instance(p->bearer_jwt);
    ctrl_tcp_set_instance(tcp, instance > 0 ? instance : 6);

    /* Bootstrap steps 2 to 6 - see the function just above. */
    shadow_auth_reply auth_reply = {0};
    shadow_encryption_reply enc_reply = {0};
    uint8_t client_key[32] = {0};
    if (!session_handshake(p, tcp, stats, width, height,
                           &auth_reply, &enc_reply, client_key))
        goto cleanup;

    /* Scratch buffer for the channel announcements that follow. */
    uint8_t buf[8192];

    /* Step 7 - see session_announce_channels(). */
    if (!session_announce_channels(p, tcp, stats, &ctx, width, height, buf, sizeof(buf)))
        goto cleanup;

    /* Step 7b: Ready + DisplayReady moved to a delayed send in the main loop, to
     * mimic the desktop's timing (= 5 s after bootstrap, after ~5 heartbeats). */

    /* Step 8 - see session_open_media(). */
    if (!session_open_media(p, &ctx, stats, auth_reply, enc_reply, client_key,
                            port_base_used, &udp_video, &udp_cursor,
                            &udp_input, &cipher))
        goto cleanup;

    emit_progress(p, "M15.streaming", "session live");

    /* ========================================================================
     * SERVICE LOOP - overview
     *
     * What follows is a loop of PERIODIC tasks, each guarded by its own
     * deadline. They are independent: reading one does not require
     * understanding the others. In the order they appear:
     *
     *   drain UDP video + cursor        every pass    the hot path
     *   session_video_feedback_tick     50 ms         feedback to the server
     *                                                 (G14/G15/G17/K12)
     *   Ready + DisplayReady            once at 3 s   desktop mimicry
     *   Request_Flush                   per setting   pre-G4 workaround, OFF
     *   click harness (K6)              diagnostic    inactive by default
     *   validation through video (I9)   diagnostic    inactive by default
     *   deferred key release            on deadline   keyboard/mouse
     *   periodic RegisterSession        per setting   diagnostic, OFF
     *   cursor channel ping (AUD4)      7.5 s         without it the server
     *                                                 stops feeding the channel
     *   registration resend (AUD5)      if nothing arrives
     *   heartbeat                       500 ms        session state
     *   adaptive bitrate (G19)          1 s           on the measured loss
     *   statistics publication          250 ms        towards the UI
     *
     * The function is 988 lines long but only ~630 of them are CODE: the rest is
     * the experiment ledger this repo deliberately keeps next to the code it
     * explains. Do not mistake length for complexity.
     * ====================================================================== */

    /* Step 9: receive loop. Drain UDP non-blocking, loop until abort_flag.
     * Heartbeat at ~10 ms granularity for minimal latency. */
    uint8_t pkt[4096];
    int last_log_sec = -1;
    int64_t t_last_pub_ms = 0;     /* counters published, 4 Hz */
    int      g_adapt_last_sec = -1; uint32_t g_adapt_prev_exp = 0, g_adapt_prev_mis = 0;
    /* CFG-1 2026-09-11 - G19's state (was g_adapt_cur / g_adapt_good), now in
     * bitrate_ctl.h. SHADOW_ADAPT_USER_CAP=0 restores the previous rule: the
     * cap frozen from the session parameters (or 20), the user's live choice
     * ignored. */
    static int g_adapt_user_cap = -1;
    if (g_adapt_user_cap < 0) {
        const char *e = getenv("SHADOW_ADAPT_USER_CAP");
        g_adapt_user_cap = e ? atoi(e) : 1;
    }
    bitrate_ctl_t bctl;
    bitrate_ctl_init(&bctl, g_adapt_user_cap
                            ? g_announced_cap_mbps
                            : (p->max_bitrate_mbps ? p->max_bitrate_mbps : 20u));
    g_session_active = 1;
    long long t_start_ms = 0;
    long long t_last_heartbeat_ms = 0;
    long long t_last_cursor_ping_ms = 0;   /* AUD4: the :base+30 channel ping */
    long long t_last_cursor_reg_ms = 0;    /* AUD5 : reenvoi de l'enregistrement */
    /* AUD16: audio channel stall detector, the mirror of D2/S41 on the video
     * side. */
    unsigned  aud16_last_pkts   = 0;
    long long aud16_last_rx_ms  = 0;
    long long aud16_last_try_ms = 0;
    long long aud16_mort_ms     = 0;
    unsigned  aud16_essais      = 0;
    int       aud16_mort        = 0;
    int       aud16_abandon     = 0;
    /* AUD-LC-4 2026-09-11 - AUD16's SESSION budget (aud16_essais restarts at
     * every REVENU, so it only ever bounded one episode) and its one line;
     * AUD5's two lines; and the S57 signature: what we last put on :base+30,
     * and how many CHANNEL_DOWN(AUDIO) this loop has already attributed.
     * Session locals like everything above - never statics. */
    unsigned  aud16_total       = 0;
    int       aud16_cap_said    = 0;
    int       aud5_cap_said     = 0;
    int       aud5_first_said   = 0;
    long long aud5_last_tx_ms   = 0;
    struct { long long ms; const char *kind; unsigned n; } aud30_tx = { 0, NULL, 0 };
    unsigned  aud18_down_seen   = 0;
    /* AUD-LC-3 2026-09-11 - AUD15's ping numbering and the 15 s :base+30
     * sentinel are SESSION state: as statics in the loop body, pings #1-#3 and
     * the sentinel were logged by the first session of a process only. */
    uint32_t  n_ping               = 0;
    int       warned_cursor_silent = 0;
    unsigned  cursor_reg_count = 0;
    /* S34: the whole video feedback state, reset ON EVERY session.
     * It used to be `static` inside the tick function, which blocked key frame
     * requests from the 3rd session on (black screen with a normal incoming
     * bitrate). See the comment on `video_feedback_t`. */
    video_feedback_t fb;
    memset(&fb, 0, sizeof fb);
    fb.ifr_counter = 1;   /* the IFR message numbers from 1 */
    fb.nack_sec    = -1;  /* -1 = aucune seconde entamee */
    memcpy(fb.reg_hash, auth_reply.hash, sizeof fb.reg_hash);  /* SRV3 */

    /* S34: ask for a key frame RIGHT FROM THE START.
     *
     * On the very first session the server spontaneously sends SPS + PPS + IDR,
     * but not when we reconnect: it continues the GOP in progress. The decoder
     * then discards everything until the first key frame (`ready_to_decode`),
     * and without a request from us the screen stays black. The G13 stall
     * detector eventually asks for one, but only after fifteen frames fed for
     * nothing - measured: 15 s of black screen on the 2nd session.
     * An immediate request costs one 6-byte packet. */
    /* S34: purge the reassembly state BEFORE asking for the key frame - it
     * survived from one session to the next (unfreed chunk buffers, stale
     * subchannel sequencing). */
    vid_reasm_reset_session();
    /* ING-A2 / AUD-LC-3 2026-09-11: the :base+30 split-frame slots (AUD12) and
     * the channel's census are session state too - see
     * ctrl_session_aud_reasm_reset(). Here, on the session thread and before the
     * first drain of :base+30, no packet of the new session can already sit in a
     * slot. The line is the live census of stale slots: never printed after an
     * Opus session, where nothing is split. */
    {
        const unsigned stale = ctrl_session_aud_reasm_reset();
        if (stale)
            clog("[ING-A2] %u split audio frame(s) from the previous session "
                 "ecartee(s) au demarrage", stale);
    }
    g_crete_img_s = 0.0;       /* L20: a peak belongs to ITS session */
    g_kernel_drops = 0;        /* ING-1: per session - it was never reset */
    g_crete_img   = 0;         /* L21 : ses jalons aussi */
    g_crete_sec   = -1;
    latency_reset_session();   /* L5: one session = one set of measurements */
    session_stats_reset();      /* L17: and the panel too - same reason */
    /* S60: the dead-channel oracle is SESSION state - letting it carry over
     * would make every following session unreadable. This is the defect family
     * that has cost this repo four failures (KB §3.28). */
    g_channels_down = 0;
    g_audio_channel_down = 0;
    g_audio_channel_down_n = 0;   /* AUD18: its count, for the same reason */
    g_idr_needed = 1;

    /* S36: the initial resolution announcement is SESSION state.
     * As a `static`, `g_nres_initial_sent` stayed true after the first stream:
     * every subsequent session NEVER announced its resolution to the server.
     * Invisible as long as the resolution does not change - but the docked /
     * handheld switch (720p <-> 1080p) then only took effect on the very first
     * stream of the process. Same family as S34. */
    long long t_last_notify_res_ms = 0;
    bool      g_nres_initial_sent  = false;
    uint32_t hb_seq = 13;  /* continues after the channel announcements, seq=12 */
    /* S118: the round trip to the VM, measured on the heartbeat we already
     * send. SESSION state, declared here and not in a function `static` - that
     * is the family of defects this repo names first. */
    rtt_t     ctrl_rtt;
    rtt_reset(&ctrl_rtt);
    /* === ING-2 2026-09-11 - THE CONTROL SOCKET JOINS THE WAIT ===
     * The idle wait at the bottom of the loop (L6) watched only the UDP
     * sockets. A control reply that landed during the wait was noticed at the
     * next datagram or when the timeout expired, then stamped - like the
     * heartbeat it answers - with the pass's `now_ms`, read once per pass and
     * truncated to the millisecond. All of it went into the S118 round trip,
     * the only network latency this client shows (net_test, 30/80 ms colour
     * thresholds). Bench (ing2_assess/rttbench.c, the loop replayed on
     * loopback, bias = measured - true, mean/p90 ms): Windows with video
     * 5.3/11.5 -> 0.06/0.08; Windows silence 7.3-10.8/12.2-12.4 -> 0.07/0.10;
     * 5 ms honoured (the Linux/Switch proxy) 2.2-2.9/3.7-5.1 -> 0.07-0.09/0.10.
     * The loop alone fabricated 1.4-3.9 ms of jitter on a path that had 0.3,
     * so S119's "jitter 1 ms" was mostly loop. Moving only the reply stamp to
     * the microsecond clock leaves +0.6 ms: both sides move.
     *
     * The same bench shows the naive version is UNSAFE: a socket in the wait
     * whose data nobody reads makes every wait return at once - 216k-229k
     * passes/s, the D1 failure mode. So the socket is waited on only while
     * the V16 reader below runs on every pass (SHADOW_AUTO_FLUSH_ACK) and until
     * that read fails ONCE - a FIN, a reset, a timeout, a frame too large:
     * `ctrl_in_poll` then drops it for the rest of the session and the wait is
     * exactly the L6 one again. ctrl_tcp_poll_fd() refuses a closed peer, and
     * a poll() error with the socket in the set drops it as well. L6's 5 ms
     * ceiling is kept.
     * SHADOW_CTRL_POLL=0 leaves the socket out of the wait, as before. The
     * microsecond stamps have no toggle: they remove a measurement error and
     * change no behaviour. */
    static int g_auto_flush_ack = -1;   /* V16 - read here since ING-2, see the reader */
    if (g_auto_flush_ack < 0) {
        const char *e = getenv("SHADOW_AUTO_FLUSH_ACK");
        g_auto_flush_ack = e ? atoi(e) : 1;
    }
    static int g_ctrl_poll = -1;
    if (g_ctrl_poll < 0) {
        const char *e = getenv("SHADOW_CTRL_POLL");
        g_ctrl_poll = e ? atoi(e) : 1;
    }
    /* SESSION state, never a function static: the first failed control read
     * clears it, and each session starts with its own. */
    bool          ctrl_in_poll    = g_ctrl_poll != 0;
    unsigned long ing2_waits      = 0;   /* idle waits, for the end-of-session line */
    unsigned long ing2_ctrl_wakes = 0;   /* waits the control socket ended */
    /* L6's own toggle (`g_poll`, read lazily inside the loop), read here only
     * so the two [ING2] lines tell the truth: SHADOW_RX_POLL=0 leaves no wait
     * to join. Same parse as g_poll. */
    const char *ing2_rxp = getenv("SHADOW_RX_POLL");
    const bool  ing2_rx_poll = !ing2_rxp || atoi(ing2_rxp) != 0;
    clog("[ING2] waiting on the control channel: %s",
         !g_ctrl_poll        ? "non (SHADOW_CTRL_POLL=0)"
         : !g_auto_flush_ack ? "no (SHADOW_AUTO_FLUSH_ACK=0: nothing reads this channel)"
         : !ing2_rx_poll     ? "no (SHADOW_RX_POLL=0: no waiting on the sockets)"
         :                     "oui");
    bool ready_sent = false;
    /* D1/D2/D3 2026-08-20 - end-of-session instrumentation. */
    long long t_last_reg_ms   = 0;   /* D3: last periodic RegisterSession */
    unsigned  reg_count       = 0;
    unsigned  d2_last_pkts    = 0;   /* D2: video stall detector */
    /* === D4 2026-08-28 - MAKING THE §3.34 STALL MEASURABLE ===
     * The threshold was 2000 ms: any shorter stall was INVISIBLE, so its base
     * rate was unknown - not zero. The one stall ever observed lasted 1.62 s and
     * never fired D2; we read it after the fact from the timestamps.
     * `SHADOW_STALL_MS` sets the threshold, default 300 ms.
     * On every event we also record what it takes to DECIDE: is the control
     * channel still answering (D4 counter), and under what radio conditions -
     * docked or handheld, Wi-Fi or wired, signal strength. Without those three,
     * one more stall would teach us nothing new. */
    unsigned  d4_evts        = 0;    /* outages seen in the session */
    long long d4_total_ms    = 0;    /* cumulative time without video */
    long long d4_pire_ms     = 0;
    unsigned  d4_hist[4]     = {0};  /* <500, <1000, <2000, >=2000 ms */
    uint32_t  d4_ctrl_debut  = 0;    /* ctrl frames at the start of the outage */
    long long t_last_video_reg_ms = 0;   /* S41 : reprise de l'abonnement video */
    /* SRV5: the re-announcement's own spacing and session budget. Locals,
     * not function statics: a static here is the "works only on the first
     * session" family this repo keeps paying for. */
    long long t_last_video_reann_ms = 0;
    int       video_reann_count     = 0;
    /* === SRV-FAULT 2026-10-02 — TEST SCAFFOLDING, OFF BY DEFAULT ============
     *
     * SRV4 (the audio `'G'`) and SRV5 (re-announcing a channel) are RECOVERY
     * paths: each one runs only after a failure a healthy session never
     * produces. SRV4 needs `:base+30` silent for 10 s, SRV5 needs the video
     * channel silent for SHADOW_VIDEO_REANN_MS (5 s). Neither had ever
     * executed once against the real server - both were read off the
     * ShadowStreamer 6.3.1 binary and shipped as executable hypotheses.
     *
     * A mechanism nobody can trigger is a mechanism nobody can trust, and the
     * same day already showed the bill: SRV5's first version fired twice on a
     * perfectly healthy channel (9995 packets, lost=0, 31 fps) and only the
     * log said so, after the fact. So the two trigger conditions are made
     * reachable on demand, without breaking the stream:
     *
     *   SHADOW_FAULT_VIDEO_MS=<n>  for n ms, stop advancing `d2_last_rx_ms`
     *       while the datagrams keep arriving, being reassembled and decoded.
     *       The picture is untouched on purpose: what is under test is OUR
     *       reaction and whether the SERVER accepts the field-9 + field-8
     *       pair, not the decoder. D4, S41's key-frame ask and then SRV5 fire
     *       exactly as they would on a real outage, and the D4 recovery branch
     *       reports the whole fake duration when the window closes.
     *   SHADOW_FAULT_AUDIO_G=1     send one `'G'` (0x47) on `:base+30`,
     *       independently of the AUD16 stall ladder, and log the number of
     *       0x02-first-byte frames in the 5 s before and the 5 s after. That
     *       pair IS the measurement for SRV4: ordinary audio frames start with
     *       0x12 and only the codec header / stream descriptor starts with
     *       0x02 (the 271 B frame of `[AUD2] audio frame #1`), so a 0x02 in
     *       the after window is the server honouring
     *       `AEncodingSession::SendHeader_` @0x140bec510.
     *
     * Inert when unset: one getenv per session and one comparison per pass.
     * Both injections wait FAULT_SETTLE_MS of real streaming first, so neither
     * can be confused with a startup artefact - which is precisely the mistake
     * SRV5's own gate made, `d2_last_rx_ms` being armed at `t_start_ms`.
     * Per-session state, never function statics: the "works only on the first
     * session" family KB 3.28 names. Only the env reads are statics, which is
     * process configuration (S34). */
    enum { FAULT_SETTLE_MS = 15000 };   /* healthy streaming before injecting */
    static int g_fault_video_ms = -1;
    if (g_fault_video_ms < 0) {
        const char *e = getenv("SHADOW_FAULT_VIDEO_MS");
        g_fault_video_ms = e ? atoi(e) : 0;
        if (g_fault_video_ms < 0) g_fault_video_ms = 0;
    }
    static int g_fault_aud_g = -1;
    if (g_fault_aud_g < 0) {
        const char *e = getenv("SHADOW_FAULT_AUDIO_G");
        g_fault_aud_g = e ? atoi(e) : 0;
    }
    long long fault_vid_t0_ms = 0;      /* 0 = the window has not opened yet */
    bool      fault_vid_done  = false;  /* one window per session, never two */
    int       fault_g_step    = 0;      /* 0 wait, 1 armed, 2 sent, 3 reported */
    long long fault_g_ms      = 0;      /* when the `G` went out */
    uint32_t  fault_g_hdr0    = 0;      /* 0x02 frames when the census armed */
    uint32_t  fault_g_hdr1    = 0;      /* 0x02 frames when the `G` went out */
    uint32_t  fault_g_aud1    = 0;      /* 0x12 frames when the `G` went out */
    /* === S41b 2026-09-03 - WHAT THE STALL WAS NOT SAYING ===
     *
     * The detector could SAY how long the stream had gone quiet, and nothing
     * more. The question that decides everything is elsewhere: during that
     * silence, did the server's encoder keep producing?
     *
     * The SUFP header carries a u32 at bytes 6-9 which `vid_reasm` publishes in
     * `g_last_frame_id`. Its DELTA between the last packet before and the first
     * after answers that, and it answers whatever the unit is - which is not
     * itself settled:
     *   - if it is a microsecond timestamp (the KB's reading, corrected on
     *     2026-08-21), a 143.9 s stall must yield ~143,900,000;
     *   - if it is a frame counter at ~45 fps, ~6,500.
     * Four orders of magnitude separate the two, so ONE measurement settles the
     * unit AND the question. A delta near zero would mean a server that produced
     * nothing; a delta on the scale of the stall means datagrams lost between
     * the VM and the console - and in that second case the whole discussion
     * about S41 and key-frame requests is beside the point. */
    uint32_t  d4_fid_start    = 0;   /* g_last_frame_id at the instant of the silence */
    unsigned  video_idr_asks  = 0;   /* S41c: key-frame requests during a silence */
    uint32_t  video_reg_count     = 0;
    long long d2_last_rx_ms   = 0;   /* armed at t_start just before the loop:
        review 2026-08-21 - the `d2_last_rx_ms > 0` guard prevented any
        detection when NO video packet ever arrived, that is, the worst case,
        the very one the detector was written for. */
    bool      d2_stalled      = false;
    uint32_t  ovfl_video = 0, ovfl_cursor = 0, ovfl_input = 0;   /* ING-1: per socket, per session */
    bool      diag_mute_said  = false;   /* VI1: SHADOW_DIAG_VIDEO_MUTE_* logged once per session */
    /* === ING-1 2026-09-11 - DID THE LOOP OR THE NETWORK HOLD THE PICTURE ===
     * The console's ~300 ms Wi-Fi freezes are still unattributed: D4 says the
     * video stopped, not whether the network held it or this loop did. Three
     * measures separate the two, without touching a buffer, a wire byte or a
     * default:
     *   - the pass BODY, L5 stage `reception/tour`: stamped at the top of the
     *     body and at its END, just before the poll wait. In the analysis
     *     benches an end-of-body timer caught every in-body absence with no
     *     false positive on network holds; one taken before the poll missed
     *     10-100 % of them. Local causes it can test: journal.c holding g_lock
     *     across fflush, ctrl_tcp_recv_cleartext(..., 100) blocked on a partial
     *     TLS record;
     *   - the gap BETWEEN passes, `max_gap=` on the stats line: end of one
     *     body -> top of the next. A body timer cannot see a thread scheduled
     *     late. The poll bounds a legitimate gap at ~5 ms on Linux/Switch and
     *     ~16 ms on Windows (timer resolution); more is scheduling lateness;
     *   - `batch=`: the largest batch one udp_drain(udp_video) returned. A loop
     *     that was away finds the socket full when it comes back.
     * The two stats-line keys cover the 5 s window and are reset once printed.
     * Session locals, never function statics - the defect family this repo
     * names first. Measured only when latency_enabled() (SHADOW_LATENCE=0
     * restores the uninstrumented loop) and there is a UDP video socket: TCP
     * video (K15i) has none, and the keys then print n/a rather than a zero
     * that would read as a measurement. */
    const bool rx_timed       = latency_enabled() && udp_video >= 0;
    int64_t   rx_pass_end_us  = 0;    /* end of the previous pass body; 0 = nothing to compare */
    int64_t   rx_gap_max_us   = -1;   /* worst end -> top gap of the window; -1 = none yet */
    uint32_t  rx_batch_max      = 0;    /* largest udp_drain(udp_video) batch of the window */
    long long t_last_synth_ms = 0;   /* D5 : injecteur d'input natif */
    unsigned  synth_count     = 0;
    {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        t_start_ms = (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
        t_last_heartbeat_ms = t_start_ms;
        d2_last_rx_ms = t_start_ms;      /* review: armed from the start */

        /* === S57 2026-08-27 - DO NOT TALK WHILE THE SERVER IS ANSWERING ===
         *
         * These two counters were 0, and `now_ms` is a monotonic clock in
         * milliseconds: `now_ms - 0 >= 7500` was therefore TRUE on the very
         * first iteration. Our `0x70` ping (AUD4) and our registration resend
         * (AUD5) both went out ~200 ms after registering the `:base+30`
         * channel.
         *
         * And that is exactly when the first audio frame is in flight: measured
         * from our own logs, it arrives 201 to 227 ms after the registration,
         * and our two emissions fall between 197 and 244 ms. The first seven
         * frames observed fall INSIDE that window.
         *
         * The official client, by contrast, emits NOTHING between its
         * registration and its first frame: its first ping goes out at
         * registration + 7.495 s, and it NEVER re-registers.
         *
         * Correlation over 69 of our sessions, a total separation on one side:
         *     AUD5 fires  -> 0 sessions with sound out of 21
         *     AUD5 silent -> 21 sessions with sound out of 26
         * And the server answers with a `CHANNEL_DOWN(AUDIO)` 40 to 130 ms
         * later.
         *
         * AUD5 had been added to RECOVER a channel that does not start. It was
         * preventing it from starting. So we arm both counters to now: the first
         * ping goes out at +7.5 s like the official client's, and the first
         * resend at +2 s, after the frame should have arrived.
         *
         * `SHADOW_AUD_QUIET=0` restores the old behaviour (both emissions
         * immediate), so the A/B can be redone. */
        {
            const char *e = getenv("SHADOW_AUD_QUIET");
            if (!e || atoi(e) != 0) {
                t_last_cursor_ping_ms = t_start_ms;
                t_last_cursor_reg_ms  = t_start_ms;
                /* === S58 2026-08-27 - S57 HAD FORGOTTEN THE GAMEPAD CHANNEL ===
                 *
                 * S57 armed the audio channel's two counters, both of them LOCAL
                 * variables. The gamepad channel keeps its own in the
                 * `video_feedback_t` struct, zeroed by `memset` - so it stayed
                 * at 0, and its K12 keepalive also went out on the very first
                 * iteration.
                 *
                 * Measured: 61 sessions out of 61 have their first K12 between
                 * 193 and 241 ms after registering the channel; the official
                 * client sends its own at +6.98 s and then every 7.0 s exactly.
                 * And the server declares CONTROLLER down at 220-508 ms (median
                 * 258), in 57 sessions out of 61 - even more often than audio.
                 *
                 * Same defect, same family, neighbouring channel. Fixing audio
                 * without fixing this one was half a fix. */
                fb.t_ka13 = t_start_ms;
            }
        }
    }
    /* G24: reset the emission head of the reorder buffer (GUI reconnection: the
     * statics persist within the process). */
    g_emit_next_sub = -1;
    g_emit_last_sub = -1;
    g_head_since_ms = 0;
    while (1) {
        /* ING-1 2026-09-11 - top of the pass (see rx_timed). It closes the gap
         * the previous pass opened at its end stamp, then clears it: a pass
         * that leaves before its end stamp (none does today) can never get a
         * gap spanning two passes filed in its name. */
        const int64_t rx_pass_t0_us = rx_timed ? latency_now_us() : 0;
        if (rx_pass_end_us) {
            if (rx_pass_t0_us - rx_pass_end_us > rx_gap_max_us)
                rx_gap_max_us = rx_pass_t0_us - rx_pass_end_us;
            rx_pass_end_us = 0;
        }
        if (p->abort_flag && *p->abort_flag) {
            /* S39: say that we SAW the stop request. Without this line there
             * was no way to tell "the flag was never raised" from "it was raised
             * but the loop does not see it". */
            clog("[S39] stop requested - leaving the receive loop");
            /* D4: the summary is what turns isolated events into a RATE. "one
             * stall" means nothing; "14 stalls, 11 of them under 500 ms, over 12
             * minutes of handheld Wi-Fi" can be compared. */
            clog("[D4] video stall summary - %u event(s), %lld ms total, "
                 "pire %lld ms | <500ms=%u <1s=%u <2s=%u >=2s=%u",
                 d4_evts, d4_total_ms, d4_pire_ms,
                 d4_hist[0], d4_hist[1], d4_hist[2], d4_hist[3]);
            stats->exit_reason = 3;
            break;
        }

        /* === G52 2026-08-28 - THE ANNOUNCEMENT NEVER WENT OUT ON DESKTOP ===
         * G50 replaced `ctrl_gamepad_attach`'s direct send with an ARMING,
         * drained by `ctrl_gamepad_replug_tick()`. But its ONLY caller,
         * `padforward::poll()`, is entirely under `#ifdef __SWITCH__` - and
         * `poll()` itself is only called from a Switch block. On desktop the
         * announcement therefore stayed armed FOREVER: the VM created no
         * gamepad, the evdev reader emitted into the void, and the diagnostic
         * displayed "announcement ARMED" indefinitely.
         * Consequence worth remembering: every gamepad A/B measurement made on
         * desktop since G50 is INVALID.
         * On the console, `pad_forward` already drains it; two concurrent drains
         * would fight over G51's pulse queue, which has no lock. */
#ifndef __SWITCH__
        ctrl_gamepad_replug_tick();
#endif

        /* VI1 - SHADOW_DIAG_VIDEO_MUTE_*, see diag_discard_packet. */
        bool vmute = false;
        if (g_diag_mute_at_s == -2) {
            const char *e = getenv("SHADOW_DIAG_VIDEO_MUTE_AT_S");
            g_diag_mute_at_s = e ? atoi(e) : 0;
            const char *m = getenv("SHADOW_DIAG_VIDEO_MUTE_MS");
            if (m && atoi(m) > 0) g_diag_mute_ms = atoi(m);
        }
        if (g_diag_mute_at_s > 0) {
            struct timespec tm_; clock_gettime(CLOCK_MONOTONIC, &tm_);
            const long long el = (long long)tm_.tv_sec * 1000 + tm_.tv_nsec / 1000000 - t_start_ms;
            vmute = el >= (long long)g_diag_mute_at_s * 1000 &&
                    el <  (long long)g_diag_mute_at_s * 1000 + g_diag_mute_ms;
            if (vmute && !diag_mute_said) {
                diag_mute_said = true;
                clog("[DIAG] video coupee volontairement pendant %d ms (SHADOW_DIAG_VIDEO_MUTE_*)",
                     g_diag_mute_ms);
            }
        }
        /* ING-A1 - SHADOW_DIAG_AUDIO_SEQ_JUMP_AT_S, see g_diag_aud_jump_at_s:
         * armed here, consumed by the next audio frame in aud_accept(). */
        if (g_diag_aud_jump_at_s == -2) {
            const char *e = getenv("SHADOW_DIAG_AUDIO_SEQ_JUMP_AT_S");
            g_diag_aud_jump_at_s = e ? atoi(e) : 0;
        }
        if (g_diag_aud_jump_at_s > 0 && ctx.aud_diag_jump == 0) {
            struct timespec tj; clock_gettime(CLOCK_MONOTONIC, &tj);
            const long long el = (long long)tj.tv_sec * 1000 + tj.tv_nsec / 1000000 - t_start_ms;
            if (el >= (long long)g_diag_aud_jump_at_s * 1000) ctx.aud_diag_jump = 1;
        }
        int got_video  = udp_drain(udp_video,  pkt, sizeof(pkt),
                                   vmute ? diag_discard_packet : on_video_packet, &ctx, &ovfl_video);
        int got_cursor = udp_drain(udp_cursor, pkt, sizeof(pkt), on_cursor_packet, &ctx, &ovfl_cursor);
        (void)got_cursor;
        if (rx_timed && (uint32_t)got_video > rx_batch_max)
            rx_batch_max = (uint32_t)got_video;   /* ING-1 2026-09-11 - batch=, see rx_batch_max */
        /* G53 - the gamepad's downstream path. The socket is already connected
         * and non-blocking; this is one more read, with no other change. */
        if (udp_input >= 0)
            (void)udp_drain(udp_input, pkt, sizeof(pkt), on_gamepad_packet, &ctx, &ovfl_input);

        /* G24: drain the ordered queue from the loop as well - guarantees the
         * head expires (~300 ms) even when no new video chunk arrives to trigger
         * it through on_video_packet. */
        vid_drain_ordered(&ctx, vid_now_ms(), got_video == 0);  /* timeout seulement socket empty */

        if (stats->udp_video_pkts > 0) any_video = true;

        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        long long now_ms = (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
        int sec = (int)((now_ms - t_start_ms) / 1000);
        if (t_last_reg_ms == 0) t_last_reg_ms = now_ms;

        /* === S1 2026-08-21 - STRICT CONTROL CHANNEL MIMICRY ===
         * Diff of the guided capture: in steady state (143 s), the official
         * client emits on `:base+11` ONLY frames of 92/93/94 bytes - the
         * heartbeat, nothing else. We emit FOURTEEN different sizes:
         * event-driven IFRs (G6), periodic DisplayReady (N47),
         * NotifyResolution, Flush... about thirty messages per session that the
         * desktop NEVER sends.
         * Hypothesis: these surplus messages make the server treat us as
         * something other than a standard client, which would explain why it
         * never enables the input channel's return path (the 104 B `@35=0x04`
         * messages the desktop receives and we never do), even though the
         * content of our events is now byte-exact.
         * SHADOW_CTRL_STRICT=1 lets nothing but the heartbeat through. */
        static int g_ctrl_strict = -1;
        if (g_ctrl_strict < 0) {
            const char *e = getenv("SHADOW_CTRL_STRICT");
            g_ctrl_strict = e ? atoi(e) : 0;
        }

        /* D1 2026-08-20 - the server closes the control channel (clean FIN,
         * err=-397) and cuts UDP right after (ECONNREFUSED). Before this guard
         * we kept writing to the dead socket: tight loop, 37,880 `err=-397`
         * lines + 20,078 `recv header FAIL` in 9 minutes, and on the user's side
         * a frozen picture without a single message. We exit cleanly with
         * exit_reason=2 (server_kicked) so the GUI can react. */
        if (ctrl_tcp_peer_closed(tcp)) {
            clog("[D1] *** SESSION CLOSED BY THE SERVER at t=%ds *** err=%d - "
                 "video=%u pkts / %u frames decodees / %u RegisterSession periodiques. "
                 "Clean exit (exit_reason=2).",
                 sec, ctrl_tcp_close_err(tcp), stats->udp_video_pkts,
                 stats->frames_decoded, reg_count);
            emit_progress(p, "disconnected", "server closed the session");
            stats->exit_reason = 2;
            break;
        }

        /* D2 - video stall detector. Without it, the stream stopping leaves NO
         * trace at all: the picture freezes and the log carries on as if nothing
         * had happened. We log once on the stall, once on recovery. */
        /* SRV-FAULT 2026-10-02 (see the block near the top of this function):
         * the injected video outage. `hiding` withholds the arrival timestamp
         * and nothing else - the datagrams of this pass were already drained,
         * reassembled and handed to the decoder above. */
        bool fault_vid_hiding = false;
        if (g_fault_video_ms > 0 && !fault_vid_done && stats->udp_video_pkts > 0
            && now_ms - t_start_ms >= FAULT_SETTLE_MS) {
            if (fault_vid_t0_ms == 0) {
                fault_vid_t0_ms = now_ms;
                clog("[SRV-FAULT] video: hiding the arrival timestamp for %d ms "
                     "from t=%ds (%u packets so far) - the picture keeps running; "
                     "D4 then SRV5 must fire",
                     g_fault_video_ms, sec, stats->udp_video_pkts);
            }
            if (now_ms - fault_vid_t0_ms < g_fault_video_ms)
                fault_vid_hiding = true;
            else {
                fault_vid_done = true;
                clog("[SRV-FAULT] video: window closed after %lld ms (t=%ds, "
                     "%u packets) - the next arrival ends the fake outage",
                     now_ms - fault_vid_t0_ms, sec, stats->udp_video_pkts);
            }
        }
        if (stats->udp_video_pkts != d2_last_pkts && !fault_vid_hiding) {
            /* D4b 2026-08-28 - TAKE THE DURATION BEFORE UPDATING THE MARK.
             * First version: `d2_last_rx_ms = now_ms` ran BEFORE the
             * computation, which therefore returned `0 ms` for every stall - 68
             * events measured at zero, the whole histogram in the first bucket.
             * The real durations had to be recomputed by hand from the log
             * timestamps, where one of them turned out to be 143 SECONDS. */
            const long long d4_duree = now_ms - d2_last_rx_ms;
            d2_last_pkts  = stats->udp_video_pkts;
            d2_last_rx_ms = now_ms;
            if (d2_stalled) {
                /* D4: the duration is only known at RECOVERY, so that is where
                 * the measurement belongs. The delta of control frames during
                 * the stall is the discriminator: non-zero means the host was
                 * still talking and only the media path went quiet - which a
                 * TCP transport would recover. Zero means the machine or the
                 * link, and TCP can do nothing about that. */
                const long long duration = d4_duree;
                const uint32_t ctrl_during = g_ctrl_rx_frames - d4_ctrl_debut;
                d4_evts++;
                d4_total_ms += duration;
                if (duration > d4_pire_ms) d4_pire_ms = duration;
                d4_hist[duration < 500 ? 0 : duration < 1000 ? 1 : duration < 2000 ? 2 : 3]++;
                /* D4b - DO NOT CONCLUDE UNDER 1.5 s.
                 * The control channel exchanges roughly every 500 ms. On a
                 * 300 ms stall, counting zero frames says NOTHING about whether
                 * the host is alive: that is the cadence, not silence. The first
                 * version nevertheless printed `l'hote se taisait aussi :
                 * machine ou lien` (the host went quiet too: machine or link)
                 * in that case - a false conclusion, stated as fact. We only
                 * decide beyond three intervals. */
                const char *verdict =
                    (duration < 1500) ? "too short to conclude (ctrl cadence ~500 ms)"
                  : (ctrl_during > 0) ? "the host was still talking: THE MEDIA PATH"
                                      : "the host went quiet too: the machine or the link";
                /* S41b: the SUFP timestamp delta. Unsigned and modular, so a wrap
                 * reads as a large value rather than a negative one - which is
                 * the sensible reading here. */
                const uint32_t fid_delta = g_last_frame_id - d4_fid_start;
                clog("[D4] video RESTORED at t=%ds - a %lld ms stall, "
                     "%u control frames received DURING (%s) | "
                     "SUFP timestamp +%u over %lld ms (%s) | %u key-frame request(s)",
                     sec, duration, ctrl_during, verdict,
                     fid_delta, duration,
                     /* The verdict this delta allows, and that one only. */
                     (duration < 1500)         ? "too short"
                   : (fid_delta > (uint32_t)(duration * 500))
                                               ? "l'encodeur a TOURNE : datagrammes perdus en route"
                   : (fid_delta < 100)         ? "l'encodeur n'a RIEN produit"
                                               : "between the two, to be taken up again",
                     video_idr_asks);
                /* === AUD-DEDUP-3 2026-09-11 - WHAT THE AUDIO DID DURING THE OUTAGE ===
                 * The audio numbers the SENT frames, 100 a second with sound, on
                 * the same link: an independent witness of every freeze. Frames
                 * lost ~ duration / 10 ms: the link LOST packets. None lost but an
                 * arrival silence ~ the duration: the link DELAYED them. None lost
                 * and normal arrivals: the VIDEO path alone. The count is lost +
                 * pending: a 300 ms freeze leaves ~30 missing frames, fewer than
                 * the 64 that make a loss final, so `lost` alone would read 0. The
                 * silence still running is included, for audio that has not come
                 * back yet. The line above is unchanged; this one is new. */
                if (ctx.aud_loss.primed) {
                    const int64_t  tn   = vid_now_ms();
                    int64_t        gap  = ctx.aud_gap_max_ms;
                    if (ctx.aud_last_accept_ms > 0 && tn - ctx.aud_last_accept_ms > gap)
                        gap = tn - ctx.aud_last_accept_ms;
                    const uint32_t miss = ctx.aud_loss.lost + audio_loss_pending(&ctx.aud_loss);
                    clog("[AUD17] during the video stall: audio lost=%u, "
                         "plus long silence d'arrivee=%lld ms",
                         miss >= ctx.aud_d4_missing0 ? miss - ctx.aud_d4_missing0 : 0u,
                         (long long)gap);
                } else {
                    clog("[AUD17] during the video stall: no audio frame accepted "
                         "in the session, no witness");
                }
                d2_stalled = false;
            }
        } else {
            /* SRV-FAULT: swallow the arrival, so the next pass still compares
             * equal and the detector stays in this silent branch for the whole
             * window. Without it the test branch above would be taken again
             * and the injected silence would never grow. */
            if (fault_vid_hiding) d2_last_pkts = stats->udp_video_pkts;
            static int g_stall_ms = -1;
            if (g_stall_ms < 0) {
                const char *e = getenv("SHADOW_STALL_MS");
                g_stall_ms = e ? atoi(e) : 300;
                if (g_stall_ms < 50) g_stall_ms = 50;
            }
            if (!d2_stalled && now_ms - d2_last_rx_ms > g_stall_ms) {
                d2_stalled = true;
                d4_fid_start = g_last_frame_id;   /* S41b : voir plus haut */
                video_idr_asks = 0;
                d4_ctrl_debut = g_ctrl_rx_frames;
                /* AUD-DEDUP-3 - the audio witness of this outage starts here. */
                ctx.aud_d4_missing0 = ctx.aud_loss.lost + audio_loss_pending(&ctx.aud_loss);
                ctx.aud_gap_max_ms  = 0;
                int docked = -1, strength = -1;
                const int link = shadow_link_info(&docked, &strength);
                clog("[D4] *** VIDEO SILENT *** for %lld ms (t=%ds) - "
                     "ctrl_ferme=%d ctrl_trames=%u | %s %s force=%d | "
                     "derniere image #%u",
                     now_ms - d2_last_rx_ms, sec, (int)ctrl_tcp_peer_closed(tcp),
                     g_ctrl_rx_frames,
                     docked == 1 ? "docke" : docked == 0 ? "portable" : "mode?",
                     link == 1 ? "Wi-Fi" : link == 2 ? "Ethernet" : "lien?",
                     strength, stats->frames_decoded);
            }
        }

        /* === S41 2026-08-25 - TAKING ACTION WHEN THE VIDEO GOES SILENT ===
         *
         * D2 could SAY that the stream was dead; it did nothing about it.
         * Measured on the console: at t=58 s the video UDP stops dead,
         * `ctrl_peer_closed=0` (the server still answers on the control channel
         * and honours our key frame requests on `:base+20`) - but not another
         * datagram for 208 s. The picture stays frozen and the user has no
         * option but to force-quit the application.
         *
         * The cursor channel already had its recovery (AUD5, resending the
         * registration every 2 s as long as nothing arrives); video, the most
         * important of the three, had none. We make the treatment symmetric. One
         * registration too many is harmless - it carries the same identifier -
         * whereas a lost subscription costs the entire session.
         *
         * `SHADOW_VIDEO_REREG=0` disables the recovery (back to the previous
         * behaviour: observe without acting). */
        /* === S41c 2026-09-03 - DURING A SILENCE WE ASKED FOR NO KEY FRAME
         *                         AT ALL. THIS IS THE REPO'S DEFECT FAMILY. ===
         *
         * Census of what raises `g_idr_needed`: `vid_reasm.c:499`, `:665` and
         * `:1059` all sit inside the handling of a PACKET; G13 in
         * `ctrl_session_glue.c:558` wants fifteen pictures DELIVERED to the
         * decoder; G34 at `:703` wants a picture dropped from the queue; G18 at
         * `:552` wants a bitstream error (and it has defaulted to 0 since G37).
         * **Every one of them requires something to arrive.**
         *
         * So when nothing arrives any more, the client asks for NOTHING.
         * Checked in the logs: not one `[G13]` or `[G18]` line during either of
         * the 143.9 s and 99.2 s stalls. This is word for word the defect
         * CLAUDE.md names at its head for audio (S57): the mechanism added to
         * catch a channel that does not start was what prevented it starting.
         *
         * AND THE REQUEST DOES NOT GO WHERE YOU WOULD THINK. Over UDP, `iP`
         * leaves on `udp_video` - precisely the socket that has gone quiet. The
         * only route that exists in both transports is `Request_Flush` on
         * `:base+11`, and that channel is ALIVE during the stall: 304 control
         * frames exchanged during the first, 209 during the second. So that is
         * the one we arm, not the other.
         *
         * Guards, because G5 already cost a whole session: this CANNOT create
         * the autofocus cycle, which was born of requests sent while video was
         * FLOWING. Here the condition is total silence, which a healthy stream
         * never knows. Plus one request every 3 s and a ceiling of 8 - beyond
         * that, a key frame is not what is missing. */
        if (d2_stalled) {
            static int g_idr_on_stall = -1;
            static long long t_last_idr_ask_ms = 0;
            if (g_idr_on_stall < 0) {
                const char *e = getenv("SHADOW_IDR_ON_STALL");
                g_idr_on_stall = e ? atoi(e) : 1;
            }
            if (g_idr_on_stall && video_idr_asks < 8
                && now_ms - t_last_idr_ask_ms >= 3000) {
                t_last_idr_ask_ms = now_ms;
                video_idr_asks++;
                g_flush_tcp_needed = 1;   /* sent further down, on the control channel */
                if (video_idr_asks <= 2)
                    clog("[S41c] video silent for %lld ms - key frame requested "
                         "on the control channel (#%u)",
                         now_ms - d2_last_rx_ms, video_idr_asks);
            }
        }

        if (d2_stalled && udp_video >= 0) {
            /* === S41 MEASURED, AND THE MEASUREMENT SAYS IT DOES NOTHING (2026-09-03) ===
             *
             * The re-registration fired ~134 times during the instrumented
             * session's stalls, and the server never answered once: the two long
             * stalls absorbed ~67 and ~47 resends, one every 2.000 s across
             * 143.9 s and 99.2 s, with zero packets in return. Across the 20
             * stalls where at least one resend went out, the delay between the
             * last resend and the recovery spreads over the whole 2 s window
             * (median 705 ms): the two events are independent.
             *
             * The packet is nevertheless correct, byte for byte. What is missing
             * is the CONTEXT: when the official client lands in this state it
             * does not resend the registration, it REDOES THE WHOLE BOOTSTRAP -
             * a new ClientHello on `:base+11`, then Capabilities,
             * Authentication, Encryption, DisplayConfig, RegisterSession and the
             * eight announcements, and ONLY THEN the registration, sent to
             * `:base+10` AND `:base+30` within the same millisecond. Video comes
             * back 60 ms later. We were sending step 12 of 12.
             *
             * Defaulted to 0. It repairs nothing measurable, and it breaks the
             * rule the audio path learned the hard way (S57): speaking on a
             * channel that does not answer, with no guard and no ceiling, 67
             * times in a row, is precisely what can kill it.
             * `SHADOW_VIDEO_REREG=1` restores it. */
            static int g_video_rereg = -1;
            if (g_video_rereg < 0) {
                const char *e = getenv("SHADOW_VIDEO_REREG");
                g_video_rereg = e ? atoi(e) : 0;
            }
            /* === SRV5 2026-10-02 - THE RECOVERY S41 WAS MISSING ==============
             *
             * === AND ITS FIRST VERSION FIRED ON A HEALTHY CHANNEL (same day) ==
             * Measured on the first live Windows session: two
             * `[SRV5] channel idx=0 RE-ANNOUNCED` lines while the video was
             * perfectly fine (9995 packets, lost=0, 31 fps, rx_age=0ms). The
             * cause is the gate it was hung on: `d2_stalled` flips as soon as
             * `now_ms - d2_last_rx_ms > SHADOW_STALL_MS`, which defaults to
             * **300 ms**. That is an ORACLE - it exists to date an outage in the
             * log - not a statement that the channel is dead, and
             * `d2_last_rx_ms` is armed at `t_start_ms`, so the very wait for the
             * first datagram trips it.
             *
             * Re-announcing a LIVE channel is not harmless: it hands the server
             * a second stream registration for a client that already works. So
             * this action gets its own threshold, on AUD16's reasoning: a silent
             * VM still sends ~10 packets/s on this channel (video runs at ~400),
             * so several seconds at zero is a death and not a hiccup. Two
             * conditions, both needed:
             *   - at least one datagram has arrived THIS session, so startup can
             *     never qualify;
             *   - and the silence is at least SHADOW_VIDEO_REANN_MS (5 s).
             * `g_video_rereg` had the same flaw and never showed it, because it
             * defaults to 0.
             *
             * The comment above is right that step 12 of 12 repairs nothing, and
             * the server binary now says why: a stream client the server has
             * invalidated is skipped by the channel's demux, so the `A` datagram
             * is discarded before anything reads it, whatever source port it
             * comes from. Re-announcing the channel on `:base+11` is the only
             * door back in - which is also, note, exactly the sequence the
             * official client was observed performing on its reconnect (the
             * "step 12 of 12" list above is that same sequence).
             *
             * Bounded like everything else on this path: at most one every 5 s,
             * and 6 per session. A re-announcement is a control message, not a
             * flood on a dead socket, so S57's lesson does not apply to it - but
             * the ceiling stays, because a channel that does not come back after
             * six will not come back after sixty.
             * `SHADOW_VIDEO_REANN=0` removes it; `SHADOW_VIDEO_REREG=1` still
             * restores the old datagram resend, independently. */
            static int g_video_reann = -1;
            if (g_video_reann < 0) {
                const char *e = getenv("SHADOW_VIDEO_REANN");
                g_video_reann = e ? atoi(e) : 1;
            }
            static int g_reann_ms = -1;
            if (g_reann_ms < 0) {
                const char *e = getenv("SHADOW_VIDEO_REANN_MS");
                g_reann_ms = e ? atoi(e) : 5000;
                if (g_reann_ms < 2000) g_reann_ms = 2000;
            }
            if (g_video_reann && video_reann_count < 6
                && stats->udp_video_pkts > 0
                && now_ms - d2_last_rx_ms >= g_reann_ms
                && now_ms - t_last_video_reann_ms >= 5000) {
                t_last_video_reann_ms = now_ms;
                /* The duration is in the line: without it, a future reader
                 * cannot tell a real death from the false positive this guard
                 * was added to stop. */
                char why[64];
                snprintf(why, sizeof why, "video silent %llu ms",
                         (unsigned long long)(now_ms - d2_last_rx_ms));
                if (session_reannounce_channel(tcp, &ctx, &hb_seq,
                                               SHADOW_CHAN_IDX_VIDEO, why))
                    video_reann_count++;
            }
            if (g_video_rereg && now_ms - t_last_video_reg_ms >= 2000) {
                uint8_t reg[32];
                int rlen = ctrl_build_udp_register(reg, sizeof(reg), auth_reply.hash);
                if (rlen > 0 && send(udp_video, (const char *)reg, (size_t)rlen, 0) == rlen) {
                    video_reg_count++;
                    if (video_reg_count <= 3 || video_reg_count % 10 == 0)
                        clog("[S41] reenvoi de l'enregistrement video :base+10 "
                             "(#%u, silent for %lld ms)",
                             video_reg_count, now_ms - d2_last_rx_ms);
                }
                t_last_video_reg_ms = now_ms;
            }
        }

        /* gE feedback packet on the video UDP socket -
         * RemoteBitrateEstimatorIOChannel.
         * RE TIER 1+3 2026-05-16 (sub_C49FE0 + rbe RE): byte-exact format:
         *   [0x67 0x45 0x01][u32_LE bytes_rx_x4][s32_LE avg_delay_us][u32_LE last_sufp_id]
         *
         * **V14 fix 2026-05-16**: we were sending `bytes_rx=0` (= "I received
         * nothing") -> the server stayed conservative (less multi-NAL, bottom
         * ratio 0.81%). With real `bytes_rx*4` the server should switch to
         * aggressive multi-NAL.
         *
         * Frequency ~20 Hz (= 50 ms, confirmed on the MASTER capture, 30+
         * samples). */
        session_video_feedback_tick(&ctx, &fb, now_ms,
                                    udp_video, udp_input, port_base_used,
                                    g_ctrl_strict);

        /* CLIP3: the clipboard's two halves, both on this thread. Applies the
         * paste the receive thread staged, then looks at the local clipboard
         * every SHADOW_CLIPBOARD_POLL_MS. Returns immediately when the channel
         * is not open, which is the console case and the SHADOW_CLIPBOARD=0
         * case. */
        session_clipboard_tick(&ctx, now_ms);

        /* Send VideoEncodingConfig + Ready + DisplayReady ~3 s after bootstrap.
         * RE 2026-05-13 strace: the desktop sends seq=17 = VideoEncodingConfig
         * (= field 13 with bitrate+fps), seq=18 = Ready, seq=19 = DispReady.
         * Our flow: seq=13..16 = 4 heartbeats, seq=17 = config, seq=18-19 =
         * ready.
         *
         * Q1+C 2026-05-18: VideoEncodingConfig advertises bitrate+fps. It now
         * passes the Q1 params (consistent with what we announce in CI_BODY_5).
         * If Q1 is not overridden -> keep the desktop defaults, 1024561 bps +
         * 142.0f. */
        /* === S47 2026-08-26 - THIS 3 s DELAY IS A SIDE EFFECT ===
         *
         * It does not come from measuring the delay itself: the comment above
         * says so - we wanted to land on the sequence numbers 17-19 observed in
         * May, and we PADDED the sequence with heartbeats to get there. Three
         * seconds of waiting for a cosmetic effect.
         *
         * Fresh capture of the official client, same VM, with sound
         * (`captures_audio_MEME_VM_*`): it sends this pair at sequences 12-13,
         * **0.8 s** after its last channel announcement. Us: 3.4 s, after five
         * heartbeats into the void.
         *
         * That is the only behavioural difference left, and it fits the defect:
         * the `:base+30` audio channel starts on roughly one session in three,
         * in a binary way decided at bootstrap. If the server arms the
         * subscription within a window, we miss it most of the time.
         *
         * `SHADOW_READY_DELAY_MS` sets the wait (default 3000 until a
         * measurement settles it; 800 reproduces the official client). */
        static int g_ready_delay = -1;
        if (g_ready_delay < 0) {
            const char *e = getenv("SHADOW_READY_DELAY_MS");
            g_ready_delay = e ? atoi(e) : 3000;
            if (g_ready_delay < 0 || g_ready_delay > 10000) g_ready_delay = 3000;
        }
        if (!ready_sent && (now_ms - t_start_ms) >= g_ready_delay) {
            uint8_t rmbuf[128];
            /* Q1 params (= the same resolved values as for vparams CI_BODY_5) */
            /* G3 2026-06-02: default 15 Mbps (it was 1024561, ~1 Mbps = the
             * desktop client's initial value BEFORE its congestion-control ramp,
             * which we do not have). At 1 Mbps, 1080p = heavy noise on motion.
             * 15 Mbps validated at runtime = clean picture. Override with
             * SHADOW_BITRATE_MBPS. TODO V17: a real gE ramp. */
            uint32_t vec_bitrate_bps = 15000000;  /* G3: 15 Mbps validated (was ~1 Mbps) */
            if (p->max_bitrate_mbps > 0) vec_bitrate_bps = p->max_bitrate_mbps * 1000000U;
            {
                const char *e;
                if ((e = getenv("SHADOW_BITRATE_MBPS")) && atoi(e) > 0)
                    vec_bitrate_bps = (uint32_t)atoi(e) * 1000000U;
            }
            /* CFG-4 2026-09-11 - no frame rate here any more: the message has
             * had no frame-rate field since S18, and this line printed a phantom
             * "fps=142.0" while the SPS said 143.85. */
            clog("[Q1+C] VideoEncodingConfig : bitrate=%.1f Mbps",
                 (double)vec_bitrate_bps / 1e6);

            /* Skip seq 13..16 (= 4 heartbeats over 4 seconds) */
            /* K15 2026-08-21 - VideoEncodingConfig is the oneof field f13
             * (= kUpdateSession). The inventory compared against the capture
             * shows the official client **NEVER** sends it: it emits f1x1 f4x1
             * f12x1 f10x1 f8x8 f3x66 f7x2 f6x2 f9x8, with neither f13 nor f14. A
             * KB note already flagged it as a false lead without our having
             * removed it. We turn it off by default; SHADOW_SEND_VEC=1 puts it
             * back (useful to drive bitrate/fps through SHADOW_BITRATE_MBPS). */
            static int g_send_vec = -1;
            if (g_send_vec < 0) {
                const char *ev = getenv("SHADOW_SEND_VEC");
                /* S18 2026-08-21 - RE-ENABLED. K15 had turned it off after
                 * observing that the desktop never emits f13: that was true in
                 * captures where nobody touched the settings. The `settings`
                 * capture shows it emits one **on every bitrate change**, and
                 * that is precisely the live bitrate adjustment mechanism. Our
                 * structure now being correct (see S18 in ctrl_msgs.c), we send
                 * it again. */
                g_send_vec = ev ? atoi(ev) : 1;
            }
            int cn = g_send_vec
                   ? ctrl_build_video_encoding_config_ex(rmbuf, sizeof(rmbuf),
                                                          hb_seq + 4,
                                                          vec_bitrate_bps)
                   : -1;
            const bool f13_sent = cn > 0 && ctrl_tcp_send_cleartext(tcp, rmbuf, (size_t)cn);
            /* CFG-1: the wire is reported to G19 under the new rule only. A
             * reported wire makes G19 re-send its own value when they differ,
             * which the old rule never did: SHADOW_ADAPT_USER_CAP=0 must
             * restore it exactly. */
            if (f13_sent && g_adapt_user_cap)
                bitrate_ctl_on_wire(&bctl, vec_bitrate_bps / 1000000u);
            int rn = ctrl_build_ready_msg(rmbuf, sizeof(rmbuf), hb_seq + 5);
            if (rn > 0) ctrl_tcp_send_cleartext(tcp, rmbuf, (size_t)rn);
            int dn = ctrl_build_display_ready_msg(rmbuf, sizeof(rmbuf), hb_seq + 6);
            if (dn > 0) ctrl_tcp_send_cleartext(tcp, rmbuf, (size_t)dn);
            hb_seq += 7;  /* skip seq 13..16 (HB) + 17 (config) + 18 (Ready) + 19 (DispReady) */

            ready_sent = true;
        }

        /* N35 2026-05-14: repeat NotifyResolution VERY often (= 200 ms) to keep
         * the bottom slice flowing. N34 at 1 Hz made the bottom slice appear
         * temporarily (= frame 0001 = 60% visible, sea + icons). More often ->
         * better odds of stabilising a continuous bottom.
         * Also: the first one goes out IMMEDIATELY after bootstrap (= before the
         * 3 s delay). Period configurable through SHADOW_NRES_PERIOD_MS
         * (default 200 ms). */
        /* N37 2026-05-14: USER FEEDBACK = the decoder builds the picture
         * PROGRESSIVELY from top to bottom after each NotifyResolution
         * (= server-side progressive refresh). A periodic NotifyResolution
         * RESETS that progress, so it never reaches the bottom.
         * Fix: send NotifyResolution ONCE at the start only and let the decoder
         * accumulate. Period configurable:
         * SHADOW_NRES_PERIOD_MS=0 -> off (the default since N37)
         * SHADOW_NRES_PERIOD_MS=N -> re-enable the periodic send every N ms */
        static int g_nres_period_ms = -1;   /* cache d'env : reste static */
        if (g_nres_period_ms < 0) {
            const char *e = getenv("SHADOW_NRES_PERIOD_MS");
            g_nres_period_ms = e ? atoi(e) : 0;  /* default 0 = no periodic */
        }
        bool should_send = false;
        if (ready_sent && !g_nres_initial_sent) {
            /* first send 100 ms after ready */
            if (now_ms - t_start_ms >= 3100) {
                should_send = true;
                g_nres_initial_sent = true;
            }
        } else if (g_nres_period_ms > 0
                   && (now_ms - t_last_notify_res_ms) >= g_nres_period_ms) {
            should_send = true;
        }
        if (should_send) {
            uint8_t nrbuf[128];
            /* K15 2026-08-21 - NotifyResolution is the oneof field f14, which
             * the official client NEVER emits (compared inventory: it sends
             * f1x1 f4x1 f12x1 f10x1 f8x8 f3x66 f7x2 f6x2 f9x8). Off by default.
             * We do not use SHADOW_CTRL_STRICT for this: that mode also removes
             * f6 and f7, which the desktop genuinely does send.
             * SHADOW_SEND_NRES=1 puts it back. */
            static int g_send_nres = -1;
            if (g_send_nres < 0) {
                const char *en = getenv("SHADOW_SEND_NRES");
                g_send_nres = en ? atoi(en) : 0;
            }
            int nrn = (g_ctrl_strict || !g_send_nres) ? -1
                    : ctrl_build_notify_resolution(nrbuf, sizeof(nrbuf), hb_seq, width, height);
            if (nrn > 0 && ctrl_tcp_send_cleartext(tcp, nrbuf, (size_t)nrn)) {
                clog("[N37] NotifyResolution %ux%u seq=%u (initial=%d period_ms=%d)",
                     width, height, hb_seq, !g_nres_initial_sent || t_last_notify_res_ms == 0,
                     g_nres_period_ms);
                hb_seq++;
                t_last_notify_res_ms = now_ms;
            }
        }

        /* V16 2026-05-16 (TIER 7 T2 breakthrough): control recv loop to ACK the
         * server's pushes. The server periodically pushes a **109 B
         * kRequestFlush** (= Reply.f3.f6, wire pattern `1a XX 32 YY`), and the
         * desktop answers with a 92 B kFlush in under 50 ms. Our client does 0
         * receives after bootstrap, so it ignores every server push.
         * Hypothesis: the server waits for the ACK before releasing aggressive
         * multi-NAL -> root cause of the taskbar bug.
         * SHADOW_AUTO_FLUSH_ACK=0 disables it. Default ON. ING-2: the toggle is
         * read before the loop, since the wait at its bottom needs it too. */
        if (g_auto_flush_ack && ctrl_tcp_has_pending(tcp)) {
            uint8_t rxbuf[4096];
            size_t rxlen = 0;
            /* ING-2 2026-09-11 - the reply is stamped HERE, on the microsecond
             * clock, just before it is read - no longer with `now_ms`. This
             * stays the ONLY read of the channel: the wait below only wakes
             * the loop, and it is ctrl_tcp_has_pending() that decides. */
            const int64_t t_rx = latency_now_us();
            const bool rx_ok = ctrl_tcp_recv_cleartext(tcp, rxbuf, sizeof(rxbuf), &rxlen, 100);
            if (!rx_ok && ctrl_in_poll) {
                /* ING-2 - the spin guard. A read that fails can leave the
                 * socket readable (FIN, reset, a frame we could not take):
                 * waiting on it would then return at once, on every pass. */
                ctrl_in_poll = false;
                clog("[ING2] control channel removed from the wait at t=%ds: read failed (%s)",
                     sec, ctrl_tcp_peer_closed(tcp) ? "peer closed" : "error or timeout");
            }
            if (rx_ok && rxlen >= 4) {
                /* === S118 2026-09-03 - THE ROUND TRIP, HERE AND NOWHERE ELSE ===
                 *
                 * The server echoes our request's sequence number in field 1 of
                 * its reply (KB §3.49, established over 170 replies). This is
                 * the only place in the client where a reply and its request
                 * meet, hence the only place a NETWORK latency can be measured:
                 * of the report's twelve stages, one contains network and it
                 * measures only jitter.
                 *
                 * No traffic is added - we timestamp a heartbeat we were already
                 * sending. An unsolicited message carries none of our numbers,
                 * and `rtt_reply` rejects it rather than inventing a round trip
                 * of zero. */
                {
                    ann_reply_t ar;
                    (void)ann_reply_parse(rxbuf, rxlen, &ar);   /* seq even when this is not an announcement */

                    /* === HID1 2026-10-02 - THE MEASUREMENT =================
                     *
                     * The one reply whose shape settles the `[C70]` half of
                     * `hid_lock.h`: the field numbers of the three locks inside
                     * the Hid message are read off the server's memory layout,
                     * not off a message. This walks the reply that answers our
                     * probe - matched by its sequence number, so it cannot be
                     * confused with a heartbeat's - and prints the field
                     * numbers it actually contains.
                     *
                     * Shape only: `f<n>:len<n>` and the lock states, never the
                     * raw bytes. The reply carries nothing secret, but a habit
                     * of dumping control bodies into the log is how a session
                     * token ends up in one (SEC2).
                     *
                     * Once per session, and only when the probe was armed. */
                    if (fb.hid_probe_sent && !fb.hid_reply_logged
                        && ar.seq && ar.seq == fb.hid_probe_seq) {
                        fb.hid_reply_logged = 1;
                        char shape[256]; int so = 0;
                        shape[0] = '\0';
                        /* Walk the top level for f3 (the Response), then the
                         * Response for the Hid field, then the Hid body. */
                        const uint8_t *resp = NULL; size_t rl2 = 0;
                        int off2 = 0;
                        while ((size_t)off2 < rxlen) {
                            uint32_t fn = 0, wt = 0;
                            const int nx = pb_read_tag(rxbuf, rxlen, off2, &fn, &wt);
                            if (nx < 0) break;
                            if (wt == 2) {
                                const uint8_t *sb = NULL; size_t sl = 0;
                                const int a2 = pb_read_lendelim(rxbuf, rxlen, nx, &sb, &sl);
                                if (a2 < 0) break;
                                if (so < (int)sizeof shape - 24)
                                    so += snprintf(shape + so, sizeof shape - so,
                                                   "f%u:len%u ", fn, (unsigned)sl);
                                if (fn == 3) { resp = sb; rl2 = sl; }
                                off2 = a2;
                            } else {
                                const int a2 = pb_skip_field(rxbuf, rxlen, nx, wt);
                                if (a2 < 0) break;
                                off2 = a2;
                            }
                        }
                        clog("[HID1] reply to seq=%u, %zu bytes, top level: %s",
                             ar.seq, rxlen, shape);

                        const uint8_t *hid = NULL; size_t hl = 0;
                        if (resp) {
                            char rs[160]; int ro = 0; rs[0] = '\0';
                            int o3 = 0;
                            while ((size_t)o3 < rl2) {
                                uint32_t fn = 0, wt = 0;
                                const int nx = pb_read_tag(resp, rl2, o3, &fn, &wt);
                                if (nx < 0) break;
                                if (wt == 2) {
                                    const uint8_t *sb = NULL; size_t sl = 0;
                                    const int a2 = pb_read_lendelim(resp, rl2, nx, &sb, &sl);
                                    if (a2 < 0) break;
                                    if (ro < (int)sizeof rs - 24)
                                        ro += snprintf(rs + ro, sizeof rs - ro,
                                                       "f%u:len%u ", fn, (unsigned)sl);
                                    if (fn == HID_REQ_FIELD) { hid = sb; hl = sl; }
                                    o3 = a2;
                                } else {
                                    const int a2 = pb_skip_field(resp, rl2, nx, wt);
                                    if (a2 < 0) break;
                                    o3 = a2;
                                }
                            }
                            clog("[HID1] Response (f3) carries: %s%s", rs,
                                 hid ? "" : "- NO field 6, so the reply is not a Hid reply");
                        }

                        if (hid) {
                            hid_locks got;
                            uint32_t seen = 0;
                            const bool ok = hid_lock_parse_reply(hid, hl, &got, &seen);
                            clog("[HID1] MEASURED - Hid body %zu bytes, fields seen 0x%x, "
                                 "parse %s | num=%s caps=%s scroll=%s",
                                 hl, seen, ok ? "ok" : "FAILED",
                                 got.num.known    ? (got.num.on    ? "on" : "off") : "absent",
                                 got.caps.known   ? (got.caps.on   ? "on" : "off") : "absent",
                                 got.scroll.known ? (got.scroll.on ? "on" : "off") : "absent");
                            /* The verdict on [C70], said plainly so a reader does
                             * not have to decode the bitmask. */
                            const uint32_t want = (1u << HID_LOCK_F_NUM)
                                                | (1u << HID_LOCK_F_CAPS)
                                                | (1u << HID_LOCK_F_SCROLL);
                            if ((seen & want) == want)
                                clog("[HID1] the [C70] field numbers %d/%d/%d are CONFIRMED",
                                     HID_LOCK_F_NUM, HID_LOCK_F_CAPS, HID_LOCK_F_SCROLL);
                            else
                                clog("[HID1] the [C70] guess is WRONG or partial: expected "
                                     "0x%x, saw 0x%x - hid_lock.h must be corrected",
                                     want, seen);
                        }
                    }
                    {
                        const uint32_t us = ar.seq
                                          ? rtt_reply(&ctrl_rtt, ar.seq, t_rx)   /* ING-2: us, before the read */
                                          : 0;
                        /* Bounded diagnostic: if no round trip ever matches, one needs to
                         * know whether the replies carry no number or whether it
                         * is OURS that are missing - the two causes call for
                         * opposite fixes. */
                        {
                            static unsigned dus = 0;
                            if (++dus <= 6)
                                clog("[S118d] reponse seq=%u f3.%u len=%zu -> %s",
                                     ar.seq, ar.response_field, rxlen,
                                     us ? "APPARIEE" : "non appariee");
                        }
                        if (us) {
                            static unsigned vus = 0;
                            /* The first three, then one every 20 s: enough for a log to
                             * carry a distribution, not enough to drown the
                             * rest. */
                            if (++vus <= 3 || (vus % 40) == 0)
                                /* The SEND cadence beside the RTT, and it
                                 * is the pair that informs: it is supposed to
                                 * be 500 ms every time. If its jitter follows
                                 * the RTT's, the loop manufactures part of it
                                 * (ING-2); if it stays flat while the RTT
                                 * moves, the network path is to blame. */
                                clog("[S118] control channel round trip: %u ms "
                                     "(moy %u, p90 %u, gigue %u, n=%u) "
                                     "| notre cadence : %u ms, gigue %u ms",
                                     us / 1000, rtt_avg_us(&ctrl_rtt) / 1000,
                                     rtt_pct_us(&ctrl_rtt, 90) / 1000,
                                     rtt_jitter_us(&ctrl_rtt) / 1000, ctrl_rtt.count,
                                     rtt_send_gap_avg_us(&ctrl_rtt) / 1000,
                                     rtt_send_jitter_us(&ctrl_rtt) / 1000);
                        }
                    }
                }
                /* Pattern match `1a XX 32 YY` (= f3 wire-2 sub + f6 wire-2 sub
                 *  = Reply.kRequestFlush). */
                /* K15n - the key frame request armed by the congestion feedback
                 * goes out HERE, where the control channel is in scope. In TCP
                 * mode this is the only possible route: `iP` is sent on the
                 * video UDP socket, which no longer exists (K15i). */
                if (g_flush_tcp_needed) {
                    g_flush_tcp_needed = 0;
                    uint8_t kf[128];
                    const int kn = ctrl_build_request_flush(kf, sizeof(kf), hb_seq);
                    if (kn > 0 && ctrl_tcp_send_cleartext(tcp, kf, (size_t)kn)) {
                        hb_seq++;
                        static int g_kf_log = 0;
                        if (g_kf_log < 5) {
                            clog("[K15n] a key frame requested through Request_Flush "
                                 "(control channel, TCP mode)");
                            g_kf_log++;
                        }
                    }
                }

                bool got_kreqflush = false;
                for (size_t i = 0; i + 4 < rxlen; i++) {
                    if (rxbuf[i] == 0x1a && rxbuf[i + 2] == 0x32) {
                        got_kreqflush = true;
                        break;
                    }
                }
                if (got_kreqflush) {
                    uint8_t fbuf[128];
                    int fn = g_ctrl_strict ? -1
                   : ctrl_build_request_flush(fbuf, sizeof(fbuf), hb_seq);
                    if (fn > 0 && ctrl_tcp_send_cleartext(tcp, fbuf, (size_t)fn)) {
                        static int g_ack_log = 0;
                        if (g_ack_log < 5) {
                            clog("[V16] kRequestFlush ack sent seq=%u rxlen=%zu",
                                 hb_seq, rxlen);
                            g_ack_log++;
                        }
                        hb_seq++;
                    }
                } else {
                    static int g_otherpush_log = 0;
                    if (g_otherpush_log < 5) {
                        clog("[V16] server push (not kRequestFlush) rxlen=%zu "
                             "hd=%02x%02x%02x%02x", rxlen,
                             rxbuf[0], rxbuf[1], rxbuf[2], rxbuf[3]);
                        g_otherpush_log++;
                    }
                }
            }
        }

        /* N38 2026-05-14: NotifyVideoCommand kFlush periodically, to flush the
         * server's encoder pipe and force a full-image IDR. Tested with enum=0
         * and enum=1. Configurable through SHADOW_FLUSH_CMD (the command enum
         * value). */
        static long long t_last_flush_ms = 0;
        static int g_flush_period_ms = -1;
        static int g_flush_cmd = -1;
        if (g_flush_period_ms < 0) {
            /* F10 2026-05-22 22h50: default 500 ms (= 2 Hz). Plan A V6 had
             * chosen 17000 ms to match the desktop capture, BUT on our stack,
             * with persistent UDP loss on the tail chunks, the 17 s gap between
             * IDR refreshes lets corruption persist for 17 s -> the user sees
             * shimmering and green blocks. Forcing an IDR at 2 Hz gives
             * sub-second recovery, with no measured bitrate regression
             * (3789 kbps). A/B test F10 at 100/500/1000/3000 -> all clean, 500 is
             * the compromise.
             * Set SHADOW_FLUSH_PERIOD_MS=17000 to go back to the "byte-exact
             * desktop" behaviour (with guaranteed persistent loss). */
            /* G5 FIX 2026-06-02: default OFF (was 500 ms). The periodic
             * RequestFlush also forced IDRs -> it contributed to the autofocus
             * effect. Obsolete pre-G4 workaround. SHADOW_FLUSH_PERIOD_MS=N to
             * re-enable. */
            const char *e = getenv("SHADOW_FLUSH_PERIOD_MS");
            g_flush_period_ms = e ? atoi(e) : 0;
        }
        if (g_flush_cmd < 0) {
            const char *e = getenv("SHADOW_FLUSH_CMD");
            g_flush_cmd = e ? atoi(e) : 0;  /* default kFlush=0 */
        }
        if (g_flush_period_ms > 0 && (now_ms - t_last_flush_ms) >= g_flush_period_ms) {
            uint8_t fbuf[128];
            /* N40: Request_Flush (= the real kFlush, case 7), not NotifyVideoCommand */
            int fn = g_ctrl_strict ? -1
                   : ctrl_build_request_flush(fbuf, sizeof(fbuf), hb_seq);
            if (fn > 0 && ctrl_tcp_send_cleartext(tcp, fbuf, (size_t)fn)) {
                static int g_flush_log = 0;
                if (g_flush_log < 5) {
                    clog("[N40] Request_Flush seq=%u (%d B)", hb_seq, fn);
                    g_flush_log++;
                }
                hb_seq++;
                t_last_flush_ms = now_ms;
            }
        }

        /* N47 2026-05-14: the MASTER capture shows the desktop sends
         * DisplayReady (= Hid case 6) PERIODICALLY, every 9 s. Our code sent it
         * once, during bootstrap. Test: send it periodically too. */
        static long long t_last_disp_ready_ms = 0;
        static int g_disp_ready_period_ms = -1;
        if (g_disp_ready_period_ms < 0) {
            /* Plan A V6: desktop = 15 per 4 min = 16000 ms (see the MASTER
             * capture). Our 9000 ms was twice too frequent -> align on the
             * desktop. */
            const char *e = getenv("SHADOW_DISP_READY_PERIOD_MS");
            g_disp_ready_period_ms = e ? atoi(e) : 16000;
        }
        if (g_disp_ready_period_ms > 0 && ready_sent
            && (now_ms - t_last_disp_ready_ms) >= g_disp_ready_period_ms) {
            uint8_t drbuf[128];
            int drn = g_ctrl_strict ? -1
                    : ctrl_build_display_ready_msg(drbuf, sizeof(drbuf), hb_seq);
            if (drn > 0 && ctrl_tcp_send_cleartext(tcp, drbuf, (size_t)drn)) {
                static int g_dr_log = 0;
                if (g_dr_log < 5) {
                    clog("[N47] DisplayReady periodic seq=%u", hb_seq);
                    g_dr_log++;
                }
                hb_seq++;
                t_last_disp_ready_ms = now_ms;
            }
        }

        /* === S1 2026-08-21 - STRICT CONTROL CHANNEL MIMICRY ===
         * Diff of the guided capture: in steady state (143 s), the official
         * client emits on `:base+11` ONLY frames of 92/93/94 bytes - the
         * heartbeat, and nothing else. We emit FOURTEEN different sizes:
         * event-driven IFRs (G6), periodic DisplayReady (N47),
         * NotifyResolution, Flush... about thirty messages per session that the
         * desktop NEVER sends.
         *
         * Hypothesis: these surplus messages make the server treat us as
         * something other than a standard client - which would explain why it
         * never enables the input channel's return path (the 104 B `@35=0x04`
         * messages the desktop receives and we do not), even though the content
         * of our events is now byte-exact.
         *
         * SHADOW_CTRL_STRICT=1 removes everything but the heartbeat. */
        /* D5 2026-08-21 - NATIVE input event injector (diagnostic, default OFF).
         * Observed correlation: 3 headless sessions WITHOUT a single native
         * event lasted 195/195/395 s; the GUI session that sent 26 events died
         * at 118 s (clean server close). test-cli's `--activity` does NOT go
         * through the native channel (the log only shows the 2 bridge set/clear
         * lines), so the previous A/B was not testing this variable at all.
         * Our events are known to be malformed (the server ACKs the 96 B Connect
         * but never applies anything): the hypothesis is that it absorbs them
         * then cuts. This toggle settles it WITHOUT a GUI.
         * SHADOW_INPUT_SYNTH_MS=500 injects a movement every 500 ms. */
        static int g_synth_ms = -1;
        if (g_synth_ms < 0) {
            const char *e = getenv("SHADOW_INPUT_SYNTH_MS");
            g_synth_ms = e ? atoi(e) : 0;
        }
        if (g_synth_ms > 0 && native_input_active()
            && now_ms - t_last_synth_ms >= g_synth_ms) {
            int dx = ((synth_count % 8) < 4) ? 7 : -7;
            int rc_m = native_input_send_mouse_move(dx, (synth_count % 3) - 1);
            /* D6 2026-08-21 - D5 only tested mouse_move: the refutation
             * "malformed events do not kill the session" therefore did not cover
             * clicks. And the GUI session that died at 118 s had sent 16
             * mouse_btn events (the user session 14), while the surviving
             * headless run had ZERO. SHADOW_INPUT_SYNTH_BTN=1 adds a
             * press/release to the synthetic stream to close that gap. */
            static int g_synth_btn = -1;
            if (g_synth_btn < 0) {
                const char *e2 = getenv("SHADOW_INPUT_SYNTH_BTN");
                g_synth_btn = e2 ? atoi(e2) : 0;
            }
            if (g_synth_btn && (synth_count % 4) == 0) {
                native_input_send_mouse_button(0, true);
                native_input_send_mouse_button(0, false);
            }
            synth_count++;
            if (synth_count % 20 == 1) {
                clog("[D5] input synthetique natif #%u (dx=%d rc=%d) a t=%ds",
                     synth_count, dx, rc_m, sec);
            }
            t_last_synth_ms = now_ms;
        }

        /* === K6 2026-08-21 - absolute-position CLICK harness ===
         * The channel only offers relative movements, but our internal tracking
         * is reliable (the server echoes back exactly the positions we compute).
         * Starting from (960,540), we walk to the target in steps and then
         * click. It exists to find out whether CLICKS are applied - an open
         * question, distinct from the keyboard one: only MOVEMENTS are proven to
         * be applied.
         * SHADOW_INPUT_CLICK_AT=<sec> SHADOW_INPUT_CLICK_XY="x,y"
         * (to be used with SHADOW_INPUT_SYNTH_MS=0 so the starting position is
         * deterministic). */
        static int g_click_at = -1, g_click_x = 0, g_click_y = 0;
        static int g_click_done = 0;
        if (g_click_at < 0) {
            const char *e = getenv("SHADOW_INPUT_CLICK_AT");
            g_click_at = e ? atoi(e) : 0;
            const char *xy = getenv("SHADOW_INPUT_CLICK_XY");
            if (xy) sscanf(xy, "%d,%d", &g_click_x, &g_click_y);
        }
        if (g_click_at > 0 && !g_click_done && sec >= g_click_at
            && native_input_active()) {
            /* K8 2026-08-21 - CLOSED-LOOP positioning.
             * The open-loop attempts failed: the VM keeps the position from one
             * session to the next, the Y axis does not respond to the expected
             * sign, and the movement is not linear (acceleration and clamping).
             * But the server sends back the position it computes in each 104 B
             * reply - we use that as feedback. */
            /* wait for a valid server position before driving */
            int steps = 0, sx = 0, sy = 0, waits = 0;
            while (!ctrl_input_tcp_server_pos(ctx.itc, &sx, &sy) && waits < 60) {
                native_input_send_mouse_move(1, 0);
                chan_backoff_sleep(50, p->abort_flag);
                waits++;
            }
            while (steps < 300 && ctrl_input_tcp_server_pos(ctx.itc, &sx, &sy)) {
                int dx = g_click_x - sx, dy = g_click_y - sy;
                if (dx > -3 && dx < 3 && dy > -3 && dy < 3) break;
                if (dx >  25) dx =  25;
                if (dx < -25) dx = -25;
                if (dy >  25) dy =  25;
                if (dy < -25) dy = -25;
                native_input_send_mouse_move(dx, dy);
                chan_backoff_sleep(25, p->abort_flag);   /* laisser l'echo revenir */
                steps++;
            }
            ctrl_input_tcp_server_pos(ctx.itc, &sx, &sy);
            clog("[K8] target (%d,%d) - server cursor (%d,%d) after %d iterations",
                 g_click_x, g_click_y, sx, sy, steps);
            /* K16 2026-08-21 - DRAG mode. Aiming at a small button is fragile;
             * a drag-select inside a web page produces a massive, unambiguous
             * effect (highlighted text). SHADOW_INPUT_DRAG="x,y" = the end
             * point; without it, a plain click. */
            const char *drag = getenv("SHADOW_INPUT_DRAG");
            int dgx = 0, dgy = 0;
            /* K17 2026-08-21 - configurable button index, to settle the
             * semantics of `@150`. The left click does not work while right and
             * middle do: the index may be 1-based on the protocol side (left=1,
             * right=2, middle=3), in which case our "right" actually produces a
             * left click and our left sends an invalid index of 0.
             * SHADOW_INPUT_CLICK_BTN=<0|1|2>. */
            static int g_cbtn = -1;
            if (g_cbtn < 0) {
                const char *eb = getenv("SHADOW_INPUT_CLICK_BTN");
                g_cbtn = eb ? atoi(eb) : 0;
            }
            /* W1 - SHADOW_INPUT_WHEEL=<n>: instead of clicking, send n wheel
             * notches (negative = downwards). Used to validate the decoding. */
            {
                const char *ew = getenv("SHADOW_INPUT_WHEEL");
                if (ew && atoi(ew) != 0) {
                    int n = atoi(ew), dir = n > 0 ? 1 : -1;
                    for (int i = 0; i < (n > 0 ? n : -n); i++) {
                        native_input_send_mouse_wheel(dir);
                        chan_backoff_sleep(120, p->abort_flag);
                    }
                    clog("[W1] %d crans de molette sent (%s)", n > 0 ? n : -n,
                         dir > 0 ? "haut" : "bas");
                    g_click_done = 1;
                    goto k6_done;
                }
            }
            native_input_send_mouse_button(g_cbtn, true);
            chan_backoff_sleep(80, p->abort_flag);
            if (drag && sscanf(drag, "%d,%d", &dgx, &dgy) == 2) {
                int st2 = 0;
                while (st2 < 300 && ctrl_input_tcp_server_pos(ctx.itc, &sx, &sy)) {
                    int ddx = dgx - sx, ddy = dgy - sy;
                    if (ddx > -3 && ddx < 3 && ddy > -3 && ddy < 3) break;
                    if (ddx >  25) ddx =  25;
                    if (ddx < -25) ddx = -25;
                    if (ddy >  25) ddy =  25;
                    if (ddy < -25) ddy = -25;
                    native_input_send_mouse_move(ddx, ddy);
                    chan_backoff_sleep(25, p->abort_flag);
                    st2++;
                }
                ctrl_input_tcp_server_pos(ctx.itc, &sx, &sy);
                clog("[K16] glisser jusqu'a (%d,%d) — curseur (%d,%d)", dgx, dgy, sx, sy);
            }
            chan_backoff_sleep(80, p->abort_flag);
            native_input_send_mouse_button(g_cbtn, false);
            g_click_done = 1;
        k6_done: ;
        }

        /* === I9 2026-08-21 - validating input through the VIDEO ===
         * The signals tested so far are unusable: the cursor channel returns 0,
         * 1928 or 3948 packets depending on the run WITHOUT any input at all,
         * and the input channel's echo is intermittent. We need an OBSERVABLE
         * effect. The Windows key opens the Start menu whatever the state of the
         * desktop: a large picture change => a video bitrate spike, measurable in
         * the stats timeline. If the bitrate does not move, the input is not
         * being applied. Single shot at t=SHADOW_INPUT_SYNTH_KEY_AT seconds.
         * Default scancode 125 = KEY_LEFTMETA (evdev). */
        static int g_key_at = -1, g_key_code = -1;
        static bool g_key_fired = false;
        if (g_key_at < 0) {
            const char *e = getenv("SHADOW_INPUT_SYNTH_KEY_AT");
            g_key_at = e ? atoi(e) : 0;
            e = getenv("SHADOW_INPUT_SYNTH_KEY");
            g_key_code = e ? atoi(e) : 125;
        }
        /* K4 2026-08-21 - HOLD the key before releasing it.
         * I9 sent press and release back to back, within the same millisecond.
         * But the official client's capture gives a median hold duration of
         * 112 ms (min 0, max 301). A zero-duration press may be filtered out by
         * the HID driver on the VM side.
         * SHADOW_INPUT_KEY_HOLD_MS=0 restores the previous behaviour. */
        static int g_hold_ms = -1;
        static long long t_release_at = 0;
        if (g_hold_ms < 0) {
            const char *eh = getenv("SHADOW_INPUT_KEY_HOLD_MS");
            g_hold_ms = eh ? atoi(eh) : 120;
        }
        if (g_key_at > 0 && !g_key_fired && sec >= g_key_at
            && native_input_active()) {
            g_key_fired = true;
            int r1 = native_input_send_scancode((uint16_t)g_key_code, true);
            if (g_hold_ms > 0) {
                t_release_at = now_ms + g_hold_ms;
                clog("[I9] *** appui scancode=%d a t=%ds (rc=%d), relachement "
                     "in %d ms ***", g_key_code, sec, r1, g_hold_ms);
            } else {
                int r2 = native_input_send_scancode((uint16_t)g_key_code, false);
                clog("[I9] *** touche scancode=%d envoyee a t=%ds (press rc=%d, "
                     "release rc=%d) ***", g_key_code, sec, r1, r2);
            }
        }
        if (t_release_at && now_ms >= t_release_at) {
            int r2 = native_input_send_scancode((uint16_t)g_key_code, false);
            clog("[I9] relachement scancode=%d (rc=%d)", g_key_code, r2);
            t_release_at = 0;
        }

        /* D3 2026-08-20 - periodic RegisterSession (EXPERIMENT, default OFF).
         * The MASTER capture diff (2026-05-14, project_image_100pct_PROOF) notes
         * "RegisterSession 8x desktop vs 1x us": the official client
         * re-registers the session ~8 times over 235 s, we do it once at
         * bootstrap. Hypothesis: the server holds a session lease that needs
         * renewing, and its expiry explains the clean close observed at
         * t~118 s (see D1). Default 0 = OFF until the A/B settles it: we only
         * flip a default with the measurement in hand.
         * Test with: SHADOW_REG_PERIOD_MS=30000 */
        static int g_reg_period_ms = -1;
        if (g_reg_period_ms < 0) {
            const char *e = getenv("SHADOW_REG_PERIOD_MS");
            g_reg_period_ms = e ? atoi(e) : 0;
        }
        if (g_reg_period_ms > 0 && now_ms - t_last_reg_ms >= g_reg_period_ms) {
            uint8_t rbuf[512];
            int rn = ctrl_build_register_session(rbuf, sizeof(rbuf),
                                                 (uint32_t)width, (uint32_t)height);
            if (rn > 0 && ctrl_tcp_send_cleartext(tcp, rbuf, (size_t)rn)) {
                reg_count++;
                clog("[D3] RegisterSession periodique #%u a t=%ds (period=%dms, %dB)",
                     reg_count, sec, g_reg_period_ms, rn);
            } else {
                clog("[D3] periodic RegisterSession FAILED at t=%ds (rn=%d)", sec, rn);
            }
            t_last_reg_ms = now_ms;
        }

        /* N48 2026-05-14: heartbeat at 2 Hz (500 ms) instead of 1 Hz.
         * The MASTER capture shows the desktop at 454 State messages over 4 min
         * = 2 Hz. Our code was at 1 Hz, half the frequency. Attempt to match the
         * desktop. */
        static int g_hb_period_ms = -1;
        if (g_hb_period_ms < 0) {
            const char *e = getenv("SHADOW_HB_PERIOD_MS");
            g_hb_period_ms = e ? atoi(e) : 500;
        }
        /* === AUD18 2026-09-11 - THE S57 SIGNATURE, WRITTEN DOWN ===
         *
         * KB §3.38 established that OUR emissions on :base+30 kill the
         * channel: AUD5's early resend drew a CHANNEL_DOWN(AUDIO) 40 to 130 ms
         * later (0 sessions with sound out of 21 when it fired). Since then
         * nothing measured that delay: the only oracle was a flag set once per
         * session, read by AUD16's MUET line alone, and 6 AUD5 resends in 10
         * left no line to time afterwards.
         * Each CHANNEL_DOWN(AUDIO) now gets one line: the time since our last
         * emission on :base+30, and which one it was (ping, AUD5, AUD16, S43)
         * with the number its own line carries. It sits after this pass's only
         * read of the control channel (V16, above) and BEFORE its emissions,
         * so an announcement is never blamed on an emission that followed it.
         * It needs that reader (SHADOW_AUTO_FLUSH_ACK, default 1). One line per
         * announcement, uncapped like the [S56] line it answers (KB §3.28).
         * Log only: nothing is sent. */
        while (aud18_down_seen < g_audio_channel_down_n) {
            aud18_down_seen++;
            if (aud30_tx.ms > 0)
                clog("[AUD18] CHANNEL_DOWN(AUDIO) at t=%ds, %lld ms after our "
                     "derniere emission :base+30 (%s #%u)",
                     sec, now_ms - aud30_tx.ms, aud30_tx.kind, aud30_tx.n);
            else
                clog("[AUD18] CHANNEL_DOWN(AUDIO) at t=%ds, nothing sent on :base+30 "
                     "since the initial registration", sec);
        }

        /* === AUD4 2026-08-21 - PING ON THE CURSOR/AUDIO CHANNEL ===
         *
         * The official client sends **one `0x70` byte every 7.50 s** on
         * `:base+30`, after its initial 25-byte registration. We registered once
         * and then went quiet.
         *
         * That is what explains why one session in two received NOTHING on this
         * channel (`cursor=0` from t=0) while the other received 100 packets per
         * second: without the ping the server stops feeding it - or never
         * starts. Since sound comes through this same channel, audio was a
         * lottery.
         *
         * Same cadence as the input channel's keepalive (K2): the server
         * visibly treats its secondary channels the same way. */
        if (udp_cursor >= 0 && now_ms - t_last_cursor_ping_ms >= 7500) {
            const uint8_t ping = 0x70;   /* 'p' */
            const int ping_sent = cursor_send(udp_cursor, &ping, 1) >= 0;   /* AUD18 */
            if (!ping_sent) {
                clog("[AUD4] :base+30 ping failed (errno=%d)", shadow_sock_errno());
            } else if (t_last_cursor_ping_ms == 0) {
                clog("[AUD4] cursor/audio channel ping active (0x70 every 7.5 s)");
            }
            /* === AUD15 2026-08-28 - LOG THE PING, AND ITS DRIFT ===
             * AUD14 names this ping as the leading candidate for the
             * mid-session `CHANNEL_DOWN(AUDIO)`: it is the ONLY thing we still
             * emit on this channel after startup, and KB §3.38 establishes that
             * our emissions on it are what kills it. The arithmetic fits
             * (294.3 s of session = 39.24 periods) but it cannot be verified,
             * because the cadence DRIFTS - `t_last = now_ms` rather than
             * `+= 7500`, so every loop overrun accumulates - and because this
             * ping was not logged. It is now: the number and the real interval
             * will be enough to say whether a `CHANNEL_DOWN` follows a ping, and
             * by how much. A ping every 7.5 s costs one line every 75 s at the
             * one-in-ten rate. */
            {
                /* AUD-LC-3: n_ping is a session local, declared with aud16_*. */
                const long long ecart = (t_last_cursor_ping_ms > 0)
                                      ? now_ms - t_last_cursor_ping_ms : 0;
                if (++n_ping <= 3 || (n_ping % 10) == 0)
                    clog("[AUD15] ping :base+30 #%u (ecart reel %lld ms, cible 7500)",
                         n_ping, ecart);
            }
            /* AUD18: the ping is an emission on :base+30 like the others;
             * its number is AUD15's, so the two lines match. */
            if (ping_sent) { aud30_tx.ms = now_ms; aud30_tx.kind = "ping"; aud30_tx.n = n_ping; }
            t_last_cursor_ping_ms = now_ms;
        }

        /* === AUD16 2026-08-28 - AUDIO HAD NO SAFETY NET, VIDEO HAS TWO ===
         *
         * AUD14 established it, and it is entirely our doing: when the server
         * tears the audio channel down mid-session, NOTHING calls it back.
         * `g_audio_channel_down` is written by ctrl_tcp.c and has NO reader;
         * AUD5 below is disarmed for good from the first packet on (its guard is
         * `udp_cursor_pkts == 0`); and there was no stall detector at all on
         * `:base+30`, where video has `d2_last_rx_ms`, `[D2]` and S41.
         * Measured: 460 s without a datagram, sound never came back, not one
         * attempt made. Without the server's `[S56]`, the death would not even
         * have left a trace - and 3.34 shows it does not always announce it.
         *
         * THE THRESHOLD IS CHOSEN SO AS NOT TO CONFUSE DEATH WITH SILENCE.
         * AUD14 showed that a silent VM still sends ~10 packets/s of 47 bytes
         * (the minimal frame), and that the official client does the same. Zero
         * packets for 5 s is therefore not silence: it is a death.
         *
         * AND WE DO NOT FORGET S57: our emissions on this channel are what kills
         * it. A recovery that re-sends too fast would reproduce exactly the
         * defect it claims to repair - that is what AUD5 did at startup (0
         * sessions with sound out of 21 when it fired, 21 out of 26 when it
         * stayed quiet). Hence three safeguards: we only act on a channel PROVEN
         * dead, never before 5 s of total silence; at most one attempt every
         * 10 s; and a cap of 12 attempts per session, after which we go
         * permanently quiet rather than flood a channel that will not answer.
         * `SHADOW_AUDIO_REVIVE=0` disables it.
         *
         * The log is written for the next measurement, not to reassure: it says
         * how long the channel stayed silent and how many attempts were made, so
         * that a future session can say whether this recovery works. Until a
         * real revival has been OBSERVED, this code is an executable hypothesis,
         * not an established fix. */
        {
            static int g_revive = -1;
            if (g_revive < 0) {
                const char *e = getenv("SHADOW_AUDIO_REVIVE");
                g_revive = e ? atoi(e) : 1;
            }
            static int g_aud_stall_ms = -1;
            if (g_aud_stall_ms < 0) {
                const char *e = getenv("SHADOW_AUDIO_STALL_MS");
                g_aud_stall_ms = e ? atoi(e) : 5000;
                if (g_aud_stall_ms < 2000) g_aud_stall_ms = 2000;
            }
            /* === AUD-LC-4 2026-09-11 - THE CAP THE COMMENT ABOVE PROMISES, PER SESSION ===
             * `aud16_essais` restarts at every REVENU, so "12 attempts per
             * session" was 12 per EPISODE: across episodes the only bound was
             * the 10 s spacing, 360 re-registrations an hour. And any datagram
             * counts as REVENU - the 11-byte type-3 ones that arrive every 5-10 s
             * included - so a channel that stops carrying sound but keeps that
             * beacon re-armed the budget every few seconds, re-registering
             * towards a socket the server was still feeding: the S57 shape.
             * Model on this block's guards (lc4_model.c, 30 min sessions), then
             * the block itself extracted into a harness, same counts: one death
             * 12 -> 12; two deaths 24 -> 12; a stray datagram every 60 s
             * 174 -> 12; the 7 s beacon 124 -> 12, of which re-registrations
             * towards a socket fed less than 10 s earlier 123 -> 11. The cost: a
             * second real death gets no attempt once 12 are spent - no attempt
             * has ever been seen to work (AUD16b), and S41 showed that a lone
             * re-registration does not bring video back either.
             * SHADOW_AUDIO_REVIVE_MAX (default 12) is the session budget; 0
             * restores the per-episode rule exactly. The AUD16 lines end with
             * `| N au total`, the session's attempts so far. */
            static int g_revive_max = -1;
            if (g_revive_max < 0) {
                const char *e = getenv("SHADOW_AUDIO_REVIVE_MAX");
                g_revive_max = e ? atoi(e) : 12;
                if (g_revive_max < 0) g_revive_max = 0;
            }
            if (g_revive && udp_cursor >= 0 && stats->udp_cursor_pkts > 0) {
                if (stats->udp_cursor_pkts != aud16_last_pkts) {
                    aud16_last_pkts = stats->udp_cursor_pkts;
                    aud16_last_rx_ms = now_ms;
                    if (aud16_mort) {
                        clog("[AUD16] channel :base+30 BACK after %lld ms and %u attempt(s)"
                             " | %u au total",
                             now_ms - aud16_mort_ms, aud16_essais, aud16_total);
                        aud16_mort = 0;
                        aud16_essais = 0;
                    }
                } else if (aud16_last_rx_ms > 0
                           && now_ms - aud16_last_rx_ms > g_aud_stall_ms) {
                    if (!aud16_mort) {
                        aud16_mort = 1;
                        aud16_mort_ms = now_ms;
                        clog("[AUD16] *** channel :base+30 SILENT *** for %lld ms "
                             "(t=%ds, server announced=%d) - the silence of a "
                             "gives ~10 packets/s, so this is a death | %u in total",
                             now_ms - aud16_last_rx_ms, sec,
                             (int)g_audio_channel_down, aud16_total);
                    }
                    /* AUD-LC-4: the session budget - see g_revive_max. */
                    const int aud16_room = g_revive_max == 0
                                           || aud16_total < (unsigned)g_revive_max;
                    if (aud16_essais < 12 && aud16_room
                        && now_ms - aud16_last_try_ms >= 10000) {
                        aud16_last_try_ms = now_ms;
                        aud16_essais++;
                        aud16_total++;
                        /* === SRV4 2026-10-02 - THE LADDER, CHEAPEST RUNG FIRST ===
                         *
                         * Two distinct failures look identical from here - no
                         * datagram - and the server has a different answer for
                         * each. Trying the cheap one first is also what tells
                         * them apart:
                         *
                         * 1. The channel is ALIVE but we never got (or lost) the
                         *    codec header. `Audio::Clients::SufpClient::DealWithInput`
                         *    @0x140be1480 answers a single `'G'` (0x47) byte by
                         *    incrementing the client's "wants a reference frame"
                         *    counter, which is exactly what
                         *    `AEncodingSession::SendHeader_` @0x140bec510 tests
                         *    (slot 8 = `+520 != +522`) before re-sending the
                         *    header. One byte, no state reset.
                         * 2. The stream client has been INVALIDATED. Then
                         *    nothing we put on this socket is even looked at
                         *    (see session_reannounce_channel), and only a
                         *    control-channel re-announcement can help.
                         *
                         * So: `'G'` on the first attempt of an episode, and a
                         * re-announcement on the ones after. If `'G'` is
                         * answered, case 1 and we are done; if it is not, the
                         * re-announcement handles case 2. The old `A` datagram
                         * resend stays as the fallback when either toggle is
                         * off.
                         * SHADOW_AUD_G=0 removes the `'G'` rung,
                         * SHADOW_REANN=0 the re-announcement, and with both off
                         * the behaviour is exactly AUD-LC-4's. */
                        static int g_aud_g = -1;
                        if (g_aud_g < 0) {
                            const char *e = getenv("SHADOW_AUD_G");
                            g_aud_g = e ? atoi(e) : 1;
                        }
                        int done = 0;
                        if (g_aud_g && aud16_essais == 1) {
                            const uint8_t g_req = 0x47;   /* 'G' */
                            if (cursor_send(udp_cursor, &g_req, 1) >= 0) {
                                aud30_tx.ms = now_ms; aud30_tx.kind = "SRV4-G";
                                aud30_tx.n = aud16_total;
                                clog("[SRV4] :base+30 'G' (0x47) sent - asks the "
                                     "server for the codec header again "
                                     "(#%u/12, silent for %lld ms) | %u in total",
                                     aud16_essais, now_ms - aud16_last_rx_ms,
                                     aud16_total);
                                done = 1;
                            }
                        }
                        if (!done
                            && session_reannounce_channel(tcp, &ctx, &hb_seq,
                                                          SHADOW_CHAN_IDX_AUDIO,
                                                          "audio channel silent")) {
                            aud30_tx.ms = now_ms; aud30_tx.kind = "SRV5-reann";
                            aud30_tx.n = aud16_total;
                            done = 1;
                        }
                        if (!done) {
                            uint8_t reg[25];
                            int rl = ctrl_build_udp_register(reg, sizeof(reg),
                                                              auth_reply.hash);
                            if (rl > 0 && cursor_send(udp_cursor, reg, (size_t)rl) > 0) {
                                aud30_tx.ms = now_ms; aud30_tx.kind = "AUD16"; aud30_tx.n = aud16_total;
                                clog("[AUD16] reenvoi de l'enregistrement :base+30 "
                                     "(#%u/12, silent for %lld ms) | %u in total",
                                     aud16_essais, now_ms - aud16_last_rx_ms, aud16_total);
                            } else
                                clog("[AUD16] :base+30 resend failed (errno=%d) | %u in total",
                                     shadow_sock_errno(), aud16_total);
                        }
                    } else if (aud16_essais >= 12 && !aud16_abandon) {
                        aud16_abandon = 1;
                        clog("[AUD16] twelve attempts with no answer - going quiet "
                             "(flooding a dead channel does not revive it) | %u in total",
                             aud16_total);
                    } else if (aud16_essais < 12 && !aud16_room && !aud16_cap_said) {
                        /* A NEW death with the session budget spent: said once. */
                        aud16_cap_said = 1;
                        clog("[AUD16] %u attempts this session - going quiet "
                             "until the end of the session (SHADOW_AUDIO_REVIVE_MAX=%d)",
                             aud16_total, g_revive_max);
                    }
                }
            }
        }

        /* === SRV-FAULT 2026-10-02 — SRV4's `G`, ON DEMAND ====================
         * See the SRV-FAULT block near the top of this function. Three steps
         * 5 s apart, so the before and after windows have the same width and
         * neither overlaps the handshake: arm at FAULT_SETTLE_MS, send at
         * +5 s, report at +10 s. The count comes from `g_aud30.d_ptype[]`, the
         * per-session census of first plaintext bytes that on_cursor_packet
         * already maintains - a second counter for the same frames would be
         * one more thing to keep in step, and this one cannot disagree with
         * what the `[AUD2]` lines say. The 0x12 delta goes on the result line
         * too: without it a zero `after` cannot be told from a channel that
         * had gone quiet on its own. */
        if (g_fault_aud_g && udp_cursor >= 0 && fault_g_step < 3) {
            const long long el = now_ms - t_start_ms;
            if (fault_g_step == 0 && el >= FAULT_SETTLE_MS) {
                fault_g_step = 1;
                fault_g_hdr0 = g_aud30.d_ptype[0x02];
                clog("[SRV-FAULT] audio: 0x02 census armed at t=%ds "
                     "(hdr=%u audio=%u) - `G` in 5 s",
                     sec, fault_g_hdr0, g_aud30.d_ptype[0x12]);
            } else if (fault_g_step == 1 && el >= FAULT_SETTLE_MS + 5000) {
                fault_g_hdr1 = g_aud30.d_ptype[0x02];
                fault_g_aud1 = g_aud30.d_ptype[0x12];
                const uint8_t g_req = 0x47;   /* `G`, SufpClient::DealWithInput */
                if (cursor_send(udp_cursor, &g_req, 1) >= 0) {
                    fault_g_step = 2;
                    fault_g_ms   = now_ms;
                    clog("[SRV4] :base+30 `G` (0x47) sent by SRV-FAULT at t=%ds "
                         "- 0x02 headers in the 5 s before: %u",
                         sec, fault_g_hdr1 - fault_g_hdr0);
                } else {
                    fault_g_step = 3;
                    clog("[SRV-FAULT] audio: `G` send failed (errno=%d) - no "
                         "measurement this session", shadow_sock_errno());
                }
            } else if (fault_g_step == 2 && now_ms - fault_g_ms >= 5000) {
                fault_g_step = 3;
                clog("[SRV4] SRV-FAULT result at t=%ds: 0x02 headers before=%u "
                     "after=%u | 0x12 audio frames after=%u - a non-zero "
                     "`after` is the server re-sending the codec header",
                     sec, fault_g_hdr1 - fault_g_hdr0,
                     g_aud30.d_ptype[0x02] - fault_g_hdr1,
                     g_aud30.d_ptype[0x12] - fault_g_aud1);
            }
        }

        /* === AUD5 - RESEND THE REGISTRATION AS LONG AS NOTHING ARRIVES ===
         *
         * The ping is not enough: some sessions stay at `cursor=0` while it goes
         * out correctly. The tenable explanation is that our initial
         * registration arrives BEFORE the server has opened this channel, and is
         * lost - UDP does not tell you. The sessions where the channel worked
         * were the ones where the VM started fast (video at 9419 packets at
         * t=20 s against 2312 in a silent session).
         *
         * So we resend it every 2 s until a packet has arrived. From the first
         * one on, we stop: the channel is established, and only the ping keeps
         * it alive. One registration too many is harmless - it carries the same
         * identifier - whereas a lost registration costs the entire session. */
        /* === AUD-LC-4 2026-09-11 - CAPPED, AND EVERY RESEND LOGGED ===
         * AUD5 was the last emitter on :base+30 with no bound: a channel that
         * never delivers got a registration every 2 s for the whole session
         * (model on the exact guard: 900 in 30 min, 30 a minute; live, 10 or
         * 11 in the 22 s of the one dead session of 10 on 2026-09-11), and only
         * #1-#3 and every tenth left a line - 807 of the 900 untimeable, where
         * the S57 window is 40-130 ms. KB §3.29: 20 resends over 40 s never
         * brought a channel back, and the official client never re-registers.
         * SHADOW_AUD5_MAX (default 12, ~24 s, inside §3.29's window) caps the
         * resends per session, each one now logged, and one line says when the
         * cap is reached. 0 turns AUD5 off, like the official client. The S57
         * arming is untouched - the first resend still leaves at +2 s - and no
         * emission changes before the 12th: this only removes emissions.
         * Whether AUD5 ever recovers a channel is now measured instead of
         * assumed: see the `[AUD5] premier paquet` line after this block. */
        static int g_aud5_max = -1;
        if (g_aud5_max < 0) {
            const char *e = getenv("SHADOW_AUD5_MAX");
            g_aud5_max = e ? atoi(e) : 12;
            if (g_aud5_max < 0) g_aud5_max = 0;
        }
        if (udp_cursor >= 0 && stats->udp_cursor_pkts == 0
            && now_ms - t_last_cursor_reg_ms >= 2000
            && cursor_reg_count < (unsigned)g_aud5_max) {
            /* === S43 2026-08-25 - RETRY FROM A FRESH SOCKET ===
             *
             * AUD5 re-emitted the registration from the SAME socket, therefore
             * from the same source port. Twenty attempts over forty seconds
             * never brought anything back.
             *
             * The diff against the official client's capture (the only one with
             * sound) eliminated everything else: our eight channel
             * announcements are identical to the byte - the audio one included
             * -, our bootstrap can be made identical, and our traffic on that
             * port is the same (registration, then 0x70 pings every 7.5 s). ONE
             * behavioural difference remains: the desktop connects `:base+30`
             * 400 ms AFTER `:base+10`, where we open everything within 6 ms. If
             * it is enough for the registration to arrive too early for the
             * server to reject it for good on that (address, source port) pair,
             * then no resend from the same socket can recover - a fresh source
             * port is needed.
             *
             * So we recreate the socket every three unsuccessful attempts.
             * `SHADOW_CURSOR_RESOCKET=0` goes back to the plain resend. */
            /* S43: default put back to 0 on 2026-08-25. This recovery was born
             * of a badly posed problem - the channel seemed never to start,
             * whereas it was S44 that was breaking it. Recreating the socket
             * never brought anything back, and thrashing a socket that might
             * start late can do harm. We keep the toggle, not the default. */
            static int g_resocket = -1;
            if (g_resocket < 0) {
                const char *e = getenv("SHADOW_CURSOR_RESOCKET");
                g_resocket = e ? atoi(e) : 0;
            }
            if (g_resocket && cursor_reg_count > 0 && (cursor_reg_count % 3) == 0) {
                int fresh = udp_register_ex(p->vm_host, port_base_used + 30,
                                           auth_reply.hash,
                                           g_cursor_peer_len > 0 ? 0 : 1);
                if (fresh >= 0) {
                    shadow_closesocket(udp_cursor);
                    udp_cursor = fresh;
                    clog("[S43] :base+30 reopened on a fresh source port "
                         "(after %u attempts with no answer)", cursor_reg_count);
                    cursor_reg_count++;
                    /* AUD18: the fresh socket's registration is an emission too */
                    aud30_tx.ms = now_ms; aud30_tx.kind = "S43"; aud30_tx.n = cursor_reg_count;
                    aud5_last_tx_ms = now_ms;
                    t_last_cursor_reg_ms = now_ms;
                    goto cursor_reg_fait;
                }
            }
            {
                uint8_t reg[32];
                int rlen = ctrl_build_udp_register(reg, sizeof(reg), auth_reply.hash);
                if (rlen > 0 && cursor_send(udp_cursor, reg, (size_t)rlen) == rlen) {
                    cursor_reg_count++;
                    aud30_tx.ms = now_ms; aud30_tx.kind = "AUD5"; aud30_tx.n = cursor_reg_count;
                    aud5_last_tx_ms = now_ms;
                    /* AUD-LC-4: every resend, so each one can be timed - at
                     * most SHADOW_AUD5_MAX lines per session. */
                    clog("[AUD5] resending the :base+30 registration (#%u, nothing received)",
                         cursor_reg_count);
                }
                t_last_cursor_reg_ms = now_ms;
            }
cursor_reg_fait: ;
        }
        /* AUD-LC-4: the cap's one line, written when the next resend would
         * have left - so it really follows N resends with no answer. */
        if (!aud5_cap_said && udp_cursor >= 0 && stats->udp_cursor_pkts == 0
            && now_ms - t_last_cursor_reg_ms >= 2000
            && cursor_reg_count >= (unsigned)g_aud5_max) {
            aud5_cap_said = 1;
            clog("[AUD5] %u resend(s) with no answer - going quiet (SHADOW_AUD5_MAX=%d; "
                 "KB 3.29: none has ever brought the channel back)",
                 cursor_reg_count, g_aud5_max);
        }
        /* AUD-LC-4: the only direct measure of whether AUD5 ever brings a
         * channel back - the first datagram after at least one resend. */
        if (!aud5_first_said && cursor_reg_count > 0 && stats->udp_cursor_pkts > 0) {
            aud5_first_said = 1;
            clog("[AUD5] first :base+30 packet after %u resend(s), %lld ms after the last one",
                 cursor_reg_count, now_ms - aud5_last_tx_ms);
        }

        /* Bitrate requested by the UI: we emit it here, with the current
         * sequence number, between two heartbeats. CFG-4 2026-09-11: the
         * message has no frame-rate field (S18), so there is nothing else to
         * fill in - the B1 note about a fallback frame rate is moot. */
        if (g_pending_video_cfg) {
            g_pending_video_cfg = 0;
            const uint32_t want_mbps = g_pending_bitrate_mbps
                                     ? g_pending_bitrate_mbps
                                     : bitrate_wire_mbps(p->max_bitrate_mbps);
            if (send_bitrate(tcp, &hb_seq, want_mbps, "ui") && g_adapt_user_cap)
                bitrate_ctl_on_wire(&bctl, want_mbps);   /* CFG-1, new rule only */
        }

        /* === HID1 2026-10-02 - THE LOCK-KEY PROBE =========================
         *
         * Sends ONE Hid request (Request field 6) per session and logs what
         * comes back. It exists to turn the `[C70]` half of `hid_lock.h` into a
         * measurement: the three field numbers inside the Hid message are read
         * off the server's memory layout, not off a captured message, and this
         * is the one session that settles it.
         *
         * The body is EMPTY on purpose - `hid_lock_build` with nothing known.
         * That asks the VM for its three states without asserting any of ours,
         * so the probe cannot toggle a key on the remote desktop while it is
         * being used. Sending our own states is what a real feature would do,
         * and it is deliberately not what a probe does.
         *
         * OFF by default: it emits a message the official client does not send
         * at this point in the session, and S1 is the standing finding that
         * surplus messages on `:base+11` may be how the server tells us apart.
         * SHADOW_HID_LOCK_PROBE=1 arms it.
         *
         * Fired at the 5 s mark rather than at bootstrap: the control channel
         * is quiet by then, so the reply is easy to attribute in the log. */
        static int g_hid_probe = -1;
        if (g_hid_probe < 0) {
            const char *e = getenv("SHADOW_HID_LOCK_PROBE");
            g_hid_probe = e ? atoi(e) : 0;
        }
        if (g_hid_probe && !fb.hid_probe_sent && now_ms - t_start_ms >= 5000) {
            fb.hid_probe_sent = 1;
            hid_locks ask;
            memset(&ask, 0, sizeof ask);
            /* === HID1 phase 2 2026-10-02 - AN EMPTY REQUEST ANSWERS NOTHING
             *
             * Phase 1 sent an empty Hid body and got `f6:len0` back. Re-reading
             * the decompile explains it: the handler reads the FIRST field slot
             * of the incoming Hid and, when it is null, jumps straight to the
             * reply without creating any of the three lock sub-messages. There
             * is no "tell me yours" mode - the VM only ever echoes the locks
             * you asserted.
             *
             * So phase 2 asserts a state. All three OFF: that is the ordinary
             * state of all three keys, so on a VM that already has them off
             * nothing changes, and on one that does not, what changes is a lock
             * key - visible, harmless and reversible. It is also exactly what
             * the feature does in normal use.
             *
             * SHADOW_HID_LOCK_PROBE=2 asks for this; =1 keeps the (now known to
             * be mute) empty question, because the difference between the two
             * replies IS the finding. */
            if (g_hid_probe >= 2) {
                ask.num.known = ask.caps.known = ask.scroll.known = true;
                ask.num.on = ask.caps.on = ask.scroll.on = false;
            }
            uint8_t hm[192];
            const int hmn = ctrl_build_hid_locks(hm, sizeof hm, hb_seq, &ask);
            if (hmn > 0 && ctrl_tcp_send_cleartext(tcp, hm, (size_t)hmn)) {
                clog("[HID1] lock-key probe sent, seq=%u, %d bytes - Request "
                     "field %d, %s",
                     hb_seq, hmn, HID_REQ_FIELD,
                     g_hid_probe >= 2
                         ? "asserting all three locks OFF (the VM will toggle to match)"
                         : "with an EMPTY Hid body, which the server answers with "
                           "nothing - kept as the counter-case");
                fb.hid_probe_seq = hb_seq;
                hb_seq++;
            } else {
                clog("[HID1] lock-key probe could NOT be sent (build=%d)", hmn);
            }
        }

        if (now_ms - t_last_heartbeat_ms >= g_hb_period_ms) {
            uint8_t hb[128];
            int hbn = ctrl_build_heartbeat(hb, sizeof(hb), hb_seq);
            if (hbn > 0 && ctrl_tcp_send_cleartext(tcp, hb, (size_t)hbn)) {
                /* S118: the send is recorded BEFORE incrementing, otherwise the
                 * reply would be matched against the next number. */
                /* ING-2 2026-09-11 - the same microsecond clock as the reply,
                 * read right after the write: moving only one side of the
                 * pair leaves +0.6 ms of bias. */
                rtt_sent(&ctrl_rtt, hb_seq, latency_now_us());
                hb_seq++;
            }
            t_last_heartbeat_ms = now_ms;
        }

        stats->session_seconds = sec;

        /* === G19 2026-08-22 - ADAPTIVE BITRATE DRIVEN BY MEASURED LOSS ===
         *
         * Root cause of the "blocks": the same thread does reception AND
         * synchronous decoding; during decoding no chunk is read at all, and at
         * high bitrate the socket buffer overflows in bursts -> lost slices ->
         * stale macroblocks that drift until the next IDR. Measured: 50 Mbps ->
         * 1.30% loss, picture stuck; 12 Mbps -> 0.14%, clean picture.
         *
         * So we drive the requested bitrate from the real loss, like a proper
         * streaming client: down when it loses, back up when it is clean, within
         * the user's cap. The server honours these changes live (kUpdateSession,
         * see G14: 8.9 -> 27 Mbps measured).
         * SHADOW_ADAPT_BITRATE=0 disables it. */
        static int g_adapt = -1;
        if (g_adapt < 0) { const char *e = getenv("SHADOW_ADAPT_BITRATE"); g_adapt = e ? atoi(e) : 1; }
        if (g_adapt && sec != g_adapt_last_sec && sec >= 3) {
            g_adapt_last_sec = sec;
            uint32_t exp = stats->chunks_expected, mis = stats->chunks_missing;
            uint32_t d_exp = exp - g_adapt_prev_exp, d_mis = mis - g_adapt_prev_mis;
            g_adapt_prev_exp = exp; g_adapt_prev_mis = mis;
            const double loss = bitrate_ctl_loss(d_exp, d_mis);

            /* === CFG-1 2026-09-11 - THE USER'S CHOICE IS THE CAP ===
             * Same rule as before (cautious start at min(cap, 25), x0.75 on
             * loss above 0.8 %, +3 after 4 clean seconds, floor 8), with one
             * difference: the cap is the user's LATEST live choice, adopted as
             * the current value with no emission of its own (the UI's message
             * already carries it). G19 used to climb over it: on the offline
             * replay, a drop from 100 to 25 at 60 s was back at 70 at +2.1 s and
             * at 100 by the end - undoing the only in-game workaround for
             * DEBIT-1 (100 breaks, 25 holds) while the screen said 25. The
             * header test fails 19/34 on the old rule and passes 34/34. */
            const uint32_t uw = g_user_word;
            const uint32_t emit = bitrate_ctl_tick(&bctl, loss,
                                                   g_adapt_user_cap ? (uw & 0xFFFFu) : 0u,
                                                   g_adapt_user_cap ? (uw >> 16) : 0u);
            if (bctl.event == BITRATE_CTL_EV_ADOPT)
                clog("[G19] choix de l'usager adopte : plafond %u Mbps", bctl.cur);
            if (emit && send_bitrate(tcp, &hb_seq, emit, "g19")) {
                if (g_adapt_user_cap) bitrate_ctl_on_wire(&bctl, emit);
                if (bctl.event == BITRATE_CTL_EV_DOWN)
                    clog("[G19] perte %.2f%% -> debit abaisse a %u Mbps", loss * 100.0, emit);
                else if (bctl.event == BITRATE_CTL_EV_UP)
                    clog("[G19] stable -> debit remonte a %u Mbps", emit);
                else if (bctl.event == BITRATE_CTL_EV_RESYNC)
                    clog("[G19] fil en desaccord -> %u Mbps renvoye", emit);
            }
        }

        /* Cursor/audio channel sentinel. One session in two receives nothing
         * at all on `:base+30` - the server does not feed it - and that silence
         * was being mistaken for a problem on our side. We say it. */
        /* AUD-LC-3 2026-09-11: once per SESSION - warned_cursor_silent is a
         * session local now, where as a static only the first session of a
         * process said it - and into the journal as well as stderr, same text:
         * stderr.log is pulled by no tool, so on console this line had never
         * been read. */
        if (!warned_cursor_silent && sec >= 15) {
            warned_cursor_silent = 1;
            if (stats->udp_cursor_pkts == 0) {
                const char *silent =
                    "[AUD2] NO packet on :base+30 after 15 s - "
                    "the server sends neither cursor nor sound on this "
                    "session; the audio test can show nothing.";
                fprintf(stderr, "%s\n", silent);
                clog("%s", silent);
            } else {
                fprintf(stderr, "[AUD2] channel :base+30 active (%u packets in 15 s)\n",
                        stats->udp_cursor_pkts);
                clog("[AUD2] channel :base+30 active (%u packets in 15 s)",
                     stats->udp_cursor_pkts);
            }
        }

        /* Publish the counters for the metrics panel.
         *
         * They used to be fed by the WebRTC stack (webrtc.c) and by the
         * libdatachannel path, both abandoned: on the native path nobody filled
         * them, so the "Network" section showed zero packets and zero bitrate in
         * the middle of a stream.
         * AUD-INS-3 2026-09-11: this was a read-modify-write of the whole struct
         * while the decode thread did the same with its own counters, so each
         * could write back the other's STALE fields - a counter stepping back,
         * read by RateMeter as a restart (L21). This loop is now the NET group's
         * one writer and merges that group only (core/stats.h). */
        /* 4 Hz: the display derives the bitrate from the variation of these
         * counters, and one publication per second gave it too coarse a
         * resolution for a value that moves on every frame. */
        if (now_ms - t_last_pub_ms >= 250) {
            t_last_pub_ms = now_ms;
            session_stats_t pub;
            memset(&pub, 0, sizeof pub);   /* AUD-INS-3: only the NET fields are ours */
            pub.rtp_video_packets = stats->udp_video_pkts;
            pub.rtp_video_bytes   = stats->udp_video_bytes;
            /* Audio arrives on the cursor channel (3.26), no longer over DTLS.
             * So we publish the frames actually played - duplicates deducted. */
            pub.rtp_audio_packets = stats->udp_audio_pkts - stats->audio_dup_skipped;
            pub.rtp_audio_bytes   = stats->udp_audio_bytes;
            pub.opus_dup_skipped  = stats->audio_dup_skipped;
            /* AUD-DEDUP-3 2026-09-11 - frames lost in BOTH copies, FINAL. In the
             * NET group with the counts it is a ratio of, so it keeps moving
             * through a video outage. */
            pub.opus_lost         = stats->audio_frames_lost;
            /* S117: integrity, published beside the rates. The panel carried
             * nothing but bitrates; a user watching their picture degrade could
             * not see their own loss. */
            pub.chunks_expected    = stats->chunks_expected;
            pub.chunks_missing     = stats->chunks_missing;
            pub.chunks_orphan_lost = stats->chunks_orphan_lost;
            pub.frames_trunc       = stats->frames_dropped_trunc;
            pub.kernel_drops       = g_kernel_drops;
            pub.kernel_drops_valid = g_kdrops_ok != 0;   /* ING-1 */
            pub.delay_offset_us    = g_delay_valid ? g_delay_offset_us : 0;
            /* S118: the only NETWORK latency this client measures. */
            pub.ctrl_rtt_us        = ctrl_rtt.last_us;
            pub.ctrl_rtt_avg_us    = rtt_avg_us(&ctrl_rtt);
            pub.ctrl_rtt_p90_us    = rtt_pct_us(&ctrl_rtt, 90);
            pub.ctrl_rtt_jitter_us = rtt_jitter_us(&ctrl_rtt);
            pub.session_seconds = sec;
            /* VI1 - seconds since the last video datagram (UDP or TCP: both
             * move d2_last_rx_ms). The banner reads it; it had no writer. */
            pub.rtp_video_stuck_secs = any_video ? (int)((now_ms - d2_last_rx_ms) / 1000) : 0;
            session_stats_merge(&pub, SESSION_STATS_NET);
        }

        /* === L20 2026-08-29 - THE LOG SMOOTHED THE PEAKS, AND MADE ME
         *     CONCLUDE WRONGLY ===
         *
         * The counters are only written every 5 s. A two-second peak at 118 fps
         * drowned in three seconds at 48 comes out at ~75 in a window average: I
         * read "peak 73" off that smoothing and concluded there was a ceiling
         * around 73, while the user's panel was showing 115-118. Both
         * measurements were correct; mine was answering a different question,
         * and nothing in the line said so.
         *
         * So we sample per SECOND and keep the session's maximum. A window
         * average and a peak cannot be derived from one another: you need both,
         * side by side, otherwise you unknowingly pick whichever suits. */
        if (sec != last_log_sec) {
            /* L21 - these two marks hold SESSION STATE. They had been put in
             * function `static`s in the very commit that cited this defect
             * family: on the next stream `frames_decoded` restarts from zero
             * while the mark keeps the old count, and the unsigned subtraction
             * yields an absurd peak. So they live with the rest of the session
             * state and are reset with it. The `>=` guard additionally covers
             * any unexpected step backwards. */
            if (g_crete_sec >= 0 && sec > g_crete_sec
                && stats->frames_decoded >= g_crete_img) {
                const double f = (double)(stats->frames_decoded - g_crete_img)
                               / (double)(sec - g_crete_sec);
                if (f > g_crete_img_s) g_crete_img_s = f;
            }
            g_crete_img = stats->frames_decoded;
            g_crete_sec = sec;
        }

        if (sec != last_log_sec && sec % 5 == 0) {
            /* AUD-DEDUP-3 2026-09-11 - 768, was 560. Saturated worst case (every
             * counter at 10 digits, every string at its buffer size): 611
             * characters BEFORE this change - the tail was already cut there - and
             * 655 with the two keys below. A key appended at the end is the first
             * one snprintf cuts. aud_lost= / aud_stale= are appended LAST and
             * named apart from the video's lost= (tools/ab_video.sh greps it). */
            char detail[768];
            char kd[16];   /* ING-1: kernel=n/a where the platform cannot count */
            if (g_kdrops_ok) snprintf(kd, sizeof kd, "%u", g_kernel_drops);
            else             snprintf(kd, sizeof kd, "n/a");
            /* ING-1 2026-09-11 - batch= and max_gap= (see rx_timed), n/a when
             * not measured. The gap is rounded to the nearest ms: the question
             * it answers is "5 ms of wait, or 300 ms of absence". */
            char lot_s[16], ecart_s[24];
            if (rx_timed) snprintf(lot_s, sizeof lot_s, "%u", rx_batch_max);
            else          snprintf(lot_s, sizeof lot_s, "n/a");
            if (rx_timed && rx_gap_max_us >= 0)
                snprintf(ecart_s, sizeof ecart_s, "%lldms",
                         (long long)((rx_gap_max_us + 500) / 1000));
            else
                snprintf(ecart_s, sizeof ecart_s, "n/a");
            snprintf(detail, sizeof(detail),
                "t=%ds video=%u (%llu B) cursor=%u audio=%u dup=%u decoded=%u ok=%u/%u "
                "parity=%u abandoned=%u lost=%u/%u orph=%u(dup=%u lost=%u other=%u) redund=%u trunc=%u nack=%u last=%u incompl=%u | "
                "NAL top=%u bot=%u idr_t=%u idr_b=%u sps=%u pps=%u | reg=%u kernel=%s rx_age=%llums peak=%.0f fps"
                " batch=%s max_gap=%s aud_lost=%u aud_stale=%u",
                sec, stats->udp_video_pkts, (unsigned long long)stats->udp_video_bytes,
                stats->udp_cursor_pkts, stats->udp_audio_pkts, stats->audio_dup_skipped,
                stats->frames_decoded,
                stats->decrypt_ok, stats->decrypt_ok + stats->decrypt_fail,
                stats->parity_skip, stats->reasm_abandoned,
                stats->chunks_missing, stats->chunks_expected,
                stats->chunks_orphan,
                stats->chunks_orphan_dup, stats->chunks_orphan_lost,
                stats->chunks_orphan_stale, stats->chunks_redundant,
                stats->frames_dropped_trunc, stats->nack_sent,
                stats->got_last_chunk, stats->incomplete_at_flush,
                stats->nal_top, stats->nal_bottom,
                stats->nal_idr_top, stats->nal_idr_bottom,
                stats->nal_sps, stats->nal_pps,
                reg_count, kd,
                (unsigned long long)(now_ms - d2_last_rx_ms), g_crete_img_s,
                lot_s, ecart_s, stats->audio_frames_lost, stats->audio_stale);
            emit_progress(p, "stats", detail);
            rx_batch_max    = 0;    /* ING-1 2026-09-11 - per window: reset once printed */
            rx_gap_max_us = -1;

            /* === L15 2026-08-29 - WHAT WE ASK FOR VERSUS WHAT WE GET ===
             *
             * The requested frame rate goes out in `[Q1]` at the start of the
             * session, the received frame rate lives in a cumulative counter:
             * nobody had ever put the two side by side. Observed consequence - a
             * setting at 120 fps that returns 55, and nothing in the log to say
             * so; you had to divide two counters by hand to notice.
             *
             * We measure over the LAST 5 s window and not since the start: a
             * cumulative average includes the bootstrap and takes a minute to
             * reflect a change, which is exactly the opposite of what a
             * diagnostic should do.
             *
             * The 85% threshold lets the server's rounding through (59.94 for
             * 60) without letting a factor of two through. */
            {
                static uint32_t l15_prec_img = 0;
                static int      l15_prec_sec = 0;
                const uint32_t  img = stats->frames_decoded;
                if (l15_prec_sec > 0 && sec > l15_prec_sec) {
                    const double got =
                        (double)(img - l15_prec_img) / (double)(sec - l15_prec_sec);
                    const int demande = (p && p->target_fps > 0) ? (int)p->target_fps : 0;
                    if (demande > 0 && got < 0.85 * demande)
                        clog("[L15] cadence : %d fps demandes, %.1f received "
                             "(%.0f %%) - the server is not keeping up; look at the "
                             "bitrate and the link before blaming the client",
                             demande, got, 100.0 * got / demande);
                }
                l15_prec_img = img;
                l15_prec_sec = sec;
            }
            last_log_sec = sec;
        }

        /* === L5 2026-08-29 - PER-STAGE LATENCY REPORT ===
         * Called on every pass; the function limits itself to one write every
         * `SHADOW_LATENCE_MS` (10 s by default). It lives here and not on the UI
         * thread: this thread lives for the whole session, including when video
         * dies mid-session (3.34) - and that is precisely the moment when we
         * still want to read the audio and the input figures. */
        latency_report_periodic(now_ms);

        /* === K16d 2026-08-29 - THE CROSS-COLUMN ALARM ===
         *
         * The K16b defect is only visible by crossing TWO columns of the same
         * line: `video>0` **and** `sps=0`. Each one on its own is normal. Nobody
         * makes that cross-check twice in a row, and that is exactly what
         * happened: eight desktop sessions were read, the line was in front of
         * us every time, and the conclusion drawn was "the server does not send
         * key frames" - while it was sending one and we were throwing it away.
         *
         * The threshold is 100 frames: enough for a key frame to have certainly
         * arrived (it comes with the 1st frame, 74 ms after registration), few
         * enough to raise the alarm within the first two seconds.
         *
         * Logged ONCE per session. No capped counter that would mask the next
         * one: this is an alarm, it has said what it had to say. The flag lives
         * in `ctx`, NOT in a function `static` - the most expensive defect
         * family in this repo, and it would be ironic to recreate it inside the
         * detector that watches for it. */
        if (!ctx.sps_alarm_said && stats->udp_video_pkts > 100
            && stats->nal_sps == 0) {
            ctx.sps_alarm_said = 1;
            clog("[K16d] ALARM: %u pictures arriving but NO parameter "
                 "sequence (sps=0, pps=%u, idr_t=%u). The key frames are "
                 "dropped UPSTREAM of the decoder - see the per-role routing in "
                 "ctrl_video_tcp.c. The decoder will never start.",
                 stats->udp_video_pkts, stats->nal_pps, stats->nal_idr_top);
        }

        /* ING-1 2026-09-11 - end of the pass body, JUST before the poll wait: a
         * timer taken here caught every in-body absence in the benches, one
         * taken before the poll missed 10-100 % of them. Whatever runs between
         * this stamp and the top of the next pass lands in max_gap=, not in
         * reception/tour: keep that stretch to the wait itself. */
        if (rx_pass_t0_us) {
            rx_pass_end_us = latency_now_us();
            latency_add(LAT_RX_PASS, rx_pass_end_us - rx_pass_t0_us);
        }
        if (got_video == 0 && got_cursor == 0) {
            /* === L6 2026-08-29 - WAIT ON PACKETS, NOT ON THE CLOCK ===
             *
             * This loop slept 5 ms whenever a pass returned nothing. The sleep
             * did avoid the busy loop, but it DELAYS everything that arrives
             * during it: a video chunk landing on the socket a microsecond after
             * the `nanosleep` waits the full 5 ms. At 50 fps a frame lasts
             * 20 ms, and a burst contains several - so the sleep is paid several
             * times per frame.
             *
             * Worse, it is paid TWICE: video and audio share this thread, so the
             * same sleep delays both chains.
             *
             * `poll()` does exactly what we want: it returns the INSTANT one of
             * the three sockets has something, and sleeps until the timeout
             * otherwise. Same protection against the busy loop, without the
             * delay.
             *
             * THE TIMEOUT STAYS, and it is not decorative: this thread does more
             * than read sockets - the congestion feedback tick, the ordered
             * drain, the outage detector, the reports. A `poll` with no timeout
             * would freeze them as long as no packet arrives, that is, exactly
             * during a video outage (3.34), at the very moment those mechanisms
             * are needed. So 5 ms as a CEILING, instead of 5 ms as a FLOOR.
             *
             * `SHADOW_RX_POLL=0` restores the fixed sleep. */
            static int g_poll = -1;
            if (g_poll < 0) {
                const char *e = getenv("SHADOW_RX_POLL");
                g_poll = e ? atoi(e) : 1;
            }
            if (g_poll) {
                struct pollfd pfd[4];
                nfds_t n = 0;
                if (udp_video  >= 0) { pfd[n].fd = udp_video;  pfd[n].events = POLLIN; n++; }
                if (udp_cursor >= 0) { pfd[n].fd = udp_cursor; pfd[n].events = POLLIN; n++; }
                if (udp_input  >= 0) { pfd[n].fd = udp_input;  pfd[n].events = POLLIN; n++; }
                /* ING-2 2026-09-11 - the control socket, under the guard (see
                 * `ctrl_in_poll`): only while the V16 reader runs on every
                 * pass and has never failed, never once the peer has closed.
                 * Bytes TLS has already decrypted are invisible to poll():
                 * with some waiting, the wait does not sleep at all - the
                 * reader takes one frame per pass, so it stays bounded. */
                const int ctrl_fd = (g_auto_flush_ack && ctrl_in_poll) ? ctrl_tcp_poll_fd(tcp) : -1;
                nfds_t ctrl_slot = 0;
                int wait_ms = 5;
                if (ctrl_fd >= 0) {
                    ctrl_slot = n;
                    pfd[n].fd = ctrl_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
                    if (ctrl_tcp_buffered(tcp)) wait_ms = 0;
                }
                ing2_waits++;
                /* No socket at all - TCP video mode opens none (K15i): there
                 * is nothing to wait on, so we fall back to the sleep. Since
                 * ING-2 the control socket counts, so this is left to
                 * SHADOW_CTRL_POLL=0 and to a session that dropped it. */
                if (n == 0) {
                    struct timespec ts = {0, 5 * 1000 * 1000};
                    nanosleep(&ts, NULL);
                } else {
                    const int pr = poll(pfd, n, wait_ms);
                    if (ctrl_fd >= 0) {
                        if (pr > 0 && pfd[ctrl_slot].revents) {
                            ing2_ctrl_wakes++;
                        } else if (pr < 0) {
                            /* ING-2 - an error returns at once, on every pass:
                             * the same spin by another road. libnx fails the
                             * whole call when one descriptor does not map to a
                             * socket. Drop ours and wait as L6 did. */
                            ctrl_in_poll = false;
                            clog("[ING2] control channel removed from the wait: poll failed (err=%d)",
                                 shadow_sock_errno());
                        }
                    }
                }
            } else {
                struct timespec ts = {0, 5 * 1000 * 1000};  /* 5ms */
                nanosleep(&ts, NULL);
            }
        }
    }

    /* No final drain needed: vid_reasm.c emits as it goes, and the cursor
     * reassembler's partial frames are freed by sufp_destroy. */

    /* ING-2 2026-09-11 - one line per session, what the live A/B reads: the
     * arm, the idle-wait rate (a spin shows as ~10^5/s; the benches' loop made
     * at most ~10^3 passes a second), how many waits the control socket ended,
     * and the round trip with sub-millisecond resolution - mean over the
     * session, p90 and jitter over the last 64 replies (rtt.h), where the
     * [S118] line prints whole milliseconds for one reply in forty. Before
     * `cleanup:` on purpose: the early `goto cleanup` paths skip the
     * declarations above. */
    {
        const int ss = stats->session_seconds > 0 ? stats->session_seconds : 1;
        clog("[DNS1] summary: %u lookup(s) of the VM name avoided, %u performed",
             session_host_hits(), session_host_lookups());
        clog("[ING2] summary: waiting on the control channel=%s, %lu waits (%lu/s), "
             "%lu wakeups from this channel | round trip avg=%.2f p90=%.2f jitter=%.2f ms n=%u",
             !(g_ctrl_poll && g_auto_flush_ack && ing2_rx_poll) ? "non"
             : ctrl_in_poll ? "oui" : "retire",
             ing2_waits, ing2_waits / (unsigned long)ss, ing2_ctrl_wakes,
             rtt_avg_us(&ctrl_rtt) / 1000.0, rtt_pct_us(&ctrl_rtt, 90) / 1000.0,
             rtt_jitter_us(&ctrl_rtt) / 1000.0, (unsigned)ctrl_rtt.count);
    }
    /* === AUD-DEDUP-3 2026-09-11 - THE SESSION'S AUDIO LOSS, IN ONE LINE ===
     * perdues = lost + pending: at the end of the session no late frame can
     * still fill a slot, so the pending ones are lost too. The jump histogram
     * keeps the numbering question answered session by session - the server
     * numbers SENT frames, so a healthy session is ~all 1. vieux = refusals
     * that were too old (S36), counted apart from the duplicates. */
    if (stats->udp_audio_pkts > 0) {
        const audio_loss_t *al = &ctx.aud_loss;
        const uint32_t lost     = al->lost + audio_loss_pending(al);
        const uint32_t expected = al->noted + lost;
        clog("[AUD17] session summary: accepted=%u lost=%u (%.2f%%) gaps=%u longest=%u "
             "tardives=%u renum=%u deltas 1/2-3/4-64/>64=%u/%u/%u/%u vieux=%u",
             al->noted, lost, expected ? 100.0 * lost / expected : 0.0,
             al->holes, al->hole_max, al->late, al->renum,
             al->deltas[AUDIO_LOSS_D1], al->deltas[AUDIO_LOSS_D2_3],
             al->deltas[AUDIO_LOSS_D4_64], al->deltas[AUDIO_LOSS_D65_UP],
             stats->audio_stale);
    }

cleanup:
    /* VI1 - no "frozen" value may survive into the next session.
     * AUD-INS-3 2026-09-11 - through a NET merge: this thread is that group's
     * one writer, and the decode thread may still be running here (the glue
     * joins it only after ctrl_session_run returns) - a whole-struct publish
     * would write back this thread's stale copy of its VIDEO and AUDIO groups. */
    {
        session_stats_t pub;
        session_stats_get(&pub);
        pub.rtp_video_stuck_secs = 0;
        session_stats_merge(&pub, SESSION_STATS_NET);
    }
    /* L5: without this last report, a session shorter than the period (an 8 s
     * trial, an outage during bootstrap) would leave NO measurement at all -
     * everything accumulated would go away with the process. */
    latency_report_final();
    /* AUD6 - we tell the server before leaving.
     *
     * We used to close the sockets without a word. The official client emits
     * `kUnregisterSession` at the end of a session, and without it the server
     * seems to keep the previous subscription: on reconnect, video comes back
     * but the cursor/audio channel stays silent. It is the best explanation we
     * have so far for one session in two being silent, all our bootstrap traces
     * being otherwise identical between a working case and a failing one. */
    if (tcp) {
        /* === S48 2026-08-26 - RELEASE EVERY STREAM, LIKE THE OFFICIAL CLIENT ===
         *
         * AUD6 sent ONE `UnregisterSession` with an EMPTY body. The official
         * client's capture shows EIGHT messages at close, each naming a stream
         * (observed order: no identifier, then 3, 1, 2, 5, 6, 4, 7). An empty
         * message does not tell the server what to release - and the server's
         * status report afterwards mentions a stream still attached from an
         * earlier session exactly when the audio is silent (8/8 correlation,
         * KB §3.35).
         *
         * The timestamp is the server's clock, which its status reports give us;
         * failing that we send our own session duration, the field most likely
         * serving for ordering.
         *
         * `SHADOW_UNREG_PER_STREAM=0` restores the single empty message. */
        static int g_unreg_par_flux = -1;
        if (g_unreg_par_flux < 0) {
            const char *e = getenv("SHADOW_UNREG_PER_STREAM");
            g_unreg_par_flux = e ? atoi(e) : 1;
        }
        uint8_t ubuf[256];
        if (g_unreg_par_flux) {
            const int ordre[] = { -1, 3, 1, 2, 5, 6, 4, 7 };
            /* The server clock if we have seen it, otherwise our own duration
             * - the field most likely serves for ordering. */
            uint32_t ts = g_srv_clock_ms;
            if (!ts) ts = (uint32_t)hs_now_ms();
            unsigned sent = 0;
            for (size_t i = 0; i < sizeof(ordre)/sizeof(ordre[0]); i++) {
                int un = ctrl_build_unregister_stream(ubuf, sizeof(ubuf),
                                                      0xFF00 + (uint32_t)i,
                                                      ordre[i], ts + (uint32_t)i);
                if (un > 0 && ctrl_tcp_send_cleartext(tcp, ubuf, (size_t)un)) sent++;
            }
            clog("[S48] %u flux desenregistres (horloge serveur=%u)", sent, ts);
        } else {
            int un = ctrl_build_unregister_session(ubuf, sizeof(ubuf), 0xFFFF);
            if (un > 0 && ctrl_tcp_send_cleartext(tcp, ubuf, (size_t)un))
                clog("[AUD6] session released on the server side (UnregisterSession)");
        }
    }

    /* I1 phase 3 2026-05-18: clear the bridge BEFORE closing itc - otherwise a
     * race can have the drain thread try to send on an already-freed itc. */
    native_input_set(NULL);
    /* AF8 2026-09-10 - unpublished BEFORE the block is freed, under the lock
     * `ctrl_session_request_refresh` takes. It used to be cleared seven lines
     * after `ctrl_video_tcp_close`, which ends with a `free`, while the pause
     * menu could still reach it. `native_input_set(NULL)` just above was the
     * right model all along. */
    pthread_mutex_lock(&g_active_vst_mtx);
    g_active_vst = NULL;
    pthread_mutex_unlock(&g_active_vst_mtx);
    session_ft_reveal_clear();   /* FT3: the credential dies with the session */
    /* CLIP3: before the others only so its summary line lands next to the
     * channel's own. It joins its receive thread, which honours the abort flag
     * within 100 ms. */
    session_clipboard_close(&ctx);
    if (ctx.comchan) ctrl_comchan_close(ctx.comchan);
    if (ctx.vst) ctrl_video_tcp_close(ctx.vst);
    if (ctx.vtcp) ctrl_video_tcp_close(ctx.vtcp);   /* K15k */
    if (ctx.itc) ctrl_input_tcp_close(ctx.itc);
    if (ctx.aud) ctrl_audio_dtls_close(ctx.aud);
    /* === AF1 2026-09-10 - DISARM THE GAMEPAD BEFORE DESTROYING ITS CIPHER ===
     *
     * The cipher used to be destroyed first and the gamepad detached six lines
     * later. In between, `ctrl_gamepad_active()` answered true on a freed
     * pointer, and an emitter - the Switch `fil_manette` at 250 Hz, the Linux
     * evdev reader, the Windows axis probe - could encrypt with it. The other
     * half of the fix is in ctrl_gamepad.c: `detach` clears the pair under the
     * lock `send_payload` re-tests under, so reordering these lines is not what
     * closes the race on its own. `udp_input` still closes after the detach. */
    ctrl_gamepad_stop_local_reader();
    ctrl_gamepad_detach();
    if (cipher) shadow_cipher_destroy(cipher);
    ctx.cipher = NULL;   /* the context no longer points at a freed block */
    if (udp_video  >= 0) shadow_closesocket(udp_video);
    if (udp_cursor >= 0) shadow_closesocket(udp_cursor);
    g_session_active = 0;
    if (udp_input  >= 0) shadow_closesocket(udp_input);
    if (tcp) ctrl_tcp_close(tcp);
    if (ctx.sufp_cursor) sufp_destroy(ctx.sufp_cursor);
    if (ctx.plain_buf) free(ctx.plain_buf);
    emit_progress(p, "cleanup", "session ended");
    /* Review 2026-08-21 - a session cut by the server (exit_reason=2) returned
     * `true` as soon as any video had arrived, so the GUI displayed "native
     * stream ended" for a stream that had just been killed. The state is now
     * reported to the caller. */
    if (stats->exit_reason == 2) return false;
    return any_video;
}
