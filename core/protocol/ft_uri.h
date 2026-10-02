/* ft_uri - the SFTP session as a URI a file manager can open.
 *
 * ############################################################################
 * ## RETRACTED 2026-10-02 AS THE WAY TO REACH THIS SERVER - measured, not   ##
 * ## reasoned. The module is CORRECT and its test still passes; what is     ##
 * ## wrong is the assumption it was built on, that the VM's credential is a ##
 * ## PASSWORD THAT CAN LIVE IN A URI. It is not:                            ##
 * ##                                                                        ##
 * ##   - the credential is a 395-byte OpenSSH PEM private key used as a     ##
 * ##     PASSWORD (the server, libssh_0.11.0, offers `password` and nothing ##
 * ##     else - no publickey), and it must be passed BYTE-EXACT, including  ##
 * ##     its two embedded newlines AND its trailing one. 394 bytes without  ##
 * ##     the final newline is refused;                                      ##
 * ##   - no single-line form of it authenticates. Newlines as spaces,       ##
 * ##     newlines removed, newlines as a literal backslash-n, the base64    ##
 * ##     body alone, the body plus a newline: all five REFUSED against the  ##
 * ##     live server, with the 395-byte original accepted in the same run;  ##
 * ##   - curl rejects the resulting URI outright - "URL using bad/illegal   ##
 * ##     format" - and curl/libssh2 refuses the credential even when it is  ##
 * ##     handed over as an argument rather than in a URI, with the exact    ##
 * ##     bytes libssh accepts;                                             ##
 * ##   - a GUI password box is one line, so WinSCP and FileZilla cannot     ##
 * ##     carry it either. That is what "Erreur d'authentification" was.     ##
 * ##                                                                        ##
 * ## So there is no third-party client on this path. `filetransfer.c`       ##
 * ## (libssh) is the one thing that authenticates, and it is ours.          ##
 * ##                                                                        ##
 * ## KEPT, not deleted, for the two reasons this repo always keeps a        ##
 * ## refuted branch: so the experiment is not run a third time, and because ##
 * ## the code is right for any credential that IS a one-line password - if  ##
 * ## the server ever issues one, this is ready and tested.                  ##
 * ############################################################################
 *
 * === FT4 2026-10-02 — WHY THIS IS A MODULE AND NOT A `snprintf` =============
 *
 * The VM's file transfer is ordinary SFTP on `:base+15` with password auth, so
 * WinSCP, FileZilla and `sftp` can drive it with no Halyard at all once they
 * have host, port and password. One URI carries all three, which turns "copy
 * four fields out of a text file" into one click:
 *
 *     sftp://shadow:PASSWORD@host:10015/
 *
 * Three things make that string easy to get WRONG, and all three fail in the
 * same unhelpful way — a password prompt, or "authentication failed", with
 * nothing saying the URI was mangled rather than the credential refused:
 *
 *   1. THE PASSWORD IS BASE64. Its alphabet contains `+`, `/` and `=`. In the
 *      userinfo part of a URI, `/` ENDS the authority, `=` is reserved, and `+`
 *      is read as a space by a careless parser. A base64 secret pasted raw is
 *      therefore a DIFFERENT password, and roughly one in two of them contains
 *      at least one of the three.
 *   2. THE VM IS OFTEN REACHED OVER IPv6. `2001:db8:1:2::` - a DOCUMENTATION
 *      address (RFC 3849), deliberately, here and in the tests: a real VM
 *      address identifies somebody's machine and this repo redacts them
 *      (KB.md writes `<VM public IPv6>`) - has colons in
 *      it, and in a URI a colon after the host starts the port. An IPv6 literal
 *      must be bracketed, and bracketing one that is ALREADY bracketed breaks
 *      it just as thoroughly.
 *   3. IT CARRIES A CREDENTIAL. So it must refuse rather than truncate: a URI
 *      cut short at the buffer's end is a valid-looking URI with a wrong
 *      password, which is the single most confusing outcome available.
 *
 * Pure, so it is tested offline (`tests/test_ft_uri.c`): no globals, no I/O, no
 * `getenv`. It is a header because that is how this repo tests header-only
 * logic — by including the header.
 *
 * WHAT IT DELIBERATELY DOES NOT DO. It does not launch anything and it does not
 * know what WinSCP is: `launch_uri.h` opens a URI, and the scheme is what makes
 * the right application answer. And it does not try to carry a host key. The
 * VM generates one per session, so there is nothing stable to pin; the file
 * manager asks once and that is the correct behaviour, not a gap to paper over.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The characters that may appear in a URI unencoded, per RFC 3986's
 * "unreserved" set. EVERYTHING else is percent-encoded, which is stricter than
 * the standard allows in userinfo and deliberately so: the cost of encoding a
 * character that did not need it is zero, and the cost of missing one is a
 * silently wrong password. */
static inline bool ft_uri_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

/* Percent-encodes `n` bytes of `in` into `out` (`cap` bytes including the NUL).
 * Returns the length written, or 0 when it does not fit — never a partial
 * result, and `out` is then an empty string.
 *
 * Upper-case hex: RFC 3986 says producers should, and a credential that differs
 * only in the case of its escapes is a difference nobody wants to debug. */
static inline size_t ft_uri_encode(const char *in, size_t n,
                                   char *out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (n > 0 && !in) return 0;

    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)in[i];
        if (ft_uri_unreserved(c)) {
            if (w + 1 + 1 > cap) { out[0] = '\0'; return 0; }
            out[w++] = (char)c;
        } else {
            if (w + 3 + 1 > cap) { out[0] = '\0'; return 0; }
            out[w++] = '%';
            out[w++] = HEX[(c >> 4) & 0xF];
            out[w++] = HEX[c & 0xF];
        }
    }
    out[w] = '\0';
    return w;
}

/* True when `host` is an IPv6 literal that needs bracketing: it has a colon and
 * is not bracketed already. A name or an IPv4 address has no colon, and a host
 * the caller already bracketed must be left alone. */
static inline bool ft_uri_needs_brackets(const char *host)
{
    if (!host || !host[0]) return false;
    if (host[0] == '[') return false;          /* already bracketed */
    for (const char *p = host; *p; p++)
        if (*p == ':') return true;
    return false;
}

/* Builds `sftp://user:password@host:port/` into `out`.
 *
 * `secret_len` is the password's length in bytes; it is NOT assumed to be
 * NUL-terminated, because the wire field is not. `user` may be NULL, in which
 * case "shadow" is used — the VM's Authenticate path does not look at the name,
 * only at the password.
 *
 * Returns the length written, or 0 on a bad argument or a buffer too small. On
 * 0, `out` is an empty string: see point 3 of the header comment.
 */
static inline size_t ft_uri_build(char *out, size_t cap,
                                  const char *host, int port,
                                  const char *user,
                                  const char *secret, size_t secret_len)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!host || !host[0]) return 0;
    if (port <= 0 || port > 65535) return 0;
    if (!secret || secret_len == 0) return 0;
    if (!user || !user[0]) user = "shadow";

    /* The user name is encoded too. It is ours today, but a module that
     * encodes one of its two userinfo halves and not the other is one edit away
     * from a defect. */
    char u_enc[128];
    size_t un = 0;
    {
        size_t ul = 0;
        while (user[ul]) ul++;
        un = ft_uri_encode(user, ul, u_enc, sizeof u_enc);
        if (un == 0) return 0;
    }

    /* The password, encoded into a buffer sized for the worst case: every byte
     * becoming three characters. The wire field is 395 bytes, so 512 is the
     * round number above it and 3x+1 is what encoding it can need. */
    char p_enc[512 * 3 + 1];
    const size_t pn = ft_uri_encode(secret, secret_len, p_enc, sizeof p_enc);
    if (pn == 0) return 0;

    const bool brackets = ft_uri_needs_brackets(host);

    /* Measured, then written: computing the length first is what lets this
     * refuse instead of truncating. `snprintf` would tell us afterwards, with
     * a half-written credential already in the caller's buffer. */
    size_t hl = 0;
    while (host[hl]) hl++;

    char portbuf[8];
    size_t pl = 0;
    {
        int v = port;
        char tmp[8]; size_t t = 0;
        while (v > 0 && t < sizeof tmp) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t > 0) portbuf[pl++] = tmp[--t];
        portbuf[pl] = '\0';
    }

    const size_t need = 7 /* "sftp://" */ + un + 1 /* ':' */ + pn + 1 /* '@' */
                      + (brackets ? 2u : 0u) + hl + 1 /* ':' */ + pl
                      + 1 /* '/' */;
    if (need + 1 > cap) return 0;

    size_t w = 0;
    const char *scheme = "sftp://";
    for (size_t i = 0; i < 7; i++) out[w++] = scheme[i];
    for (size_t i = 0; i < un; i++) out[w++] = u_enc[i];
    out[w++] = ':';
    for (size_t i = 0; i < pn; i++) out[w++] = p_enc[i];
    out[w++] = '@';
    if (brackets) out[w++] = '[';
    for (size_t i = 0; i < hl; i++) out[w++] = host[i];
    if (brackets) out[w++] = ']';
    out[w++] = ':';
    for (size_t i = 0; i < pl; i++) out[w++] = portbuf[i];
    out[w++] = '/';
    out[w] = '\0';
    return w;
}

#ifdef __cplusplus
}
#endif
