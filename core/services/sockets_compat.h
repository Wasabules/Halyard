/* sockets_compat.h - single include that papers over BSD vs Winsock.
 *
 * Use this header instead of including <sys/socket.h> / <netdb.h> /
 * <arpa/inet.h> / <netinet/in.h> / <sys/select.h> / <unistd.h> directly in
 * network code. On Linux/macOS/Switch (newlib + libnx) it pulls in the real
 * POSIX headers. On Windows (MinGW-w64 / UCRT64) it includes winsock2/ws2tcpip
 * and defines the minimal shims for `closesocket`, `errno`, `EAGAIN`, etc.
 *
 * Call shadow_sockets_init() once at startup (WSAStartup on Windows, a no-op
 * elsewhere). Call shadow_sockets_shutdown() at exit.
 */

#pragma once

#if defined(_WIN32)

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mstcpip.h>     /* SIO_UDP_CONNRESET */
#  include <iphlpapi.h>
#  include <io.h>
/* Winsock's close-socket is not POSIX close, and THERE IS NO REMAP of close():
 * every socket must be closed through `shadow_closesocket()`. A plain close()
 * on a socket compiles everywhere and, under Windows only, fails silently with
 * EBADF, leaving the connection open. This comment used to promise a remap
 * that never existed; AF7 (2026-09-10) found a socket left open by exactly
 * that belief. For file descriptors (fopen/fread/fclose) the C runtime handles
 * it, independently of winsock. */
#  define shadow_closesocket(s) closesocket(s)
#  define shadow_sock_errno()   WSAGetLastError()
/* errno-style aliases so the POSIX code can be reused */
#  ifndef SHUT_RDWR
#    define SHUT_RDWR SD_BOTH
#  endif
/* MSG_DONTWAIT does not exist on Winsock - we emulate it by making the socket
 * non-blocking. Code that relies on MSG_DONTWAIT must toggle through
 * shadow_set_nonblocking() then do a plain recv(...). */
#  ifndef MSG_DONTWAIT
#    define MSG_DONTWAIT 0
#  endif
/* Winsock signals "would block" through WSAEWOULDBLOCK; we align on the POSIX
 * names so the existing `errno == EAGAIN || errno == EWOULDBLOCK` tests keep
 * working (provided shadow_sock_errno() is used). */
/* MinGW-w64's <errno.h> ALREADY defines EAGAIN/EWOULDBLOCK/EINPROGRESS with
 * POSIX values (140, 112, etc.) that do NOT match what Winsock returns through
 * WSAGetLastError() (WSAEWOULDBLOCK = 10035). We undef them to force the remap
 * onto the Winsock codes - otherwise the `errno == EWOULDBLOCK` check fails
 * silently after a non-blocking connect or recv. To be used only with
 * shadow_sock_errno(). */
#  undef EAGAIN
#  undef EWOULDBLOCK
#  undef EINPROGRESS
#  define EAGAIN       WSAEWOULDBLOCK
#  define EWOULDBLOCK  WSAEWOULDBLOCK
#  define EINPROGRESS  WSAEWOULDBLOCK
/* MinGW-w64 provides ssize_t through <sys/types.h>. We include it explicitly so
 * code that assigns `ssize_t = send(...)` (POSIX) compiles. */
#  include <sys/types.h>
/* === WIN1 2026-09-10 - poll(2) ===
 *
 * Winsock already declares `struct pollfd`, `POLLIN` and `WSAPoll`, which is
 * poll(2) under another name. Only the spelling and `nfds_t` are missing.
 *
 * A FUNCTION and not a `#define poll WSAPoll`: a macro named `poll` would also
 * rewrite `padforward::poll()`, a C++ method that has nothing to do with
 * sockets, and the error would surface far from here.
 *
 * Note for anyone reading a timeout: WSAPoll never reports POLLHUP for a peer
 * that closed, unlike Linux. The session loop only ever asks for POLLIN, so
 * this does not bite us - it would if someone started relying on POLLHUP. */
typedef ULONG nfds_t;
static __inline int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    return WSAPoll(fds, (ULONG)nfds, timeout_ms);
}

#else /* !_WIN32 — Linux/macOS/Switch */

#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>   /* TCP_NODELAY (perf : disable Nagle) */
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <errno.h>
#  define shadow_closesocket(s) close(s)
#  define shadow_sock_errno()   errno
typedef socklen_t socklen_t_compat;
typedef ssize_t   ssize_t_compat;

#endif /* _WIN32 */

#ifdef __cplusplus
extern "C" {
#endif

/* WSAStartup / no-op. Idempotent. Returns 0 on success, -1 on failure. */
int  shadow_sockets_init(void);
void shadow_sockets_shutdown(void);

/* Switches a socket to non-blocking portably. On POSIX = fcntl O_NONBLOCK; on
 * Windows = ioctlsocket FIONBIO. Returns 0/-1. */
int  shadow_set_nonblocking(int sock, int enable);

/* === WIN1 2026-09-10 - SO_RCVTIMEO / SO_SNDTIMEO, IN MILLISECONDS ===
 *
 * `which` is SO_RCVTIMEO or SO_SNDTIMEO. Returns 0/-1.
 *
 * This one is a trap, which is why it is a function and not a cast at the call
 * site. POSIX takes a `struct timeval`; Winsock takes a `DWORD` of
 * MILLISECONDS. The two are not merely spelled differently - a `struct timeval`
 * cast to `const char *` COMPILES on Windows and hands the kernel the first
 * four bytes of a seconds field, i.e. a timeout that is wrong by orders of
 * magnitude and silent about it. That is precisely the kind of number this repo
 * keeps a ledger of, so the conversion lives in one place. */
int  shadow_set_sock_timeout(int sock, int which, int timeout_ms);

#ifdef __cplusplus
}
#endif
