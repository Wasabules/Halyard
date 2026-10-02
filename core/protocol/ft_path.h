/* ft_path - is this remote path one we are willing to send to the VM?
 *
 * Header-only and PURE: no state, no I/O, no getenv. That is what earns it a
 * test (tests/test_ft_path.c), and this is a module whose bug has a blast
 * radius, so it gets one.
 *
 * === FT1 2026-10-02 — WHY A CLIENT-SIDE CHECK AT ALL =========================
 *
 * The VM's SFTP server does NOT confine paths. Read in ShadowStreamer 6.3.1
 * (KB §3.50, full write-up in halyard-lab/notes/findings/filetransfer.md): the
 * root is the interactive user's Downloads folder, but the join @0x140b8b710 is
 * purely lexical - it tests only for an empty name, a leading `\` or `/`, or a
 * drive-letter prefix, and on any of those **the client's path replaces the
 * root**; a relative path containing `..` is concatenated verbatim. There is no
 * canonicalisation and no containment check anywhere on that path. An
 * authenticated client can therefore read and write anywhere on the VM.
 *
 * We cannot fix the server. What we can decide is not to be the tool that walks
 * out of Downloads by accident: a path built from a filename the VM itself
 * supplied, from a UI field, or from a future client's drag-and-drop should not
 * be able to reach `C:\Windows` because nobody normalised it. So every remote
 * path crosses this check first, and the default REFUSES anything that is not
 * plainly inside the root.
 *
 * This is a seatbelt, not a security boundary: the credential that reaches the
 * server already grants everything, and a caller that passes
 * `SHADOW_FT_ALLOW_ABSOLUTE` gets what it asks for. The value is that the
 * dangerous case has to be asked for by name instead of arriving by mistake.
 *
 * Note what the server does NOT give us to help: its dispatch loop
 * (@0x140c890f0) answers status 8 UNSUPPORTED to REALPATH, LSTAT, READLINK and
 * SYMLINK, so there is no way to ask it to resolve a path for us and compare.
 * The check has to be lexical on our side too - which is why it is strict
 * rather than clever.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FT_PATH_OK = 0,
    FT_PATH_EMPTY,          /* "" - the server would read it as "the root itself" */
    FT_PATH_ABSOLUTE,       /* leading / or \, or a drive letter: REPLACES the root */
    FT_PATH_PARENT,         /* a `..` component: walks out by concatenation */
    FT_PATH_TOO_LONG,
    FT_PATH_BAD_CHAR,       /* a NUL inside, or a control character */
    FT_PATH_UNC,            /* \\server\share - a drive-letter check misses this */
} ft_path_verdict;

/* The longest remote path we will send. The server imposes no limit of its own;
 * this one exists so a caller cannot hand us an unbounded string. */
#define FT_PATH_MAX 1024

/* Judges `p` (a NUL-terminated remote path, as it would go on the wire).
 * `len` is its length, so a caller that already knows it does not re-scan.
 *
 * Returns FT_PATH_OK only for a RELATIVE path with no `..` component, which the
 * server will resolve inside the interactive user's Downloads folder. Both `/`
 * and `\` count as separators: the server is Windows and accepts either, so a
 * check that only looked at one would be trivially bypassed by using the other -
 * which is exactly the kind of half-check that reads as a guarantee.
 */
static inline ft_path_verdict ft_path_check(const char *p, size_t len)
{
    if (!p || len == 0)      return FT_PATH_EMPTY;
    if (len > FT_PATH_MAX)   return FT_PATH_TOO_LONG;

    /* A NUL or a control byte inside: the wire carries a length, so an embedded
     * NUL would not truncate as a C caller might assume. Refuse rather than
     * reason about it. */
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)p[i];
        if (c == 0 || c < 0x20) return FT_PATH_BAD_CHAR;
    }

    /* UNC first: `\\host\share` also starts with a separator, but naming it
     * separately is what makes a log line useful. */
    if (len >= 2 && (p[0] == '\\' || p[0] == '/')
                 && (p[1] == '\\' || p[1] == '/'))
        return FT_PATH_UNC;

    if (p[0] == '/' || p[0] == '\\') return FT_PATH_ABSOLUTE;

    /* `X:` - the server's own test is a drive letter followed by a colon, so
     * ours is the same shape. `X:foo` (drive-relative) is just as dangerous as
     * `X:\foo` and is refused the same way. */
    if (len >= 2 && p[1] == ':') {
        const char c = p[0];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            return FT_PATH_ABSOLUTE;
    }
    /* A colon anywhere else is an NTFS alternate data stream or a device name;
     * neither has any business in a transfer. */
    for (size_t i = 0; i < len; i++) if (p[i] == ':') return FT_PATH_BAD_CHAR;

    /* `..` as a whole COMPONENT. A filename like `..hidden` or `a..b` is fine;
     * only a component that is exactly `..` walks up. Checked on both
     * separators, and at the start and end of the string. */
    for (size_t i = 0; i < len; ) {
        size_t j = i;
        while (j < len && p[j] != '/' && p[j] != '\\') j++;
        const size_t n = j - i;
        if (n == 2 && p[i] == '.' && p[i + 1] == '.') return FT_PATH_PARENT;
        i = (j < len) ? j + 1 : j;
    }
    return FT_PATH_OK;
}

/* A short, stable reason string for a log line or an error reply. */
static inline const char *ft_path_reason(ft_path_verdict v)
{
    switch (v) {
        case FT_PATH_OK:       return "ok";
        case FT_PATH_EMPTY:    return "empty path";
        case FT_PATH_ABSOLUTE: return "absolute path: it would REPLACE the VM's Downloads root";
        case FT_PATH_PARENT:   return "`..` component: it would walk out of the root";
        case FT_PATH_TOO_LONG: return "path too long";
        case FT_PATH_BAD_CHAR: return "control character, NUL or colon in the path";
        case FT_PATH_UNC:      return "UNC path";
        default:               return "refused";
    }
}

#ifdef __cplusplus
}
#endif
