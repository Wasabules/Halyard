/* ctrl_gamepad - see ctrl_gamepad.h for the wire format. */
#include "ctrl_gamepad.h"
#include "rumble_state.h"

#include "../services/log.h"
/* S81 - this module's log category. See shadow/journal.h: the category is
 * declared here, never inferred from the text of the messages. */
#define gplog(...) JOURNAL_INFO_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)
#define gpdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)

#include "../services/sockets_compat.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#define glog(fmt, ...) gplog("[gamepad] " fmt, ##__VA_ARGS__)

static int             g_fd = -1;
static shadow_cipher  *g_cipher = NULL;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned        g_sent = 0;
/* P3 2026-08-22 - COUNTERS PER CATEGORY, not samples.
 * The logs in this file are capped (6 lines, then 24 for the byte trace):
 * twice in a row I drew a FALSE conclusion from them ("no event sent", then
 * "no button sent") when in fact only the log had gone quiet. A counter does
 * not lie. */
static unsigned        g_n_btn = 0, g_n_axis = 0, g_n_dpad = 0, g_n_plug = 0;

/* === MIRROR OF WHAT WE ACTUALLY SENT (2026-08-27) ===
 * The gamepad test screen compares what the CONSOLE reads with what we SEND.
 * That is the only way to see the three faults found that day: axes at the
 * wrong scale (G48), Start invisible because press and release left within the
 * same microsecond (G51), and a key that never went out at all. None of them
 * shows up in a log.
 *
 * Written by the input thread, read by the display thread. These are
 * independent bytes and flags: a read landing between two writes sees a slightly
 * mixed snapshot, which is harmless for a display - and far preferable to a lock
 * on the send path. */
static uint8_t  g_mirror_axes[6] = {128, 128, 128, 128, 0, 0};
static uint16_t g_mirror_buttons = 0;   /* bit N = Shadow identifier N */
static uint8_t  g_mirror_dpad    = 0;

bool ctrl_gamepad_active(void) { return g_fd >= 0 && g_cipher != NULL; }

/* State of the deferred announcement. Reset by `ctrl_gamepad_attach`: a
 * function-scope `static` here would mean a 2nd session never announces at all
 * - that is the family of faults that produced the black screen and the mute
 * audio. */
/* === G51 2026-08-27 - A PULSE HAS TO LAST ===
 * Buttons sent as a pulse (Start, Select) did `button(t,true)` then
 * `button(t,false)` back to back: both UDP packets left within the same
 * microsecond. A game polling its gamepad at 60 Hz never sees the button
 * pressed - the reported symptom is exactly "I don't get the feeling that
 * start is really being sent". So we hold the button long enough for the VM to
 * see it, and release it from the input tick, which runs on every frame. */
#define PULSE_MAX 4
static void drainer_pulses(void);   /* defined below, called by the tick */
static struct { int id; struct timespec echeance; } g_pulses[PULSE_MAX];

static int g_plug_arme = 0;
/* G54: messages dropped for want of a prior announcement. SESSION state - a
 * function-scope `static` would have capped the log from the 2nd session on. */
static uint32_t g_n_avant_annonce = 0;
static long long g_plug_envoyee_ms = -1;   /* -1 = never sent this session */
static struct timespec g_plug_t0 = {0, 0};

/* G45: presence of a REAL physical gamepad (implemented below on the Linux
 * side, stubbed elsewhere). Not to be confused with ctrl_gamepad_active(),
 * which means the channel is open. */
bool ctrl_gamepad_present(void);

/* Encrypts and sends a 14-byte payload. `shadow_cipher_encrypt` produces
 * exactly `[ct][nonce 12][tag 16]`, i.e. 42 bytes - the size we observe. */
static int send_payload(const uint8_t body[14], const char *what)
{
    if (!ctrl_gamepad_active()) return -1;

    /* === G54 2026-08-28 - NOTHING GOES OUT BEFORE THE PLUG-IN ANNOUNCEMENT ===
     *
     * Switch log of 2026-08-28, session 150.5 s -> 220 s:
     *
     *   [150.500] gamepad detected -> plug-in announcement deferred
     *   [150.504] send #00 button released   body= 04 00 00 ... 03 00 00
     *   [150.505] send #01..#08 button released  (the nine other buttons)
     *   [150.506] send #09 d-pad
     *   [151.006] plug-in announcement sent at 503 ms
     *
     * TEN input messages leave BEFORE the announcement: the first pass of the
     * HID forwarder broadcasts the neutral state (all buttons released, d-pad
     * centred) to prime the VM. But KB.md §3.36 is explicit about what the
     * official client does: `b1 = 0` on all its messages "EXCEPT ON THE VERY
     * FIRST MESSAGE OF THE SESSION", which is `04 01 03 02 ...`, the
     * announcement. So for the official client the announcement is the FIRST
     * byte on the channel; for us it arrived eleventh, after inputs describing a
     * device the VM does not know about yet.
     *
     * This is not a regression from today: G50 (2026-08-27) DEFERRED the
     * announcement to 0.5 s - the right value, measured on the official client -
     * without deferring what came before it. That is when the order flipped,
     * which is exactly when the symptom appeared.
     *
     * The guard lives here rather than in the caller: this is the channel's
     * single gateway, and a future sender inherits the rule without having to
     * know about it. The neutral state we drop costs nothing - it describes
     * rest, which is already the default state of the gamepad the VM is about to
     * create.
     *
     * SHADOW_GAMEPAD_STRICT_ORDER=0 restores the previous order. */
    {
        static int g_ordre_strict = -1;
        if (g_ordre_strict < 0) {
            const char *e = getenv("SHADOW_GAMEPAD_STRICT_ORDER");
            g_ordre_strict = e ? atoi(e) : 1;
        }
        /* The announcement is the only message whose byte 1 is 1 (KB §3.36). */
        const int est_annonce = (body[1] == 0x01);
        /* === G55 2026-08-28 - GATE ON "THE kPlug HAS GONE OUT", NOT ON ARMING ===
         * G54 gated on `g_plug_arme`, which is 1 only between an attach WITH a
         * gamepad detected and the send itself. That left two holes: with no
         * gamepad detected the guard was inert, and the two external callers of
         * `ctrl_gamepad_plug()` (pause menu, developer menu) bypass the tick and
         * never set the latch. `g_plug_envoyee_ms < 0` covers all three cases and
         * stays SESSION state (reset to -1 by `ctrl_gamepad_attach`). */
        if (g_ordre_strict && !est_annonce && g_plug_envoyee_ms < 0) {
            if (++g_n_avant_annonce <= 3)
                glog("%s DROPPED: the plug-in announcement has not gone out yet "
                     "(G54 — l'officiel annonce en tout premier)", what);
            return -1;
        }
    }
    uint8_t pkt[14 + SHADOW_AEAD_OVERHEAD];
    memcpy(pkt, body, 14);
    /* === P2 2026-08-22 - TRACE THE EXACT BYTES WE SEND ===
     * The gamepad works on Linux but not on Switch, even though both go through
     * this very function. The difference can therefore only be in the VALUES. We
     * log the 14-byte body in the clear (before encryption) for the first few
     * messages: a diff between the two platforms will point at the discrepancy,
     * instead of us arguing about it. */
    {
        /* P4: we skip the axes - they flooded the trace (2620 of them on Linux)
         * before a single button showed up. What we want to compare across the
         * two platforms are the button IDENTIFIERS and the d-pad masks. */
        static int n = 0;
        if (n < 40 && body[2] != 0x01) {
            char hex[3 * 14 + 1]; int o = 0;
            for (int i = 0; i < 14; i++) o += snprintf(hex + o, sizeof(hex) - o, "%02x ", body[i]);
            glog("[P2] send #%02d %-24s body= %s", n, what, hex);
            n++;
        }
    }
    /* === G55 - WHAT IS THE FIRST BYTE WE PUT ON THIS CHANNEL? ===
     * Answering that took cross-referencing three families of log lines. The
     * server tears the gamepad channel down when we talk on it within the ~500 ms
     * following the handshake (5 sessions out of 5); the official client does not
     * even open the socket before handshake+518 ms. This single line therefore
     * says whether the invariant holds. */
    if (g_sent == 0)
        glog("[G55] FIRST byte of the gamepad channel: %s (14 B plaintext)", what);

    /* AF1 - fault injection, OFF by default. SHADOW_DIAG_AF1_WIDEN_MS=<ms>
     * sleeps HERE, between the unlocked `ctrl_gamepad_active()` test above and
     * the lock below: exactly the window the race lives in, widened from
     * microseconds to something a session close can land in. With it, the
     * re-test below must REFUSE the send (logged `[AF1]`) instead of
     * encrypting with a destroyed cipher. On a desktop without ASan it is the
     * only way to make that path run at all. A toggle cache, not session
     * state. */
    {
        static int widen = -1;
        if (widen < 0) {
            const char *e = getenv("SHADOW_DIAG_AF1_WIDEN_MS");
            widen = e ? atoi(e) : 0;
        }
        if (widen > 0) usleep((unsigned)(widen * 1000));
    }
    pthread_mutex_lock(&g_mtx);
    /* === AF1 2026-09-10 - THE REAL TEST IS UNDER THE LOCK ===
     *
     * `ctrl_gamepad_active()` at the top of this function runs OUTSIDE the
     * lock. An emitter could pass it, the session teardown then destroy the
     * cipher, and the line below encrypt with freed memory: a mutex already
     * destroyed in encryption.c, a nonce incremented inside a block handed back
     * to the allocator. Reordering the teardown alone only NARROWS that window;
     * testing again here, where `detach` cannot run concurrently, closes it.
     * The shape of BUG2 - a teardown with no guard on the reader's side -
     * applied to the cipher. */
    if (g_fd < 0 || !g_cipher) {
        pthread_mutex_unlock(&g_mtx);
        glog("[AF1] send refused after detach (re-tested under the lock): %s", what);
        return -1;
    }
    int total = shadow_cipher_encrypt(g_cipher, pkt, 14);
    int rc = -1;
    if (total == (int)sizeof(pkt)) {
        rc = (send(g_fd, (const char *)pkt, (size_t)total, 0) == total) ? 0 : -1;
        if (rc == 0) g_sent++;
    }
    pthread_mutex_unlock(&g_mtx);
    if (total != (int)sizeof(pkt))
        glog("encryption FAILED for %s (rc=%d)", what, total);
    else if (g_sent <= 6)
        glog("%s -> %d B chiffres", what, total);
    return rc;
}

int ctrl_gamepad_plug(void)
{
    g_n_plug++;
    /* `04 01 03 <type>` + zeros: plug-in announcement, 1st message of the
     * session.
     *
     * === G56 2026-08-28 - BYTE 3 IS THE GAMEPAD TYPE ===
     *
     * It was hardcoded to `02` from the guided capture of 26/08 - which was made
     * with a DualShock 4. The official binary carries, in one contiguous pool:
     *
     *   Google, Microsoft, Nintendo, Logitech, Default,
     *   ControllerRight, ControllerLeft, Dualshock4, XboxOne, Xbox360
     *
     * and those names are NOT sorted alphabetically, unlike the pools that
     * trapped K13 and K14: this one can therefore carry declaration order. Under
     * that reading `Dualshock4 = 2`, which matches exactly the byte we copied
     * from a DualShock 4 capture.
     *
     * The official launcher exposes an "Improved compatibility" setting whose
     * text reads: "if your PlayStation controller is not detected in game, turn
     * this on to emulate an Xbox (XInput) controller". On the VM side the stack
     * is ViGEm - the neighbouring error codes say so
     * (`XusbUserIndexOutOfRange`, `BusAlreadyConnected`).
     *
     * ENUM CONFIRMED on console on 2026-08-28: with value 0, Steam inside the VM
     * shows "Xbox 360 Controller". The reading is therefore the REVERSE of the
     * pool order:
     *
     *   Xbox360 = 0   XboxOne = 1   Dualshock4 = 2
     *   ControllerLeft = 3   ControllerRight = 4   Default = 5
     *
     * The server REFUSES anything above 5: it returns a kReply carrying its
     * error flag (byte 1 = 1, byte 7 = code) and emits NO kVibration at all -
     * the rumble burst only follows an accepted announcement, which is the proof
     * that the VM created the device.
     *
     * DEFAULT 0 (XInput), no longer 2. The 2 came from the guided capture of
     * 26/08, made with a DualShock 4: we were therefore announcing a PlayStation
     * controller, which is precisely the case Shadow's "Improved compatibility"
     * setting exists to fix ("if your PlayStation controller is not detected in
     * game, turn this on to emulate an Xbox controller"). The Switch is neither
     * a DS4 nor an Xbox; announcing XInput is the choice Windows games support
     * best.
     *
     * Adjustable in the UI (Settings -> Gamepad -> announced gamepad type);
     * `SHADOW_GAMEPAD_TYPE=<n>` still wins. */
    static int g_type = -1;
    if (g_type < 0) {
        const char *e = getenv("SHADOW_GAMEPAD_TYPE");
        g_type = e ? atoi(e) : 0;
        if (g_type < 0 || g_type > 5) g_type = 0;
    }
    uint8_t b[14] = {0};
    b[0] = 0x04; b[1] = 0x01; b[2] = 0x03; b[3] = (uint8_t)g_type;
    const int rc = send_payload(b, "annonce de branchement");
    /* G55: the latch is set HERE and not in the tick, so that the external
     * callers (pause menu, developer menu) set it too. The tick then overwrites
     * it with the real delay, which is the useful piece of information. */
    if (rc == 0 && g_plug_envoyee_ms < 0) g_plug_envoyee_ms = 0;
    return rc;
}

/* === G50 2026-08-27 - THE SINGLE, DEFERRED ANNOUNCEMENT (see ctrl_gamepad_attach) ===
 *
 * The history of this point, because it cost two round trips:
 *   P6 (22/08) repeated the announcement at 1 s / 3 s / 6 s, assuming that "one
 *     extra announcement is harmless". False: Steam showed FOUR of them, and a
 *     game binding to a phantom instance stopped receiving anything - hence
 *     "works on the test site, dead in the game".
 *   G49 (27/08) therefore removed them all. Result: no gamepad at all. Which
 *     PROVES that the announcement from `ctrl_gamepad_attach`, sent at 0.19 s,
 *     was not landing - only the repeats worked.
 *   G50 therefore keeps G49's SINGLE send and moves it to the moment the
 *     official client sends it (~0.5 s), which is the difference P6 had measured
 *     without drawing the conclusion from it.
 *
 * If the gamepad goes missing again, raise `SHADOW_GAMEPAD_DELAY_MS` before
 * adding a repeat back: every repeat costs a phantom gamepad. */
void ctrl_gamepad_replug_tick(void)
{
    if (!ctrl_gamepad_active()) return;
    drainer_pulses();
    if (!g_plug_arme) return;

    static int delay = -1;
    if (delay < 0) {
        const char *e = getenv("SHADOW_GAMEPAD_DELAY_MS");
        delay = e ? atoi(e) : 500;   /* measured on the official client: ~0.51 s */
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (g_plug_t0.tv_sec == 0) { g_plug_t0 = now; return; }

    const long long dt = (long long)(now.tv_sec - g_plug_t0.tv_sec) * 1000
                       + (now.tv_nsec - g_plug_t0.tv_nsec) / 1000000;
    if (dt < delay) return;

    g_plug_arme = 0;                 /* ONCE only - that is the whole point */
    g_plug_envoyee_ms = dt;
    glog("annonce de branchement envoyee a %lld ms", dt);
    ctrl_gamepad_plug();
}

/* Presses `button_id` now, releases it in ~`PULSE_MS`.
 * `SHADOW_PAD_PULSE_MS` sets the duration: raise it if a game still misses the
 * press, lower it if the button feels "stuck". */
int ctrl_gamepad_button_pulse(int button_id)
{
    if (ctrl_gamepad_button(button_id, true) != 0) return -1;

    static int ms = -1;
    if (ms < 0) {
        const char *e = getenv("SHADOW_PAD_PULSE_MS");
        ms = e ? atoi(e) : 80;   /* ~5 frames at 60 Hz: every game sees it */
    }

    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    t.tv_sec  += ms / 1000;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }

    for (int i = 0; i < PULSE_MAX; i++) {
        if (g_pulses[i].id < 0) { g_pulses[i].id = button_id; g_pulses[i].echeance = t; return 0; }
    }
    /* Queue full: we release right away rather than leave the button held
     * forever - a stuck button is far worse than a missed press. */
    ctrl_gamepad_button(button_id, false);
    return 0;
}

/* Releases the pulses whose deadline has passed. Called by the input tick. */
static void drainer_pulses(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = 0; i < PULSE_MAX; i++) {
        if (g_pulses[i].id < 0) continue;
        const struct timespec *e = &g_pulses[i].echeance;
        if (now.tv_sec > e->tv_sec
            || (now.tv_sec == e->tv_sec && now.tv_nsec >= e->tv_nsec)) {
            ctrl_gamepad_button(g_pulses[i].id, false);
            g_pulses[i].id = -1;
        }
    }
}

int ctrl_gamepad_button(int button_id, bool pressed)
{
    g_n_btn++;
    if (button_id < 0 || button_id > 10) return -1;
    if (pressed) g_mirror_buttons |=  (uint16_t)(1u << button_id);
    else         g_mirror_buttons &= (uint16_t)~(1u << button_id);
    uint8_t b[14] = {0};
    b[0] = 0x04; b[2] = 0x00;
    b[11] = (uint8_t)button_id;
    b[13] = pressed ? 1 : 0;
    return send_payload(b, pressed ? "bouton enfonce" : "bouton relache");
}

/* === SRV6 2026-10-02 — @12..13 IS ONE SIGNED 16-BIT VALUE ====================
 *
 * The old comment said byte 13 "carries the same value in the other
 * representation (constant offset of 128)" and called it measured but
 * unexplained. It is neither a second representation nor a checksum: bytes 12
 * and 13 are a single **little-endian int16**, and `(value, value+128)` happens
 * to encode `257*value + 32768 (mod 2^16)`.
 *
 * Read off ShadowStreamer 6.3.1,
 * `Controller::Clients::SufpClientV4::DealWithInput` @0x140c53f70, kind 1:
 *   - trigger axes (mapped type 4 or 5):
 *         (double)i16@12 * 0.00390625 + 128.0      -> 0..255
 *   - stick axes: the raw u16@12 is forwarded, and the consumer reads it as the
 *     signed thumb value XUSB expects.
 * Check the arithmetic on our encoding: value 0 -> i16 -32768 -> 0;
 * value 128 -> i16 128 -> 128; value 255 -> i16 32767 -> 255. It round-trips
 * exactly, which is why the 8-bit path has always worked - it just uses 256 of
 * the 65,536 values the wire carries.
 *
 * So `ctrl_gamepad_axis` below is CORRECT and stays the default. What is new is
 * `ctrl_gamepad_axis16`, for a caller that has more than 8 bits to give (the
 * Switch samples its sticks at 16). NOTE: nothing calls it yet - the pad
 * pipeline (`core/input/pad_forward.cpp`, `padmap`) is uint8_t end to end, and
 * widening it is a separate change on two consoles that cannot be tested from a
 * Windows checkout. The entry point exists so that change is a rewiring and not
 * a re-derivation. */
int ctrl_gamepad_axis(int axis_idx, uint8_t value)
{
    g_n_axis++;
    if (axis_idx < 0 || axis_idx > 5) return -1;
    g_mirror_axes[axis_idx] = value;
    uint8_t b[14] = {0};
    b[0] = 0x04; b[2] = 0x01;
    b[11] = (uint8_t)axis_idx;
    b[12] = value;
    /* = the low half of the int16 `257*value + 32768`; see SRV6 above. */
    b[13] = (uint8_t)((value + 128) & 0xFF);
    return send_payload(b, "axe");
}

int ctrl_gamepad_axis16(int axis_idx, int16_t value)
{
    g_n_axis++;
    if (axis_idx < 0 || axis_idx > 5) return -1;
    /* The mirror stays 8-bit: it feeds the developer menu's display and the
     * replug state, both of which only ever showed 0..255. The coarse value is
     * the same transform the server applies to the triggers. */
    g_mirror_axes[axis_idx] = (uint8_t)(((int)value + 32768) >> 8);
    uint8_t b[14] = {0};
    b[0] = 0x04; b[2] = 0x01;
    b[11] = (uint8_t)axis_idx;
    const uint16_t u = (uint16_t)value;
    b[12] = (uint8_t)(u & 0xFF);
    b[13] = (uint8_t)(u >> 8);
    return send_payload(b, "axe16");
}

int ctrl_gamepad_dpad(uint8_t value)
{
    g_n_dpad++;
    uint8_t b[14] = {0};
    g_mirror_dpad = value;
    b[0] = 0x04; b[2] = 0x02; b[3] = value;
    return send_payload(b, "croix directionnelle");
}

/* Returns what was ACTUALLY sent, for the test screen. See the mirror above:
 * it is the comparison with what the console reads that reveals a fault between
 * reading and sending. */
/* === A DIAGNOSTIC READABLE FROM THE PAUSE MENU (2026-08-27) ===
 *
 * Reported symptom: "first connection, gamepad perfect; I disconnect and
 * reconnect, and the gamepad is no longer declared". Nothing in the UI let you
 * find out WHY - the status line said "gamepad read / channel open", but on
 * console the first half always returns true and the second says nothing about
 * the ANNOUNCEMENT, which is precisely what creates the gamepad on the VM side.
 *
 * This line answers the three questions in the order they arise:
 *   1. is the :base+13 channel even open? (if not, `ctrl_gamepad_attach` was
 *      never called: UDP registration failed, and NO announcement will go out
 *      this session);
 *   2. has the announcement gone out, and when?
 *   3. are we still sending anything? (the per-category counters) */
void ctrl_gamepad_diagnostic(char *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    if (!ctrl_gamepad_active()) {
        snprintf(buf, cap, "channel CLOSED - no announcement possible");
        return;
    }
    if (g_plug_envoyee_ms >= 0)
        snprintf(buf, cap, "annonce a %lld ms — %u msg (b%u a%u c%u)",
                 g_plug_envoyee_ms, g_sent, g_n_btn, g_n_axis, g_n_dpad);
    else if (g_plug_arme)
        snprintf(buf, cap, "announcement ARMED, waiting out the delay");
    else
        snprintf(buf, cap, "announcement NOT ARMED - no gamepad will be created");
}

void ctrl_gamepad_last_sent(uint8_t axes[6], uint16_t *buttons, uint8_t *dpad)
{
    if (axes) for (int i = 0; i < 6; i++) axes[i] = g_mirror_axes[i];
    if (buttons) *buttons = g_mirror_buttons;
    if (dpad)    *dpad    = g_mirror_dpad;
}

void ctrl_gamepad_attach(int udp_fd, shadow_cipher *cipher)
{
    /* AF1 2026-09-10 - the pair is published under `g_mtx`, the lock `detach`
     * clears it under and `send_payload` re-tests it under. An emitter can
     * OUTLIVE a session - the Switch `fil_manette` is joined only in
     * ~StreamView - and be inside `send_payload` while a reconnection attaches
     * the next session. */
    pthread_mutex_lock(&g_mtx);
    g_fd = udp_fd; g_cipher = cipher;
    pthread_mutex_unlock(&g_mtx);
    g_sent = 0;
    g_plug_arme = 0; g_plug_t0.tv_sec = 0; g_plug_t0.tv_nsec = 0;
    g_plug_envoyee_ms = -1;
    g_n_avant_annonce = 0;
    g_mirror_buttons = 0; g_mirror_dpad = 0;
    for (int i = 0; i < 6; i++) g_mirror_axes[i] = (i < 4) ? 128 : 0;
    for (int i = 0; i < PULSE_MAX; i++) g_pulses[i].id = -1;
    glog("attached to the :base+13 socket (fd=%d)", udp_fd);
    /* G45 2026-08-22 - ANNOUNCE A GAMEPAD ONLY IF THERE IS ONE.
     * We used to send the announcement on every session: the VM then created a
     * permanent virtual gamepad, games switched to gamepad mode and ignored
     * keyboard and mouse ("stuck in game"). We now announce only when a physical
     * gamepad is detected. SHADOW_GAMEPAD_PLUG=1 forces the announcement (gamepad
     * not detected but wanted), =0 forbids it; the developer menu keeps the
     * manual "Announce plug-in" action. */
    /* === G58 2026-09-14 - READ THE TOGGLE PER SESSION, NOT PER PROCESS ===
     *
     * This was a function `static` resolved at the FIRST attach of the process.
     * `Settings::applyToggles()` sets SHADOW_GAMEPAD_PLUG from the settings
     * screen, so turning the gamepad on and reconnecting left the OLD answer in
     * place: the setting appeared to do nothing, and the only way to make it
     * take was to quit and relaunch the application. Reported from the console:
     * "I cannot enable my gamepad".
     *
     * That is the family this repository names at its head - session state in a
     * function `static` - and it has now produced a black screen, sound heard
     * once, mute hardware detectors, a resolution never re-announced, and this.
     * The attach happens once per session and a `getenv` costs nothing there. */
    const char *plug_env = getenv("SHADOW_GAMEPAD_PLUG");
    const int   g_force  = plug_env ? atoi(plug_env) : -1;
    bool present = ctrl_gamepad_present();
    if (g_force == 1 || (g_force != 0 && present)) {
        /* === G50 2026-08-27 - ONE ANNOUNCEMENT ONLY, BUT AT THE RIGHT MOMENT ===
         * G49 removed P6's repeats because each of them created one more gamepad
         * (four of them in Steam). But the result was NO gamepad at all: so it
         * really was a repeat that was landing, not the initial announcement.
         *
         * P6's measurement says why: we announce **0.19 s** after the channel
         * announcements, the official client **0.51 s**. At 0.19 s the server has
         * not finished opening the input channel and the announcement falls into
         * the void. The "just repeat it" reflex fixed the symptom at the price of
         * one gamepad per repeat.
         *
         * The official client sends exactly ONE, as the first message of the
         * session (KB §3.36): we do the same, deferring it to give the server
         * time to open the channel. `SHADOW_GAMEPAD_DELAY_MS` sets the delay. */
        g_plug_arme = 1;
        glog("gamepad %s -> the plug-in announcement is deferred", present ? "detected" : "forced");
    } else {
        g_plug_arme = 0;
        glog("no gamepad detected -> no announcement (keyboard/mouse preserved)");
    }
}

/* Self-test: sends a known sequence (Cross, then a left-stick sweep) to check
 * the wire without depending on a gamepad being plugged in.
 * SHADOW_GAMEPAD_SELFTEST=1 */
int ctrl_gamepad_selftest(void)
{
    const char *e = getenv("SHADOW_GAMEPAD_SELFTEST");
    if (!e || atoi(e) == 0 || !ctrl_gamepad_active()) return 0;
    glog("self-test: Cross, then a sweep of the left stick");
    ctrl_gamepad_button(SHADOW_PAD_CROSS, true);
    ctrl_gamepad_button(SHADOW_PAD_CROSS, false);
    for (int v = 128; v <= 255; v += 16) ctrl_gamepad_axis(SHADOW_AXIS_LX, (uint8_t)v);
    for (int v = 255; v >= 0;  v -= 16) ctrl_gamepad_axis(SHADOW_AXIS_LX, (uint8_t)v);
    ctrl_gamepad_axis(SHADOW_AXIS_LX, 128);
    return 0;
}

/* Probe: sweeps ONE SINGLE axis index, in a loop, so that the user can see on
 * screen which stick and which direction move. The two vertical indices were the
 * only weak correlations in the decoding (deviation 24.7 and 25.6 against 0.1
 * for the horizontal ones): telling them apart by elimination turned out to be
 * wrong, so we measure instead of assuming.
 * SHADOW_PAD_AXIS_PROBE=<0..5>, or the developer menu bar. */
static volatile int g_probe_axis    = -1;   /* indice balaye, -1 = arret */
static volatile int g_probe_running = 0;

static void *probe_thread(void *unused)
{
    (void)unused;
    int announced = -1;
    while (ctrl_gamepad_active()) {
        int idx = g_probe_axis;
        if (idx < 0) { announced = -1; usleep(100000); continue; }
        if (idx != announced) {
            announced = idx;
            glog("SONDE : balayage de l'indice d'axe %d — regarde quel stick bouge", idx);
        }
        /* We re-read g_probe_axis at every step: switching probe from the menu
         * takes effect immediately, without waiting for the sweep to finish. */
        for (int v = 128; v <= 255 && g_probe_axis == idx && ctrl_gamepad_active(); v += 8) {
            ctrl_gamepad_axis(idx, (uint8_t)v); usleep(40000);
        }
        for (int v = 255; v >= 0  && g_probe_axis == idx && ctrl_gamepad_active(); v -= 8) {
            ctrl_gamepad_axis(idx, (uint8_t)v); usleep(40000);
        }
        for (int v = 0; v <= 128 && g_probe_axis == idx && ctrl_gamepad_active(); v += 8) {
            ctrl_gamepad_axis(idx, (uint8_t)v); usleep(40000);
        }
        for (int i = 0; i < 7 && g_probe_axis == idx && ctrl_gamepad_active(); i++)
            usleep(100000);
    }
    g_probe_running = 0;
    return NULL;
}

int ctrl_gamepad_axis_probing(void)
{
    return g_probe_axis;
}

int ctrl_gamepad_axis_sweep(int idx)
{
    if (idx > 5) return -1;
    if (idx < 0) idx = -1;              /* any negative value = stop */
    g_probe_axis = idx;
    if (idx < 0) return 0;
    if (!ctrl_gamepad_active()) return -1;
    /* One thread only, however many times the probe is switched: it follows
     * g_probe_axis. Without this, every click in the menu would stack another
     * one and the sweeps would trample each other. */
    if (!g_probe_running) {
        pthread_t t;
        g_probe_running = 1;
        if (pthread_create(&t, NULL, probe_thread, NULL) != 0) {
            g_probe_running = 0;
            return -1;
        }
        pthread_detach(t);
    }
    return 0;
}

/* RE probe: sweeps one gamepad axis across its full range to watch what the VM
 * does with it, while looking for the mapping between our axis codes and the
 * ones the server expects. Purely manual - SHADOW_PAD_AXIS_PROBE=<axis>
 * triggers a single sweep. Without the variable, it does nothing. */
int ctrl_gamepad_axis_probe(void)
{
    const char *e = getenv("SHADOW_PAD_AXIS_PROBE");
    if (!e) return 0;
    return ctrl_gamepad_axis_sweep(atoi(e));
}

void ctrl_gamepad_detach(void)
{
    glog("detache — %u messages emis (branchements=%u buttons=%u axes=%u croix=%u)",
         g_sent, g_n_plug, g_n_btn, g_n_axis, g_n_dpad);
    /* G55: the DOWNSTREAM summary must be printed EVEN when it is zero. G53's
     * counters only spoke when a packet arrived: a log with no [G53] line could
     * not tell "the server sent nothing" from "this binary does not contain
     * G53". */
    ctrl_session_log_gamepad_rx();
    /* === G57 - SWITCH IT OFF ON THE WAY OUT ===
     * The protocol carries NO duration: a rumble holds until the next message.
     * And the last message of a session is often NON-ZERO - `04 00 07 ff 00` was
     * seen at the end of a log, big motor at full power. Without this line the
     * gamepad would keep vibrating after the stream ends, until the user puts it
     * down. Switching off is a link in the chain, not a finishing touch. */
    rumble_state_stop();
    /* AF1 - cleared under the lock `send_payload` holds while it encrypts:
     * when this returns, no emitter is between its test and its use of the
     * cipher. The session destroys the cipher only AFTER this call
     * (ctrl_session.c), which is the other half of the fix. */
    pthread_mutex_lock(&g_mtx);
    g_fd = -1; g_cipher = NULL;
    pthread_mutex_unlock(&g_mtx);
}

/* ===================== local evdev reader (Linux) =====================
 *
 * The condition NAMES the platforms that have evdev instead of subtracting the
 * two that do not. `!__SWITCH__ && !_WIN32` meant "Linux" only for as long as
 * there were three platforms: a PS Vita build fell in here and stopped on
 * `linux/input.h`. It has SceCtrl, which Borealis already reads, and no
 * business anywhere near this file. */
#if defined(__linux__) || defined(__FreeBSD__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

static pthread_t        g_rd_thread;
static volatile int     g_rd_running = 0;
static volatile int    *g_rd_abort = NULL;

/* evdev -> Shadow identifiers. Table established by correlation on a
 * DualShock 4 (KB.md §3.25): each button yielded exactly one identifier. */
static int map_button(int code)
{
    switch (code) {
    case BTN_WEST:   return SHADOW_PAD_SQUARE;      /* 0x134 */
    case BTN_NORTH:  return SHADOW_PAD_TRIANGLE;   /* 0x133 */
    case BTN_SOUTH:  return SHADOW_PAD_CROSS;      /* 0x130 */
    case BTN_EAST:   return SHADOW_PAD_CIRCLE;     /* 0x131 */
    case BTN_THUMBR: return SHADOW_PAD_R3;
    case BTN_THUMBL: return SHADOW_PAD_L3;
    case BTN_TR:     return SHADOW_PAD_R1;
    case BTN_TL:     return SHADOW_PAD_L1;
    case BTN_START:  return SHADOW_PAD_START;
    case BTN_SELECT: return SHADOW_PAD_SELECT;
    case BTN_MODE:   return SHADOW_PAD_GUIDE;
    default:         return -1;
    }
}

/* evdev -> Shadow axis index. Measured mapping: wire 0=RX, 1=LY, 2=LX, 3=RY,
 * 4=L2, 5=R2; on the evdev DualShock side 0=LX 1=LY 2=L2 3=RX 4=RY 5=R2. */
static int g_swap_y = -1;
static int map_axis(int code)
{
    if (g_swap_y < 0) {
        const char *e = getenv("SHADOW_PAD_SWAP_Y");
        g_swap_y = e ? atoi(e) : 0;
    }
    switch (code) {
    case ABS_X:  return SHADOW_AXIS_LX;
    case ABS_Y:  return g_swap_y ? SHADOW_AXIS_RY : SHADOW_AXIS_LY;
    case ABS_Z:  return SHADOW_AXIS_L2;
    case ABS_RX: return SHADOW_AXIS_RX;
    case ABS_RY: return g_swap_y ? SHADOW_AXIS_LY : SHADOW_AXIS_RY;
    case ABS_RZ: return SHADOW_AXIS_R2;
    default:     return -1;
    }
}

/* === G45 2026-08-22 - DETECTING A REAL GAMEPAD ===
 *
 * Reported bug: with no gamepad plugged in at all, the VM saw one (we sent the
 * plug-in announcement on every session) - Windows and games then switch to
 * gamepad mode and IGNORE keyboard and mouse, hence "stuck in game". And the
 * menu showed "connected" because it was reading the state of the CHANNEL, not
 * the presence of a gamepad. So we test the evdev capabilities: a gamepad
 * exposes a button from the gamepad family (BTN_SOUTH/BTN_A ... BTN_THUMBR) AND
 * absolute axes. Keyboards, mice and sensors do not pass that test. */
static bool dev_is_gamepad(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
    unsigned long absb[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
    memset(keys, 0, sizeof(keys)); memset(absb, 0, sizeof(absb));
    bool ok = false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 &&
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absb)), absb) >= 0) {
        #define BIT_AT(arr, b) ((arr)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long))) & 1)
        bool has_pad_btn = false;
        for (int b = BTN_GAMEPAD; b <= BTN_THUMBR; b++)
            if (BIT_AT(keys, b)) { has_pad_btn = true; break; }
        bool has_stick = BIT_AT(absb, ABS_X) && BIT_AT(absb, ABS_Y);
        #undef BIT_AT
        ok = has_pad_btn && has_stick;
    }
    close(fd);
    return ok;
}

/* G47 - the same test as dev_is_gamepad() but WITHOUT opening the device: we
 * read the capability masks sysfs exposes as text. Opening a /dev/input/event*
 * wakes the driver up (tens of ms on some Bluetooth devices); reading a sysfs
 * file is instant. The mask is a sequence of 64-bit hex words, most significant
 * first: the LAST word carries bits 0-63. */
static bool sysfs_bit_set(const char *evname, const char *which, int bit)
{
    char path[320];
    snprintf(path, sizeof(path), "/sys/class/input/%s/device/capabilities/%s",
             evname, which);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[1024] = {0};
    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
    fclose(f);
    /* Split into words, then index from the END (word 0 = bits 0-63). */
    char *words[64]; int nw = 0;
    for (char *tok = strtok(line, " \t\n"); tok && nw < 64; tok = strtok(NULL, " \t\n"))
        words[nw++] = tok;
    if (nw == 0) return false;
    int widx = bit / 64, woff = bit % 64;
    if (widx >= nw) return false;
    unsigned long long w = strtoull(words[nw - 1 - widx], NULL, 16);
    return (w >> woff) & 1ULL;
}

static bool sysfs_is_gamepad(const char *evname)
{
    bool pad_btn = false;
    for (int b = BTN_GAMEPAD; b <= BTN_THUMBR && !pad_btn; b++)
        if (sysfs_bit_set(evname, "key", b)) pad_btn = true;
    if (!pad_btn) return false;
    return sysfs_bit_set(evname, "abs", ABS_X) && sysfs_bit_set(evname, "abs", ABS_Y);
}

/* True if at least one physical gamepad is present (hotplug included).
 *
 * G47 2026-08-22 - RESULT CACHED (~1 s). The scan opens ALL the
 * /dev/input/event* devices (26 of them on the dev machine) with two ioctls
 * each: that is very slow (some devices, Bluetooth ones in particular, take tens
 * of ms to open). Called from the developer menu, it ran ON EVERY DRAW -> the
 * render loop collapsed to ~5 frames/s ("Displayed 5 fps" while decoding was
 * keeping up at 57/s). So we rescan only once per second; hotplug is still seen,
 * at worst one second later. */
bool ctrl_gamepad_present(void)
{
    static int64_t last_scan_ms = 0;
    static bool    cached = false;
    struct timespec tsx; clock_gettime(CLOCK_MONOTONIC, &tsx);
    int64_t now = (int64_t)tsx.tv_sec * 1000 + tsx.tv_nsec / 1000000;
    if (last_scan_ms != 0 && (now - last_scan_ms) < 1000) return cached;
    last_scan_ms = now;
    cached = false;

    DIR *d = opendir("/dev/input");
    if (!d) return false;
    struct dirent *e; bool found = false;
    while (!found && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[320], namep[320], nm[128] = {0};
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        snprintf(namep, sizeof(namep), "/sys/class/input/%s/device/name", e->d_name);
        FILE *f = fopen(namep, "r");
        if (f) { if (fgets(nm, sizeof(nm), f)) {} fclose(f); }
        if (strstr(nm, "Motion Sensor") || strstr(nm, "halyard-virtual")) continue;
        (void)path;   /* G47: detection via sysfs, without opening the device */
        if (sysfs_is_gamepad(e->d_name)) found = true;
    }
    closedir(d);
    cached = found;
    return cached;
}

static void *reader_thread(void *arg)
{
    (void)arg;
    int fds[16]; int nfd = 0;
    DIR *d = opendir("/dev/input");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && nfd < 16) {
            if (strncmp(e->d_name, "event", 5) != 0) continue;
            char path[320], namep[320], nm[128] = {0};
            snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
            snprintf(namep, sizeof(namep),
                     "/sys/class/input/%s/device/name", e->d_name);
            FILE *f = fopen(namep, "r");
            if (f) { if (fgets(nm, sizeof(nm), f)) {} fclose(f); }
            /* Motion sensors expose THE SAME axis codes 0-5 but with
             * out-of-range values: including them drowns everything (seen during
             * the decoding work). We skip them, and our own injection device. */
            if (strstr(nm, "Motion Sensor") || strstr(nm, "halyard-virtual"))
                continue;
            /* G45: open ONLY real gamepads - we used to open every
             * /dev/input/event*, so keyboard and mouse too, whose codes fell
             * inside the gamepad mapping. */
            if (!dev_is_gamepad(path)) continue;
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) fds[nfd++] = fd;
        }
        closedir(d);
    }
    glog("lecteur local : %d peripheriques ouverts", nfd);

    while (g_rd_running && !(g_rd_abort && *g_rd_abort)) {
        fd_set set; FD_ZERO(&set); int mx = 0;
        for (int i = 0; i < nfd; i++) { FD_SET(fds[i], &set); if (fds[i] > mx) mx = fds[i]; }
        struct timeval tv = {0, 50 * 1000};
        if (select(mx + 1, &set, NULL, NULL, &tv) <= 0) continue;
        for (int i = 0; i < nfd; i++) {
            if (!FD_ISSET(fds[i], &set)) continue;
            struct input_event ev[32];
            ssize_t n = read(fds[i], ev, sizeof(ev));
            if (n <= 0) continue;
            /* G2 2026-08-21 - the 1st version mixed indexing by STRUCT
             * (`&ev[k]`) with advancing by BYTES (`k += sizeof-1`): it skipped
             * events, button releases among them. Reported symptom: "if I press
             * Cross, it stays held down". */
            ssize_t cnt = n / (ssize_t)sizeof(ev[0]);
            for (ssize_t k = 0; k < cnt; k++) {
                struct input_event *e = &ev[k];
                if (e->type == EV_KEY && e->value != 2) {
                    int id = map_button(e->code);
                    if (id >= 0) ctrl_gamepad_button(id, e->value != 0);
                } else if (e->type == EV_ABS) {
                    if (e->code == ABS_HAT0X || e->code == ABS_HAT0Y) {
                        /* G2 - we sent 1 for EVERY direction, hence "whichever
                         * arrow I press, it always goes up". The capture only
                         * showed up (value 1) and release (0). We assume a
                         * "hat"-style encoding with 8 positions, clockwise from
                         * up, and we keep the state of both axes so that
                         * diagonals can be composed. SHADOW_PAD_DPAD_BASE lets
                         * you try a different starting point if the assumption
                         * turns out to be wrong. */
                        static int hx = 0, hy = 0;
                        if (e->code == ABS_HAT0X) hx = e->value;
                        else                      hy = e->value;
                        /* G3 2026-08-21 - A BIT MASK, not an index.
                         * Measured by trial: sending 5 gives "up + left"
                         * (5 = 1 + 4), sending 3 gives "down" (3 = 1 + 2, down
                         * winning). Hence:
                         *   bit 0 = up, bit 1 = down, bit 2 = left, bit 3 = right. */
                        uint8_t dir = 0;
                        if (hy < 0) dir |= 1;
                        if (hy > 0) dir |= 2;
                        if (hx < 0) dir |= 4;
                        if (hx > 0) dir |= 8;
                        ctrl_gamepad_dpad(dir);
                    } else {
                        /* === G48 2026-08-26 - SCALE THE AXES ===
                         *
                         * We forwarded ONLY the values that already fell between
                         * 0 and 255, as they were. But evdev returns
                         * -32768..32767 on an ordinary gamepad (measured on a
                         * DualShock 4: -32767..32767). Nearly every stick
                         * movement was therefore dropped SILENTLY, and the
                         * little that got through - the noise around the centre
                         * - went out unconverted. That is the cause of "the
                         * server acknowledges but the gamepad does nothing":
                         * the format, the identifiers and the axes were correct
                         * all along (KB.md §3.36).
                         *
                         * Guided capture of the official client, correlated with
                         * the local joystick API over 1626 packets:
                         *     byte = round(+/-value / 258) + 128
                         * with 258 = 32767/127 and the Y axis INVERTED (negative
                         * slope measured, R^2 = 0.98).
                         *
                         * The real range is read from the device rather than
                         * assumed: not every gamepad advertises +-32767
                         * (triggers often run from 0 to 255).
                         * SHADOW_PAD_NO_SCALE=1 restores the old raw
                         * pass-through. */
                        int ax = map_axis(e->code);
                        if (ax >= 0) {
                            static int g_brut = -1;
                            if (g_brut < 0) {
                                const char *v = getenv("SHADOW_PAD_NO_SCALE");
                                g_brut = v ? atoi(v) : 0;
                            }
                            if (g_brut) {
                                if (e->value >= 0 && e->value <= 255)
                                    ctrl_gamepad_axis(ax, (uint8_t)e->value);
                            } else {
                                struct input_absinfo ai;
                                int lo = -32768, hi = 32767;
                                if (ioctl(fds[i], EVIOCGABS(e->code), &ai) == 0
                                    && ai.maximum > ai.minimum) {
                                    lo = ai.minimum; hi = ai.maximum;
                                }
                                const int centre = (lo + hi) / 2;
                                const int demi   = (hi - lo) / 2;
                                int rel = e->value - centre;
                                /* The wire's Y axis is inverted relative to evdev. */
                                if (e->code == ABS_Y || e->code == ABS_RY) rel = -rel;
                                int b = demi > 0 ? (rel * 127) / demi : 0;
                                if (b >  127) b =  127;
                                if (b < -127) b = -127;
                                ctrl_gamepad_axis(ax, (uint8_t)(b + 128));
                            }
                        }
                    }
                }
            }
        }
    }
    for (int i = 0; i < nfd; i++) close(fds[i]);
    glog("lecteur local termine");
    return NULL;
}

/* LOCAL gamepad reader (evdev on Linux). Explicitly opt-in: forwarding the
 * gamepad to the VM is not yet validated end to end (the server acknowledges but
 * Steam sees nothing, cf. the ongoing work on `:base+14`), and an active reader
 * would open input devices for nothing in return. SHADOW_GAMEPAD=1 enables it. */
int ctrl_gamepad_start_local_reader(volatile int *abort_flag)
{
    const char *e = getenv("SHADOW_GAMEPAD");
    if (!e || atoi(e) == 0) return 0;         /* opt-in while unvalidated */
    if (g_rd_running) return 0;
    g_rd_abort = abort_flag; g_rd_running = 1;
    if (pthread_create(&g_rd_thread, NULL, reader_thread, NULL) != 0) {
        g_rd_running = 0; glog("pthread_create KO"); return -1;
    }
    return 0;
}

void ctrl_gamepad_stop_local_reader(void)
{
    if (!g_rd_running) return;
    g_rd_running = 0;
    pthread_join(g_rd_thread, NULL);
}
#else
int  ctrl_gamepad_start_local_reader(volatile int *a) { (void)a; return 0; }
void ctrl_gamepad_stop_local_reader(void) {}
/* G45: no evdev probe outside Linux.
 * On a HANDHELD CONSOLE the question does not arise: the device IS a gamepad -
 * the Switch's Joy-Cons attached or a Pro Controller paired, the Vita's own
 * sticks and buttons - and that is the port's only input mode. Answering
 * `false` here would block the plug-in announcement -> the VM would create no
 * virtual gamepad -> no gamepad input at all.
 * Elsewhere (Windows), with no probe available, we let `SHADOW_GAMEPAD_PLUG=1`
 * decide. */
bool ctrl_gamepad_present(void)
{
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
    return true;
#else
    return false;
#endif
}
#endif
