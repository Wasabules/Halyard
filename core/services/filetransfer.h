/* filetransfer - the SFTP channel on `:base+15`, CORE side.
 *
 * === FT1 2026-10-02 — WHAT THIS IS AND WHO IT IS FOR ========================
 *
 * The VM runs a libssh SSH/SFTP **server** on `:base+15`, outside the control
 * TLS. We are the SSH *client*: VM -> PC is an SFTP read, PC -> VM an SFTP
 * write. The control channel only negotiates it. Decoded from ShadowStreamer
 * 6.3.1 — KB §3.50, with addresses in
 * halyard-lab/notes/findings/filetransfer.md.
 *
 * Deliberately **not wired into `clients/`**. This exists so a future desktop
 * client (Qt, Tauri, whatever) has the protocol ready; the homebrew UI on
 * Switch and PS Vita does not expose file transfer, and nothing here is reached
 * from it. The module compiles to nothing unless the build asks for it:
 * `cmake -DSHADOW_FILETRANSFER=ON`, which also links libssh. Without that flag
 * every entry point below returns SHADOW_FT_UNSUPPORTED, so
 * `file(GLOB_RECURSE core/[*].c)` picking the .c up on a console build costs one
 * empty translation unit and no dependency.
 *
 * === THE SECRET IS A FULL-FILESYSTEM CREDENTIAL =============================
 *
 * Authentication is a PASSWORD and nothing else (`Connection::Authenticate`
 * @0x140c88d40 offers libssh's PASSWORD method, denies the rest, two attempts,
 * constant-time compare). The password is the string the server puts in the
 * FileTransfer announcement reply — historically mis-described as "an ed25519
 * private key", functionally a bearer secret.
 *
 * And the server does NOT confine SFTP paths (the join @0x140b8b710 is purely
 * lexical: an absolute client path REPLACES the Downloads root, and `..` is
 * concatenated verbatim). So that one string grants read AND write to the
 * **whole** VM filesystem. Everything in this module's handling of it follows
 * from that:
 *   - it is copied once into a heap buffer owned by the session and zeroed
 *     before the buffer is freed;
 *   - it never appears in a log line, in an error string, or in a devlink dump;
 *   - it is session-scoped and never persisted — `ann_reply.c` still does not
 *     copy it out of the reply at all, which is why no Halyard log has carried
 *     it since SEC1. A caller that wants this module has to extract it
 *     deliberately (see shadow_ft_secret_from_reply) and is responsible for
 *     keeping it out of its own logs.
 *
 * Every remote path is checked by `ft_path.h` first and a path that would leave
 * the Downloads root is REFUSED by default. That is a seatbelt, not a boundary:
 * the credential already grants everything. Its value is that the dangerous
 * case must be asked for by name.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shadow_ft shadow_ft;

typedef enum {
    SHADOW_FT_OK = 0,
    SHADOW_FT_UNSUPPORTED,   /* built without -DSHADOW_FILETRANSFER=ON */
    SHADOW_FT_CONNECT,       /* TCP or SSH transport failed */
    SHADOW_FT_AUTH,          /* the server refused the secret */
    SHADOW_FT_SUBSYSTEM,     /* SSH is up, the sftp subsystem is not */
    SHADOW_FT_REFUSED_PATH,  /* ft_path.h said no - see ft_path_reason() */
    SHADOW_FT_REMOTE,        /* the server answered an SFTP error */
    SHADOW_FT_LOCAL_IO,      /* our own file could not be read or written */
    SHADOW_FT_ARG,
    /* INT1 2026-10-02: a transfer the progress callback stopped. It used to
     * return SHADOW_FT_OK, so a cancelled download was indistinguishable from
     * a complete one - and the partial local file was left in place. A UI
     * cannot build a correct "cancel" on that. */
    SHADOW_FT_CANCELLED,
} shadow_ft_err;

/* A stable, allocation-free description. NEVER contains the secret. */
const char *shadow_ft_strerror(shadow_ft_err e);

/* === Opening ===============================================================
 *
 * `secret`/`secret_len` is the announcement-reply field, verbatim. It is copied
 * and zeroed on close; the caller should zero its own copy as soon as this
 * returns.
 *
 * The SSH host key is NOT verified, and cannot be: the server mints a throwaway
 * ed25519 host key per session (`Server::Init` @0x140c729b0) and never
 * publishes it, so there is nothing to pin and no known_hosts entry that could
 * be right. What makes that acceptable HERE and nowhere else: the secret we are
 * about to present was itself delivered inside the authenticated control TLS
 * session, so a man in the middle on `:base+15` would have to already be inside
 * that session. This module never writes a known_hosts file.
 */
shadow_ft_err shadow_ft_open(shadow_ft **out,
                             const char *host, uint16_t port,
                             const char *secret, size_t secret_len);

/* Zeroes the secret, closes the SFTP channel and the socket, frees everything.
 * Safe on NULL. */
void shadow_ft_close(shadow_ft *ft);

/* === Listing =============================================================== */

typedef struct {
    char     name[512];      /* the entry's own name, not a path */
    uint64_t size;
    bool     is_dir;
} shadow_ft_entry;

/* Lists `dir` (a remote path, "" or "." for the Downloads root). Writes at most
 * `cap` entries and reports how many exist in `*n_total`, so a caller can tell
 * a truncated listing from a complete one. */
shadow_ft_err shadow_ft_list(shadow_ft *ft, const char *dir,
                             shadow_ft_entry *out, size_t cap, size_t *n_total);

/* === Transfers =============================================================
 *
 * `cb` may be NULL. It is called with the bytes moved so far and the total when
 * known (0 when it is not); returning false aborts the transfer, which is how a
 * UI cancels. Reads are issued in 64 KiB chunks because that is the server's
 * cap per reply (its dispatch loop @0x140c890f0); asking for more just costs a
 * round trip.
 */
typedef bool (*shadow_ft_progress)(uint64_t done, uint64_t total, void *user);

/* === INT1 2026-10-02 — the contract a UI needs spelled out =================
 *
 * THREADING. A `shadow_ft` is NOT thread-safe and owns a libssh session: use it
 * from one thread at a time. A UI should keep it on a worker thread and treat
 * the handle as owned by that thread; two threads calling into the same handle
 * will corrupt the SSH stream. Separate handles on separate threads are fine -
 * the server accepts many connections, one thread each.
 *
 * CANCELLING. Returning false from the progress callback stops the transfer and
 * the call returns SHADOW_FT_CANCELLED, not OK. On a cancelled or failed `get`
 * the partial LOCAL file is removed, so a UI never shows a truncated download
 * as a file. On a cancelled or failed `put` the partial REMOTE file is removed
 * too, for the same reason - and because a half-written file in someone's
 * Downloads folder is worse than none.
 *
 * TIMEOUTS. The session carries an SSH-level timeout (see shadow_ft_open), so a
 * VM that stops answering fails the call instead of blocking the thread for
 * ever. Without it a UI thread parked in sftp_read has no way out.
 */

/* VM -> PC. */
shadow_ft_err shadow_ft_get(shadow_ft *ft, const char *remote,
                            const char *local_path,
                            shadow_ft_progress cb, void *user);

/* PC -> VM. */
shadow_ft_err shadow_ft_put(shadow_ft *ft, const char *local_path,
                            const char *remote,
                            shadow_ft_progress cb, void *user);

/* === The rest of the verb set the server implements ========================
 *
 * The server serves OPEN, CLOSE, READ, WRITE, FSTAT, SETSTAT, OPENDIR,
 * READDIR, REMOVE, MKDIR, RMDIR, STAT and RENAME, and answers status 8
 * UNSUPPORTED to everything else - notably REALPATH, LSTAT, READLINK and
 * SYMLINK. So there is deliberately no shadow_ft_realpath(): the server cannot
 * resolve a path for us, which is also why ft_path.h has to be lexical.
 */
shadow_ft_err shadow_ft_mkdir(shadow_ft *ft, const char *remote);
shadow_ft_err shadow_ft_remove(shadow_ft *ft, const char *remote);
shadow_ft_err shadow_ft_rmdir(shadow_ft *ft, const char *remote);
shadow_ft_err shadow_ft_rename(shadow_ft *ft, const char *from, const char *to);

typedef struct {
    uint64_t size;
    bool     is_dir;
    bool     exists;
} shadow_ft_stat;

shadow_ft_err shadow_ft_stat_remote(shadow_ft *ft, const char *remote,
                                    shadow_ft_stat *out);

/* === The escape hatch ======================================================
 *
 * Allows absolute paths and `..` for this session, i.e. the full filesystem the
 * credential already grants. Off by default and never flipped from inside this
 * module. A caller that turns it on is stating that it means to, and should say
 * so in its own log - this module logs the fact once when it is enabled.
 * `SHADOW_FT_ALLOW_ABSOLUTE=1` in the environment does the same, for a test
 * harness that cannot reach the setter.
 */
void shadow_ft_allow_absolute(shadow_ft *ft, bool allow);

/* === Getting the secret out of the announcement reply ======================
 *
 * `ann_reply.c` deliberately does not read the FileTransfer field: not copying
 * it is what guarantees it is never logged, and `test_ann_reply.c` fails if any
 * field of its output structure ever carries it. This function is the ONE
 * deliberate way to obtain it, kept here rather than in the parser so that the
 * dangerous act is a separate, searchable call.
 *
 * Writes at most `cap` bytes into `out` and the length into `*n`. Returns false
 * when the reply is not a FileTransfer grant or the field is absent. The caller
 * must zero `out` when done.
 */
bool shadow_ft_secret_from_reply(const uint8_t *reply, size_t reply_len,
                                 char *out, size_t cap, size_t *n);

#ifdef __cplusplus
}
#endif

/* === The self-test (FT2 2026-10-02) =======================================
 *
 * Opens a real session against the VM and reports what the server actually
 * allows, because everything else about this module is read off a binary and
 * compiled — not exercised. It is the only way to settle the one inference the
 * RE could not: that the announcement-reply field really is the SSH password
 * the accept path compares against.
 *
 * Non-destructive by construction: it lists the root, writes ONE obviously
 * named temporary file, reads it back, compares the bytes, then removes it and
 * confirms the removal. It touches nothing else and creates no directory.
 *
 * `SHADOW_FT_SELFTEST=1` runs it once per session from the announcement path in
 * `ctrl_session.c`, which is where the reply (and therefore the secret) is in
 * hand. The secret stays in memory for the length of this call and is zeroed
 * by `shadow_ft_close`: it is never written to disk, never logged, never
 * persisted. Results only go to the journal, prefixed `[FT2]`.
 *
 * Returns SHADOW_FT_OK when the session opened, whatever the individual
 * operations did — the point is to REPORT capability, not to pass or fail.
 */
shadow_ft_err shadow_ft_selftest(const char *host, uint16_t port,
                                 const char *secret, size_t secret_len);
