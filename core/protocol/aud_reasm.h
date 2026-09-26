/* aud_reasm.h - reassembly of the AUDIO frames the server splits over two
 * datagrams of `:base+30` (AUD12), and the rule that its state is SESSION
 * state (ING-A2).
 *
 * PURE module: no dependency, no I/O, no global state. The caller owns the state
 * and passes it by pointer, the way audio_dedup.h does, so the rule is testable
 * offline (tests/test_aud_reasm.c). Extracted from ctrl_session.c on 2026-09-11;
 * the bench found it equal to the code it replaces over 5.0 M chunk steps.
 *
 * The wire (AUD12, verified on the hex dump): a FLAC frame too large for one
 * datagram is cut into chunks, EACH sealed on its own, so it is the plaintexts
 * that get concatenated. Byte 1 of the header is a frame counter modulo 256 that
 * links the chunks of one frame - not a subchannel. `max` is the LAST index, as
 * on the video channel. Every frame is sent twice; the second copy restarts at
 * index 0, which resets the slot, and the anti-duplicate window downstream
 * (audio_dedup.h) drops the replayed frame.
 *
 * THE STATE IS SESSION STATE, AND ITS SIZE DECIDES WHERE IT LIVES. Four slots of
 * 8 KiB do not belong in `session_ctx_t`, a stack local of ctrl_session_run
 * (32 KiB more on a console thread stack). So the state is a file-scope
 * variable, and aud_reasm_reset() MUST run at every session start. File scope by
 * itself makes nothing safe: it lives exactly as long as a function `static`.
 *
 * COUNTER-CASE (ING-A2, 2026-09-11) - ONE LOST DATAGRAM MUTES A WHOLE SESSION.
 * The reset existed (ctrl_session_aud_reasm_reset) but nothing called it. A slot
 * stays active when the last copy of a split frame loses its final chunk: there
 * is no timeout. In the next session of the same process, the first split frame
 * with the same byte 1 whose chunk 0 is lost pairs its chunk 1 with the OLD
 * chunk 0, and the frame handed on carries the previous session's sequence
 * number. The server renumbers from 1 on every stream (KB 3.31), so that number
 * is far ahead: the anti-duplicate window jumps to it and refuses every later
 * frame as too late, until the new session catches up - about as long as the
 * previous session lasted. Offline, after a 10-minute session: 90 frames played
 * out of 30000. With the reset: 30000. ING-A1's resync (audio_dedup.h) now
 * bounds that mute to four frames, but the merged frame still reaches the
 * decoder once - a decode error, or a click of wrong PCM. Only the reset keeps
 * it out.
 *
 * WHY THE WIRE ORDER MATTERS. With the copies sent A0 A1 B0 B1, one lost
 * datagram (B1) leaves a slot and one lost datagram (A0) merges it. Sent
 * A0 B0 A1 B1, B0 resets the slot before A1 arrives: it takes A1+B1 lost to
 * leave a slot and A0+B0 lost to merge it. The `decoupees: ok=` count of the
 * [AUD2] line tells the two apart on a live FLAC session: ok= is half of the
 * split datagrams (`rejets: multi=`) in the first order, a quarter in the second.
 */
#pragma once

#include <stdint.h>
#include <string.h>

#define AUD_REASM_MAX   8192   /* a split FLAC frame is ~1259 B of plaintext */
#define AUD_REASM_SLOTS 4      /* 2 in flight in practice; margin, not weight */

typedef struct {
    uint8_t  sub;       /* byte 1 = frame counter mod 256, NOT a subchannel */
    int      active;
    uint16_t max;       /* index of the LAST chunk */
    uint16_t expected;  /* next index accepted */
    int      len;
    uint8_t  buf[AUD_REASM_MAX];
} aud_reasm_t;

typedef struct {
    aud_reasm_t slot[AUD_REASM_SLOTS];
    uint32_t    ok;            /* frames completed: "decoupees: ok=" */
    uint32_t    out_of_order;  /* chunks refused: "decoupees: desordre=" */
} aud_reasm_state_t;

/* Session start. Returns how many slots were still holding a partial frame -
 * the number worth logging: each one could have muted the new session. */
static inline unsigned aud_reasm_reset(aud_reasm_state_t *st)
{
    unsigned stale = 0;
    for (unsigned i = 0; i < AUD_REASM_SLOTS; i++) {
        if (st->slot[i].active) stale++;
        st->slot[i].active = 0;
    }
    st->ok = 0;
    st->out_of_order = 0;
    return stale;
}

/* The slot collecting frame `sub`. `create` (true for a chunk 0) may claim a
 * free slot, or slot 0 when all are taken: a frame abandoned midway must not
 * block the following ones for the rest of the session. */
static inline aud_reasm_t *aud_reasm_slot(aud_reasm_state_t *st, uint8_t sub,
                                          int create)
{
    for (unsigned i = 0; i < AUD_REASM_SLOTS; i++)
        if (st->slot[i].active && st->slot[i].sub == sub)
            return &st->slot[i];
    if (!create) return NULL;
    for (unsigned i = 0; i < AUD_REASM_SLOTS; i++)
        if (!st->slot[i].active) {
            st->slot[i].active = 1;
            st->slot[i].sub = sub;
            return &st->slot[i];
        }
    st->slot[0].sub = sub;
    st->slot[0].active = 1;
    return &st->slot[0];
}

/* One DECRYPTED chunk (`plain`, `len` bytes; the caller has already bounded
 * `len` to 1..AUD_REASM_MAX). Returns the length of the frame this chunk
 * completes and points `*frame` at it, or 0. The frame stays valid until the
 * next call: hand it on before feeding another chunk. */
static inline int aud_reasm_chunk(aud_reasm_state_t *st, uint8_t sub,
                                  uint16_t idx, uint16_t max_chunks,
                                  const uint8_t *plain, int len,
                                  const uint8_t **frame)
{
    aud_reasm_t *r = aud_reasm_slot(st, sub, idx == 0);
    if (!r) return 0;
    if (idx == 0) { r->len = 0; r->expected = 0; r->max = max_chunks; }
    if (idx == r->expected && r->len + len <= AUD_REASM_MAX) {
        memcpy(r->buf + r->len, plain, (size_t)len);
        r->len += len;
        r->expected++;
        if (idx == r->max) {
            st->ok++;
            r->active = 0;
            *frame = r->buf;
            return r->len;
        }
    } else {
        st->out_of_order++;
    }
    return 0;
}
