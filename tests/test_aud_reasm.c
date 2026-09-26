/* test_aud_reasm.c - reassembly of split audio frames (AUD12), and the rule
 * that its state is SESSION state (ING-A2).
 *
 * The wire is modelled on plaintext: every frame is sent twice, one frame in six
 * is too large for a datagram and is cut into 1241 + 18 bytes, the frame counter
 * (byte 1) is the sequence number modulo 256, and the server renumbers from 1
 * at every session (KB 3.31).
 *
 * Campaign ING-A2 (2026-09-11): after a reconnect in the same process, ONE lost
 * datagram muted the whole new FLAC session. The counter-case is below. It is
 * shown on the anti-duplicate window as it stood when ING-A2 was measured
 * (`no_resync`, what SHADOW_AUDIO_DEDUP_RESYNC=0 gives); with the default window
 * (ING-A1) the same stale slot costs four frames and still plays one foreign
 * frame - pinned as well, because only the reset keeps that one out.
 */
#include "../core/protocol/audio_dedup.h"
#include "../core/protocol/aud_reasm.h"
#include <stdio.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                              \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* 4 x 8 KiB: never on a thread stack - the reason it is not in session_ctx_t. */
static aud_reasm_state_t st, snapshot;

#define CHUNK0 1241
#define CHUNK1 18

typedef struct {
    audio_dedup_t dedup;      /* session_ctx_t.aud_dedup, zeroed per session */
    unsigned played;          /* frames the window let through */
    unsigned merged;          /* completions whose chunk 0 was another frame */
    unsigned merged_played;   /* ... and that the window let through */
    uint32_t sending;         /* the frame being sent (test bookkeeping only) */
} sess_t;

static uint8_t frame[CHUNK0 + CHUNK1];

static int is_split(uint32_t seq) { return seq % 6 == 0; }

static void make_frame(uint32_t seq)
{
    frame[0] = 0x12;
    frame[1] = (uint8_t)seq;         frame[2] = (uint8_t)(seq >> 8);
    frame[3] = (uint8_t)(seq >> 16); frame[4] = (uint8_t)(seq >> 24);
    for (int i = 5; i < (int)sizeof frame; i++) frame[i] = (uint8_t)(seq * 31u + (uint32_t)i);
}

/* aud_livrer(): the same anti-duplicate window as the single-packet path. */
static void deliver(sess_t *s, const uint8_t *p, int len, int merged)
{
    if (len < 5 || p[0] != 0x12) return;
    const int ok = audio_dedup_accepte(&s->dedup, audio_dedup_seq(p));
    if (ok) s->played++;
    if (merged && ok) s->merged_played++;
}

static void chunk(sess_t *s, uint8_t sub, uint16_t idx, const uint8_t *c, int n)
{
    const uint8_t *out = NULL;
    const int len = aud_reasm_chunk(&st, sub, idx, 1, c, n, &out);
    if (len <= 0) return;
    const int merged = audio_dedup_seq(out) != s->sending;
    if (merged) s->merged++;
    deliver(s, out, len, merged);
}

/* Datagrams, as bits: 1 = A0, 2 = A1, 4 = B0, 8 = B1 (a single-packet frame
 * only has A0 and B0). `lose` drops them; `interleaved` sends A0 B0 A1 B1
 * instead of A0 A1 B0 B1. The sequential order is the one a live FLAC session
 * showed on 2026-09-11: `decoupees: ok=952` for `multi=1904`, two completions
 * per split frame - so the higher reconnect rates of ING-A2 are the live ones. */
enum { A0 = 1, A1 = 2, B0 = 4, B1 = 8 };

static void send_frame(sess_t *s, uint32_t seq, int lose, int interleaved)
{
    const uint8_t sub = (uint8_t)seq;
    make_frame(seq);
    s->sending = seq;
    if (!is_split(seq)) {
        if (!(lose & A0)) deliver(s, frame, 200, 0);
        if (!(lose & B0)) deliver(s, frame, 200, 0);
        return;
    }
    static const int seq_order[4] = { A0, A1, B0, B1 };
    static const int int_order[4] = { A0, B0, A1, B1 };
    const int *order = interleaved ? int_order : seq_order;
    for (int k = 0; k < 4; k++) {
        if (lose & order[k]) continue;
        const int second = (order[k] == A1 || order[k] == B1);
        chunk(s, sub, (uint16_t)second, second ? frame + CHUNK0 : frame,
              second ? CHUNK1 : CHUNK0);
    }
}

static void new_session(sess_t *s)
{
    const sess_t zero = {0};
    *s = zero;
}

/* The window as it stood before ING-A1: it never moves back. */
static void old_window(sess_t *s) { s->dedup.no_resync = true; }

/* The normal stream: two copies, one play; and the completion count that tells
 * the two wire orders apart on a live [AUD2] line (the user-assisted check). */
static void two_copies_one_play(void)
{
    for (int inter = 0; inter <= 1; inter++) {
        sess_t s; new_session(&s);
        aud_reasm_reset(&st);
        for (uint32_t seq = 1; seq <= 600; seq++) send_frame(&s, seq, 0, inter);
        CHECK(s.played == 600, "600 frames sent twice, split or not, play 600 times");
        CHECK(st.ok == (inter ? 100u : 200u),
              inter ? "A0 B0 A1 B1: one completion per split frame (ok = multi/4)"
                    : "A0 A1 B0 B1: two completions per split frame (ok = multi/2)");
        CHECK(s.merged == 0, "no completion ever mixes two frames on a clean wire");
    }
}

/* A copy that loses its last chunk is rescued by the other copy's chunk 0. */
static void second_copy_rescues(void)
{
    sess_t s; new_session(&s);
    aud_reasm_reset(&st);
    for (uint32_t seq = 1; seq <= 60; seq++) send_frame(&s, seq, seq == 30 ? A1 : 0, 0);
    CHECK(s.played == 60, "A1 lost: B0 restarts the slot, B1 completes the frame");
}

/* Within ONE session a stale slot is harmless: the frame it hands on is at
 * least 256 frames old, so the window refuses it as too late. That is why the
 * finding's optional eviction heuristic has nothing to fix inside a session. */
static void stale_within_session_is_too_late(void)
{
    sess_t s; new_session(&s);
    aud_reasm_reset(&st);
    /* 1200 leaves a slot (B1 lost); 1200 + 768 is the next split frame with the
     * same byte 1 (1456 and 1712 are not split), and its A0 is lost. */
    for (uint32_t seq = 1; seq <= 3000; seq++) {
        int lose = 0;
        if (seq == 1200) lose = B1;
        if (seq == 1968) lose = A0;
        send_frame(&s, seq, lose, 0);
    }
    CHECK(s.merged == 1, "the stale slot of frame 1200 is merged by frame 1968");
    CHECK(s.merged_played == 0, "the merged frame is 768 behind: refused as too late");
    CHECK(s.played == 3000, "and no frame is lost: copy B still delivers 1968");
}

/* COUNTER-CASE ING-A2 - ONE LOST DATAGRAM MUTES THE NEXT SESSION. */
static void counter_case_reconnect(void)
{
    /* Session 1: 60000 frames (10 min of FLAC). Its last split frame, 59994,
     * loses the final chunk of its second copy - the slot stays active with
     * byte 1 = 90 and the sequence number 59994. */
    sess_t s1; new_session(&s1);
    aud_reasm_reset(&st);
    for (uint32_t seq = 1; seq <= 60000; seq++) send_frame(&s1, seq, seq == 59994 ? B1 : 0, 0);
    CHECK(s1.played == 60000, "session 1 plays every frame");
    snapshot = st;

    /* Session 2 in the same process, WITH the reset. The server restarts at 1;
     * frame 90 is the first split frame with byte 1 = 90, and its A0 is lost. */
    const unsigned cleared = aud_reasm_reset(&st);
    CHECK(cleared == 1, "the reset finds the one stale slot - the count to log");
    sess_t s2; new_session(&s2);
    for (uint32_t seq = 1; seq <= 30000; seq++) send_frame(&s2, seq, seq == 90 ? A0 : 0, 0);
    CHECK(s2.played == 30000, "with the reset, the lost A0 is covered by copy B: 30000 played");
    CHECK(s2.played >= 29700, "with the reset, session 2 plays at least 99 % of its frames");
    CHECK(s2.merged == 0, "with the reset, nothing from session 1 can be merged");

    /* The demonstration: the same session 2 on the slots session 1 left, which
     * is what the tree did while the reset had no caller - on the window as it
     * then stood. */
    st = snapshot;
    sess_t s3; new_session(&s3); old_window(&s3);
    for (uint32_t seq = 1; seq <= 30000; seq++) send_frame(&s3, seq, seq == 90 ? A0 : 0, 0);
    printf("  counter-case: without the reset, session 2 plays %u of 30000 (%u from session 1)\n",
           s3.played, s3.merged_played);
    CHECK(s3.merged_played == 1, "COUNTER-CASE: A1 of frame 90 completes session 1's frame 59994");
    CHECK(s3.dedup.plus_haut == 59994, "COUNTER-CASE: the window jumps to the old number");
    CHECK(s3.played < 300, "COUNTER-CASE: without the reset, the session is mute from frame 90 on");

    /* The default window (ING-A1), still without the reset: it re-primes after
     * 8 packets beyond it, so frames 90-93 are lost and the session keeps its
     * sound - but session 1's frame 59994, glued to frame 90's tail, is still
     * played once: a decode error, or a click. Only the reset removes it. */
    st = snapshot;
    sess_t s4; new_session(&s4);
    for (uint32_t seq = 1; seq <= 30000; seq++) send_frame(&s4, seq, seq == 90 ? A0 : 0, 0);
    CHECK(s4.merged_played == 1 && s4.dedup.resyncs == 1,
          "ING-A1 without the reset: the foreign frame is still played once, then the window re-primes");
    CHECK(s4.played == 29997,
          "ING-A1 without the reset: frames 90-93 lost - 29996 of 30000, plus the foreign one");
}

/* The same two single losses on the other wire order do nothing: B0 resets the
 * slot before A1. It takes two datagrams at each end - p squared, twice. */
static void interleaved_order(void)
{
    sess_t s1; new_session(&s1);
    aud_reasm_reset(&st);
    for (uint32_t seq = 1; seq <= 60000; seq++) send_frame(&s1, seq, seq == 59994 ? B1 : 0, 1);
    snapshot = st;
    CHECK(aud_reasm_reset(&st) == 0, "A0 B0 A1 B1: losing B1 alone leaves no slot");
    st = snapshot;   /* no reset: the tree's behaviour */
    sess_t s2; new_session(&s2);
    for (uint32_t seq = 1; seq <= 30000; seq++) send_frame(&s2, seq, seq == 90 ? A0 : 0, 1);
    CHECK(s2.played == 30000, "A0 B0 A1 B1, no reset: one lost A0 is harmless");

    /* The two-datagram version does mute - on the window before ING-A1 - and
     * the reset still cures it. */
    new_session(&s1);
    aud_reasm_reset(&st);
    for (uint32_t seq = 1; seq <= 60000; seq++) send_frame(&s1, seq, seq == 59994 ? (A1 | B1) : 0, 1);
    snapshot = st;
    new_session(&s2); old_window(&s2);
    for (uint32_t seq = 1; seq <= 30000; seq++) send_frame(&s2, seq, seq == 90 ? (A0 | B0) : 0, 1);
    CHECK(s2.played < 300, "A0 B0 A1 B1, no reset: A1+B1 then A0+B0 lost mutes session 2");
    st = snapshot;
    CHECK(aud_reasm_reset(&st) == 1, "the reset clears the slot A1+B1 left");
    new_session(&s2);
    for (uint32_t seq = 1; seq <= 30000; seq++) send_frame(&s2, seq, seq == 90 ? (A0 | B0) : 0, 1);
    CHECK(s2.played == 29999, "with the reset only frame 90 itself is lost (both chunk 0 gone)");
}

/* The existing edges, pinned so that a refactor cannot move them. */
static void edges(void)
{
    aud_reasm_reset(&st);
    CHECK(aud_reasm_reset(&st) == 0, "a clean state resets to zero stale slots");

    uint8_t c[16] = { 0x12, 1, 0, 0, 0 };
    const uint8_t *out = NULL;
    /* Five frames that only ever get their chunk 0: the fifth takes slot 0. */
    for (uint8_t sub = 1; sub <= 5; sub++)
        CHECK(aud_reasm_chunk(&st, sub, 0, 1, c, 16, &out) == 0, "a chunk 0 alone completes nothing");
    CHECK(st.slot[0].sub == 5 && st.slot[1].sub == 2 && st.slot[3].sub == 4,
          "all four slots taken: the next frame reuses slot 0");
    CHECK(aud_reasm_chunk(&st, 1, 1, 1, c, 16, &out) == 0, "frame 1 lost its slot: its chunk 1 is dropped");
    CHECK(st.out_of_order == 0, "... without being counted: it found no slot at all");
    CHECK(aud_reasm_chunk(&st, 2, 1, 1, c, 16, &out) == 32 && out == st.slot[1].buf,
          "frame 2 completes from its own slot");

    aud_reasm_reset(&st);
    CHECK(aud_reasm_chunk(&st, 9, 1, 1, c, 16, &out) == 0, "a chunk 1 first opens nothing");
    CHECK(aud_reasm_chunk(&st, 9, 0, 1, c, 16, &out) == 0, "chunk 0");
    CHECK(aud_reasm_chunk(&st, 9, 0, 1, c, 16, &out) == 0, "chunk 0 again restarts the slot");
    CHECK(aud_reasm_chunk(&st, 9, 1, 1, c, 16, &out) == 32, "chunk 1: one frame of 32 bytes, not 48");

    static uint8_t big[AUD_REASM_MAX];
    aud_reasm_reset(&st);
    CHECK(aud_reasm_chunk(&st, 7, 0, 1, big, AUD_REASM_MAX, &out) == 0, "a full-size chunk 0 fits");
    CHECK(aud_reasm_chunk(&st, 7, 1, 1, c, 1, &out) == 0 && st.out_of_order == 1,
          "one byte more is refused and counted, never written");
}

int main(void)
{
    printf("== split audio frames (AUD12) and their session state (ING-A2) ==\n");
    two_copies_one_play();
    second_copy_rescues();
    stale_within_session_is_too_late();
    counter_case_reconnect();
    interleaved_order();
    edges();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
