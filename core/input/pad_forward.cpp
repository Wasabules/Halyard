/* padforward - see pad_forward.hpp. */
#include "pad_forward.hpp"

#include "rear_touch_sce.h"
#include "settings.hpp"

#include <cstdlib>
#include <ctime>
#include <pthread.h>

extern "C" {
#include "../services/log.h"
#include "../protocol/latency.h"
}

/* This module is about the gamepad: its lines carry that category. */
#define pflog(...) JOURNAL_INFO_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)

#include "pad_map.hpp"

extern "C" {
#include "../protocol/ctrl_gamepad.h"
}

/* WHICH CONSOLES FORWARD THEIR OWN BUTTONS. This file was `#ifdef __SWITCH__`
 * from end to end, so on the Vita nothing but START and SELECT ever reached the
 * VM - those two travel by another route entirely (Borealis actions, then
 * `stream_view`'s short/long press logic), which is exactly why they worked
 * while every other button and both sticks did nothing. Reported from hardware
 * 2026-09-13, together with the observation that the pad TESTER saw everything:
 * the reading was fine, the forwarding was not compiled.
 *
 * `pad_sce.h` supplies the four fetch calls in libnx's shape, so everything
 * below - the mapping, the d-pad accumulation, the axes, the change detection -
 * is the same code on both consoles. */
#if defined(__vita__) || defined(__psp2__)
#include "pad_sce.h"
#endif

#define SHADOW_PAD_FORWARDS (defined(__SWITCH__) || defined(__vita__) || defined(__psp2__))

/* libnx only where libnx exists - `pad_sce.h` above already provided the same
 * four calls for the Vita. This one guard stays platform-shaped on purpose:
 * it names a HEADER, not a capability. */
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace padforward {

namespace {

#if SHADOW_PAD_FORWARDS

/* The HID bits matching each padmap::Btn entry, in the same order. */
const uint64_t HID_BITS[(size_t)padmap::Btn::COUNT] = {
    HidNpadButton_A,        HidNpadButton_B,
    HidNpadButton_X,        HidNpadButton_Y,
    HidNpadButton_L,        HidNpadButton_R,
    HidNpadButton_ZL,       HidNpadButton_ZR,
    HidNpadButton_Minus,    HidNpadButton_Plus,
    HidNpadButton_StickL,   HidNpadButton_StickR,
    HidNpadButton_Up,       HidNpadButton_Down,
    HidNpadButton_Left,     HidNpadButton_Right,
};

PadState g_pad;
bool     g_pad_ready = false;

/* The rear-pad recogniser's state. A file static is right here and nowhere
 * else: this forwarder is its single owner, one console has one rear panel, and
 * `releaseAll()` below is what stops it being the session-state-in-a-static
 * defect this repo keeps meeting - it is reset there, on every path out. */
rear_touch_config g_rear_cfg;
rear_touch_state  g_rear_state;
bool              g_rear_cfg_set = false;
bool              g_l2_digital = false, g_r2_digital = false;

/* Settings -> `rear_touch_config`. The integers persisted in `settings.txt` are
 * the enum's own values; the clamps here are the second line of defence, since
 * that file is hand-editable on a memory card. */
void rearConfigFromSettings(rear_touch_config *c)
{
    rear_touch_defaults(c);
    const Settings &s = Settings::instance();
    const auto act = [](uint32_t v) {
        return (rear_action)(v < (uint32_t)REAR_ACT_COUNT ? v : 0);
    };
    const auto ges = [](uint32_t v) {
        return (rear_gesture)(v <= (uint32_t)REAR_GEST_SLIDE ? v : 0);
    };
    c->enabled                    = s.rear_touch;
    c->action[REAR_ZONE_TL]       = act(s.rear_zone_tl);
    c->action[REAR_ZONE_TR]       = act(s.rear_zone_tr);
    c->action[REAR_ZONE_BL]       = act(s.rear_zone_bl);
    c->action[REAR_ZONE_BR]       = act(s.rear_zone_br);
    c->gesture[REAR_ZONE_TL]      = ges(s.rear_gest_tl);
    c->gesture[REAR_ZONE_TR]      = ges(s.rear_gest_tr);
    c->gesture[REAR_ZONE_BL]      = ges(s.rear_gest_bl);
    c->gesture[REAR_ZONE_BR]      = ges(s.rear_gest_br);
    c->two_finger                 = act(s.rear_two_finger);
    c->slide_full                 = (float)s.rear_slide_pct / 100.0f;
}
bool     g_held[(size_t)padmap::Btn::COUNT] = { false };
uint8_t  g_dpad      = 0xff;   /* 0xff = never sent */
uint8_t  g_axis[6]   = { 128, 128, 128, 128, 0, 0 };
bool     g_primed    = false;

/* === L9 2026-08-29 — THE GAMEPAD IS NO LONGER READ AT THE SCREEN'S RATE ===
 *
 * `poll()` was called from `stream_view`'s draw. The gamepad was therefore read
 * ONCE PER DISPLAYED FRAME - `video/cadence` measures that period at 17.4 ms,
 * flat at p50 as at p99. A press lands on average in the middle of that
 * interval: 8.7 ms of delay before the event even exists, and up to 17.4 at
 * worst. That is pure delay, added before the network, before the server, before
 * the game.
 *
 * The mouse, meanwhile, had had its own 100 Hz thread all along. The gamepad had
 * none - and nobody could notice, because the `input/send` stage covers ONLY
 * mouse and keyboard: nine minutes of gamepad play produced SIX samples. A path
 * nothing measures is a path nothing defends.
 *
 * The thread below reads at 250 Hz (4 ms), i.e. an average delay of 2 ms instead
 * of 8.7. We deliberately go no lower: the console's HID hardware publishes its
 * samples at around 200 Hz, and reading faster than the source only burns CPU.
 *
 * === WHY A HEARTBEAT RATHER THAN A LIFECYCLE ===
 *
 * Forwarding must stop as soon as the pause menu opens, the stream view
 * disappears, or the session drops. Wiring those three cases would have been
 * three chances to forget one - and a thread that keeps emitting while you read
 * a menu would send the menu's buttons to the VM.
 *
 * `poll()`, still called by the draw under the same conditions as before, now
 * only sets a HEARTBEAT. The thread emits only while that heartbeat is less than
 * 100 ms old. The three cases take care of themselves: when nobody calls
 * `poll()` any more, the thread falls silent on its own.
 *
 * `SHADOW_PAD_THREAD=0` restores reading from the draw, `SHADOW_PAD_HZ` changes
 * the rate. */
pthread_mutex_t g_mtx        = PTHREAD_MUTEX_INITIALIZER;
pthread_t       g_thread;
volatile int    g_thread_on  = 0;
volatile int    g_thread_stop   = 0;
int64_t         g_heartbeat_us = 0;
int64_t         g_last_sample_us = 0;
int             g_emissions      = 0;   /* turns that actually wrote */

int64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Converts a libnx stick position (-32768..32767) into a Shadow value
 * (0..255, centre 128), zone morte comprise. */
uint8_t axisValue(int32_t raw, bool invert)
{
    if (invert) raw = -raw;
    const int32_t dead = (int32_t)(32767.0f * (float)padmap::deadzone() / 100.0f);
    if (raw > -dead && raw < dead) return 128;
    /* We stretch what remains of the travel so the value starts again from 128
     * just past the dead zone: otherwise the stick jumps at the moment it
     * "catches". */
    int32_t span = 32767 - dead;
    if (span <= 0) span = 1;
    int32_t adj = (raw > 0) ? (raw - dead) : (raw + dead);
    int32_t v   = 128 + (adj * 127) / span;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/* === P5 2026-08-22 - KEEPING THE GAMEPAD CHANNEL ALIVE ===
 *
 * Measured comparison, Linux (works) against Switch (does not): identical
 * encoding,
 * the same channel, the same plug-in announcement, buttons and d-pad emitted on
 * both sides. The only notable difference is the RATE at rest.
 *   - On Linux, a Bluetooth controller's stick JITTERS constantly (values
 *     0x7c/0x7d instead of 0x80): ~19 axis messages per second, without a break,
 *     even when the player touches nothing.
 *   - On Switch, HID returns an EXACT centre: the deduplication below then
 *     silences the channel completely as soon as the sticks are at rest.
 * But the KB (K9) notes that the official client emits on this port **every
 * ~7 s**, permanently. A server that considers the gamepad absent after a
 * silence would explain exactly what we observe: Linux kept alive by accident
 * (the jitter), while the Switch declares itself inactive.
 * So we re-emit the axis state at least every 2 s, even unchanged.
 * `SHADOW_PAD_KEEPALIVE_MS=0` disables it. */
static long long pf_now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void sendAxis(int idx, uint8_t v)
{
    if (idx < 0 || idx > 5) return;
    static long long t_last[6] = {0};
    static int period = -1;
    if (period < 0) { const char *e = getenv("SHADOW_PAD_KEEPALIVE_MS"); period = e ? atoi(e) : 2000; }
    long long now = pf_now_ms();
    bool du = (period > 0 && (now - t_last[idx]) >= period);   /* rappel periodique */
    if (g_axis[idx] == v && g_primed && !du) return;
    g_axis[idx] = v;
    t_last[idx] = now;
    ctrl_gamepad_axis(idx, v);
}

#endif  /* SHADOW_PAD_FORWARDS */

}  // namespace

#if SHADOW_PAD_FORWARDS
/* Defined below, next to `reset()`, of which it is the lock-free half. */
static void reset_nolock();
#endif

/* === S76 2026-08-28 - RELEASE BEFORE GOING SILENT ===
 *
 * `reset()` zeroes the LOCAL state without emitting anything. Enough when the
 * session ends - the server forgets everything - but wrong when suspending
 * forwarding while keeping the session alive: the VM would keep the button that
 * was down at the moment of suspension pressed, and the game would see it held
 * until we resume. So we send the releases BEFORE zeroing the state.
 *
 * The triggers are AXES on Shadow (L2/R2): releasing them means bringing them
 * back to 0, not sending a button release. The sticks return to centre (128) so
 * a character does not keep walking while you read the menu. */
void releaseAll()
{
#if SHADOW_PAD_FORWARDS
    /* The lock covers the WHOLE sequence: without it the sampling thread could
     * slip a press in between our releases, and the VM would keep the button
     * pressed while you read the menu - exactly the defect S76
     * avait corrige. */
    pthread_mutex_lock(&g_mtx);
    if (!ctrl_gamepad_active()) {
        reset_nolock();
        pthread_mutex_unlock(&g_mtx);
        return;
    }
    for (size_t i = 0; i < (size_t)padmap::Btn::COUNT; i++) {
        if (!g_held[i]) continue;
        const int t = padmap::target((padmap::Btn)i);
        if (t == padmap::TARGET_L2)      ctrl_gamepad_axis(SHADOW_AXIS_L2, 0);
        else if (t == padmap::TARGET_R2) ctrl_gamepad_axis(SHADOW_AXIS_R2, 0);
        else if (t >= 0 && t <= SHADOW_PAD_GUIDE) ctrl_gamepad_button(t, false);
    }
    if (g_dpad != 0xff && g_dpad != 0) ctrl_gamepad_dpad(0);
    for (int i = 0; i < 4; i++) if (g_axis[i] != 128) ctrl_gamepad_axis(i, 128);
    for (int i = 4; i < 6; i++) if (g_axis[i] != 0)   ctrl_gamepad_axis(i, 0);
    reset_nolock();
    pthread_mutex_unlock(&g_mtx);
#endif
}

/* The LOCK-FREE variant: called from the paths that already hold it.
 * Two functions rather than a flag, because a lock taken again by accident does
 * not show - it freezes the console. */
#if SHADOW_PAD_FORWARDS
static void reset_nolock()
{
    /* RULE 4 of `rear_touch.h`: a press that started must end. Without this a
     * finger down at the moment forwarding is suspended would leave ZL held on
     * the VM until it resumed - the very defect the release sequence above
     * exists to prevent, arriving through a different door. */
    rear_touch_release_all(&g_rear_state);
    for (size_t i = 0; i < (size_t)padmap::Btn::COUNT; i++) g_held[i] = false;
    g_dpad   = 0xff;
    g_primed = false;
    for (int i = 0; i < 4; i++) g_axis[i] = 128;
    g_axis[4] = g_axis[5] = 0;
}
#endif

void reset()
{
#if SHADOW_PAD_FORWARDS
    pthread_mutex_lock(&g_mtx);
    reset_nolock();
    pthread_mutex_unlock(&g_mtx);
#endif
}

#if SHADOW_PAD_FORWARDS
/* One HID read and the emissions that follow from it. The lock is held by
 * l'appelant. */
static void echantillonner_nolock()
{
    if (!ctrl_gamepad_active()) {
        /* Session closed: everything must be re-sent on the next plug-in,
         * otherwise a button held at the moment of the cut would stay "pressed"
         * in our state and would never be resent. */
        if (g_primed) reset_nolock();
        return;
    }

    if (!g_pad_ready) {
        padConfigureInput(8, HidNpadStyleSet_NpadStandard);
        padInitializeAny(&g_pad);
        g_pad_ready = true;
    }
    ctrl_gamepad_replug_tick();   /* P6 : rappels d'annonce en debut de session */

    padUpdate(&g_pad);
    /* L9 - the interval between two reads IS the sampling latency: a press falls
     * uniformly inside it, so it waits half of it on average and all of it at
     * worst. That is the measurement that says whether this thread is worth
     * running at all, and it compares directly with `video/cadence`. */
    const int64_t t_read = latency_enabled() ? now_us() : 0;
    if (t_read && g_last_sample_us)
        latency_add(LAT_IN_PAD_RATE, t_read - g_last_sample_us);
    if (t_read) g_last_sample_us = t_read;
    const int emissions_before = g_emissions;

    uint64_t buttons = padGetButtons(&g_pad);

    /* === THE REAR PANEL SUPPLIES THE FOUR BUTTONS THIS CONSOLE LACKS =====
     *
     * A Vita has no ZL, no ZR and no stick clicks - exactly four of the sixteen
     * `padmap::Btn` values, and exactly the ones a PC game binds to aiming,
     * sprinting and melee. They are merged in HERE rather than in the reader,
     * because the reader's job is to say what the HARDWARE reports and the rear
     * pad is not a gamepad button: it is a gesture that this frame happens to
     * mean one.
     *
     * Merged with OR and with max, never assigned: a physical L or R must keep
     * working while a rear zone drives the same axis, and whichever asks for
     * more wins. The recogniser and its rules are in `core/input/rear_touch.h`,
     * with `tests/test_rear_touch.c` behind them. */
    /* The config is read from the settings EVERY poll, not cached at startup.
     * A cached copy is how a settings screen comes to show one thing while the
     * pad does another - the defect `env_override` was written to make visible.
     * `poll()` is called from `stream_view`'s draw, the same thread the settings
     * screen runs on, so this is neither a cross-thread read nor a race - and a
     * struct copy of a dozen integers once a frame is not the cost worth
     * saving. */
    rearConfigFromSettings(&g_rear_cfg);
    rear_touch_out rear = shadow_rear_touch_poll(&g_rear_cfg, &g_rear_state, NULL, NULL);
    if (rear.zl) buttons |= HidNpadButton_ZL;
    if (rear.zr) buttons |= HidNpadButton_ZR;
    if (rear.l3) buttons |= HidNpadButton_StickL;
    if (rear.r3) buttons |= HidNpadButton_StickR;


    uint8_t dpad = 0;
    for (size_t i = 0; i < (size_t)padmap::Btn::COUNT; i++) {
        /* "+" is the only way to open the application's menu: we do not emit it
         * here. A short press is resent by stream_view once the long press has
         * been ruled out, otherwise opening the menu would also send the button
         * to the VM. */
        if ((padmap::Btn)i == padmap::Btn::Plus) continue;

        const bool down = (buttons & HID_BITS[i]) != 0;
        const int  t    = padmap::target((padmap::Btn)i);

        /* The d-pad is a bitmask, not four buttons: we accumulate and emit only
         * once, further down. */
        if (t == padmap::TARGET_DPAD) {
            if (down) {
                switch ((padmap::Btn)i) {
                    case padmap::Btn::Up:    dpad |= 1; break;
                    case padmap::Btn::Down:  dpad |= 2; break;
                    case padmap::Btn::Left:  dpad |= 4; break;
                    case padmap::Btn::Right: dpad |= 8; break;
                    default: break;
                }
            }
            g_held[i] = down;
            continue;
        }

        if (down == g_held[i] && g_primed) continue;
        g_held[i] = down;

        if (t == padmap::TARGET_NONE) continue;
        /* The triggers are handled AFTER the loop, not here: this branch only
         * runs when the button's state CHANGES, and a finger sliding on the
         * rear pad changes its value on every frame while no button moves. */
        if (t == padmap::TARGET_L2)      g_l2_digital = down;
        else if (t == padmap::TARGET_R2) g_r2_digital = down;
        else                           { ctrl_gamepad_button(t, down); g_emissions++; }
    }

    /* === THE TRIGGERS, ONCE PER POLL ====================================
     *
     * A digital press is 255 and the rear pad's pull is 0..255: the LARGER
     * wins, so a mapped button always beats a half-pulled finger and the two
     * coexist instead of cancelling each other every other frame.
     *
     * `sendAxis` already drops a value equal to the last one sent, so emitting
     * unconditionally here costs nothing on the wire when nothing moves - and
     * it is what makes a slide actually travel, which putting it in the
     * change-gated loop above did not. */
    {
        const uint8_t l2 = (g_l2_digital && 255 > rear.l2) ? 255 : rear.l2;
        const uint8_t r2 = (g_r2_digital && 255 > rear.r2) ? 255 : rear.r2;
        sendAxis(SHADOW_AXIS_L2, l2);
        sendAxis(SHADOW_AXIS_R2, r2);
    }

    if (dpad != g_dpad) {
        g_dpad = dpad;
        ctrl_gamepad_dpad(dpad);
        g_emissions++;
    }

    const bool inv = padmap::invertY();
    HidAnalogStickState l = padGetStickPos(&g_pad, 0);
    HidAnalogStickState r = padGetStickPos(&g_pad, 1);
    sendAxis(SHADOW_AXIS_LX, axisValue(l.x, false));
    sendAxis(SHADOW_AXIS_LY, axisValue(l.y, !inv));  /* HID counts up towards +, Shadow towards - */
    sendAxis(SHADOW_AXIS_RX, axisValue(r.x, false));
    sendAxis(SHADOW_AXIS_RY, axisValue(r.y, !inv));

    g_primed = true;

    /* L9 - from the HID read to the last write on the socket. We only record it
     * when this turn actually emitted: measuring the empty turns would fill the
     * histogram with zeroes and give a flattering p50 that would
     * decrit aucun appui reel. */
    if (t_read && g_emissions != emissions_before)
        latency_add(LAT_IN_PAD, now_us() - t_read);
}

/* The reading thread. It knows nothing of the menu, the view or the session: it
 * watches the heartbeat. */
static void *fil_manette(void *)
{
    int periode_us = 4000;   /* 250 Hz */
    if (const char *e = getenv("SHADOW_PAD_HZ")) {
        const int hz = atoi(e);
        if (hz >= 30 && hz <= 1000) periode_us = 1000000 / hz;
    }
    pflog("[L9] gamepad thread started at %d Hz (was the draw rate, ~57 Hz)",
          1000000 / periode_us);

    while (!g_thread_stop) {
        const int64_t age = now_us() - g_heartbeat_us;
        /* 100 ms: the view draws every ~17 ms, so three missed frames are enough
         * to conclude that nobody wants forwarding any more. It is also the
         * granularity imposed on long-lived threads on this console. */
        if (age < 100000) {
            pthread_mutex_lock(&g_mtx);
            echantillonner_nolock();
            pthread_mutex_unlock(&g_mtx);
        } else {
            /* Stale heartbeat: the thread goes silent, and it FORGETS the
             * instant of its last read. Without that forgetting, resuming would
             * measure the whole pause as one sampling interval - the first
             * session published `worst=1001,7 ms` for a menu left open for one
             * second. A later reader would have seen a frozen gamepad for a
             * second; this repo has already lost entire campaigns chasing a
             * measurement artefact taken for a symptom. */
            g_last_sample_us = 0;
        }
        struct timespec ts = { 0, (long)periode_us * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}
#endif

void poll()
{
#if SHADOW_PAD_FORWARDS
    static int g_use_thread = -1;
    if (g_use_thread < 0) {
        const char *e = getenv("SHADOW_PAD_THREAD");
        g_use_thread = e ? atoi(e) : 1;
    }

    if (!g_use_thread) {               /* revert: read from the draw */
        pthread_mutex_lock(&g_mtx);
        echantillonner_nolock();
        pthread_mutex_unlock(&g_mtx);
        return;
    }

    g_heartbeat_us = now_us();         /* the thread only reads while this is fresh */
    if (!g_thread_on) {
        g_thread_stop = 0;
        if (pthread_create(&g_thread, NULL, fil_manette, NULL) == 0) {
            g_thread_on = 1;
        } else {
            /* No thread: we fall back to the old behaviour rather than lose the
             * gamepad. A console that refuses a thread is already in a bad state;
             * this is not the moment to take away its
             * commandes. */
            pflog("[L9] gamepad thread impossible - reading inside the draw");
            g_use_thread = 0;
        }
    }
#endif
}

void stop()
{
#if SHADOW_PAD_FORWARDS
    if (!g_thread_on) return;
    g_thread_stop  = 1;
    pthread_join(g_thread, NULL);
    g_thread_on = 0;
    pflog("[L9] fil manette arrete");
#endif
}

}  // namespace padforward
