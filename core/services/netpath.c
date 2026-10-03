/* netpath.c - see the header for why the round trip is split and why only the
 * local hop is measured. */
#include "netpath.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <icmpapi.h>
#elif defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
/* The consoles have no route table to read and no unprivileged ICMP. Both
 * entry points below compile to a refusal, and the caller shows the total
 * alone - which is what it does on a desktop whose gateway drops ICMP. */
#  define NETPATH_UNSUPPORTED 1
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <netinet/in.h>
#  include <netinet/ip_icmp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

/* ====================================================== the default gateway */

#if defined(NETPATH_UNSUPPORTED)

bool netpath_default_gateway(char *out, int cap)
{
    if (out && cap > 0) out[0] = '\0';
    return false;
}

int64_t netpath_ping_us(const char *host, int timeout_ms)
{
    (void)host; (void)timeout_ms;
    return -1;
}

#elif defined(_WIN32)

bool netpath_default_gateway(char *out, int cap)
{
    if (!out || cap < 16) return false;
    out[0] = '\0';

    /* `GetBestRoute` to 0.0.0.0 asks the stack the same question the stack
     * asks itself for any unrouted packet, so the answer follows a VPN or a
     * second adapter without us having to rank interfaces by hand - which is
     * where a hand-rolled walk of the route table goes wrong. */
    MIB_IPFORWARDROW row;
    memset(&row, 0, sizeof row);
    if (GetBestRoute(0, 0, &row) != NO_ERROR) return false;

    const DWORD gw = row.dwForwardNextHop;
    /* An on-link route has no next hop: the destination IS the link, so there
     * is no gateway to measure against and the split does not apply. */
    if (gw == 0) return false;

    struct in_addr a;
    a.S_un.S_addr = gw;
    return inet_ntop(AF_INET, &a, out, (size_t)cap) != NULL;
}

int64_t netpath_ping_us(const char *host, int timeout_ms)
{
    if (!host || !host[0]) return -1;

    struct in_addr dst;
    if (inet_pton(AF_INET, host, &dst) != 1) return -1;

    HANDLE h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) return -1;

    /* The payload is ours and arbitrary; the reply buffer must hold the reply
     * structure plus the echoed payload, and Microsoft documents the extra 8
     * bytes for an ICMP error that may come back instead. */
    static const char payload[32] = "halyard-netpath-probe";
    char reply[sizeof(ICMP_ECHO_REPLY) + sizeof payload + 8];

    const DWORD n = IcmpSendEcho(h, dst.S_un.S_addr, (LPVOID)payload,
                                 (WORD)sizeof payload, NULL,
                                 reply, (DWORD)sizeof reply,
                                 (DWORD)(timeout_ms > 0 ? timeout_ms : 1000));
    int64_t us = -1;
    if (n > 0) {
        const ICMP_ECHO_REPLY *r = (const ICMP_ECHO_REPLY *)reply;
        if (r->Status == IP_SUCCESS) {
            /* RoundTripTime is in WHOLE MILLISECONDS, so a 0.4 ms gateway
             * reports 0 and a sub-millisecond LAN reads as "0 ms" rather than
             * as a failure. That is the API's resolution and not something
             * this function can improve on; the caller shows "<1 ms". */
            us = (int64_t)r->RoundTripTime * 1000;
        }
    }
    IcmpCloseHandle(h);
    return us;
}

#else  /* POSIX */

bool netpath_default_gateway(char *out, int cap)
{
    if (!out || cap < 16) return false;
    out[0] = '\0';

    /* /proc/net/route, which is the destination-0 row. Parsed rather than
     * asked of netlink because this is twelve lines against a netlink dialogue,
     * and the file is a kernel interface with a stable format. */
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return false;

    char line[256];
    bool found = false;
    /* The header row. */
    if (!fgets(line, sizeof line, f)) { fclose(f); return false; }
    while (fgets(line, sizeof line, f)) {
        char iface[64];
        unsigned long dest = 0, gw = 0, flags = 0;
        if (sscanf(line, "%63s %lx %lx %lx", iface, &dest, &gw, &flags) != 4)
            continue;
        /* RTF_UP (0x1) and RTF_GATEWAY (0x2), destination 0.0.0.0. */
        if (dest != 0 || !(flags & 0x1) || !(flags & 0x2) || gw == 0) continue;

        struct in_addr a;
        a.s_addr = (in_addr_t)gw;   /* the file gives it in network order */
        found = inet_ntop(AF_INET, &a, out, (socklen_t)cap) != NULL;
        break;
    }
    fclose(f);
    return found;
}

static uint16_t icmp_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (; len > 1; len -= 2, p += 2) sum += (uint32_t)((p[0] << 8) | p[1]);
    if (len) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

int64_t netpath_ping_us(const char *host, int timeout_ms)
{
    if (!host || !host[0]) return -1;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) return -1;

    /* SOCK_DGRAM and not SOCK_RAW: the datagram form of ICMP needs no
     * capability when the caller's gid is inside net.ipv4.ping_group_range,
     * which is how `ping` has shipped unprivileged on most distributions for
     * years. The kernel writes the identifier itself and rewrites it in the
     * reply, so the echo id we set is ignored - which is why the reply is
     * matched on the SEQUENCE alone below. */
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) return -1;

    static uint16_t seq_counter = 0;
    const uint16_t seq = ++seq_counter;

    struct {
        struct icmphdr h;
        char payload[32];
    } pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.h.type = ICMP_ECHO;
    pkt.h.code = 0;
    pkt.h.un.echo.id = 0;
    pkt.h.un.echo.sequence = htons(seq);
    memcpy(pkt.payload, "halyard-netpath-probe", 21);
    pkt.h.checksum = 0;
    /* The kernel fills the checksum for a datagram ICMP socket, but setting it
     * is harmless and keeps this correct if the socket ever becomes raw. */
    pkt.h.checksum = htons(icmp_checksum(&pkt, sizeof pkt));

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    if (sendto(fd, &pkt, sizeof pkt, 0, (struct sockaddr *)&dst, sizeof dst) < 0) {
        close(fd);
        return -1;
    }

    /* Poll rather than a socket timeout, so a reply that is not ours does not
     * consume the whole budget: the loop keeps waiting for what is LEFT. */
    int remaining = timeout_ms > 0 ? timeout_ms : 1000;
    for (;;) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        const int pr = poll(&pfd, 1, remaining);
        if (pr <= 0) { close(fd); return -1; }

        char buf[128];
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        gettimeofday(&t1, NULL);
        if (n < (ssize_t)sizeof(struct icmphdr)) { close(fd); return -1; }

        const struct icmphdr *rh = (const struct icmphdr *)buf;
        if (rh->type == ICMP_ECHOREPLY && ntohs(rh->un.echo.sequence) == seq) {
            close(fd);
            return (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000
                 + (int64_t)(t1.tv_usec - t0.tv_usec);
        }

        const int64_t spent = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000
                            + (int64_t)(t1.tv_usec - t0.tv_usec) / 1000;
        remaining = (int)((timeout_ms > 0 ? timeout_ms : 1000) - spent);
        if (remaining <= 0) { close(fd); return -1; }
    }
}

#endif

/* ================================================================ the split */

/* See the header: pure, so the offline suite can reach it. */
void netpath_split_compute(int64_t total_us, int64_t local_us, netpath_split *out)
{
    if (!out) return;
    /* The gateway string is the caller's and is NOT cleared here: this
     * function is about the numbers, and `netpath_measure` fills the name
     * before calling it. Clearing the whole struct would wipe it. */
    out->total_us  = total_us > 0 ? total_us : -1;
    out->local_us  = local_us >= 0 ? local_us : -1;
    out->have_local = local_us >= 0;

    if (local_us < 0 || total_us <= 0) {
        /* Without both, there is no split. The remote hop is the TOTAL when
         * that is all we have - not "unknown" - because the whole round trip
         * did happen out there somewhere, and showing it as one unlabelled
         * figure is honest where showing a dash is merely unhelpful. */
        out->remote_us = total_us > 0 ? total_us : -1;
        out->ordered = false;
        return;
    }

    /* See the header: the two numbers come from different mechanisms at
     * different moments, so local > total is routine rather than impossible.
     * Clamp, and SAY that it was clamped. */
    out->ordered = (local_us <= total_us);
    out->remote_us = out->ordered ? total_us - local_us : 0;
}

bool netpath_measure(int64_t total_us, int timeout_ms, netpath_split *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);

    if (!netpath_default_gateway(out->gateway, (int)sizeof out->gateway)) {
        netpath_split_compute(total_us, -1, out);
        return total_us > 0;
    }

    const int64_t local = netpath_ping_us(out->gateway, timeout_ms);
    netpath_split_compute(total_us, local, out);
    return local >= 0 || total_us > 0;
}
