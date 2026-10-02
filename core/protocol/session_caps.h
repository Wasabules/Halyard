/* session_caps - what the server granted this session, as a public snapshot.
 *
 * === INT1 2026-10-02 — THE INTEGRATION BLOCKER THIS REMOVES =================
 *
 * The server answers our eight channel announcements with a grant per channel:
 * the transport it chose, the port offset, and a session handle. Until now all
 * of that was parsed into `session_ctx_t` — a PRIVATE struct in
 * `ctrl_session_int.h` — and never exposed. So a client outside this file could
 * not learn:
 *   - which channels exist at all on this session,
 *   - which port to dial for one,
 *   - whether the transport it assumed was actually granted,
 *   - or, for file transfer, the credential without which the channel is shut.
 * Every one of those had to be re-derived or hardcoded, which is how the
 * self-test came to knock on port 7015 while the channel listened on 14015.
 *
 * A future desktop client (Qt, Tauri) needs exactly this snapshot and nothing
 * else to drive the side channels: the clipboard codec, the SFTP client and the
 * microphone all take (host, port, secret) and no session internals.
 *
 * CONTRACT
 *   - Filled during the bootstrap, from the server's own replies. A channel
 *     with `granted == false` was not granted: do not dial it.
 *   - `port` is the ABSOLUTE port, already `port_base + offset`. The offset the
 *     reply carries is relative to a base of 7000 that is NOT the live base -
 *     that trap is resolved here, once, so no caller repeats it.
 *   - Read it after `ctrl_session_caps()` returns true. It is a snapshot by
 *     value: no pointers into session state, nothing to free, safe to copy and
 *     to keep after the session ends (the ports and handles then describe a
 *     session that is over, which is why `generation` is there).
 *
 * THE SECRET IS NOT IN HERE, DELIBERATELY. The file-transfer grant carries an
 * SSH password that grants read AND write to the VM's whole filesystem (the
 * server does not confine SFTP paths - KB §3.50). Putting it in a struct that
 * callers copy around, log and serialise is how it would end up in a crash
 * dump. `ctrl_session_file_transfer_secret()` hands it over on demand, into a
 * buffer the caller owns and must zero.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctrl_msgs_chan.h"   /* SHADOW_CHAN_IDX_* and the two channel orders */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     granted;      /* false = the server did not grant it; do not dial */
    bool     tcp;          /* true = TCP, false = UDP, as GRANTED (not asked) */
    uint16_t port;         /* absolute: port_base + the reply's offset */
    uint64_t handle;       /* the granted stream handle, for an unregister */
} shadow_chan_caps;

typedef struct {
    /* Indexed by SHADOW_CHAN_IDX_* — the body order, not the server's channel
     * numbers. `shadow_chan_idx_from_ann()` converts; mixing the two is the
     * mistake that would re-announce the wrong channel. */
    shadow_chan_caps chan[8];

    unsigned n_granted;    /* how many of the eight came back granted */
    int      port_base;    /* the base every `port` above was computed from */

    /* Bumped on every session. A caller that cached a snapshot can tell whether
     * it still describes the live session. */
    uint32_t generation;

    /* The server's own build, from the Capabilities reply (SRV8). 0 when the
     * reply could not be read. Every byte-exact decision in this client is
     * dated against ONE build, so a client that behaves differently per version
     * has the number here rather than having to ask. */
    unsigned srv_major, srv_minor, srv_patch;

    /* Video, as granted — the one channel whose parameters the server restates
     * and may not honour as asked. 0 when the grant carried no mode. */
    uint32_t video_width, video_height;
    float    video_fps;
    uint32_t video_bitrate_bps;
    uint32_t video_codec;      /* 0 H.264, 1 H.265, 2 AV1 (wire values) */

    /* Audio, as granted. `audio_codec` is the WIRE value: 1 Opus, 2 FLAC. The
     * server picks it; asking is not getting (KB §3.37). */
    uint32_t audio_sample_rate, audio_bits, audio_codec;
} shadow_session_caps;

/* Copies the live session's snapshot into `out`. Returns false when no session
 * has completed its bootstrap yet, in which case `out` is zeroed.
 *
 * Thread-safe for readers: the snapshot is published once, at the end of the
 * bootstrap, and read under no lock because it is only ever written before any
 * caller can observe a session as active. */
bool ctrl_session_caps(shadow_session_caps *out);

/* The file-transfer SSH password, for a caller that means to open the channel.
 *
 * Writes at most `cap` bytes plus a NUL and the length into `*n`. Returns false
 * when there is no session, when file transfer was not granted, or when the
 * buffer is too small — and in every one of those cases writes an empty string
 * rather than a truncated credential.
 *
 * THE CALLER MUST ZERO THE BUFFER when it is done. This is a read/write
 * credential for the VM's entire filesystem: never persist it, never log it,
 * never put it in a crash report, and do not keep it past the session
 * (`generation` in the snapshot says whether it is still current).
 */
bool ctrl_session_file_transfer_secret(char *out, size_t cap, size_t *n);

/* === FT4 2026-10-02 - THE SAME ACCESS, AS ONE CLICKABLE URI ================
 *
 * `sftp://shadow:PASSWORD@host:port/` - what WinSCP, FileZilla or `sftp` need,
 * complete, with the base64 password percent-encoded and an IPv6 host bracketed
 * (`ft_uri.h` says why both of those are not optional).
 *
 * This exists because the four fields of the credential are USELESS to a person
 * until they are assembled: copying a 395-byte base64 password out of a text
 * file by hand is where a one-character mistake turns into "authentication
 * failed" with nothing pointing at the paste.
 *
 * Same contract and same warnings as `ctrl_session_file_transfer_secret()`: the
 * URI CONTAINS the credential. Zero the buffer when done, never log it, never
 * put it in a crash report, and note that handing it to another application
 * makes it that application's problem too - a file manager will offer to SAVE
 * the session, and a saved session is a stored password.
 *
 * Returns false when there is no session, when file transfer was not granted,
 * when the VM address is unknown, or when `cap` is too small - and writes an
 * empty string rather than a truncated URI, which would authenticate as a
 * different password. 2 KiB is comfortably enough. */
bool ctrl_session_file_transfer_uri(char *out, size_t cap, size_t *n);

#ifdef __cplusplus
}
#endif
