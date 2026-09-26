/* test_vid_reasm.c - the REAL video reassembly (vid_reasm.c), fed synthetic
 * pictures, offline. No console, no VM, no network.
 *
 * REASM-1 (2026-09-11). G43 flushes an incomplete picture when the NEXT one
 * opens - but the code ran that trigger only when the next picture was
 * multi-chunk. A single-chunk picture was decrypted, emitted and returned
 * before reaching it. Each case below builds the packet sequence that exposed
 * one consequence, feeds it to on_video_packet(), and checks what reaches the
 * decoder callback, byte for byte.
 *
 * TWO RUNS, and both must pass (tests/run_tests.sh runs this binary twice):
 *   - default: the fix is on, and the expectations are the FIXED behaviour;
 *   - SHADOW_FLUSH_PREV_INCOMPLETE=0: the toggle must restore the previous
 *     behaviour of BOTH opening paths exactly, so this run asserts the defect
 *     itself - out-of-order emission with three GAP lines, a picture in no
 *     counter. That is the counter-case, demonstrated again on every run.
 *   The cases that must not move (the clean stream above all) carry the same
 *   expectations in both runs.
 * TEST_VID_REASM_EXPECT=fixed|legacy overrides which expectations apply. It
 * is for demonstrations only: SHADOW_FLUSH_PREV_INCOMPLETE=0 with
 * TEST_VID_REASM_EXPECT=fixed prints, as FAIL lines, exactly what the fix
 * changes. SHADOW_FLUSH_PREV_MAX_AGE=0 (no age guard) makes the stale-B case
 * fail: that is the age guard's own counter-case.
 *
 * Stubs: the journal (it counts the [G42] GAP and [G43] lines), the latency
 * module (a fake clock), and shadow_cipher_decrypt_unsafe() as the identity -
 * chunk 0 travels in the clear with a dummy nonce and tag, and the AEAD is
 * test_encryption.c's business. The wolfSSL HEADERS are still needed, because
 * ctrl_audio_dtls.h includes them; the library is not linked.
 *
 * "parity" in vid_reasm.c means CONTINUATION (F31): there is no FEC.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/protocol/ctrl_session_int.h"
#include "../core/protocol/vid_reasm.h"
#include "../core/protocol/latency.h"
#include "../core/services/journal.h"

static int total = 0, failed = 0;
static const char *g_case = "";
static int g_fixed = 1;   /* 1: expect the fixed behaviour; 0: the previous one */

static void check(bool cond, const char *tag, const char *what)
{
    total++;
    if (!cond) { failed++; printf("  FAIL  [%s] %s: %s\n", tag, g_case, what); }
}

/* ---- stubs ------------------------------------------------------------------ */
static int g_gap_lines, g_single_lines, g_lost_lines;

void journal_write(journal_severity_t sev, journal_category_t cat, const char *fmt, ...)
{
    char b[4096];
    va_list ap;
    (void)sev; (void)cat;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (strstr(b, "GAP subchan")) g_gap_lines++;
    if (strstr(b, "[G43] previous picture flushed by a single-chunk one")) g_single_lines++;
    if (strstr(b, "[G43] picture never emitted")) g_lost_lines++;
}

static int64_t g_now_us;
int latency_enabled(void) { return 1; }
int64_t latency_now_us(void) { return g_now_us; }
void latency_add(latency_stage_t e, int64_t us) { (void)e; (void)us; }
void latency_video_assembled(uint32_t server_stamp, int64_t t_asm_us)
{
    (void)server_stamp; (void)t_asm_us;
}

bool shadow_cipher_decrypt_unsafe(shadow_cipher *c, uint8_t *buf, int ct_len,
                                  const uint8_t *nonce, const uint8_t *tag)
{
    (void)c; (void)buf; (void)ct_len; (void)nonce; (void)tag;
    return true;   /* identity: chunk 0 is built in the clear */
}

/* ---- the picture model --------------------------------------------------------
 * One picture = one slice = one subchannel (subchannel = picture number & 0xff),
 * like the server's single-slice streams. Datagrams of 1280 bytes: chunk 0 holds
 * C0 plaintext bytes plus nonce and tag, a continuation holds CN. */
#define NPIC_MAX 640
#define C0       1241
#define CN       1269
#define T0       2000000   /* us */
#define T_PIC    22000     /* picture interval, ~45 fps */
#define T_CHUNK  150       /* chunk spacing inside a picture's burst */
#define T_DUP    3000      /* the server resends the last chunk ~3 ms later (V11) */

static uint8_t *g_au[NPIC_MAX];     /* what the decoder must receive, per picture */
static int      g_au_len[NPIC_MAX];
static int      g_nch[NPIC_MAX];    /* chunk count, per picture */

static uint32_t g_rng;
static uint8_t nz_byte(void)   /* xorshift32, never 0: no false start code anywhere */
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (uint8_t)(1u + g_rng % 255u);
}

static void start_code(uint8_t *vf, int *o, uint8_t nal_hdr)
{
    vf[(*o)++] = 0; vf[(*o)++] = 0; vf[(*o)++] = 0; vf[(*o)++] = 1;
    vf[(*o)++] = nal_hdr;
}

/* Picture k's VideoFrame, `vl` bytes: [0x02][ts u32 LE][key][13-byte extension
 * on a key frame] then the Annex-B access unit. Returns where the Annex-B
 * starts, which is what emit_legacy strips up to. */
static int make_vf(uint8_t *vf, int k, int key, int vl)
{
    const uint32_t ts = (uint32_t)k * 90000u;   /* pts_ms = ts / 90 = k * 1000 */
    int o = 0;
    vf[o++] = 0x02;                               /* codec 0 (H.264), version 2 */
    vf[o++] = (uint8_t)ts;         vf[o++] = (uint8_t)(ts >> 8);
    vf[o++] = (uint8_t)(ts >> 16); vf[o++] = (uint8_t)(ts >> 24);
    vf[o++] = key ? 1 : 0;
    if (key) { memset(vf + o, 0x11, 13); o += 13; }
    const int hdr = o;
    g_rng = 2463534242u ^ ((uint32_t)k * 2654435761u);
    if (key) {
        start_code(vf, &o, 0x67);                 /* SPS */
        for (int i = 0; i < 8; i++) vf[o++] = nz_byte();
        start_code(vf, &o, 0x68);                 /* PPS */
        for (int i = 0; i < 4; i++) vf[o++] = nz_byte();
        start_code(vf, &o, 0x65);                 /* IDR slice */
    } else {
        start_code(vf, &o, 0x41);                 /* P slice */
    }
    vf[o++] = 0x9a;   /* top bit set: first_mb_in_slice == 0, the picture's first slice (G40) */
    while (o < vl) vf[o++] = nz_byte();
    return hdr;
}

/* ---- packets ------------------------------------------------------------------ */
#define PK_MAX 4096
typedef struct { uint8_t b[1300]; int n; int64_t t; int ord; int pic; } pkt_t;
static pkt_t g_pk[PK_MAX];
static int   g_order[PK_MAX];
static int   g_npk;

static void push(const uint8_t *b, int n, int64_t t, int pic)
{
    if (g_npk >= PK_MAX) { printf("  test bug: packet store full\n"); exit(2); }
    pkt_t *p = &g_pk[g_npk];
    memcpy(p->b, b, (size_t)n);
    p->n = n; p->t = t; p->ord = g_npk; p->pic = pic;
    g_npk++;
}

/* The 11-byte SUFP v3 header (vid_reasm.c), then the payload. A sealed chunk
 * carries a dummy nonce and tag after it, which the identity stub ignores. */
static int datagram(uint8_t *d, int sub, int idx, int max_field, uint32_t fid,
                    const uint8_t *pl, int len, int sealed)
{
    d[0] = 0x23; d[1] = (uint8_t)sub;
    d[2] = (uint8_t)idx;        d[3] = (uint8_t)(idx >> 8);
    d[4] = (uint8_t)max_field;  d[5] = (uint8_t)(max_field >> 8);
    d[6] = (uint8_t)fid;        d[7] = (uint8_t)(fid >> 8);
    d[8] = (uint8_t)(fid >> 16); d[9] = (uint8_t)(fid >> 24);
    d[10] = sealed ? 1 : 0;
    memcpy(d + 11, pl, (size_t)len);
    int n = 11 + len;
    if (sealed) { memset(d + n, 0xA5, 12); memset(d + n + 12, 0x5A, 16); n += 28; }
    return n;
}

typedef struct { int drop_sof, drop_orig_tail, drop_dup_tail; int64_t dup_delay; } popt_t;
static const popt_t CLEAN = { 0, 0, 0, T_DUP };

/* Picture k in `nch` chunks, the last one carrying `tail` bytes (for nch == 1,
 * `tail` is the whole VideoFrame), first chunk sent at t0. Returns the time of
 * its last ORIGINAL chunk. */
static int64_t add_picture(int k, int key, int nch, int tail, int64_t t0, popt_t o)
{
    static uint8_t vf[C0 + 32 * CN];
    const int vl  = (nch == 1) ? tail : C0 + (nch - 2) * CN + tail;
    const int hdr = make_vf(vf, k, key, vl);
    g_au_len[k] = vl - hdr;
    g_au[k] = (uint8_t *)malloc((size_t)g_au_len[k]);
    if (!g_au[k]) exit(2);
    memcpy(g_au[k], vf + hdr, (size_t)g_au_len[k]);
    g_nch[k] = nch;

    uint8_t d[1300];
    int off = 0, n = 0;
    int64_t t = t0;
    for (int i = 0; i < nch; i++) {
        const int room = (i == 0) ? C0 : CN;
        const int cl = (vl - off) < room ? (vl - off) : room;
        n = datagram(d, k & 0xff, i, nch - 1, (uint32_t)t, vf + off, cl, i == 0);
        off += cl;
        const bool is_tail = (i == nch - 1) && nch > 1;
        if (!((i == 0 && o.drop_sof) || (is_tail && o.drop_orig_tail))) push(d, n, t, k);
        if (i < nch - 1) t += T_CHUNK;
    }
    if (nch > 1 && !o.drop_dup_tail) push(d, n, t + o.dup_delay, k);   /* same bytes */
    return t;
}

static int std_nch(int k)  { return 2 + k % 5; }           /* 2..6 chunks */
static int std_tail(int k) { return 1 + (k * 97) % CN; }   /* 1..CN: short tails too (V11) */

/* ---- the session and the decoder side ------------------------------------------ */
typedef struct { int pic; int len; int exact; int prefix; } emit_t;
static emit_t   g_em[2 * NPIC_MAX];
static int      g_nem;
static uint64_t g_hash;

static void hash_bytes(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) { g_hash ^= b[i]; g_hash *= 1099511628211ull; }
}

static void sink(const uint8_t *nal, size_t len, uint64_t pts_ms, bool key, void *user)
{
    (void)key; (void)user;
    emit_t e = { (int)(pts_ms / 1000), (int)len, 0, 0 };
    if (pts_ms % 1000 == 0 && e.pic >= 0 && e.pic < NPIC_MAX && g_au[e.pic]) {
        e.exact  = e.len == g_au_len[e.pic] && !memcmp(nal, g_au[e.pic], len);
        e.prefix = e.len <  g_au_len[e.pic] && !memcmp(nal, g_au[e.pic], len);
    }
    const uint8_t id[8] = { (uint8_t)e.pic, (uint8_t)(e.pic >> 8), (uint8_t)(e.pic >> 16),
                            (uint8_t)(e.pic >> 24), (uint8_t)e.len, (uint8_t)(e.len >> 8),
                            (uint8_t)(e.len >> 16), (uint8_t)(e.len >> 24) };
    hash_bytes(id, sizeof id);
    hash_bytes(nal, len);
    if (g_nem < 2 * NPIC_MAX) g_em[g_nem++] = e;
}

static ctrl_session_params g_params;
static ctrl_session_stats  g_stats;
static session_ctx_t       g_ctx;

static void session_begin(const char *name)
{
    g_case = name;
    memset(&g_params, 0, sizeof g_params);
    memset(&g_stats, 0, sizeof g_stats);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_params.on_video = sink;
    g_ctx.p = &g_params; g_ctx.stats = &g_stats; g_ctx.magic = SESSION_CTX_MAGIC;
    vid_reasm_reset_session();
    for (int i = 0; i < NPIC_MAX; i++) { free(g_au[i]); g_au[i] = NULL; g_au_len[i] = 0; g_nch[i] = 0; }
    g_npk = 0; g_nem = 0; g_hash = 14695981039346656037ull;
    g_gap_lines = g_single_lines = g_lost_lines = 0;
}

static int cmp_pkt(const void *a, const void *b)
{
    const pkt_t *x = &g_pk[*(const int *)a], *y = &g_pk[*(const int *)b];
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return x->ord - y->ord;
}

typedef void (*hook_t)(const pkt_t *p, int after);
static void feed(hook_t hook)
{
    for (int i = 0; i < g_npk; i++) g_order[i] = i;
    qsort(g_order, (size_t)g_npk, sizeof g_order[0], cmp_pkt);
    for (int i = 0; i < g_npk; i++) {
        const pkt_t *p = &g_pk[g_order[i]];
        if (hook) hook(p, 0);
        g_now_us = p->t;
        on_video_packet(p->b, (size_t)p->n, &g_ctx);
        if (hook) hook(p, 1);
    }
}

static unsigned nacks(void)
{
    return (unsigned)((g_ctx.nack_tail - g_ctx.nack_head + NACK_QUEUE_CAP) % NACK_QUEUE_CAP);
}

static int n_emit(int pic)
{
    int c = 0;
    for (int i = 0; i < g_nem; i++) if (g_em[i].pic == pic) c++;
    return c;
}

static int first_emit(int pic)
{
    for (int i = 0; i < g_nem; i++) if (g_em[i].pic == pic) return i;
    return -1;
}

static bool is_prefix(int pic) { const int i = first_emit(pic); return i >= 0 && g_em[i].prefix; }
static bool is_exact(int pic)  { const int i = first_emit(pic); return i >= 0 && g_em[i].exact; }

static bool all_in_order(void)
{
    for (int i = 1; i < g_nem; i++) if (g_em[i].pic <= g_em[i - 1].pic) return false;
    return true;
}

/* Every picture but `except` arrived whole and byte-exact. */
static bool others_exact(int except)
{
    for (int i = 0; i < g_nem; i++) if (g_em[i].pic != except && !g_em[i].exact) return false;
    return true;
}

/* The emission order restricted to pictures lo..hi, e.g. "38 39 41 40 42". */
static const char *window(int lo, int hi)
{
    static char s[512];
    int o = 0;
    s[0] = 0;
    for (int i = 0; i < g_nem && o < (int)sizeof s - 16; i++)
        if (g_em[i].pic >= lo && g_em[i].pic <= hi)
            o += snprintf(s + o, sizeof s - (size_t)o, "%s%d", o ? " " : "", g_em[i].pic);
    return s;
}

/* Everything emitted from picture `from` onwards, in emission order. */
static const char *tail_from(int from)
{
    static char s[512];
    int o = 0;
    const int i0 = first_emit(from);
    s[0] = 0;
    for (int i = i0 < 0 ? g_nem : i0; i < g_nem && o < (int)sizeof s - 16; i++)
        o += snprintf(s + o, sizeof s - (size_t)o, "%s%d", o ? " " : "", g_em[i].pic);
    return s;
}

/* Is `pic` emitted AFTER picture `after`? The resurrection test. */
static bool emitted_after(int pic, int after)
{
    const int ia = first_emit(after);
    if (ia < 0) return false;
    for (int i = ia + 1; i < g_nem; i++) if (g_em[i].pic == pic) return true;
    return false;
}

static void print_counters(const char *order)
{
    printf("  %-8s %s | GAP=%d lost=%u/%u miss1=%u trunc=%u orph=%u(dup=%u perdu=%u autre=%u) "
           "nack=%u idr=%d emitted=%d\n",
           g_case, order, g_gap_lines, (unsigned)g_stats.chunks_missing,
           (unsigned)g_stats.chunks_expected, (unsigned)g_stats.frames_miss1,
           (unsigned)g_stats.frames_dropped_trunc, (unsigned)g_stats.chunks_orphan,
           (unsigned)g_stats.chunks_orphan_dup, (unsigned)g_stats.chunks_orphan_lost,
           (unsigned)g_stats.chunks_orphan_stale, nacks(), (int)g_idr_needed, g_nem);
}

/* ---- v1 / v2 / v3: the finding's three variants ---------------------------------
 * Picture 40 has 20 chunks and loses its tail. v1: the tail's second copy comes
 * 6 ms later, after picture 41. v2: both copies are lost. In v1 and v2 picture
 * 41 is single-chunk and arrives 4 ms after 40 opened; in v3, the control, it is
 * multi-chunk and on time. */
typedef struct { unsigned missing, miss1, nacks; int idr, gaps, n40, prefix40; } res_t;

static res_t run_variant(int variant)
{
    int64_t t = T0;
    for (int k = 0; k < 60; k++, t += T_PIC) {
        popt_t o = CLEAN;
        if (k == 40) {
            o.drop_orig_tail = 1;
            if (variant == 1) o.dup_delay = 6000; else o.drop_dup_tail = 1;
            add_picture(k, 0, 20, 489, t, o);
        } else if (k == 41 && variant != 3) {
            add_picture(k, 0, 1, 300, t - T_PIC + 4000, o);
        } else {
            add_picture(k, k == 0, std_nch(k), std_tail(k), t, o);
        }
    }
    feed(NULL);
    print_counters(window(38, 44));
    res_t r = { g_stats.chunks_missing, g_stats.frames_miss1, nacks(), (int)g_idr_needed,
                g_gap_lines, n_emit(40), is_prefix(40) };
    return r;
}

static void test_variants(void)
{
    session_begin("v3");
    const res_t v3 = run_variant(3);
    if (g_fixed) {
        /* The multi-chunk path already had G43: unchanged by the fix. */
        check(!strcmp(window(38, 44), "38 39 40 41 42 43 44"), "REASM-1",
              "control: in order, 40 before 41");
        check(v3.n40 == 1 && v3.prefix40, "G43", "control: 40 emitted once, truncated (tail loss)");
        check(v3.missing == 1 && v3.miss1 == 1, "G43", "control: the lost tail is counted");
        check(v3.gaps == 0 && v3.idr == 1 && v3.nacks == 1, "G26/V10",
              "control: no GAP, one key-frame request (G26), one flush-time NACK");
    } else {
        /* The toggle at 0 turns the trigger off on BOTH paths, as before. */
        check(v3.n40 == 0 && v3.gaps == 1 && v3.missing == 0, "REASM-1",
              "toggle 0: no flush-previous at all - 40 never emitted, one GAP, not counted");
    }
    check(others_exact(40), "REASM-1", "every other picture byte-exact");

    for (int variant = 1; variant <= 2; variant++) {
        session_begin(variant == 1 ? "v1" : "v2");
        const res_t v = run_variant(variant);
        check(others_exact(40), "REASM-1", "every other picture byte-exact");
        check(g_nem == (g_fixed || variant == 1 ? 60 : 59), "REASM-1",
              g_fixed ? "60 pictures emitted" : "toggle 0: 60 (v1) / 59 (v2) pictures emitted");
        if (g_fixed) {
            check(!strcmp(window(38, 44), "38 39 40 41 42 43 44"), "REASM-1",
                  "COUNTER-CASE: 40 is flushed BEFORE the single-chunk 41 (G43 trigger)");
            check(all_in_order(), "REASM-1", "the whole stream in order");
            check(v.n40 == 1 && v.prefix40, "G43", "40 emitted once, truncated (tail loss)");
            check(v.gaps == 0, "REASM-1", "COUNTER-CASE: no [G42] GAP line");
            check(v.missing == 1 && v.miss1 == 1, "REASM-1",
                  "COUNTER-CASE: the lost tail is counted (perdus stops undercounting)");
            check(v.idr == 1, "G26", "one key-frame request, from G26 instead of G36");
            check(v.missing == v3.missing && v.miss1 == v3.miss1 && v.nacks == v3.nacks
                  && v.idr == v3.idr && v.gaps == v3.gaps && v.n40 == v3.n40
                  && v.prefix40 == v3.prefix40, "REASM-1",
                  "identical to the multi-chunk control v3 (counters, NACK, IDR, emission)");
            check(g_single_lines == 1, "REASM-1", "one [G43] single-chunk flush line");
            if (variant == 1)
                check(g_stats.chunks_orphan_lost == 1, "V11",
                      "cosmetic, documented: the late tail copy is now an orphan 'perdu', not a 'dup'");
        } else if (variant == 1) {
            check(!strcmp(window(38, 44), "38 39 41 40 42 43 44"), "REASM-1",
                  "toggle 0 restores the defect: 41 reaches the decoder before 40");
            check(v.gaps == 3, "REASM-1", "toggle 0: three [G42] GAP lines (39->41, 41->40, 40->42)");
            check(v.n40 == 1 && is_exact(40) && v.missing == 0, "REASM-1",
                  "toggle 0: 40 completed by the late copy, nothing counted");
            check(v.idr == 1 && g_single_lines == 0, "G36", "toggle 0: the key frame comes from G36");
        } else {
            check(!strcmp(window(38, 44), "38 39 41 42 43 44") && v.n40 == 0, "REASM-1",
                  "toggle 0 restores the defect: 40 never reaches the decoder");
            check(v.missing == 0 && g_stats.chunks_orphan_lost == 0, "REASM-1",
                  "toggle 0 restores the defect: the loss is in NO counter");
            check(v.gaps == 1 && v.idr == 1, "G36", "toggle 0: one GAP, the key frame comes from G36");
        }
    }
}

/* ---- the clean stream: the fix must not move a single byte ------------------------
 * 600 pictures, one in three single-chunk, each placed right after the previous
 * burst - before (odd k) or after (even k) that burst's duplicate tail. No loss.
 * G5/G37: no key frame may be requested on a clean stream. */
static void test_clean(void)
{
    session_begin("clean");
    int64_t t = T0, tlast = 0;
    unsigned expected = 0;
    for (int k = 0; k < 600; k++, t += T_PIC) {
        if (k % 3 == 1) {
            tlast = add_picture(k, 0, 1, 40 + (k * 13) % 1200, tlast + ((k & 1) ? 1000 : 5000), CLEAN);
        } else {
            tlast = add_picture(k, k == 0, std_nch(k), std_tail(k), t, CLEAN);
            expected += (unsigned)std_nch(k);
        }
    }
    feed(NULL);
    printf("  %-8s emitted=%d hash=%08lx%08lx\n", g_case, g_nem,
           (unsigned long)(g_hash >> 32), (unsigned long)(g_hash & 0xffffffffu));
    print_counters("");
    bool exact = g_nem == 600;
    for (int i = 0; i < g_nem; i++) exact = exact && g_em[i].exact;
    check(exact, "REASM-1", "600 pictures, each emitted once, byte-identical to its access unit");
    check(all_in_order(), "REASM-1", "in order");
    check(g_idr_needed == 0, "G5/G37", "no key-frame request on a clean stream");
    check(g_gap_lines == 0, "K19", "no [G42] GAP line");
    check(g_stats.chunks_missing == 0 && g_stats.frames_miss1 == 0
          && g_stats.frames_dropped_trunc == 0, "REASM-1", "nothing counted lost or dropped");
    check(g_stats.chunks_expected == expected, "REASM-1",
          "chunks_expected = the multi-chunk pictures' chunks: no counter-only accounting fired");
    check(g_stats.chunks_orphan_lost == 0 && g_stats.chunks_orphan_stale == 0, "V11",
          "every orphan is a duplicate tail");
    check(g_single_lines == 0 && g_lost_lines == 0, "REASM-1", "no [G43] line");
    check(nacks() == 0, "V10", "no retransmission request");
}

/* ---- reuse: a lap-old picture counted, never answered ------------------------------
 * 40 loses both tail copies and 41 its opening chunk, so neither path flushes 40.
 * The key-frame flag is cleared at picture 60; subchannel 40 is reused by 296.
 * The review variant that called vid_account_loss() there raised a key-frame
 * request ~5.7 s after the loss and queued a NACK for a picture 256 frames old. */
static int      g_armed, g_flip_pic, g_counted_pic, g_lost_lines0;
static unsigned g_nack0;
static uint32_t g_miss0, g_miss1_0;

static void reuse_hook(const pkt_t *p, int after)
{
    if (!after) {
        if (!g_armed && p->pic >= 60) {
            g_armed = 1; g_idr_needed = 0; g_nack0 = nacks();
            g_miss0 = g_stats.chunks_missing; g_miss1_0 = g_stats.frames_miss1;
            g_lost_lines0 = g_lost_lines;
        }
        return;
    }
    if (g_armed && g_idr_needed && g_flip_pic < 0) g_flip_pic = p->pic;
    if (g_armed && g_stats.chunks_missing != g_miss0 && g_counted_pic < 0) g_counted_pic = p->pic;
}

static void test_reuse(void)
{
    session_begin("reuse");
    int64_t t = T0;
    for (int k = 0; k < 330; k++, t += T_PIC) {
        popt_t o = CLEAN;
        if (k == 40) { o.drop_orig_tail = 1; o.drop_dup_tail = 1; }
        if (k == 41) o.drop_sof = 1;
        add_picture(k, k == 0, k == 40 ? 20 : std_nch(k), k == 40 ? 489 : std_tail(k), t, o);
    }
    g_armed = 0; g_flip_pic = -1; g_counted_pic = -1;
    feed(reuse_hook);
    print_counters("");
    printf("  %-8s after the reset at 60: key-frame flag raised at picture %d (-1 = never), "
           "NACKs queued=%u, chunks counted lost=%u at picture %d\n", g_case, g_flip_pic,
           nacks() - g_nack0, (unsigned)(g_stats.chunks_missing - g_miss0), g_counted_pic);
    check(g_flip_pic == -1, "G5/G37", "no key-frame request for a picture a lap old");
    check(nacks() == g_nack0, "V10", "no NACK for a picture the server finished long ago");
    check(n_emit(40) == 0, "REASM-1", "40 never reaches the decoder");
    if (g_fixed) {
        check(g_stats.chunks_missing - g_miss0 == 1 && g_stats.frames_miss1 - g_miss1_0 == 1,
              "REASM-1", "COUNTER-CASE: the discarded picture is counted (one missing chunk)");
        check(g_counted_pic == 296, "REASM-1", "counted when subchannel 40 is reused (picture 296)");
        check(g_lost_lines - g_lost_lines0 == 1, "REASM-1", "one [G43] never-emitted line");
    } else {
        check(g_stats.chunks_missing == g_miss0, "REASM-1",
              "toggle 0: wiped in silence at reuse, as before");
        check(g_lost_lines == 0, "REASM-1", "toggle 0: no [G43] line");
    }
}

/* ---- stale: an ancient picture must never reach the decoder -------------------------
 * 40 loses both tail copies. A: 41 and 296 are single-chunk, 297 multi-chunk -
 * on the old code 297's opening flushed 40's leftover between 296 and 297, a lap
 * late. B: 41 and 296 lose their opening chunk, 297 is multi-chunk - the latent
 * defect of the multi-chunk trigger, caught only by the age guard. C: 41 loses
 * its opening, 296 and 297 are single-chunk - the path a flush-previous-only fix
 * would have ADDED (the review's stale2). */
static void test_stale(char variant)
{
    static char name[16];
    snprintf(name, sizeof name, "stale-%c", variant);
    session_begin(name);
    int64_t t = T0;
    for (int k = 0; k < 300; k++, t += T_PIC) {
        popt_t o = CLEAN;
        int nch = std_nch(k), tail = std_tail(k);
        int64_t at = t;
        if (k == 40) { o.drop_orig_tail = 1; o.drop_dup_tail = 1; nch = 20; tail = 489; }
        if (variant == 'A' && k == 41)                 { nch = 1; tail = 300; at = t - T_PIC + 4000; }
        if (variant == 'A' && k == 296)                { nch = 1; tail = 300; }
        if (variant == 'B' && (k == 41 || k == 296))   o.drop_sof = 1;
        if (variant == 'C' && k == 41)                 o.drop_sof = 1;
        if (variant == 'C' && (k == 296 || k == 297))  { nch = 1; tail = 300; }
        add_picture(k, k == 0, nch, tail, at, o);
    }
    feed(NULL);
    print_counters(tail_from(293));
    check(!emitted_after(40, 295), "REASM-1",
          "COUNTER-CASE: picture 40 never reaches the decoder after 295 (no resurrection)");
    if (variant == 'A') {
        if (g_fixed)
            check(n_emit(40) == 1 && is_prefix(40) && !strcmp(window(39, 41), "39 40 41"),
                  "G43", "40 emitted once, truncated, in its place");
        else
            check(n_emit(40) == 0, "REASM-1", "toggle 0: 40 never emitted");
    } else {
        check(n_emit(40) == 0, "REASM-1", "40 never emitted");
        check(g_stats.chunks_missing == (g_fixed ? 1u : 0u), "REASM-1",
              g_fixed ? "counted once, counters only" : "toggle 0: not counted");
    }
    if (variant == 'B')
        check(g_stats.chunks_orphan_stale > 0, "V11",
              "296's continuations meet 40's leftover and are refused (different chunk count)");
}

/* ---- the [G43] line budget is PER SESSION (CONC-4's family) --------------------------
 * Six single-chunk flushes in one session, then one in the next: 5 + 1 lines.
 * A function static would have spent the budget in the first session. */
static void test_budget(int events, const char *name)
{
    session_begin(name);
    int64_t t = T0;
    for (int k = 0; k < 100; k++, t += T_PIC) {
        popt_t o = CLEAN;
        const bool lossy  = k % 10 == 0 && k >= 10 && k <= 10 * events;
        const bool single = k % 10 == 1 && k >= 11 && k <= 10 * events + 1;
        if (lossy) { o.drop_orig_tail = 1; o.drop_dup_tail = 1; add_picture(k, 0, 20, 489, t, o); }
        else if (single) add_picture(k, 0, 1, 300, t - T_PIC + 4000, o);
        else add_picture(k, k == 0, std_nch(k), std_tail(k), t, o);
    }
    feed(NULL);
    print_counters("");
    if (g_fixed) {
        check(g_single_lines == (events > 5 ? 5 : events), "REASM-1/CONC-4",
              "the [G43] single-chunk line: at most 5 per session, and a new session starts again");
        check(g_stats.chunks_missing == (unsigned)events && all_in_order(), "REASM-1",
              "every event flushed in order and counted");
    } else {
        check(g_single_lines == 0, "REASM-1", "toggle 0: no [G43] line");
    }
}

int main(void)
{
    const char *e = getenv("SHADOW_FLUSH_PREV_INCOMPLETE");
    const char *x = getenv("TEST_VID_REASM_EXPECT");
    g_fixed = x ? (strcmp(x, "legacy") != 0) : (e ? atoi(e) != 0 : 1);
    printf("REASM-1 - SHADOW_FLUSH_PREV_INCOMPLETE=%s, expectations: %s\n", e ? e : "(unset)",
           g_fixed ? "the FIXED behaviour" : "the PREVIOUS behaviour (the defect, the counter-case)");

    test_variants();
    test_clean();
    test_reuse();
    test_stale('A');
    test_stale('B');
    test_stale('C');
    test_budget(6, "budget-1");
    test_budget(1, "budget-2");

    for (int i = 0; i < NPIC_MAX; i++) free(g_au[i]);
    printf("%d checks, %d failure(s)\n", total, failed);
    if (!failed) printf("OK\n");
    return failed ? 1 : 0;
}
