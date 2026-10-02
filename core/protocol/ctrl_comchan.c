/* ComChan client. See ctrl_comchan.h for the RE context. */
#include "ctrl_comchan.h"
#include "../services/sockets_compat.h"
#include "session_host.h"   /* DNS1: one lookup of the VM name per session */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "../common/log.h"
/* S81 - the log category is DECLARED here, not inferred from the text of the
 * messages. `cclog` stays at INFO: the existing calls do not disappear.
 * `ccdbg` is there for the noisy lines, which move over to it one at a time. */
#define cclog(...) JOURNAL_INFO_(JOURNAL_CAT_SESSION, __VA_ARGS__)
#define ccdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_SESSION, __VA_ARGS__)
struct ctrl_comchan_s {
    int               sock;
    WOLFSSL_CTX      *ctx;
    WOLFSSL          *ssl;
    pthread_t         hb_thread;
    bool              hb_running;
    bool              abort_flag;
    pthread_mutex_t   tx_mtx;
    char              app_name[128];
};

static void put_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >>  8) & 0xff;
    p[3] = (v >>  0) & 0xff;
}

static void put_u16_be(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xff;
    p[1] = (v >> 0) & 0xff;
}

/* === RE-5 2026-09-03 - THE FIELD SPLIT WAS WRONG, AND THE BINARY SETTLES IT ===
 *
 * We wrote `[u32_be type][u32_be sub][u16_be len]`. There is no `sub`, and the
 * length is not where we put it. The official client's own accessors say so:
 *
 *   headerSize() @0xabd440 : `mov eax,0xa ; ret`            -> 10 bytes, fixed
 *   bodySize()   @0xabd460 : `cmp WORD PTR [rsi],0x0`       -> bytes 0..1 must be ZERO
 *                            `jne -> return -1`                a hard gate, not a spare
 *                            `mov eax,DWORD PTR [rsi+0x6]`  -> LENGTH IS A u32 AT OFFSET 6
 *
 * and the serialisers pin the rest to the instruction:
 *
 *   Flush  @0xabd290 : `mov QWORD [rsi],0x1000000` + `mov WORD [rsi+8],0`
 *                      -> 00 00 00 01 00 00 00 00 00 00
 *   Update @0xabd360 : `mov DWORD [rsi],0x20000`, `mov WORD [rsi+2],0x200`,
 *                      `rol ax,8 ; mov WORD [rsi+4],ax`, `mov DWORD [rsi+6],0`
 *                      -> that `rol` IS the byte swap: the u16 fields are
 *                         BIG-ENDIAN on the wire.
 *
 * So the header is FOUR fields, still ten bytes:
 *
 *   [u16_be reserved][u16_be opcode][u16_be format][u32_be length][payload]
 *    0..1             2..3            4..5           6..9
 *
 * Our split was right ONLY BY ACCIDENT, and the accident runs deeper than it
 * first looks. `reserved` and `format` are both 0 today, so `u32 type` read the
 * opcode correctly; and `u16 len at 8` reads the LOW HALF of the real u32, which
 * is the whole value for anything under 65 536 bytes. Measured: the two encoders
 * produce byte-identical headers for every length below 64 KiB.
 *
 * So the break is not at 256 bytes - an earlier note here said so and was wrong
 * by a factor of 256 - it is at **65 536**. A clipboard that big is not exotic:
 * a long document, a spreadsheet selection, a base64 blob. Past it the old model
 * writes a truncated length, the reader consumes the wrong number of bytes, and
 * the framing never resynchronises.
 *
 * `format` is a ClipboardFormatType. The formatter @0x78ece0 names one value,
 * `0 = TEXT`, and the receive handler @0x768080 refuses anything else with
 * "Invalid clipboard format: {}". */
static bool comchan_send(ctrl_comchan_t *c, uint16_t opcode, uint16_t format,
                          const void *payload, uint32_t len) {
    uint8_t buf[10 + 1024];
    if (len > 1024) return false;
    put_u16_be(buf + 0, 0);        /* reserved - non-zero drops the channel */
    put_u16_be(buf + 2, opcode);
    put_u16_be(buf + 4, format);
    put_u32_be(buf + 6, len);
    if (len > 0 && payload) memcpy(buf + 10, payload, len);
    int total = 10 + (int)len;
    pthread_mutex_lock(&c->tx_mtx);
    int wr = wolfSSL_write(c->ssl, buf, total);
    pthread_mutex_unlock(&c->tx_mtx);
    if (wr != total) {
        cclog("comchan: write FAIL rc=%d want=%d opcode=%u", wr, total, opcode);
        return false;
    }
    return true;
}

/* === S52 2026-08-26 - OPCODE 4 IS THE CLIPBOARD CONTENT ===
 *
 * This function used to send `1`, then `2`, then `4` carrying the application
 * name. We believed the channel was a "focus bus" (V15 / TIER 8 U4), hence its
 * old name `send_triplet` and its "focus change pattern" comment.
 *
 * The official client's telemetry names its sockets after their port: this one
 * is called `Clipboard` (KB.md §3.37, verified on two different port bases).
 * Its opcode 4 carries **the clipboard content** - we decoded it in plaintext
 * in both directions. Sending `4` with "Halyard" **replaces the remote
 * machine's clipboard**.
 *
 * Cutting the 7 s heartbeat was the first move, and it was not enough:
 * `ctrl_comchan_open()` sent the same triplet AT OPEN TIME. So opcode 4 is
 * removed from the module entirely. What the official client sends at open -
 * measured - is `0`, `1`, `2`, all of zero length: a handshake, no data.
 *
 * There is deliberately NO toggle to restore the old behaviour: we do not keep
 * a default that destroys the user's data. The day we genuinely want to push a
 * clipboard, it will carry REAL clipboard content, through a function that
 * says so in its name. */
static void send_hello(ctrl_comchan_t *c) {
    /* RE-5: what these two opcodes ARE, now that they are named. 1 is FLUSH
     * ("drop whatever you hold"), 2 is UPDATE ("my clipboard now holds this
     * FORMAT"). Neither carries data, so neither can overwrite anything - which
     * is what made them safe to keep when opcode 4 was removed. The old names
     * in this file, FOCUS_CHANGED and WINDOW_STATE_CHANGED, were a misreading
     * of the same two bytes. */
    comchan_send(c, COMCHAN_OP_FLUSH,  COMCHAN_FORMAT_TEXT, NULL, 0);
    comchan_send(c, COMCHAN_OP_UPDATE, COMCHAN_FORMAT_TEXT, NULL, 0);
}

/* === DANGER 2026-08-26 - THIS THREAD WOULD OVERWRITE THE VM'S CLIPBOARD ===
 *
 * We believed `:base+14` carried a "lifecycle / focus bus", hence this
 * heartbeat republishing `app_name` on it every seven seconds.
 *
 * The official client's telemetry NAMES its sockets after their port: this
 * channel is called `Clipboard` (KB.md §3.37, verified on two different port
 * bases). Its opcode 4 carries the clipboard content - we decoded it in
 * plaintext in both directions. Republishing `app_name` there every seven
 * seconds means **replacing the remote machine's clipboard with the string
 * "Halyard"**, over and over, for the whole session.
 *
 * `SHADOW_COMCHAN` defaults to 0, so nothing happens today; but the toggle
 * exists, and its comment invited turning it back on "if a future RE justifies
 * it". So the heartbeat is cut right here: the RE has now happened, and it
 * says exactly the opposite.
 *
 * `SHADOW_COMCHAN_HEARTBEAT=1` restores it - only use it on a VM whose
 * clipboard you are willing to lose. */
static void *hb_thread_fn(void *arg) {
    ctrl_comchan_t *c = (ctrl_comchan_t *)arg;
    {
        const char *e = getenv("SHADOW_COMCHAN_HEARTBEAT");
        if (!e || atoi(e) == 0) {
            cclog("comchan: heartbeat DISABLED - and this channel now transfers "
                  "nothing at all, see CLIP below");
            return NULL;
        }
        /* === CLIP 2026-10-02 - THIS LINE PROMISED A DATA LOSS IT CANNOT CAUSE
         *
         * It used to announce that the VM's clipboard was "about to be
         * overwritten every 7 s". That was true when it was written; it has not
         * been since S52 (2026-08-26) removed opcode 4 from this module.
         * Decompiling the server settles it three times over:
         *   - only REPLY (opcode 4) writes the VM clipboard, and this module no
         *     longer contains it;
         *   - what the heartbeat does send is FLUSH (1) and UPDATE (2), and the
         *     server SILENTLY IGNORES both - its dispatch has cases for 3 and 4
         *     only;
         *   - the bodies are zero-length anyway.
         * So the heartbeat is harmless and useless: it keeps a socket warm and
         * transfers nothing. A warning that cries data loss on a code path that
         * cannot cause it is worse than no warning - it is what makes the next
         * real one unreadable. (See KB §3.37 and §9, 2026-10-02 CLIP.)
         *
         * The real clipboard lives in `clip_wire.c` / `clip_chan.c`. Adding a
         * REPLY(4) send HERE instead of there would re-create the original
         * defect exactly. */
        cclog("comchan: heartbeat FORCED - harmless: since S52 this module has "
              "no opcode 4, and the server ignores the FLUSH/UPDATE it does send");
    }
    cclog("comchan: hb_thread started");
    while (!c->abort_flag) {
        /* Sleep 7s in 100ms chunks for abort responsiveness. */
        for (int i = 0; i < 70 && !c->abort_flag; i++) {
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        if (c->abort_flag) break;
        send_hello(c);
        static int g_log = 0;
        if (g_log < 5) {
            cclog("comchan: heartbeat triplet sent (app=%s)", c->app_name);
            g_log++;
        }
    }
    c->hb_running = false;
    cclog("comchan: hb_thread exited");
    return NULL;
}

int ctrl_comchan_open(ctrl_comchan_t **out,
                       const char *vm_host, uint16_t base_port,
                       const char *app_name) {
    if (!out || !vm_host) return -1;
    *out = NULL;

    const char *connect_host = vm_host;
    if (strncmp(vm_host, "ipv6-", 5) == 0) connect_host = vm_host + 5;

    uint16_t port = (uint16_t)(base_port + 14);
    cclog("comchan: opening %s:%u (base=%u)", connect_host, port, base_port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);
    int gai = session_getaddrinfo(connect_host, port_str, &hints, &res);   /* DNS1 */
    if (gai != 0 || !res) {
        cclog("comchan: getaddrinfo FAIL gai=%d", gai);
        return -1;
    }
    int sock = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, SOCK_STREAM, 0);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        shadow_closesocket(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) {
        cclog("comchan: connect FAIL errno=%d", shadow_sock_errno());
        return -1;
    }

    /* TLS 1.2 (= confirmed C95 desktop). No SNI, no ALPN, no cert validation. */
    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (!ctx) { shadow_closesocket(sock); return -1; }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);

    WOLFSSL *ssl = wolfSSL_new(ctx);
    if (!ssl) { wolfSSL_CTX_free(ctx); shadow_closesocket(sock); return -1; }
    wolfSSL_set_fd(ssl, sock);

    int rc = wolfSSL_connect(ssl);
    if (rc != WOLFSSL_SUCCESS) {
        int err = wolfSSL_get_error(ssl, rc);
        char errbuf[80] = {0};
        wolfSSL_ERR_error_string((unsigned long)err, errbuf);
        cclog("comchan: TLS handshake FAIL rc=%d err=%d (%s)", rc, err, errbuf);
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        return -1;
    }
    cclog("comchan: TLS handshake OK cipher=%s", wolfSSL_get_cipher(ssl));

    ctrl_comchan_t *c = (ctrl_comchan_t *)calloc(1, sizeof(*c));
    if (!c) {
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); shadow_closesocket(sock);
        return -1;
    }
    c->sock = sock;
    c->ctx = ctx;
    c->ssl = ssl;
    snprintf(c->app_name, sizeof(c->app_name), "%s",
             app_name ? app_name : "Halyard");
    pthread_mutex_init(&c->tx_mtx, NULL);

    /* type=0 init (= 10B all zeros). */
    if (!comchan_send(c, COMCHAN_OP_CONNECT, COMCHAN_FORMAT_TEXT, NULL, 0)) {
        cclog("comchan: init send FAIL");
        ctrl_comchan_close(c);
        return -1;
    }
    cclog("comchan: Connect(0) envoye");

    /* S52: the 1/2 handshake, WITHOUT opcode 4 - see send_hello(). */
    send_hello(c);
    cclog("comchan: handshake 1/2 sent (without opcode 4: "
          "this channel is the VM's clipboard)");

    /* Spawn heartbeat thread. */
    c->hb_running = true;
    c->abort_flag = false;
    if (pthread_create(&c->hb_thread, NULL, hb_thread_fn, c) != 0) {
        cclog("comchan: pthread_create FAIL");
        ctrl_comchan_close(c);
        return -1;
    }

    *out = c;
    return 0;
}

/* `ctrl_comchan_notify_focus()` used to live here: it republished the
 * application name on this channel, that is, on the VM's clipboard. It had no
 * caller outside this module (verified). Removed on 2026-08-26 - see
 * send_hello() for why. */

void ctrl_comchan_close(ctrl_comchan_t *c) {
    if (!c) return;
    c->abort_flag = true;
    if (c->hb_thread) {
        pthread_join(c->hb_thread, NULL);
        c->hb_thread = 0;
    }
    if (c->ssl) { wolfSSL_shutdown(c->ssl); wolfSSL_free(c->ssl); c->ssl = NULL; }
    if (c->ctx) { wolfSSL_CTX_free(c->ctx); c->ctx = NULL; }
    if (c->sock >= 0) { shadow_closesocket(c->sock); c->sock = -1; }
    pthread_mutex_destroy(&c->tx_mtx);
    free(c);
}
