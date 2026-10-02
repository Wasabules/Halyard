/* ctrl_input_tcp - native Shadow input channel on :base+14 (TCP+TLS+FlatBuffer).
 * See header for design + RE refs.
 *
 * I1 2026-05-18: MVP - Connect blob byte-exact from the V16 desktop capture,
 * mouse/keyboard templates with dynamic seq + timestamp.
 */

#include "ctrl_input_tcp.h"
#include "kbd_scancode.h"
#include "msgframe.h"
#include "../services/log.h"
/* S81 - this module's log category. See shadow/journal.h: it is declared
 * here, never inferred from the text of the messages. */
#define itlog(...) JOURNAL_INFO_(JOURNAL_CAT_INPUT, __VA_ARGS__)
#define itdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_INPUT, __VA_ARGS__)

#include "../services/sockets_compat.h"
/* pthread.h BEFORE any wolfSSL header: `wolfssl/options.h` does
 * `#undef _POSIX_THREADS`, and newlib gates half of pthread.h behind that macro
 * (mutex, create, join - the rwlocks stay visible, hence an error message that
 * unhelpfully suggests `pthread_rwlock_unlock`). A file that includes wolfSSL
 * first therefore no longer builds for the Switch. */
#include <pthread.h>

#include "ctrl_audio_dtls.h"   /* S11 : ctrl_dtls_connect_raw */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifndef _WIN32
#include <netdb.h>
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
/* AFTER <pthread.h>: tls_chan.h pulls in wolfssl/options.h, which does
 * `#undef _POSIX_THREADS` - see the note above. */
#include "tls_chan.h"
#include "session_host.h"   /* DNS1: one lookup of the VM name per session */

#define vlog(fmt, ...) itlog("[input-tcp] " fmt, ##__VA_ARGS__)

struct ctrl_input_tcp {
    int             sock;
    WOLFSSL_CTX    *ctx;
    WOLFSSL        *ssl;
    pthread_mutex_t tx_mtx;
    pthread_t       rx_thread;
    volatile bool   abort_flag;
    volatile bool   rx_running;
    uint32_t        seq;      /* incremented on every message sent (offset 32-35) */
    /* RE5 2026-05-18 - cursor position tracking, used to patch X/Y inside
     * MouseMove. X @ bytes 128-129 u16 LE, Y @ bytes 130-131 u16 LE (validated
     * on 250 desktop samples, V16). Two modes: accumulated deltas, or absolute
     * (overridden through the API). */
    int             cursor_x;
    int             cursor_y;
    int             srv_x, srv_y, srv_valid;  /* S16: position reported by the server */
    uint32_t        mouse_seq; /* W1 : compteur d evenements SOURIS (@100 en
                                * vtable elargie, @96 sinon) */
    uint32_t        kbd_seq;  /* K5 : compteur d'evenements CLAVIER (@96), distinct
                               * from the mouse's own - see the K3/K5 block below */
    ctrl_input_tcp_stats_t stats;
    /* AUD1: sink for unrecognised messages (see the header). */
    ctrl_input_tcp_audio_cb audio_cb;
    void                   *audio_user;
    uint32_t                rx_hist[8];   /* a size histogram, by class */
    /* S31 - bytes carried over between two TCP reads, for the framing. It lives
     * here rather than on the thread stack: 16 KiB more on a Switch thread
     * stack, whose default size is not set explicitly, would be a pointless
     * gamble. */
    uint8_t                 rx_acc[16384];
    size_t                  rx_acc_len;
    int64_t                 hist_log_ms;
};

static int64_t aud_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Size class for the census: we do not want a full histogram, only to know
 * whether messages of another kind are arriving, and which. */
static int rx_size_class(int n)
{
    if (n == 104) return 0;             /* echo d'input connu */
    if (n < 32)   return 1;
    if (n < 96)   return 2;
    if (n < 160)  return 3;             /* typical size of an Opus frame */
    if (n < 400)  return 4;
    if (n < 1000) return 5;
    return 6;
}

/* === Bytes captured on desktop V16, ssl=0x3781b750, on :15014 ===
 *
 * Connect blob (= first SSL_WRITE, 96 B, byte-exact):
 *   5c 00 00 00              size_le = 92 (= 96-4)
 *   18 00 00 00 00 00 00 00 00 00 0e 00   FlatBuffer header
 *   1c 00 06 00 0c 00 14 00 07 00 08 00 0e 00 00 00
 *   00 00 01 03 14 00 00 00  | type marker + sub-len
 *   01 00 00 00 00 00 00 00  | seq=1 + 4B zeros
 *   95 2a bd 6f 00 00 00 00  | timestamp u64_LE (= dynamic per session)
 *   f4 ff ff ff 00 00 00 01
 *   0c 00 00 00 08 00 0c 00 07 00 08 00 08 00 00 00
 *   00 00 00 01 08 00 00 00 04 00 04 00 04 00 00 00
 */
static const uint8_t CONNECT_BLOB[96] = {
    0x5c, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x1c, 0x00, 0x06, 0x00, 0x0c, 0x00, 0x14, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03,
    0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x95, 0x2a, 0xbd, 0x6f, 0x00, 0x00, 0x00, 0x00, 0xf4, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x01, 0x0c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0c, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
};

/* BUG4 2026-05-18 - Hello 128 B + PointerEnter 136 B, byte-exact from desktop.
 * Copied from webrtc/shadow_input.c (init_blob_hello + init_blob_pointer).
 * Without these two messages after Connect the server sends no echo and accepts
 * no subsequent input (see project_shadow_pointer_enter_unlock.md - same symptom
 * reverse-engineered on the WebRTC SCTP path, fix transposed here onto the
 * native TCP wire). */
static const uint8_t HELLO_BLOB[128] = {
    0x7c, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x18, 0x00, 0x17, 0x00, 0x00, 0x00, 0x0c, 0x00,
    0x0b, 0x00, 0x04, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x92, 0xff, 0x9e, 0xe8, 0x9d, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x0a, 0x00, 0x09, 0x00, 0x04, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0a, 0x00,
    0x0a, 0x00, 0x00, 0x00, 0x09, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0a, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x0b, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x08, 0x00, 0x06, 0x00, 0x05, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x01, 0x5b, 0x00,
};

static const uint8_t POINTER_ENTER_BLOB[136] = {
    0x84, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x1c, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x0c, 0x00,
    0x0b, 0x00, 0x04, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x3b, 0xdc, 0x8e, 0xe9, 0x9d, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x0a, 0x00,
    0x09, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x0a, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x04, 0x00,
    0x0a, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x10, 0x00, 0x10, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x0c, 0x00, 0x0b, 0x00,
    0x0a, 0x00, 0x04, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x07, 0x00, 0x9b, 0x02, 0x04, 0x00, 0x04, 0x00,
    0x04, 0x00, 0x00, 0x00,
};

/* RE1 2026-05-18 - templates copied byte-exact from webrtc/shadow_input.c.
 * Hypothesis at C85: the native TCP path accepts the SAME FlatBuffers as WebRTC
 * SCTP, because the three init blobs (Connect/Hello/PointerEnter) are identical
 * between the two paths (checked at runtime 2026-05-18 - the server answers the
 * init ACKs over TCP).
 *
 * Offset layout:
 *   - seq @100 (mirror, mutable u32 LE)
 *   - timestamp @40 (u64 LE, monotonic ms)
 *   - X/Y @132/134 (u16 LE each)
 *   - button discriminator: bytes 130-131
 *   - scroll discriminator: bytes 150-151 (IEEE 754 float16)
 *   - scancode discriminator: bytes 130-131 = 06 00, plus keycode @142
 */

/* LEFT mousedown, 152 B - I5 FIX 2026-06-02: BYTE-EXACT desktop (MASTER
 * capture, the n=11 group of 152 B messages, vt 1c0006). The OLD one came from
 * session5.json (WebRTC path, vt 20001f) = a format the native TCP channel does
 * not accept, so the click was ignored.
 * Fields patched at runtime: seq@40, ts@48, @96, X@128, Y@130. */
static const uint8_t CLICK_LEFT_TEMPLATE[152] = {
    0x94, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x1c, 0x00, 0x06, 0x00, 0x0c, 0x00, 0x14, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03,
    0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0e, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x10, 0x00, 0x08, 0x00,
    0x07, 0x00, 0x0c, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x06, 0x00, 0x07, 0x00, 0x0c, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x07, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

/* RIGHT mousedown, 152 B - told apart by bytes 140=07, 142=06, 150=01. */
static const uint8_t CLICK_RIGHT_TEMPLATE[152] = {
    0x94, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x20, 0x00, 0x1f, 0x00, 0x14, 0x00, 0x0c, 0x00,
    0x0b, 0x00, 0x04, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0xc6, 0x27, 0xc8, 0xe9, 0x9d, 0x01, 0x00, 0x00,
    0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x0a, 0x00, 0x09, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0a, 0x00, 0x10, 0x00, 0x0c, 0x00,
    0x0b, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x21, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x0c, 0x00, 0x0b, 0x00, 0x0a, 0x00, 0x04, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01,
    0x94, 0x00, 0x3c, 0x03, 0x08, 0x00, 0x08, 0x00, 0x07, 0x00, 0x06, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
};

/* MouseUp, 144 B - I5 FIX 2026-06-02: BYTE-EXACT desktop (MASTER capture, the
 * second 144 B signature, n=12, told apart from a move by byte 127 = 0x02 vs
 * 0x01). The old one came from the WebRTC path (vt 20001f).
 * Fields patched: seq@40, ts@48, @96, X@128, Y@130. */
static const uint8_t MOUSEUP_TEMPLATE[144] = {
    0x8c, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x1c, 0x00, 0x06, 0x00, 0x0c, 0x00, 0x14, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03,
    0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0e, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x10, 0x00, 0x08, 0x00,
    0x07, 0x00, 0x0c, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x06, 0x00, 0x07, 0x00, 0x0c, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
};



/* Keyboard, 144 B - marker b130-131 = 06 00, keycode @142, press/release at
 * bytes 114/116/128 */
/* KBD_TEMPLATE - K1 2026-08-21: REPLACED by a real desktop message.
 * The old template differed on **50 constant bytes**: it was not the same
 * message at all (most likely inherited from the WebRTC/browser era, like the
 * click templates). This one is extracted from a guided capture of 51 keyboard
 * messages whose scancode matches the corresponding /dev/input event.
 * The variable fields are zeroed here and patched when sending:
 *   @40 seq, @48-55 u64 timestamp, @96 counter, @110 press/release,
 *   @122 (role unknown), @142 evdev scancode
 */
/* === K7 2026-08-27 - THE EXTENDED FLAG IS A FIELD, NOT A PREFIX ===
 *
 * Guided capture of the official client (tools/capture_keys.sh, 91 messages,
 * 22 distinct scancode/shape pairs): extended keys carry NO 0xE0 prefix. The
 * high byte of the scancode (@143) is 0 in 100% of the messages - checked live
 * here, and across the 22,000+ messages of the repo's 29 captures. What the
 * client sends is the BASE "set 1" scancode in a single byte, plus a separate
 * BOOLEAN.
 *
 * Structural proof: the same scancode byte appears in TWO shapes.
 *   0x1C plain = main Enter; 0x1C extended = KEYPAD Enter.
 *   0x47 plain = keypad 7;   0x47 extended = Home.
 * Without that boolean the two keys would be indistinguishable - so it is
 * indeed the boolean that carries the information.
 *
 * The message is FlatBuffers. The table starts at @136 and points back to its
 * vtable (i32 soffset @136):
 *   plain:    vtable @130, size 6, table 8 -> ONE field, +6 = scancode u16
 *   extended: vtable @128, size 8, table 8 -> TWO fields, +6 = scancode,
 *                                             +5 = boolean @141 = 1
 * The field missing from the plain shape is simply FlatBuffers omitting a field
 * equal to its default (false). Hence the seven bytes of difference, measured
 * IDENTICAL on two different scancodes - structural, not accidental.
 *
 * Agreement checked: 22 matches out of 22 against the PC set 1. Every E0-prefixed
 * key (arrows, Home/End/PgUp/PgDn, Insert/Delete, keypad Enter and keypad /)
 * arrives in the extended shape; the rest (keypad 7, keypad *, keypad -,
 * keypad +, Backspace, right Shift, main Enter) in the plain shape.
 * NumLock arrives EXTENDED even though its scancode has no E0 prefix: that is
 * the documented Windows oddity, which marks this key as extended in bit 24 of
 * lParam. It confirms that the field really is "the Windows extended flag" and
 * not "an E0 was present".
 *
 * WHAT WE USED TO DO: write 0xE0 into @143 (kbd_scancode.h, KBD_SET1_EXTENDED).
 * The server therefore received scancode 0xE048 instead of 0x48 - a code that
 * does not exist. That is the exact cause of "the arrow keys do nothing". The
 * 0xE0 prefix stays as an INTERNAL MARKER, convenient for carrying the flag in
 * a single u16 through the event queue, but it is stripped here and never
 * reaches the wire. */
static const uint8_t KBD_TEMPLATE[144] = {
    0x8c, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x1c, 0x00, 0x06, 0x00, 0x0c, 0x00, 0x14, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03,
    0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0e, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x12, 0x00, 0x08, 0x00,
    0x07, 0x00, 0x0c, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x0e, 0x00, 0x00, 0x00, 0x07, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00,
    0x08, 0x00, 0x06, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* MOVE template, 144 B - I5 FIX 2026-06-02: BYTE-EXACT desktop (MASTER capture,
 * 1888 steady-state moves). The OLD template was zero-padded at @96-143, i.e.
 * the FlatBuffer table that carries X/Y was ZERO, so the message was invalid:
 * the server ACKed but did NOT apply the moves (cursor frozen). This template
 * has the real structure at @96-143. Fields patched at runtime: seq@40, ts@48,
 * @96 (event counter), X@128, Y@130. */
static const uint8_t MOUSE_TICK_TEMPLATE[144] = {
    0x8c, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x1c, 0x00, 0x06, 0x00, 0x0c, 0x00, 0x14, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03,
    0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0e, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x10, 0x00, 0x08, 0x00,
    0x07, 0x00, 0x0c, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x0a, 0x00, 0x06, 0x00, 0x07, 0x00, 0x0c, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
};

static uint32_t monotonic_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000ULL + t.tv_nsec / 1000ULL);
}

/* I10 2026-08-21 - the timestamp @48 is a **u64**, not a u32.
 * The comment on the keyboard path already said so ("u64 LE, low 4 bytes; high
 * @52"), but monotonic_us() TRUNCATES to 32 bits: we only wrote the low half,
 * and @52.. kept the template's value.
 * The capture settles it: the desktop has `0x0b` at @52 on 697/697 messages
 * = 11 x 2^32 us ~= 13.1 h of machine uptime. So it was NOT a constant field of
 * the protocol (my initial I8 analysis took it for one), but the high half of
 * their monotonic clock. Hardcoding 0x0b would have sent another machine's
 * uptime. */
static uint64_t monotonic_us64(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ULL + (uint64_t)t.tv_nsec / 1000ULL;
}

/* Writes the u64 LE timestamp over 8 bytes starting at `off`. */
static void put_ts64(uint8_t *msg, size_t off) {
    uint64_t ts = monotonic_us64();
    for (int i = 0; i < 8; i++) msg[off + i] = (uint8_t)((ts >> (8 * i)) & 0xFF);
}

/* I7 2026-06-02 - dump the input messages we send, to diff them against the
 * desktop's. Gated on SHADOW_DUMP_INPUT=1 -> ./halyard-data/our_input.bin
 * Format: [u32 len][len bytes] per message (move/click/kbd/init all mixed). */
static void input_dump_tx(const uint8_t *msg, size_t len) {
    static int g_dump = -1;
    static FILE *f = NULL;
    static int n = 0;
    if (g_dump < 0) {
        const char *e = getenv("SHADOW_DUMP_INPUT");
        g_dump = e ? atoi(e) : 0;
    }
    if (!g_dump || n >= 2000) return;
    if (!f) f = fopen("./halyard-data/our_input.bin", "wb");
    if (!f) { g_dump = 0; return; }
    uint32_t l = (uint32_t)len;
    fwrite(&l, 4, 1, f); fwrite(msg, 1, len, f); fflush(f);
    n++;
}

/* Handles ONE complete message from the input channel, size prefix included.
 * Pulled out of the receive thread on 2026-08-25 so the framing can call it
 * once per message rather than once per TCP read. */
static void input_rx_message(ctrl_input_tcp_t *c, const uint8_t *buf, int n)
{
    pthread_mutex_lock(&c->tx_mtx);
    c->stats.messages_recv++;
    pthread_mutex_unlock(&c->tx_mtx);
    /* S16 - cursor position as the SERVER computes it
     * (@96-97 = X, @98-99 = Y, u16 LE). Closed loop: tells us where the cursor
     * really is, without assuming an origin. */
    if (n == 104) {
        unsigned sx = (unsigned)buf[96] | ((unsigned)buf[97] << 8);
        unsigned sy = (unsigned)buf[98] | ((unsigned)buf[99] << 8);
        c->srv_x = (int)sx; c->srv_y = (int)sy; c->srv_valid = 1;
        static unsigned last_sx = 0xFFFF, last_sy = 0xFFFF;
        static int pos_log = 0;
        if ((sx != last_sx || sy != last_sy) && pos_log < 40) {
            vlog("[S16] curseur serveur = (%u,%u)", sx, sy);
            last_sx = sx; last_sy = sy; pos_log++;
        }
    }
    /* AUD1 - census of the sizes received. As long as we do not know which
     * channel carries the sound, this line is what will say: if playing sound
     * on the remote desktop makes a size class appear that did not exist
     * before, the channel is found. */
    c->rx_hist[rx_size_class(n)]++;
    {
        int64_t now = aud_now_ms();
        if (c->hist_log_ms == 0) c->hist_log_ms = now;
        if (now - c->hist_log_ms >= 10000) {
            c->hist_log_ms = now;
            vlog("[AUD1] tailles recues : echo104=%u <32=%u <96=%u <160=%u "
                 "<400=%u <1000=%u >=1000=%u",
                 c->rx_hist[0], c->rx_hist[1], c->rx_hist[2], c->rx_hist[3],
                 c->rx_hist[4], c->rx_hist[5], c->rx_hist[6]);
        }
    }

    /* Anything that is not the known echo goes to the audio decoder. */
    if (n != 104 && c->audio_cb) {
        c->audio_cb(buf, (size_t)n, 0, c->audio_user);
    }

    static int g_rd_log = 0;
    if (g_rd_log < 4) {
        /* S15 2026-08-21 - hex dump of the server replies. Now that the
         * channel answers at all (see @83=0x01), we need to know WHAT it
         * answers: if these messages carry coordinates that follow our
         * movements, then our events are being applied. */
        char hex[3 * 104 + 1]; int ho = 0;
        for (int i = 0; i < n && i < 104; i++)
            ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x ", buf[i]);
        vlog("RX %d B : %s", n, hex);
        g_rd_log++;
    }
}

static void *vst_input_rx_thread(void *arg) {
    ctrl_input_tcp_t *c = (ctrl_input_tcp_t *)arg;
    uint8_t buf[4096];
    static int g_rx_framing = -1;
    if (g_rx_framing < 0) {
        const char *e = getenv("SHADOW_INPUT_RX_FRAMING");
        g_rx_framing = e ? atoi(e) : 1;   /* S31: framing is on by default */
    }
    vlog("rx_thread started");
    int idle_ticks = 0;

    /* === K2 2026-08-21 - PERIODIC KEEPALIVE on the input channel ===
     * The 96 B message is NOT a plain "Connect" sent once: the guided capture
     * shows the official client emitting it **19 times in 135 s, at exactly
     * 7.50 s intervals**, with only the counter (@40) and the timestamp
     * (@48-51) changing. We were sending it just ONCE, at open time.
     *
     * This is the strongest lead on "the server ACKs but never applies
     * anything": the content of our messages is now byte-exact (clicks and
     * keyboard, see KB §3.20) and input still does not get through, so what is
     * missing is a channel STATE condition, not content. A channel that stops
     * sending its keepalive is most likely deactivated.
     *
     * Converging clue: the desktop RECEIVES 104 B messages with the same
     * FlatBuffer header but `@35=0x04` instead of `0x03` (the direction
     * discriminator), carrying cursor coordinates at @96/@98 - the channel's
     * return path. We receive NONE of those.
     *
     * SHADOW_INPUT_KEEPALIVE_MS=0 disables it; default 7500 ms. */
    static int g_ka_ms = -1;
    if (g_ka_ms < 0) {
        const char *e = getenv("SHADOW_INPUT_KEEPALIVE_MS");
        g_ka_ms = e ? atoi(e) : 7500;
    }
    struct timespec ka_t;
    clock_gettime(CLOCK_MONOTONIC, &ka_t);
    long long ka_last = (long long)ka_t.tv_sec * 1000 + ka_t.tv_nsec / 1000000;
    unsigned ka_seq = 2;          /* the initial Connect used seq=1 */
    unsigned ka_count = 0;

    while (!c->abort_flag) {
        if (g_ka_ms > 0) {
            struct timespec nt;
            clock_gettime(CLOCK_MONOTONIC, &nt);
            long long now = (long long)nt.tv_sec * 1000 + nt.tv_nsec / 1000000;
            if (now - ka_last >= g_ka_ms) {
                uint8_t kb[sizeof(CONNECT_BLOB)];
                memcpy(kb, CONNECT_BLOB, sizeof(kb));
                kb[40] = (uint8_t)(ka_seq & 0xFF);
                kb[41] = (uint8_t)((ka_seq >> 8) & 0xFF);
                kb[42] = (uint8_t)((ka_seq >> 16) & 0xFF);
                kb[43] = (uint8_t)((ka_seq >> 24) & 0xFF);
                put_ts64(kb, 48);
                kb[83] = 0x02;
                pthread_mutex_lock(&c->tx_mtx);
                int kw = wolfSSL_write(c->ssl, kb, (int)sizeof(kb));
                pthread_mutex_unlock(&c->tx_mtx);
                ka_seq++; ka_count++;
                if (ka_count <= 3 || ka_count % 10 == 0) {
                    vlog("[K2] keepalive 96B #%u (seq=%u, wr=%d, periode=%dms)",
                         ka_count, ka_seq - 1, kw, g_ka_ms);
                }
                ka_last = now;
            }
        }
        int n = wolfSSL_read(c->ssl, buf, sizeof(buf));
        if (n <= 0) {
            int err = wolfSSL_get_error(c->ssl, n);
            int se = shadow_sock_errno();
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE
                || se == EAGAIN || se == EWOULDBLOCK) {
                idle_ticks++;
                if (idle_ticks % 50 == 0) {
                    vlog("idle %ds (no echo from server)", idle_ticks / 10);
                }
                continue;
            }
            vlog("wolfSSL_read returned %d (err=%d sockerr=%d) — closing", n, err, se);
            break;
        }
        idle_ticks = 0;
        pthread_mutex_lock(&c->tx_mtx);
        c->stats.bytes_recv += (uint32_t)n;
        pthread_mutex_unlock(&c->tx_mtx);

        /* === S31 2026-08-25 - FRAMING BY SIZE PREFIX ===
         * The wire carries length-prefixed FlatBuffers: `[size u32 LE][payload]`,
         * total = size + 4. The proof is on both sides - our Connect blob starts
         * with `5c 00 00 00` (= 92 = 96-4) and the server echo with
         * `64 00 00 00` (= 100 = 104-4).
         *
         * The code nevertheless treated EACH `wolfSSL_read` as one whole message
         * (the `n == 104` test). TCP is a stream with no boundaries: two echoes
         * delivered together gave n=208, hence "not 104", hence a hand-off to
         * the audio decoder; one echo split in two gave two fragments, equally
         * misrouted.
         *
         * MEASURED: across the logged sessions, 100% of the reads are exactly
         * 104 bytes (1365 and 282 messages counted, every other size class at
         * zero). The defect is therefore REAL but DORMANT - and it would wake up
         * precisely on the day this channel finally carries traffic, which is
         * the work in progress. So we frame now, while the observable behaviour
         * is unchanged. SHADOW_INPUT_RX_FRAMING=0 restores the old "one read =
         * one message". */
        if (g_rx_framing) {
            if (c->rx_acc_len + (size_t)n > sizeof(c->rx_acc)) {
                vlog("cadrage : tampon sature (%zu + %d) — resynchronisation", c->rx_acc_len, n);
                c->rx_acc_len = 0;
            }
            memcpy(c->rx_acc + c->rx_acc_len, buf, (size_t)n);
            c->rx_acc_len += (size_t)n;

            size_t off = 0;
            for (;;) {
                int total = msgframe_next(c->rx_acc + off, c->rx_acc_len - off, sizeof(c->rx_acc));
                if (total == 0) break;                 /* incomplete: we wait */
                if (total < 0) {
                    vlog("framing: absurd announced size - resynchronising");
                    off = c->rx_acc_len;               /* discard: there is no resync point */
                    break;
                }
                input_rx_message(c, c->rx_acc + off, total);
                off += (size_t)total;
            }
            if (off > 0) {
                if (off < c->rx_acc_len)
                    memmove(c->rx_acc, c->rx_acc + off, c->rx_acc_len - off);
                c->rx_acc_len -= off;
            }
        } else {
            input_rx_message(c, buf, n);
        }
    }
    c->rx_running = false;
    vlog("rx_thread exited");
    return NULL;
}


/* S9 2026-08-21 - SCAN for the input channel.
 * Established: the server accepts only ONE connection on :base+14, and the
 * capture proves it is the ComChan that takes it on the official client (fd 317
 * carries exactly its encrypted lengths 112/32/32/61). The desktop's input
 * channel, on the other hand, shows up on NO socket: the LD_PRELOAD hook sees
 * neither its connect nor its I/O, so its port cannot be deduced from the
 * capture.
 * This scan settles it empirically: on a live session, TLS handshake on each
 * offset, send the 96 B Connect, and see who answers. The input channel is the
 * one that replies with ~104 B.
 * SHADOW_SCAN_INPUT=1 (offsets 0..40 by default, SHADOW_SCAN_MAX to bound). */
void ctrl_input_tcp_scan(const char *vm_host, uint16_t base_port) {
    const char *en = getenv("SHADOW_SCAN_INPUT");
    if (!en || atoi(en) == 0 || !vm_host) return;
    const char *emax = getenv("SHADOW_SCAN_MAX");
    int max_off = emax ? atoi(emax) : 40;

    const char *connect_host = vm_host;
    if (strncmp(vm_host, "ipv6-", 5) == 0) connect_host = vm_host + 5;
    vlog("[S9] scanning for the input channel on %s:%u..%u",
         connect_host, (unsigned)base_port, (unsigned)(base_port + max_off));

    for (int off = 0; off <= max_off; off++) {
        uint16_t port = (uint16_t)(base_port + off);
        char portstr[8];
        snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (session_getaddrinfo(connect_host, portstr, &hints, &res) != 0 || !res) continue;
        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { freeaddrinfo(res); continue; }
        /* WIN1 2026-09-10 - was two inline setsockopt fed a `struct timeval`.
         * Winsock wants a DWORD of milliseconds; same 2 s everywhere. */
        shadow_set_sock_timeout(fd, SO_RCVTIMEO, 2000);
        shadow_set_sock_timeout(fd, SO_SNDTIMEO, 2000);
        int crc = connect(fd, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);
        if (crc != 0) { shadow_closesocket(fd); continue; }

        WOLFSSL_CTX *sctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
        if (!sctx) { shadow_closesocket(fd); continue; }
        wolfSSL_CTX_set_verify(sctx, WOLFSSL_VERIFY_NONE, NULL);
        WOLFSSL *sssl = wolfSSL_new(sctx);
        if (!sssl) { wolfSSL_CTX_free(sctx); shadow_closesocket(fd); continue; }
        wolfSSL_set_fd(sssl, fd);
        if (wolfSSL_connect(sssl) != WOLFSSL_SUCCESS) {
            vlog("[S9] +%-2d (:%u) open but NO TLS", off, (unsigned)port);
            wolfSSL_free(sssl); wolfSSL_CTX_free(sctx); shadow_closesocket(fd);
            continue;
        }
        uint8_t cb[sizeof(CONNECT_BLOB)];
        memcpy(cb, CONNECT_BLOB, sizeof(cb));
        cb[40] = 1; cb[41] = cb[42] = cb[43] = 0;
        put_ts64(cb, 48);
        cb[83] = 0x02;
        int wr = wolfSSL_write(sssl, cb, (int)sizeof(cb));
        uint8_t rx[256];
        int n = wolfSSL_read(sssl, rx, sizeof(rx));
        if (n > 0) {
            char hex[3 * 32 + 1]; int ho = 0;
            for (int i = 0; i < n && i < 32; i++)
                ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x ", rx[i]);
            vlog("[S9] +%-2d (:%u) TLS OK, Connect wr=%d, *** REPONSE %d B *** : %s",
                 off, (unsigned)port, wr, n, hex);
        } else {
            vlog("[S9] +%-2d (:%u) TLS OK, Connect wr=%d, no answer",
                 off, (unsigned)port, wr);
        }
        wolfSSL_free(sssl); wolfSSL_CTX_free(sctx); shadow_closesocket(fd);
    }
    vlog("[S9] scan termine");
}

/* S11 2026-08-21 - the post-handshake part, shared by both transports
 * (DTLS/UDP on :base+12 = the real channel, and the old TLS/TCP on :base+14,
 * kept behind SHADOW_INPUT_TRANSPORT=tcp). Everything below only does
 * wolfSSL_read/write, so nothing here depends on the transport.
 * The caller hands over ownership of sock/ctx/ssl. */
static int input_finish_open(ctrl_input_tcp_t **out, int sock,
                              WOLFSSL_CTX *ctx, WOLFSSL *ssl) {
    uint8_t cblob[sizeof(CONNECT_BLOB)];
    memcpy(cblob, CONNECT_BLOB, sizeof(cblob));
    {
        static int g_raw = -1;
        if (g_raw < 0) {
            const char *e = getenv("SHADOW_INPUT_CONNECT_RAW");
            g_raw = e ? atoi(e) : 0;
        }
        if (!g_raw) {
            uint32_t s0 = 1;
            cblob[40] = (uint8_t)(s0 & 0xFF);
            cblob[41] = (uint8_t)((s0 >> 8) & 0xFF);
            cblob[42] = (uint8_t)((s0 >> 16) & 0xFF);
            cblob[43] = (uint8_t)((s0 >> 24) & 0xFF);
            put_ts64(cblob, 48);
            /* S14 2026-08-21 - @83 varies by session: the 2026-08-21 captures
             * (the most recent ones, DTLS channel) give 0x01, three older
             * captures gave 0x02. F2 forced it to 0x02.
             * A/B settled on 2026-08-21: with 0x02 the server stays SILENT (0
             * datagram after the handshake); with 0x01 it answers - 45
             * datagrams, including the 104 B messages of the return path.
             * Default 0x01. */
            static int g_b83 = -1;
            if (g_b83 < 0) {
                const char *e83 = getenv("SHADOW_INPUT_B83");
                g_b83 = e83 ? (int)strtol(e83, NULL, 0) : 0x01;
            }
            cblob[83] = (uint8_t)g_b83;
            vlog("[F2] Connect patche : seq=%u ts64 @48, @83=0x%02x", s0, g_b83);
        }
    }
    /* S14 - SHADOW_INPUT_SILENT=1: open the DTLS session and send NOTHING.
     * Tells us whether the server pushes on its own after the handshake (in
     * which case it is our first message that silences it) or whether it waits
     * for a valid message. */
    static int g_silent = -1;
    if (g_silent < 0) {
        const char *es = getenv("SHADOW_INPUT_SILENT");
        g_silent = es ? atoi(es) : 0;
    }
    int wr = g_silent ? (int)sizeof(CONNECT_BLOB)
                      : wolfSSL_write(ssl, cblob, sizeof(cblob));
    if (g_silent) vlog("[S14] quiet mode: no message emitted");
    if (wr != (int)sizeof(CONNECT_BLOB)) {
        vlog("Connect write FAIL rc=%d (want=%zu)", wr, sizeof(CONNECT_BLOB));
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        return -1;
    }
    vlog("Connect 96B sent");

    /* Sending focus events has been REMOVED: F1 showed it wrote big-endian
     * frames into a little-endian length-prefixed stream, desynchronising the
     * input channel for the whole session. Refuted AND harmful - verdict in the
     * KB. */

    /* I8 FIX 2026-06-02 - the official desktop NEVER sends Hello (128 B) or
     * PointerEnter (136 B): a complete LD_PRELOAD capture (134 MB, streaming +
     * clicks) has 0 messages of size 128 or 136 on ANY channel. Those two blobs
     * came from the WebRTC path (project_shadow_pointer_enter_unlock.md), NOT
     * from the native TCP channel. We sent them after Connect, so the server
     * received two unexpected messages (plus a huge frozen seq): it ACKed but
     * did NOT apply the input.
     * The native input handshake is Connect (96 B) ALONE, then the events.
     * Default: DO NOT send. SHADOW_INPUT_SEND_HELLO=1 restores it (BUG4). */
    {
        const char *e = getenv("SHADOW_INPUT_SEND_HELLO");
        if (e && atoi(e)) {
            uint8_t hello[128];
            memcpy(hello, HELLO_BLOB, sizeof(HELLO_BLOB));
            hello[40] = 2; hello[41] = 0; hello[42] = 0; hello[43] = 0;
            uint32_t hts = monotonic_us();
            hello[48] = (uint8_t)hts; hello[49] = (uint8_t)(hts >> 8);
            hello[50] = (uint8_t)(hts >> 16); hello[51] = (uint8_t)(hts >> 24);
            wolfSSL_write(ssl, hello, sizeof(hello));
            uint8_t pe[136];
            memcpy(pe, POINTER_ENTER_BLOB, sizeof(POINTER_ENTER_BLOB));
            pe[40] = 3; pe[41] = 0; pe[42] = 0; pe[43] = 0;
            uint32_t pts = monotonic_us();
            pe[48] = (uint8_t)pts; pe[49] = (uint8_t)(pts >> 8);
            pe[50] = (uint8_t)(pts >> 16); pe[51] = (uint8_t)(pts >> 24);
            wolfSSL_write(ssl, pe, sizeof(pe));
            vlog("Hello+PointerEnter sent (SHADOW_INPUT_SEND_HELLO=1)");
        } else {
            vlog("Hello+PointerEnter SKIPPED (I8: the native desktop sends none)");
        }
    }

    /* Set the socket recv timeout, so the rx thread does not block */
#ifdef _WIN32
    DWORD tv_ms = 100;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
#else
    struct timeval tv = {0, 100 * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    ctrl_input_tcp_t *c = (ctrl_input_tcp_t *)calloc(1, sizeof(*c));
    if (!c) {
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        return -1;
    }
    c->sock = sock;
    c->ctx  = ctx;
    c->ssl  = ssl;
    c->seq  = 2;  /* I8: Connect=1 alone (no Hello/Pointer) -> events start at 2.
                   * (With SHADOW_INPUT_SEND_HELLO=1 they consume 2 and 3, but that is
                   * the opt-in path.) */
    c->cursor_x = 960;  /* RE5: start at the centre of 1920x1080 (= will move via dx/dy) */
    c->cursor_y = 540;
    c->stats.handshake_ok = true;
    c->stats.bytes_sent   = sizeof(CONNECT_BLOB);
    c->stats.messages_sent = 1;
    c->abort_flag = false;
    c->rx_running = true;
    pthread_mutex_init(&c->tx_mtx, NULL);

    if (pthread_create(&c->rx_thread, NULL, vst_input_rx_thread, c) != 0) {
        vlog("pthread_create FAIL");
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        free(c);
        return -1;
    }

    *out = c;
    return 0;
}

/* S7 2026-08-21 - raw port probe (diagnostic).
 * The LD_PRELOAD hook does not intercept the socket I/O of the official
 * client's input channel (no TCP_READ/TCP_WRITE carries its ciphertext), so its
 * port cannot be deduced from the capture. The only desktop TCP socket with no
 * captured I/O is :base+15. This probe opens a port in the clear, optionally
 * sends a preamble, and logs the first bytes received, to find out what the
 * port actually speaks.
 * SHADOW_PROBE_PORTS="15,16" (comma-separated offsets). */
void ctrl_input_tcp_probe_ports(const char *vm_host, uint16_t base_port,
                                 const uint8_t *preamble, size_t preamble_len) {
    const char *spec = getenv("SHADOW_PROBE_PORTS");
    if (!spec || !*spec || !vm_host) return;
    const char *connect_host = vm_host;
    if (strncmp(vm_host, "ipv6-", 5) == 0) connect_host = vm_host + 5;

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int off = atoi(tok);
        uint16_t port = (uint16_t)(base_port + off);
        char portstr[8];
        snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (session_getaddrinfo(connect_host, portstr, &hints, &res) != 0 || !res) {
            vlog("[S7] :%u resolution KO", (unsigned)port);
            continue;
        }
        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { freeaddrinfo(res); continue; }
        /* WIN1 2026-09-10 - see above. Same 3 s everywhere. */
        shadow_set_sock_timeout(fd, SO_RCVTIMEO, 3000);
        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            vlog("[S7] :%u connect KO (%s)", (unsigned)port, strerror(errno));
            shadow_closesocket(fd); freeaddrinfo(res); continue;
        }
        freeaddrinfo(res);
        if (preamble && preamble_len) {
            ssize_t w = send(fd, preamble, preamble_len, 0);
            vlog("[S7] :%u amorce %zu B envoyee (rc=%zd)", (unsigned)port, preamble_len, w);
        }
        uint8_t rx[64];
        ssize_t n = recv(fd, rx, sizeof(rx), 0);
        if (n <= 0) {
            vlog("[S7] :%u opened, no spontaneous data (rc=%zd %s)",
                 (unsigned)port, n, n < 0 ? strerror(errno) : "peer closed");
        } else {
            char hex[3 * 64 + 1]; int ho = 0;
            for (ssize_t i = 0; i < n && ho < (int)sizeof(hex) - 3; i++)
                ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x ", rx[i]);
            vlog("[S7] :%u a repondu %zd B : %s", (unsigned)port, n, hex);
        }
        shadow_closesocket(fd);
    }
}

int ctrl_input_tcp_open(ctrl_input_tcp_t **out,
                         const char *vm_host, uint16_t base_port) {
    if (!out || !vm_host) return -1;
    *out = NULL;

    const char *connect_host = vm_host;
    if (strncmp(vm_host, "ipv6-", 5) == 0) connect_host = vm_host + 5;

    /* === S11 2026-08-21 - THE INPUT CHANNEL IS DTLS/UDP ON :base+12 ===
     * The capture `captures_inputport_20260821_135207/` (hook extended to
     * writev/readv) gives this contiguous sequence:
     *     UDP_RECVMSG sockfd=366 peer=:14012 len=141
     *     BIO_WRITE   bio=0x75cf24007760      len=141
     *     BIO_READ    bio=0x75cf24007760      len=141
     *     SSL_READ    ssl=0x33a60350          len=104
     * The SSL object 0x33a60350 carries exactly the input traffic: 1833 moves of
     * 144 B, 19 clicks of 152 B, 3 Connects of 96 B, 1212 server replies of
     * 104 B; and `:14012` receives exactly 1212 datagrams of 141 B (= 104 plus
     * DTLS overhead). See KB §3.21.
     *
     * Until now we were sending over TLS/TCP on :base+14 - a port that carries
     * only the ComChan and accepts only ONE TLS session. The server therefore
     * accepted our frames (byte-exact, verified across 4 sessions) without ever
     * applying them or answering a single byte.
     *
     * SHADOW_INPUT_TRANSPORT=tcp goes back to the old transport.
     * The rest of the module is unchanged: everything goes through
     * wolfSSL_read/write. */
    static int g_dtls = -1;
    if (g_dtls < 0) {
        const char *e = getenv("SHADOW_INPUT_TRANSPORT");
        g_dtls = (e && strcmp(e, "tcp") == 0) ? 0 : 1;
    }
    if (g_dtls) {
        int dsock = -1; WOLFSSL_CTX *dctx = NULL; WOLFSSL *dssl = NULL;
        uint16_t dport = (uint16_t)(base_port + 12);
        vlog("opening the input channel over DTLS/UDP on :%u (S11)", (unsigned)dport);
        if (ctrl_dtls_connect_raw(connect_host, dport, &dsock, &dctx, &dssl) != 0) {
            vlog("DTLS input channel :%u - handshake FAILED", (unsigned)dport);
            return -1;
        }
        return input_finish_open(out, dsock, dctx, dssl);
    }

    /* S6 2026-08-21 - the input channel offset is configurable.
     * The LD_PRELOAD capture of the official client shows it opening TWO nearby
     * TCP sockets: fd=317 to :base+14 and fd=385 to :base+15. fd=317 carries
     * only 4 writes and 2 reads = the ComChan (the focus bus, whose id=4 message
     * contains the title of the focused window on the client side).
     * The real input channel (96 B Connect, 152 B clicks, 144 B keyboard, 104 B
     * server replies, ~100 messages) is on the other socket, the only one whose
     * socket I/O the hook never captures -> :base+15.
     * So we were sending input into the ComChan, which explains why the server
     * accepted the frames without ever applying them.
     * SHADOW_INPUT_PORT_OFF=14 goes back to the old behaviour. */
    static int g_off = -1;
    if (g_off < 0) {
        const char *e = getenv("SHADOW_INPUT_PORT_OFF");
        g_off = e ? atoi(e) : 14;
    }
    uint16_t port = (uint16_t)(base_port + g_off);
    vlog("opening %s:%u (base=%u)", connect_host, port, base_port);

    /* TCP + TLS connection: tls_chan.c, shared with the control channel.
     * This module used to duplicate the same sequence with NO timeout on the
     * connect - an unreachable server therefore blocked this thread forever,
     * on the very channel whose latency matters most (input events at
     * 30-60 Hz). */
    tls_chan ch;
    if (!tls_chan_open(&ch, connect_host, port, 8000, "input-tcp")) return -1;
    int sock = ch.sock;
    WOLFSSL_CTX *ctx = ch.ctx;
    WOLFSSL *ssl = ch.ssl;

    vlog("TLS handshake OK cipher=%s", wolfSSL_get_cipher(ssl));

    /* Send the 96 B Connect blob, byte-exact from desktop */
    /* === F2 2026-08-21 - the Connect was a frozen REPLAY ===
     * Diff of the 16 desktop 96 B messages (June capture): 90 of 96 bytes are
     * constant, and its ONLY variable offsets are `@40-41` (seq) and `@48-51`
     * (low timestamp) - so it patches this message every session.
     * We were sending CONNECT_BLOB as-is: seq and timestamp frozen at the values
     * from the original capture, `@52` (high half of the u64 timestamp) at zero.
     * Shadow has a documented anti-replay (see patch_timestamp on the WebRTC
     * path): a replayed Connect can be silently ignored, which would explain why
     * the server sometimes ACKs and then never applies anything.
     * `@83` also differed: desktop 0x02, us 0x01 - the only other divergent
     * constant byte.
     * SHADOW_INPUT_CONNECT_RAW=1 sends the raw blob again. */
    return input_finish_open(out, sock, ctx, ssl);
}

void ctrl_input_tcp_close(ctrl_input_tcp_t *c) {
    if (!c) return;
    c->abort_flag = true;
    if (c->rx_thread) {
        pthread_join(c->rx_thread, NULL);
        c->rx_thread = 0;
    }
    if (c->ssl) {
        wolfSSL_shutdown(c->ssl);
        wolfSSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ctx) {
        wolfSSL_CTX_free(c->ctx);
        c->ctx = NULL;
    }
    if (c->sock >= 0) {
        shadow_closesocket(c->sock);
        c->sock = -1;
    }
    pthread_mutex_destroy(&c->tx_mtx);
    free(c);
}

/* Internal helper: sends a 144 B FlatBuffer message with seq + timestamp + X/Y
 * patched. RE5 2026-05-18: the X/Y offsets were identified by diffing 250
 * plaintext samples from desktop V16. */
/* === C8 2026-08-21 - THE LEFT RELEASE IS A MOVE MESSAGE ===
 * The left click never released: the VM stayed in a permanent drag, which froze
 * the desktop ("the picture ended up frozen", while the stream was still
 * running at ~29 fps). We were looking for the release among the 152 B click
 * messages, where it does not exist - hence the long dead end.
 * It is in fact carried by a MOVE message (144 B) with `@127 = 2` instead of 1.
 * Proof, on the guided capture 031020: the 10 messages of 144 B with `@127=2`
 * each follow a real left release, 10 for 10, offset by +15 to +1131 ms (the
 * Shadow latency). And splitting the holds along the timeline shows it
 * directly: 152 B on press, 144 B on release (a 12.7 s hold -> 152 B at 0.19 s
 * then 144 B at 13.36 s).
 * `mark` = the value to write at @127 (1 = ordinary move). */
static int input_tcp_send_event_marked(ctrl_input_tcp_t *c, uint8_t mark) {
    if (!c || !c->ssl) return -1;
    uint8_t msg[144];
    memcpy(msg, MOUSE_TICK_TEMPLATE, sizeof(MOUSE_TICK_TEMPLATE));
    msg[127] = mark;
    /* I4 FIX 2026-06-02 - seq is @40 (u32 LE), NOT @32. Diff of the MASTER
     * capture (1915 desktop move messages of 144 B): @40 = a clean monotonic
     * counter (3, 4, 5, ...), @32-35 = `00 00 01 03`, STRUCTURAL (constant).
     * The old patch at @32 corrupted the FlatBuffer structure AND froze the real
     * seq at @40, so the server dropped everything (no echo at all).
     * See ts@48 and X/Y@128, which were already correct. */
    pthread_mutex_lock(&c->tx_mtx);
    uint32_t s = c->seq++;
    int cx = c->cursor_x;
    int cy = c->cursor_y;
    pthread_mutex_unlock(&c->tx_mtx);
    msg[40] = (uint8_t)(s & 0xFF);
    msg[41] = (uint8_t)((s >>  8) & 0xFF);
    msg[42] = (uint8_t)((s >> 16) & 0xFF);
    msg[43] = (uint8_t)((s >> 24) & 0xFF);
    /* === I8 2026-08-21 - @52 = 0x0b, a constant field we never filled in ===
     * Byte-by-byte diff of 697 desktop 144 B messages (capture
     * captures_input_20260602_132138, fd=316 on :8014) against 18 of ours: of
     * the desktop's 109 CONSTANT bytes, **only one differs on our side**, offset
     * 52. The desktop puts 0x0b there on 697/697 messages; our template leaves
     * 0x00. A direct candidate for "the server ACKs but does not apply".
     * (The SHADOW_INPUT_F52 toggle mentioned in an earlier version of this
     * comment does not exist: @52 is now written by put_ts64, reverted with
     * SHADOW_INPUT_TS32=1.) */
    /* I10: full u64 timestamp over @48..55 (see monotonic_us64).
     * SHADOW_INPUT_TS32=1 goes back to the old truncated u32. */
    {
        static int g_ts32 = -1;
        if (g_ts32 < 0) {
            const char *e = getenv("SHADOW_INPUT_TS32");
            g_ts32 = e ? atoi(e) : 0;
        }
        if (g_ts32) {
            uint32_t ts = monotonic_us();
            msg[48] = (uint8_t)(ts & 0xFF);
            msg[49] = (uint8_t)((ts >>  8) & 0xFF);
            msg[50] = (uint8_t)((ts >> 16) & 0xFF);
            msg[51] = (uint8_t)((ts >> 24) & 0xFF);
        } else {
            put_ts64(msg, 48);
        }
    }
    /* I5 2026-06-02 - patch @96 (u32 LE), a second monotonic counter (on the
     * desktop @96 increments by 1 per event). Without it (template frozen at
     * @96=0) the server can deduplicate all the identical events, leaving the
     * cursor frozen. */
    msg[96]  = (uint8_t)(s & 0xFF);
    msg[97]  = (uint8_t)((s >>  8) & 0xFF);
    msg[98]  = (uint8_t)((s >> 16) & 0xFF);
    msg[99]  = (uint8_t)((s >> 24) & 0xFF);

    /* RE5 2026-05-18 - patch the X/Y coordinates (bytes 128-131, u16 LE).
     * Validated on 250 plaintext samples from desktop V16 (mouse moved across
     * the Shadow VM screen, giving a continuous trackable trajectory). */
    uint16_t x = (uint16_t)(cx & 0xFFFF);
    uint16_t y = (uint16_t)(cy & 0xFFFF);
    msg[128] = (uint8_t)(x & 0xFF);
    msg[129] = (uint8_t)((x >> 8) & 0xFF);
    msg[130] = (uint8_t)(y & 0xFF);
    msg[131] = (uint8_t)((y >> 8) & 0xFF);

    input_dump_tx(msg, sizeof(msg));
    pthread_mutex_lock(&c->tx_mtx);
    int wr = wolfSSL_write(c->ssl, msg, sizeof(msg));
    if (wr == (int)sizeof(msg)) {
        c->stats.bytes_sent += (uint32_t)wr;
        c->stats.messages_sent++;
    }
    pthread_mutex_unlock(&c->tx_mtx);
    if (wr != (int)sizeof(msg)) {
        vlog("write FAIL rc=%d", wr);
        return -1;
    }
    return 0;
}

static int input_tcp_send_event_internal(ctrl_input_tcp_t *c) {
    return input_tcp_send_event_marked(c, 1);   /* 1 = mouvement ordinaire */
}

/* === C3 2026-08-21 - move to an ABSOLUTE position ===
 * The mouse message already carries the absolute position at `@128-131` (u16
 * LE): the server echoes back exactly the values we write there. Going through
 * deltas was therefore a detour, and a detour that drifts:
 *   - `shadow_input.c` kept ITS OWN tracker, initialised somewhere other than
 *     ours (960,540) -> a constant offset from the very first move;
 *   - every delta clipped by the u16 clamp added a permanent drift, never
 *     recovered since nothing resynchronises the two trackers.
 * User-visible symptom: "a big offset between my mouse, the cursor shown, and
 * where I click". Yet the GUI already computes a correct absolute target
 * (stream_view.cpp maps the window onto the VM resolution) - all that was
 * missing was a path that passed it through unchanged. */
int ctrl_input_tcp_send_mouse_move_abs(ctrl_input_tcp_t *c, int x, int y) {
    if (!c) return -1;
    pthread_mutex_lock(&c->tx_mtx);
    c->cursor_x = x < 0 ? 0 : (x > 0xFFFF ? 0xFFFF : x);
    c->cursor_y = y < 0 ? 0 : (y > 0xFFFF ? 0xFFFF : y);
    pthread_mutex_unlock(&c->tx_mtx);
    return input_tcp_send_event_internal(c);
}

int ctrl_input_tcp_send_mouse_move(ctrl_input_tcp_t *c, int dx, int dy) {
    if (!c) return -1;
    /* Update the cursor position (absolute tracking).
     * Clamped to the u16 range (the byte-exact desktop format). */
    pthread_mutex_lock(&c->tx_mtx);
    c->cursor_x += dx;
    c->cursor_y += dy;
    if (c->cursor_x < 0) c->cursor_x = 0;
    if (c->cursor_y < 0) c->cursor_y = 0;
    if (c->cursor_x > 0xFFFF) c->cursor_x = 0xFFFF;
    if (c->cursor_y > 0xFFFF) c->cursor_y = 0xFFFF;
    pthread_mutex_unlock(&c->tx_mtx);
    return input_tcp_send_event_internal(c);
}

/* RE1 2026-05-18 - helper that sends a byte-exact template through wolfSSL.
 * Patches seq @100 + timestamp @40 + X/Y @132/134 (mouse) or scancode @142
 * (keyboard). For a scancode press/release it also patches bytes 114/116/128
 * (the press toggle).
 * NOTE: `seq_offset` can be 100 (most messages) or 48 (PointerDown, 144 B). */
/* The toggle is read ONCE and shared: both sites that touch the keyboard must
 * see the same value, otherwise the byte and the flag can diverge again. */
static int kbd_set1_actif(void)
{
    static int g_set1 = -1;
    if (g_set1 < 0) {
        const char *es = getenv("SHADOW_KBD_SET1");
        g_set1 = es ? atoi(es) : 1;
    }
    return g_set1;
}

/* Same reason as above: two separate reads of this toggle can diverge, and that
 * is exactly what let the EXTENDED template go out with a RAW evdev byte when it
 * was set to 0. */
static int kbd_v2_actif(void)
{
    static int g_kbd_v2 = -1;
    if (g_kbd_v2 < 0) {
        const char *ek = getenv("SHADOW_INPUT_KBD_V2");
        g_kbd_v2 = ek ? atoi(ek) : 1;
    }
    return g_kbd_v2;
}

static int send_template_with_xy(ctrl_input_tcp_t *c,
                                   const uint8_t *tmpl, size_t tmpl_len,
                                   bool patch_xy, bool patch_keycode,
                                   uint16_t keycode, bool key_pressed) {
    if (!c || !c->ssl || tmpl_len > 256) return -1;
    uint8_t msg[256];
    memcpy(msg, tmpl, tmpl_len);
    pthread_mutex_lock(&c->tx_mtx);
    uint32_t s = c->seq++;
    int cx = c->cursor_x;
    int cy = c->cursor_y;
    pthread_mutex_unlock(&c->tx_mtx);
    /* I4 FIX 2026-06-02 - seq @40 (u32 LE), confirmed byte-exact on the MASTER
     * capture (desktop: monotonic seq at @40 on 144 B moves AND on 152 B
     * click/scroll). BEFORE: seq@100 + ts@40 = a DOUBLE bug (the timestamp
     * overwrote the real seq at @40, so the server saw nonsensical seq values
     * and dropped them). */
    if (tmpl_len >= 44) {
        msg[40] = (uint8_t)(s & 0xFF);
        msg[41] = (uint8_t)((s >>  8) & 0xFF);
        msg[42] = (uint8_t)((s >> 16) & 0xFF);
        msg[43] = (uint8_t)((s >> 24) & 0xFF);
    }
    /* K1 2026-08-21 - keyboard encoding decoded from a guided capture (52
     * timestamped events, known phrases typed; see KB §3.20):
     *     PRESS    @110 = 6
     *     RELEASE  @110 = 0
     *     @142     = evdev scancode  (51/52 exact matches)
     * Modifiers have NO separate state: SHIFT_L is encoded like any other key,
     * with its own scancode 42.
     *
     * What the code did BEFORE, and got wrong: it wrote @114, @116 and @128 with
     * invented press/release values (0x09/0x00, 0x08/0x09, 0x01/0x00) whereas
     * those three offsets are CONSTANT on the desktop (8, 10, 0) - so we were
     * corrupting structural fields. And the real press/release field, @110, was
     * never written: our presses and releases were indistinguishable.
     * Only @142 (the scancode) was correct.
     * SHADOW_INPUT_KBD_V2=0 restores the old behaviour. */
    /* I10 2026-08-21 - timestamp @48 = u64 LE over 8 bytes. This comment already
     * said "u64 LE, low 4 bytes; high @52 kept from the template" - but keeping
     * the template's high half means sending another machine's frozen uptime.
     * We write all 8 bytes. SHADOW_INPUT_TS32=1 to revert. */
    {
        static int g_ts32 = -1;
        if (g_ts32 < 0) {
            const char *e = getenv("SHADOW_INPUT_TS32");
            g_ts32 = e ? atoi(e) : 0;
        }
        if (tmpl_len >= 56 && !g_ts32) {
            put_ts64(msg, 48);
        } else if (tmpl_len >= 52) {
            uint32_t ts = monotonic_us();
            msg[48] = (uint8_t)(ts & 0xFF);
            msg[49] = (uint8_t)((ts >>  8) & 0xFF);
            msg[50] = (uint8_t)((ts >> 16) & 0xFF);
            msg[51] = (uint8_t)((ts >> 24) & 0xFF);
        }
    }
    /* I5 2026-06-02 - @96 (u32 LE) = second monotonic counter (event index). */
    if (tmpl_len >= 100) {
        msg[96]  = (uint8_t)(s & 0xFF);
        msg[97]  = (uint8_t)((s >>  8) & 0xFF);
        msg[98]  = (uint8_t)((s >> 16) & 0xFF);
        msg[99]  = (uint8_t)((s >> 24) & 0xFF);
    }
    /* I4 FIX 2026-06-02 - X/Y at @128/130 (u16 LE), NOT @132/134. Confirmed on
     * the MASTER capture (desktop 152 B click/scroll: X@128, Y@130, the same
     * offsets as the moves). The old @132 shifted the click position by 4
     * bytes. */
    if (patch_xy && tmpl_len >= 132) {
        uint16_t x = (uint16_t)(cx & 0xFFFF);
        uint16_t y = (uint16_t)(cy & 0xFFFF);
        msg[128] = (uint8_t)(x & 0xFF);
        msg[129] = (uint8_t)((x >> 8) & 0xFF);
        msg[130] = (uint8_t)(y & 0xFF);
        msg[131] = (uint8_t)((y >> 8) & 0xFF);
    }
    /* Scancode @142 + state @110 (see the K1 block above) */
    if (patch_keycode && tmpl_len >= 144) {
        if (kbd_v2_actif()) {
            /* K4 2026-08-27 - the EXTENDED keys (arrows, Delete, Windows) do
             * not carry the same number in evdev and in the PC "set 1"
             * scancodes. The rest of the keyboard agrees, which is why only
             * those keys stayed without effect. See kbd_scancode.h for the
             * demonstration and for what a capture still has to confirm.
             * SHADOW_KBD_SET1=0 sends the raw evdev code. */
            /* K7b: the byte comes from kbd_wire_byte, exactly like the extended
             * flag chosen by send_scancode. Deriving the two separately has
             * already made the arrow keys type "8". */
            msg[142] = kbd_set1_actif() ? kbd_wire_byte(keycode, NULL)
                                        : (uint8_t)(keycode & 0xFF);
            /* K7 2026-08-27 - @143 STAYS AT ZERO. The high byte used to hold the
             * 0xE0 prefix; the official client never writes it (measured: 0
             * across 100% of the repo's 22,000+ messages) and carries the
             * extended bit in a separate field, selected by the template.
             * Writing 0xE0 here sent a scancode that does not exist: that is
             * what made the arrow keys inert. */
            msg[110] = key_pressed ? 6 : 0;
            /* @114/@116/@128 are CONSTANT on the desktop (8, 10, 0): we leave
             * them alone, unlike the old code. */

            /* === K3 2026-08-21 - the TWO bytes that were missing ===
             * Ground truth: the maintainer typed "azertyuiop" three times during a
             * capture. On AZERTY those are the physical keys Q..P, hence the
             * consecutive evdev scancodes 16..25 - an ideal signature. The 65
             * isolated events (mouse held still) confirm @142 = scancode and
             * @110 = 6/0, but reveal two fields we were leaving at zero:
             *   @122 = 1 on press, 0 on release (32 presses -> 1, 33 releases
             *          -> 0, without a single exception). We left it at 0, so
             *          every one of our keystrokes looked like a lone release.
             *   @96  = a **per-device** counter, not a global one. Extracted
             *          from the capture, following @96 and @40 (the message
             *          counter, which IS global) over time:
             *              mouse    189/905  190/906  191/907
             *              keyboard   1/909    2/910    3/911 ... 9/917
             *              mouse    192/918  193/919  194/920 ...
             *          The keyboard RESTARTS AT 1 and counts for itself, while
             *          the mouse counter carries on undisturbed. We were writing
             *          the global counter there: our keystrokes carried numbers
             *          the mouse had already used.
             * SHADOW_INPUT_KBD_V3=0 goes back to the previous behaviour. */
            static int g_kbd_v3 = -1;
            if (g_kbd_v3 < 0) {
                const char *e3 = getenv("SHADOW_INPUT_KBD_V3");
                g_kbd_v3 = e3 ? atoi(e3) : 1;
            }
            if (g_kbd_v3) {
                msg[122] = key_pressed ? 1 : 0;
                uint32_t kseq = ++c->kbd_seq;   /* the keyboard counter, starts at 1 */
                msg[96]  = (uint8_t)(kseq & 0xFF);
                msg[97]  = (uint8_t)((kseq >> 8) & 0xFF);
                msg[98]  = (uint8_t)((kseq >> 16) & 0xFF);
                msg[99]  = (uint8_t)((kseq >> 24) & 0xFF);
            }
        } else {
            msg[142] = (uint8_t)(keycode & 0xFF);
            /* K7: same as above. */
            if (key_pressed) {
                msg[114] = 0x09; msg[116] = 0x08; msg[128] = 0x01;
            } else {
                msg[114] = 0x00; msg[116] = 0x09; msg[128] = 0x00;
            }
        }
    }

    input_dump_tx(msg, tmpl_len);
    pthread_mutex_lock(&c->tx_mtx);
    int wr = wolfSSL_write(c->ssl, msg, (int)tmpl_len);
    if (wr == (int)tmpl_len) {
        c->stats.bytes_sent += (uint32_t)wr;
        c->stats.messages_sent++;
    }
    pthread_mutex_unlock(&c->tx_mtx);
    if (wr != (int)tmpl_len) {
        vlog("write FAIL rc=%d (want=%zu)", wr, tmpl_len);
        return -1;
    }
    return 0;
}

/* === C1 2026-08-21 - click encoding, decoded from a guided capture ===
 *
 * Table established on 40 timestamped transitions (2 captures with isolated
 * transitions; see KB §3.20). The message is 152 B in EVERY case - including on
 * release, where we were using a 144 B MOUSEUP_TEMPLATE.
 *
 *   PRESS (3 buttons)   @106=16 @136=8 @138=8 @144=8
 *                       @140 = 0 (left) / 6 (right, middle)
 *                       @150 = 0 left, 1 right, 2 middle
 *   RELEASE             @106=18 @136=0 @138=6 @144=6 @140=8 @150=0
 *                       (the button is NOT identified: the server tracks state)
 *
 * @106/@136/@138/@144 separate press from release with no value in common: this
 * is a structural variant, not a button field.
 *
 * LEFT-BUTTON ASYMMETRY, confirmed 6/6: the official client emits NOTHING on a
 * left release. Checked even when releasing the left button FIRST with the
 * right one still held - so it is the button, not the ordering. We reproduce
 * that behaviour: emitting a left release would be a divergence from the
 * desktop on the very point we have just established.
 *
 * The 134 constant bytes of CLICK_LEFT_TEMPLATE ALREADY match the desktop
 * exactly (measured diff: 0 differences) - only these 6 fields were missing.
 *
 * SHADOW_INPUT_CLICK_V2=0 restores the old behaviour (frozen templates).
 */
int ctrl_input_tcp_send_mouse_button(ctrl_input_tcp_t *c, int button, bool pressed) {
    if (!c) return -1;
    /* button: GLFW codes - 0=left, 1=RIGHT, 2=middle.
     * (This comment used to say "1=middle, 2=right": STALE and wrong, corrected
     * on 2026-08-21, see the detailed block fifteen lines below. It outlived its
     * own refutation and contradicted the code it introduces.) */
    static int g_v2 = -1;
    if (g_v2 < 0) {
        const char *e = getenv("SHADOW_INPUT_CLICK_V2");
        g_v2 = e ? atoi(e) : 1;
    }

    if (!g_v2) {                                   /* ancien comportement */
        const uint8_t *tmpl; size_t tl;
        if (pressed) {
            if (button == 0) { tmpl = CLICK_LEFT_TEMPLATE;  tl = sizeof(CLICK_LEFT_TEMPLATE); }
            else             { tmpl = CLICK_RIGHT_TEMPLATE; tl = sizeof(CLICK_RIGHT_TEMPLATE); }
        } else { tmpl = MOUSEUP_TEMPLATE; tl = sizeof(MOUSEUP_TEMPLATE); }
        return send_template_with_xy(c, tmpl, tl, true, false, 0, false);
    }

    /* GLFW -> protocol index. Correction reviewed 2026-08-21: glfw3.h:589-591
     * defines LEFT=BUTTON_1=0, RIGHT=BUTTON_2=1, MIDDLE=BUTTON_3=2 - and
     * stream_view passes the raw GLFW index through. The previous comment
     * ("1=middle, 2=right") was wrong and swapped right and middle on the wire.
     * The protocol index is 0 left, 1 right, 2 middle (KB §3.20), so the mapping
     * is the IDENTITY. */
    /* === C4 2026-08-21 - the button index is 1-BASED ===
     * Direct proof: cursor brought exactly onto Firefox's "+" button (confirmed
     * by the server echo at (244,22)), then clicked with each index in turn -
     * **only index 1 opens a tab**. So `@150 = 1` is the LEFT button.
     * We were sending 0 for left (the GLFW identity), an index the server
     * ignores: hence "I cannot click on anything".
     * Corollary: the "right click works" the user reported was in fact a LEFT
     * click, since our right button was sending index 1.
     * SHADOW_INPUT_BTN_BASE=0 goes back to the old mapping. */
    static int g_btn_base = -1;
    if (g_btn_base < 0) {
        const char *eb = getenv("SHADOW_INPUT_BTN_BASE");
        g_btn_base = eb ? atoi(eb) : 0;
    }
    int idx = ((button >= 0 && button <= 2) ? button : 0) + g_btn_base;

    /* === C2 2026-08-21 - THE LEFT RELEASE MUST BE SENT ===
     * C1 skipped the left button release, on the strength of captures where
     * left presses `(106=16,140=0,150=0)` were followed by no message at all.
     * Observed consequence on screen: after a single click, the WHOLE Firefox
     * page in the VM ended up selected in blue and scrolling - the signature of
     * a left button held down while the cursor moves. The VM was therefore in a
     * permanent drag, which also made the keyboard useless (the OS is busy with
     * a selection).
     * BUT that reading did not hold: we later established that NO click is
     * injected into the VM at all, so the selection that cleared most likely
     * came from the page refreshing, not from us. And the capture is
     * unambiguous: across two guided captures, the releases
     * `(106=18,140=8,150=0)` exactly equal the number of right and middle
     * presses `(140=6)`, never more - a left press `(140=0,150=0)` is followed
     * by nothing. Default put back to "send nothing", faithful to the capture;
     * SHADOW_INPUT_LEFT_RELEASE=1 sends the generic release anyway, should the
     * stuck-button suspicion come back. */
    /* === C5 2026-08-21 - on which EDGE should the left click be sent? ===
     * Arithmetic of the guided captures (031020): `(18,8,0)` = 7 = exactly the
     * right+middle releases, `(16,6,1)` = 6 = the right presses, `(16,6,2)` = 1
     * = the middle press. For the left button: 10 messages for 10 complete
     * press-release cycles - so ONE message per click, not a pair. What remains
     * unknown is whether it goes out on press or on release.
     * Sending it on press triggers nothing (tested on the Start button), while
     * dragging does work - consistent with a message emitted on release, the
     * "held down" state being carried some other way.
     * SHADOW_INPUT_LEFT_EDGE=press goes back to the rising edge. */
    {
        static int g_left_edge_rel = -1;
        if (g_left_edge_rel < 0) {
            const char *e = getenv("SHADOW_INPUT_LEFT_EDGE");
            /* Default = rising edge, faithful to the capture. Sending on
             * release was tried too: no effect either. */
            g_left_edge_rel = (e && strcmp(e, "release") == 0) ? 1 : 0;
        }
        if (idx == g_btn_base && g_left_edge_rel) {
            if (pressed) return 0;          /* rien a l'appui */
            pressed = true;                 /* the click message goes out on release */
        }
    }
    /* C8 - LEFT RELEASE: a MOVE message marked `@127 = 2`, not a click message.
     * See the C8 block above for the proof.
     * SHADOW_INPUT_LEFT_RELEASE_MOVE=0 goes back to the old behaviour. */
    if (!pressed && idx == g_btn_base) {
        static int g_lrm = -1;
        if (g_lrm < 0) {
            const char *e = getenv("SHADOW_INPUT_LEFT_RELEASE_MOVE");
            g_lrm = e ? atoi(e) : 1;
        }
        if (g_lrm) {
            vlog("[C8] relachement gauche : mouvement marque @127=2");
            return input_tcp_send_event_marked(c, 2);
        }
    }
    if (!pressed && idx == g_btn_base) {
        /* C1 - does the server expect a LEFT button release?
         * On by default: a click with no release leaves the button held down on
         * the VM side, which turns the next move into a drag. Setting the toggle
         * to 0 replays the old behaviour (no message on release), kept so this
         * case can be isolated if a click misbehaves. */
        static int g_lrel = -1;
        if (g_lrel < 0) {
            const char *e = getenv("SHADOW_INPUT_LEFT_RELEASE");
            g_lrel = e ? atoi(e) : 1;
        }
        if (!g_lrel) {
            vlog("[C1] relachement gauche : rien a envoyer");
            return 0;
        }
    }

    /* K13 2026-08-21 - experiment: encode the LEFT press like right and middle
     * (`@140=6`) instead of `@140=0`. The captures do give
     * `(106=16,140=0,150=0)` for a left press, but none of our clicks is applied
     * by the VM, whereas the cursor position is tracked exactly. `@140=6` is the
     * value shared by the two other buttons, the ones that do have a matching
     * release.
     * SETTLED BY REAL USE 2026-08-21: the maintainer reports that the RIGHT and MIDDLE
     * clicks work but the LEFT one does not - and those are precisely the two
     * that use `@140=6`, against `@140=0` for the left. So `@140=6` marks the
     * button event and `@150` carries the index. The capture's
     * `(106=16,140=0,150=0)` is most likely something else (the wheel?), which
     * would also explain why it never has a release.
     * Default moved to 6. SHADOW_INPUT_LEFT_V2=0 goes back to the initial
     * reading. */
    static int g_left_v2_val = -1;
    if (g_left_v2_val < 0) {
        const char *e = getenv("SHADOW_INPUT_LEFT_V2");
        g_left_v2_val = e ? atoi(e) : 0;
    }
    uint8_t t[sizeof(CLICK_LEFT_TEMPLATE)];
    memcpy(t, CLICK_LEFT_TEMPLATE, sizeof(t));     /* base: constants already exact */
    if (pressed) {
        t[106] = 16; t[136] = 8; t[138] = 8; t[144] = 8;
        t[140] = (idx == 0) ? (uint8_t)g_left_v2_val : 6;
        t[150] = (uint8_t)idx;
    } else {
        t[106] = 18; t[136] = 0; t[138] = 6; t[144] = 6;
        t[140] = 8;  t[150] = 0;
    }
    return send_template_with_xy(c, t, sizeof(t), true, false, 0, false);
}

/* Builds the EXTENDED variant of the keyboard template. Derived from the plain
 * template rather than copied: the two forms differ ONLY by these seven bytes,
 * and duplicating 144 bytes would silently let the rest drift apart the day the
 * base template is recaptured. */
static void kbd_gabarit_etendu(uint8_t *dst)
{
    memcpy(dst, KBD_TEMPLATE, sizeof(KBD_TEMPLATE));
    dst[108] = 0x0c;   /* size of the inner table: 14 -> 12 */
    dst[128] = 0x08;   /* vtable: size 8 (two fields) */
    dst[130] = 0x08;   /* size of the table */
    dst[132] = 0x06;   /* decalage du champ scancode */
    dst[134] = 0x05;   /* decalage du champ « etendue » */
    dst[136] = 0x08;   /* the table's soffset to its vtable */
    dst[141] = 0x01;   /* the boolean itself */
}

int ctrl_input_tcp_send_scancode(ctrl_input_tcp_t *c, uint16_t scancode, bool pressed) {
    if (!c) return -1;

    /* K7 - see the big block above KBD_TEMPLATE. `0xE0xx` is our INTERNAL
     * marker, never a wire byte: here it is turned into a choice of template,
     * and the scancode goes out as its single base byte.
     * SHADOW_KBD_ETENDU=0 sends EVERY key on the PLAIN template. That is the
     * fallback should the extended template (seven vtable bytes) ever be
     * refused by the server - it is NOT a return to the pre-K7 behaviour, since
     * the 0xE0 prefix is no longer written anywhere. With this toggle at 0 the
     * extended keys fall back to their keypad namesakes: the Up arrow types
     * "8". */
    static int g_etendu = -1;
    if (g_etendu < 0) {
        const char *e = getenv("SHADOW_KBD_ETENDU");
        g_etendu = e ? atoi(e) : 1;
    }
    /* The extended template only makes sense if the byte is converted: with
     * SHADOW_INPUT_KBD_V2=0 the byte goes out as raw evdev, and giving it the
     * extended flag would make the two diverge again - the K7b defect wearing
     * another toggle. */
    if (!g_etendu || !kbd_v2_actif())
        return send_template_with_xy(c, KBD_TEMPLATE, sizeof(KBD_TEMPLATE),
                                      false, true, scancode, pressed);

    /* K7b - `scancode` is the RAW EVDEV code; the conversion to set 1 happens
     * further down, inside send_template_with_xy. Testing `0xE0` here against
     * the evdev value could therefore NEVER be true: that is what kept the plain
     * template and made the Up arrow type "8" (KP8, the right byte without its
     * flag). So we ask the flag from the very function that produces the byte,
     * and pass the raw code through unchanged. */
    int etendue = 0;
    if (kbd_set1_actif()) (void)kbd_wire_byte(scancode, &etendue);

    if (etendue) {
        uint8_t gabarit[sizeof(KBD_TEMPLATE)];
        kbd_gabarit_etendu(gabarit);
        return send_template_with_xy(c, gabarit, sizeof(gabarit),
                                      false, true, scancode, pressed);
    }
    return send_template_with_xy(c, KBD_TEMPLATE, sizeof(KBD_TEMPLATE),
                                  false, true, scancode, pressed);
}


/* The two SCROLL_UP/DOWN templates that used to sit here are GONE, and what
 * removed them is in this file: W1 (2026-08-21, below) measured the wheel
 * against 124 real detents and found the delta is a **float32 at @148**
 * (+120.0 / -120.0), not the float16 marker at @150-151 those two encoded.
 * They were a superseded hypothesis kept as dead data, contradicted by the
 * template that actually ships. The bytes are not lost - the capture is
 * `captures_scroll_20260821_174331`, and W1's own comment carries the reading
 * that replaced them. */
/* === W1 2026-08-21 - THE WHEEL is a message family of its own ===
 * Guided capture `captures_scroll_20260821_174331` (124 real detents, ground
 * truth timestamped in actions.tsv). Correlation: of 126 messages of 152 B,
 * **125 land within 1.5 s of a detent** and only one is off.
 * The wheel has nothing to do with clicks despite the identical size:
 *   - combination `(106=0, 140=0, 150=240)`, where a click gives (16,.,.);
 *   - **delta as a float32 LE at `@148` = +120.0 for up, -120.0 for down**
 *     (the sign of the float matches the real direction 125 times out of 125);
 *   - `@144-147` = `08 00 00 00`, constant.
 * This family uses the WIDENED vtable (`@16 = 0x20`), so the fields that follow
 * are shifted by 4: the event counter is at `@100`, not at `@96`. Hence its own
 * sender - going through the click patcher would write at the wrong offsets.
 * Across the 125 messages, only `@100` varies apart from the timestamp and the
 * delta: the template is stable and carries no coordinates, the server already
 * knowing where the cursor is. */
static const uint8_t WHEEL_TEMPLATE[152] = {
    0x94, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x20, 0x00, 0x06, 0x00, 0x0c, 0x00, 0x14, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03,
    0x20, 0x00, 0x00, 0x00, 0x9e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x51, 0xa9, 0x51, 0x5a, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x0e, 0x00, 0x07, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x10, 0x00, 0x08, 0x00, 0x07, 0x00, 0x0c, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x08, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x0c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x42,
};

int ctrl_input_tcp_send_mouse_wheel(ctrl_input_tcp_t *c, int direction) {
    if (!c || !c->ssl) return -1;
    uint8_t msg[sizeof(WHEEL_TEMPLATE)];
    memcpy(msg, WHEEL_TEMPLATE, sizeof(msg));

    pthread_mutex_lock(&c->tx_mtx);
    uint32_t s = c->seq++;
    uint32_t ev = ++c->mouse_seq;
    pthread_mutex_unlock(&c->tx_mtx);

    msg[40] = (uint8_t)(s & 0xFF);
    msg[41] = (uint8_t)((s >>  8) & 0xFF);
    msg[42] = (uint8_t)((s >> 16) & 0xFF);
    msg[43] = (uint8_t)((s >> 24) & 0xFF);
    put_ts64(msg, 48);
    msg[100] = (uint8_t)(ev & 0xFF);
    msg[101] = (uint8_t)((ev >>  8) & 0xFF);
    msg[102] = (uint8_t)((ev >> 16) & 0xFF);
    msg[103] = (uint8_t)((ev >> 24) & 0xFF);
    float delta = (direction > 0) ? 120.0f : -120.0f;
    memcpy(msg + 148, &delta, 4);

    pthread_mutex_lock(&c->tx_mtx);
    int wr = wolfSSL_write(c->ssl, msg, (int)sizeof(msg));
    if (wr == (int)sizeof(msg)) { c->stats.bytes_sent += (uint32_t)wr; c->stats.messages_sent++; }
    pthread_mutex_unlock(&c->tx_mtx);
    input_dump_tx(msg, sizeof(msg));
    static int n = 0;
    if (++n <= 3) vlog("[W1] molette %s (delta %+.0f)", direction > 0 ? "haut" : "bas", (double)delta);
    return wr == (int)sizeof(msg) ? 0 : -1;
}

void ctrl_input_tcp_set_audio_cb(ctrl_input_tcp_t *c,
                                  ctrl_input_tcp_audio_cb cb, void *user)
{
    if (!c) return;
    c->audio_cb   = cb;
    c->audio_user = user;
}

void ctrl_input_tcp_get_stats(const ctrl_input_tcp_t *c, ctrl_input_tcp_stats_t *out) {
    if (!c || !out) return;
    pthread_mutex_lock((pthread_mutex_t *)&c->tx_mtx);
    *out = c->stats;
    pthread_mutex_unlock((pthread_mutex_t *)&c->tx_mtx);
}

/* S16 - last cursor position as the SERVER computes it.
 * Returns 0 until a reply has been received. Enables closed-loop positioning,
 * immune to axis direction, to acceleration and to clamping. */
int ctrl_input_tcp_server_pos(ctrl_input_tcp_t *c, int *x, int *y) {
    if (!c || !c->srv_valid) return 0;
    if (x) *x = c->srv_x;
    if (y) *y = c->srv_y;
    return 1;
}
