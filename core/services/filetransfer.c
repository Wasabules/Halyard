/* filetransfer - see filetransfer.h for what this is, who it is for, and why
 * the secret is handled the way it is. */
#include "filetransfer.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "../protocol/ft_path.h"
#include "../protocol/proto.h"
#include "../protocol/ann_reply.h"

/* === The secret extractor, built WITHOUT libssh =============================
 *
 * This half is pure protobuf walking, so it compiles and is testable on every
 * target even when the transfer itself is not built. Kept above the
 * SHADOW_HAVE_FILETRANSFER guard on purpose: a future client needs to be able
 * to tell whether a reply even carries a grant before it decides to link
 * libssh.
 *
 * Nesting, mirrored from ann_reply.c: Message f3 = the response, f8 =
 * ChannelInfo, and inside it the field NUMBER is the channel (8 =
 * FileTransfer), whose f2 is the secret. We walk it by hand rather than reusing
 * `ann_reply_parse`, because that function's whole guarantee is that it never
 * copies this field out - adding an output to it would break
 * `test_ann_reply.c`'s leak check, which is exactly the safeguard we want to
 * keep. */

struct secret_hunt {
    char  *out;
    size_t cap;
    size_t n;
    bool   found;
};

static bool hunt_ft_body(uint32_t field, uint32_t wire,
                         const uint8_t *buf, size_t len, int off,
                         int *next_off, void *user)
{
    struct secret_hunt *h = (struct secret_hunt *)user;
    const uint8_t *sub; size_t sl;
    (void)next_off;
    if (field != 2 || wire != 2 || h->found) return true;
    if (pb_read_lendelim(buf, len, off, &sub, &sl) < 0) return true;
    if (sl == 0 || sl >= h->cap) return true;     /* must fit WITH a NUL */
    memcpy(h->out, sub, sl);
    h->out[sl] = '\0';
    h->n = sl;
    h->found = true;
    return true;
}

static bool hunt_channel_info(uint32_t field, uint32_t wire,
                              const uint8_t *buf, size_t len, int off,
                              int *next_off, void *user)
{
    const uint8_t *sub; size_t sl;
    (void)next_off;
    /* The field number IS the channel; 8 = FileTransfer (ANN_CHAN_FILEXFER). */
    if (field != (uint32_t)ANN_CHAN_FILEXFER || wire != 2) return true;
    if (pb_read_lendelim(buf, len, off, &sub, &sl) < 0) return true;
    pb_iter_fields(sub, sl, hunt_ft_body, user);
    return true;
}

static bool hunt_response(uint32_t field, uint32_t wire,
                          const uint8_t *buf, size_t len, int off,
                          int *next_off, void *user)
{
    const uint8_t *sub; size_t sl;
    (void)next_off;
    if (field != 8 || wire != 2) return true;     /* f3.8 = ChannelInfo */
    if (pb_read_lendelim(buf, len, off, &sub, &sl) < 0) return true;
    pb_iter_fields(sub, sl, hunt_channel_info, user);
    return true;
}

static bool hunt_top(uint32_t field, uint32_t wire,
                     const uint8_t *buf, size_t len, int off,
                     int *next_off, void *user)
{
    const uint8_t *sub; size_t sl;
    (void)next_off;
    if (field != 3 || wire != 2) return true;     /* f3 = the response payload */
    if (pb_read_lendelim(buf, len, off, &sub, &sl) < 0) return true;
    pb_iter_fields(sub, sl, hunt_response, user);
    return true;
}

bool shadow_ft_secret_from_reply(const uint8_t *reply, size_t reply_len,
                                 char *out, size_t cap, size_t *n)
{
    if (n) *n = 0;
    if (!reply || reply_len == 0 || !out || cap == 0) return false;
    out[0] = '\0';
    struct secret_hunt h = { out, cap, 0, false };
    pb_iter_fields(reply, reply_len, hunt_top, &h);
    if (!h.found) { out[0] = '\0'; return false; }
    if (n) *n = h.n;
    return true;
}

const char *shadow_ft_strerror(shadow_ft_err e)
{
    switch (e) {
        case SHADOW_FT_OK:           return "ok";
        case SHADOW_FT_UNSUPPORTED:  return "built without -DSHADOW_FILETRANSFER=ON";
        case SHADOW_FT_CONNECT:      return "TCP or SSH transport failed";
        case SHADOW_FT_AUTH:         return "the VM refused the session secret";
        case SHADOW_FT_SUBSYSTEM:    return "SSH up, sftp subsystem unavailable";
        case SHADOW_FT_REFUSED_PATH: return "remote path refused by ft_path";
        case SHADOW_FT_REMOTE:       return "the VM answered an SFTP error";
        case SHADOW_FT_LOCAL_IO:     return "local file could not be read or written";
        case SHADOW_FT_ARG:          return "bad argument";
        case SHADOW_FT_CANCELLED:    return "cancelled by the caller";
        default:                     return "unknown";
    }
}

#if defined(SHADOW_HAVE_FILETRANSFER) && SHADOW_HAVE_FILETRANSFER

#include <fcntl.h>      /* O_RDONLY & co: sftp_open takes POSIX open flags */
#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include "../common/log.h"
#define ftlog(...) JOURNAL_INFO_(JOURNAL_CAT_NETWORK, __VA_ARGS__)

/* The server caps a READ reply at 64 KiB (its dispatch loop @0x140c890f0), so
 * asking for more only costs a round trip. */
#define FT_CHUNK (64u * 1024u)

struct shadow_ft {
    ssh_session   ssh;
    sftp_session  sftp;
    char         *secret;        /* owned; zeroed before free */
    size_t        secret_len;
    bool          allow_absolute;
};

/* Zero that a compiler may not elide. `memset` on a buffer about to be freed is
 * exactly the call optimisers remove; this one cannot be. */
static void secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* Every remote path goes through here. Returns NULL and logs the reason when
 * the path is refused - the REASON, never the path's origin, and never the
 * secret. */
static const char *vet(shadow_ft *ft, const char *p, shadow_ft_err *e)
{
    if (!p) { *e = SHADOW_FT_ARG; return NULL; }
    if (ft->allow_absolute) return p;
    const ft_path_verdict v = ft_path_check(p, strlen(p));
    if (v == FT_PATH_OK) return p;
    ftlog("[FT1] remote path refused: %s", ft_path_reason(v));
    *e = SHADOW_FT_REFUSED_PATH;
    return NULL;
}

shadow_ft_err shadow_ft_open(shadow_ft **out,
                             const char *host, uint16_t port,
                             const char *secret, size_t secret_len)
{
    if (!out || !host || !secret || secret_len == 0) return SHADOW_FT_ARG;
    *out = NULL;

    shadow_ft *ft = (shadow_ft *)calloc(1, sizeof *ft);
    if (!ft) return SHADOW_FT_CONNECT;

    ft->secret = (char *)malloc(secret_len + 1);
    if (!ft->secret) { free(ft); return SHADOW_FT_CONNECT; }
    memcpy(ft->secret, secret, secret_len);
    ft->secret[secret_len] = '\0';
    ft->secret_len = secret_len;

    {   /* The env form exists for a harness that cannot reach the setter. It is
         * read once, here, so the decision is visible in the open log line. */
        const char *e = getenv("SHADOW_FT_ALLOW_ABSOLUTE");
        ft->allow_absolute = (e && atoi(e) == 1);
        if (ft->allow_absolute)
            ftlog("[FT1] absolute remote paths ALLOWED for this session "
                  "(SHADOW_FT_ALLOW_ABSOLUTE=1): the VM does not confine them, "
                  "so this grants the whole filesystem");
    }

    ft->ssh = ssh_new();
    if (!ft->ssh) { shadow_ft_close(ft); return SHADOW_FT_CONNECT; }

    int iport = (int)port;
    ssh_options_set(ft->ssh, SSH_OPTIONS_HOST, host);
    ssh_options_set(ft->ssh, SSH_OPTIONS_PORT, &iport);
    /* INT1: without a timeout a VM that stops answering parks the calling
     * thread in connect or read for ever, and a UI has no way out of it. 20 s
     * is long enough for a loaded VM to answer a handshake and short enough
     * that a user does not conclude the application is dead. */
    { long tmo = 20; ssh_options_set(ft->ssh, SSH_OPTIONS_TIMEOUT, &tmo); }
    /* The server mints a throwaway host key per session and never publishes it:
     * there is nothing to pin, and no known_hosts entry that could be right. We
     * therefore do not verify it AND we never write one - see the header for
     * why that is acceptable here and nowhere else. */
    {
        /* Pointing both known_hosts files at the null device is what actually
         * stops libssh writing or consulting one. There is no portable
         * "disable the check" option across libssh versions, and we do not need
         * one: with no known_hosts, nothing is remembered and nothing is
         * compared, which is the correct behaviour for a host key that is
         * regenerated on every session. */
        const char *devnull =
#if defined(_WIN32)
            "NUL";
#else
            "/dev/null";
#endif
        ssh_options_set(ft->ssh, SSH_OPTIONS_KNOWNHOSTS, devnull);
        ssh_options_set(ft->ssh, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, devnull);
    }
    /* Any username: the server's Authenticate path does not look at it
     * (@0x140c88d40 reads only the password). A fixed, boring one keeps the
     * handshake reproducible. */
    ssh_options_set(ft->ssh, SSH_OPTIONS_USER, "shadow");

    if (ssh_connect(ft->ssh) != SSH_OK) {
        ftlog("[FT1] ssh_connect to :%u failed: %s", (unsigned)port,
              ssh_get_error(ft->ssh));
        shadow_ft_close(ft);
        return SHADOW_FT_CONNECT;
    }

    /* Password, and only password: the server offers method 2 alone and denies
     * every other, with two attempts before it drops us. */
    if (ssh_userauth_password(ft->ssh, NULL, ft->secret) != SSH_AUTH_SUCCESS) {
        /* ssh_get_error() here cannot contain the secret - libssh reports the
         * method and the server's reply, not the credential - but the message
         * is still not worth echoing: "refused" is the whole actionable
         * content, and an error string is exactly where a secret leaks. */
        ftlog("[FT1] the VM refused the session secret (password auth)");
        shadow_ft_close(ft);
        return SHADOW_FT_AUTH;
    }

    ft->sftp = sftp_new(ft->ssh);
    if (!ft->sftp || sftp_init(ft->sftp) != SSH_OK) {
        ftlog("[FT1] sftp subsystem unavailable: %s", ssh_get_error(ft->ssh));
        shadow_ft_close(ft);
        return SHADOW_FT_SUBSYSTEM;
    }

    ftlog("[FT1] SFTP session open on :%u (paths %s)", (unsigned)port,
          ft->allow_absolute ? "UNRESTRICTED" : "confined to the VM's Downloads");
    *out = ft;
    return SHADOW_FT_OK;
}

void shadow_ft_close(shadow_ft *ft)
{
    if (!ft) return;
    if (ft->sftp) sftp_free(ft->sftp);
    if (ft->ssh)  { ssh_disconnect(ft->ssh); ssh_free(ft->ssh); }
    if (ft->secret) {
        secure_zero(ft->secret, ft->secret_len + 1);
        free(ft->secret);
    }
    free(ft);
}

void shadow_ft_allow_absolute(shadow_ft *ft, bool allow)
{
    if (!ft) return;
    if (allow && !ft->allow_absolute)
        ftlog("[FT1] absolute remote paths ALLOWED by the caller: the VM does "
              "not confine them, so this grants the whole filesystem");
    ft->allow_absolute = allow;
}

shadow_ft_err shadow_ft_list(shadow_ft *ft, const char *dir,
                             shadow_ft_entry *out, size_t cap, size_t *n_total)
{
    if (n_total) *n_total = 0;
    if (!ft || !ft->sftp) return SHADOW_FT_ARG;

    /* "" and "." both mean the root; the server resolves a relative name
     * against Downloads, and "." is what its own OpenDir accepts. ft_path
     * accepts "." on its own (one component, and it is not ".."), so the skip
     * below is not papering over a refusal - it just avoids a pointless call on
     * the one path we substitute ourselves. */
    const char *d = (dir && *dir) ? dir : ".";
    if (strcmp(d, ".") != 0) {
        shadow_ft_err e = SHADOW_FT_OK;
        if (!vet(ft, d, &e)) return e;
    }

    sftp_dir h = sftp_opendir(ft->sftp, d);
    if (!h) return SHADOW_FT_REMOTE;

    size_t n = 0;
    sftp_attributes a;
    while ((a = sftp_readdir(ft->sftp, h)) != NULL) {
        const bool dot = a->name && (strcmp(a->name, ".") == 0
                                     || strcmp(a->name, "..") == 0);
        if (!dot) {
            if (out && n < cap) {
                snprintf(out[n].name, sizeof out[n].name, "%s",
                         a->name ? a->name : "");
                out[n].size   = (uint64_t)a->size;
                out[n].is_dir = (a->type == SSH_FILEXFER_TYPE_DIRECTORY);
            }
            n++;
        }
        sftp_attributes_free(a);
    }
    sftp_closedir(h);
    if (n_total) *n_total = n;
    return SHADOW_FT_OK;
}

shadow_ft_err shadow_ft_get(shadow_ft *ft, const char *remote,
                            const char *local_path,
                            shadow_ft_progress cb, void *user)
{
    if (!ft || !ft->sftp || !local_path) return SHADOW_FT_ARG;
    shadow_ft_err e = SHADOW_FT_OK;
    const char *r = vet(ft, remote, &e);
    if (!r) return e;

    uint64_t total = 0;
    { shadow_ft_stat st; if (shadow_ft_stat_remote(ft, r, &st) == SHADOW_FT_OK) total = st.size; }

    sftp_file f = sftp_open(ft->sftp, r, O_RDONLY, 0);
    if (!f) return SHADOW_FT_REMOTE;

    FILE *lf = fopen(local_path, "wb");
    if (!lf) { sftp_close(f); return SHADOW_FT_LOCAL_IO; }

    unsigned char *buf = (unsigned char *)malloc(FT_CHUNK);
    if (!buf) { fclose(lf); sftp_close(f); return SHADOW_FT_LOCAL_IO; }

    uint64_t done = 0;
    shadow_ft_err rc = SHADOW_FT_OK;
    for (;;) {
        const ssize_t got = sftp_read(f, buf, FT_CHUNK);
        if (got < 0)  { rc = SHADOW_FT_REMOTE; break; }
        if (got == 0) break;                         /* EOF */
        if (fwrite(buf, 1, (size_t)got, lf) != (size_t)got) {
            rc = SHADOW_FT_LOCAL_IO; break;
        }
        done += (uint64_t)got;
        if (cb && !cb(done, total, user)) { rc = SHADOW_FT_CANCELLED; break; }
    }
    free(buf);
    fclose(lf);
    sftp_close(f);
    /* A partial download is not a file. Remove it rather than hand a UI
     * something it would list as complete. */
    if (rc != SHADOW_FT_OK) remove(local_path);
    return rc;
}

shadow_ft_err shadow_ft_put(shadow_ft *ft, const char *local_path,
                            const char *remote,
                            shadow_ft_progress cb, void *user)
{
    if (!ft || !ft->sftp || !local_path) return SHADOW_FT_ARG;
    shadow_ft_err e = SHADOW_FT_OK;
    const char *r = vet(ft, remote, &e);
    if (!r) return e;

    FILE *lf = fopen(local_path, "rb");
    if (!lf) return SHADOW_FT_LOCAL_IO;

    uint64_t total = 0;
    if (fseek(lf, 0, SEEK_END) == 0) { long sz = ftell(lf); if (sz > 0) total = (uint64_t)sz; }
    rewind(lf);

    sftp_file f = sftp_open(ft->sftp, r, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!f) { fclose(lf); return SHADOW_FT_REMOTE; }

    unsigned char *buf = (unsigned char *)malloc(FT_CHUNK);
    if (!buf) { sftp_close(f); fclose(lf); return SHADOW_FT_LOCAL_IO; }

    uint64_t done = 0;
    shadow_ft_err rc = SHADOW_FT_OK;
    for (;;) {
        const size_t got = fread(buf, 1, FT_CHUNK, lf);
        if (got == 0) { if (ferror(lf)) rc = SHADOW_FT_LOCAL_IO; break; }
        /* WRITE is uncapped per call server-side, but we keep to the read chunk
         * so progress is reported at the same granularity in both directions. */
        if (sftp_write(f, buf, got) != (ssize_t)got) { rc = SHADOW_FT_REMOTE; break; }
        done += (uint64_t)got;
        if (cb && !cb(done, total, user)) { rc = SHADOW_FT_CANCELLED; break; }
    }
    free(buf);
    sftp_close(f);
    fclose(lf);
    /* Same reasoning in the other direction, and stronger: a half-written file
     * left in someone's Downloads folder is worse than no file. */
    if (rc != SHADOW_FT_OK) (void)sftp_unlink(ft->sftp, r);
    return rc;
}

shadow_ft_err shadow_ft_stat_remote(shadow_ft *ft, const char *remote,
                                    shadow_ft_stat *out)
{
    if (!ft || !ft->sftp) return SHADOW_FT_ARG;
    /* The path is vetted FIRST, before the output pointer is checked. The
     * first version tested `!out` first and so answered SHADOW_FT_ARG to a
     * caller that passed a refused path and no output buffer - which is
     * precisely how the FT2 self-test reported "a `..` path was NOT refused"
     * on a guard that works (its 47 unit checks never doubted it). A guard
     * whose verdict depends on an unrelated argument is a guard you cannot
     * test. */
    shadow_ft_err e = SHADOW_FT_OK;
    const char *r = vet(ft, remote, &e);
    if (!r) return e;
    if (!out) return SHADOW_FT_ARG;
    memset(out, 0, sizeof *out);
    /* stat, never lstat: the server answers status 8 UNSUPPORTED to LSTAT. */
    sftp_attributes a = sftp_stat(ft->sftp, r);
    if (!a) return SHADOW_FT_OK;                 /* absent, not an error */
    out->exists = true;
    out->size   = (uint64_t)a->size;
    out->is_dir = (a->type == SSH_FILEXFER_TYPE_DIRECTORY);
    sftp_attributes_free(a);
    return SHADOW_FT_OK;
}

#define FT_SIMPLE(fn, call)                                   \
    shadow_ft_err fn(shadow_ft *ft, const char *remote) {     \
        if (!ft || !ft->sftp) return SHADOW_FT_ARG;           \
        shadow_ft_err e = SHADOW_FT_OK;                       \
        const char *r = vet(ft, remote, &e);                  \
        if (!r) return e;                                     \
        return (call) == 0 ? SHADOW_FT_OK : SHADOW_FT_REMOTE; \
    }
FT_SIMPLE(shadow_ft_mkdir,  sftp_mkdir(ft->sftp, r, 0755))
FT_SIMPLE(shadow_ft_remove, sftp_unlink(ft->sftp, r))
FT_SIMPLE(shadow_ft_rmdir,  sftp_rmdir(ft->sftp, r))
#undef FT_SIMPLE

shadow_ft_err shadow_ft_rename(shadow_ft *ft, const char *from, const char *to)
{
    if (!ft || !ft->sftp) return SHADOW_FT_ARG;
    shadow_ft_err e = SHADOW_FT_OK;
    const char *a = vet(ft, from, &e); if (!a) return e;
    const char *b = vet(ft, to,   &e); if (!b) return e;
    return sftp_rename(ft->sftp, a, b) == 0 ? SHADOW_FT_OK : SHADOW_FT_REMOTE;
}

shadow_ft_err shadow_ft_selftest(const char *host, uint16_t port,
                                 const char *secret, size_t secret_len)
{
    shadow_ft *ft = NULL;
    const shadow_ft_err e = shadow_ft_open(&ft, host, port, secret, secret_len);
    if (e != SHADOW_FT_OK) {
        ftlog("[FT2] selftest: session NOT opened on :%u - %s",
              (unsigned)port, shadow_ft_strerror(e));
        return e;
    }
    ftlog("[FT2] selftest: SSH + sftp subsystem UP on :%u, password auth accepted "
          "- the announcement-reply field IS the SSH password", (unsigned)port);

    /* 1. list the root. Reports the count and the first few names only: a
     * listing of someone's Downloads folder is their business, and the count is
     * what tells us the verb works. */
    {
        shadow_ft_entry ents[16];
        size_t n = 0;
        const shadow_ft_err le = shadow_ft_list(ft, ".", ents, 16, &n);
        if (le == SHADOW_FT_OK) {
            size_t dirs = 0, files = 0;
            const size_t shown = n < 16 ? n : 16;
            for (size_t i = 0; i < shown; i++) {
                if (ents[i].is_dir) dirs++; else files++;
            }
            ftlog("[FT2] LIST root: %u entries (%u dir / %u file in the first %u) - OK",
                  (unsigned)n, (unsigned)dirs, (unsigned)files, (unsigned)shown);
        } else {
            ftlog("[FT2] LIST root: %s", shadow_ft_strerror(le));
        }
    }

    /* 2. a put/get round trip under a name nobody could mistake for theirs. */
    static const char NAME[] = "halyard-selftest-DELETE-ME.txt";
    static const char BODY[] = "halyard FT2 selftest 2026-10-02\n";
    const size_t BODY_N = sizeof BODY - 1;
    char local[512];
    snprintf(local, sizeof local, "./halyard-data/%s", NAME);

    bool wrote_local = false;
    { FILE *f = fopen(local, "wb");
      if (f) { wrote_local = (fwrite(BODY, 1, BODY_N, f) == BODY_N); fclose(f); } }

    if (!wrote_local) {
        ftlog("[FT2] PUT skipped: could not stage the local file");
    } else {
        const shadow_ft_err pe = shadow_ft_put(ft, local, NAME, NULL, NULL);
        ftlog("[FT2] PUT %u bytes -> VM: %s", (unsigned)BODY_N, shadow_ft_strerror(pe));
        if (pe == SHADOW_FT_OK) {
            shadow_ft_stat st;
            if (shadow_ft_stat_remote(ft, NAME, &st) == SHADOW_FT_OK)
                ftlog("[FT2] STAT on the VM: exists=%d size=%u (expected %u)",
                      (int)st.exists, (unsigned)st.size, (unsigned)BODY_N);

            char back[512];
            snprintf(back, sizeof back, "%s.back", local);
            const shadow_ft_err ge = shadow_ft_get(ft, NAME, back, NULL, NULL);
            ftlog("[FT2] GET back from the VM: %s", shadow_ft_strerror(ge));
            if (ge == SHADOW_FT_OK) {
                char buf[256]; size_t got = 0;
                FILE *f = fopen(back, "rb");
                if (f) { got = fread(buf, 1, sizeof buf, f); fclose(f); }
                const bool same = (got == BODY_N) && memcmp(buf, BODY, BODY_N) == 0;
                ftlog("[FT2] ROUND TRIP: %s (%u bytes back)",
                      same ? "BYTE-IDENTICAL" : "MISMATCH", (unsigned)got);
            }
            remove(back);
            const shadow_ft_err re = shadow_ft_remove(ft, NAME);
            ftlog("[FT2] REMOVE on the VM: %s", shadow_ft_strerror(re));
            shadow_ft_stat st2;
            if (shadow_ft_stat_remote(ft, NAME, &st2) == SHADOW_FT_OK)
                ftlog("[FT2] cleanup verified: still present=%d (0 = gone)",
                      (int)st2.exists);
        }
        remove(local);
    }

    /* 3. the containment check, against the real server. We ask for a path our
     * own guard refuses, to prove the guard is what stops it and not luck - and
     * we do NOT then retry with the guard off: knowing the server would allow
     * it is already established by reading it, and actually reaching outside
     * someone's Downloads folder is not something a self-test should do. */
    {
        shadow_ft_stat ignored;
        const shadow_ft_err ae = shadow_ft_stat_remote(ft, "../x", &ignored);
        ftlog("[FT2] guard: a `..` path was %s",
              ae == SHADOW_FT_REFUSED_PATH ? "REFUSED by ft_path, as designed"
                                           : "NOT refused - investigate ft_path");
    }

    shadow_ft_close(ft);     /* zeroes the secret */
    ftlog("[FT2] selftest done, session closed and secret zeroed");
    return SHADOW_FT_OK;
}

#else  /* !SHADOW_HAVE_FILETRANSFER — the console and default desktop builds */

shadow_ft_err shadow_ft_open(shadow_ft **out, const char *host, uint16_t port,
                             const char *secret, size_t secret_len)
{
    (void)host; (void)port; (void)secret; (void)secret_len;
    if (out) *out = NULL;
    return SHADOW_FT_UNSUPPORTED;
}

void shadow_ft_close(shadow_ft *ft) { (void)ft; }
void shadow_ft_allow_absolute(shadow_ft *ft, bool allow) { (void)ft; (void)allow; }

shadow_ft_err shadow_ft_list(shadow_ft *ft, const char *dir,
                             shadow_ft_entry *out, size_t cap, size_t *n_total)
{ (void)ft; (void)dir; (void)out; (void)cap; if (n_total) *n_total = 0;
  return SHADOW_FT_UNSUPPORTED; }

shadow_ft_err shadow_ft_get(shadow_ft *ft, const char *remote, const char *local_path,
                            shadow_ft_progress cb, void *user)
{ (void)ft; (void)remote; (void)local_path; (void)cb; (void)user;
  return SHADOW_FT_UNSUPPORTED; }

shadow_ft_err shadow_ft_put(shadow_ft *ft, const char *local_path, const char *remote,
                            shadow_ft_progress cb, void *user)
{ (void)ft; (void)local_path; (void)remote; (void)cb; (void)user;
  return SHADOW_FT_UNSUPPORTED; }

shadow_ft_err shadow_ft_mkdir(shadow_ft *ft, const char *r)  { (void)ft; (void)r; return SHADOW_FT_UNSUPPORTED; }
shadow_ft_err shadow_ft_remove(shadow_ft *ft, const char *r) { (void)ft; (void)r; return SHADOW_FT_UNSUPPORTED; }
shadow_ft_err shadow_ft_rmdir(shadow_ft *ft, const char *r)  { (void)ft; (void)r; return SHADOW_FT_UNSUPPORTED; }
shadow_ft_err shadow_ft_rename(shadow_ft *ft, const char *a, const char *b)
{ (void)ft; (void)a; (void)b; return SHADOW_FT_UNSUPPORTED; }

shadow_ft_err shadow_ft_stat_remote(shadow_ft *ft, const char *remote, shadow_ft_stat *out)
{ (void)ft; (void)remote; if (out) memset(out, 0, sizeof *out);
  return SHADOW_FT_UNSUPPORTED; }

shadow_ft_err shadow_ft_selftest(const char *host, uint16_t port,
                                 const char *secret, size_t secret_len)
{ (void)host; (void)port; (void)secret; (void)secret_len;
  return SHADOW_FT_UNSUPPORTED; }

#endif
