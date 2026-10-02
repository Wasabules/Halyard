/* pad_mouse_hid - see the header. */
#include "pad_mouse_hid.hpp"
#include "pad_mouse.h"
#include "settings.hpp"

extern "C" {
#include "shadow_input.h"
#include "../services/log.h"
/* S81 - this module's category. See shadow/journal.h: it is declared here,
 * never inferred from the text of the messages. */
#define pmlog(...) JOURNAL_INFO_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)
#define pmdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)
}

#ifdef __SWITCH__
#include <switch.h>
#endif

/* === THE PAD-AS-MOUSE ON PS VITA =======================================
 *
 * `poll()` was `#ifndef __SWITCH__ -> do nothing`, written back when "not the
 * Switch" meant "a desktop". On Vita the result was worse than having nothing
 * at all: `active()` returns TRUE as soon as the mode is armed (and it is by
 * default, pad_mouse_mode=1), so `stream_view` took the mouse branch, called
 * `padforward::releaseAll()` and NEVER reached `padforward::poll()`. Neither
 * gamepad nor mouse was sent. Measured on console: [P2] send = 1 -- the single
 * connection announcement -- with the input/send and input/gamepad stages
 * silent across a whole play session.
 *
 * `pad_sce.h` supplies the three calls the body needs (buttons and both
 * sticks, in libnx shape), so the logic stays the same on both consoles. The
 * GYROSCOPE does stay Switch-only: it goes through
 * `hidGetSixAxisSensorStates`, and the Vita equivalent (SceMotion) is a
 * separate piece of work. So Gyro mode is simply unavailable here -- not
 * broken, absent. */
#if defined(__vita__) || defined(__psp2__)
#include "pad_sce.h"
#endif

/* The console pad API: libnx on the Switch, `pad_sce.h`'s shim of it on the
 * Vita. A desktop has neither, and gets an empty poll().
 *
 * 2026-09-26 - that empty poll() used to exist as `#ifndef __SWITCH__`, which
 * meant "a desktop" only while the Switch was the only console. The Vita fix of
 * 2026-09-13 (5fe550c) needed the real body there and removed the guard - which
 * gave the Vita its body and took the desktop's stub away. The desktop build had
 * been broken ever since, unseen, because the local loop builds only the two
 * consoles. Naming the capability is what stops the two from trading places. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
#  define SHADOW_PADMOUSE_HAS_PAD 1
#else
#  define SHADOW_PADMOUSE_HAS_PAD 0
#endif

#if defined(__SWITCH__)
#  define SHADOW_PADMOUSE_GYRO 1
#else
#  define SHADOW_PADMOUSE_GYRO 0
#endif

namespace padmouse {
namespace {

Mode             g_mode  = Mode::Off;
pad_mouse_state  g_state;
uint64_t         g_last_us = 0;

/* Which mouse buttons WE are currently holding down. Kept so leaving the mode
 * can release exactly those and nothing else: a click left pressed in the
 * remote machine cannot be released once the mode is gone, and the user is left
 * dragging the desktop. */
bool g_btn_down[3] = { false, false, false };

#ifdef __SWITCH__
/* PM8 - per-sensor readings, kept apart. [0] = left / first handle, [1] = right. */
static float g_gyro[2][2] = {{0,0},{0,0}};
static bool  g_gyro_live[2] = { false, false };
static int   g_gyro_pick = 0;

/* === PM4 2026-09-02 - THE HANDLES ARE TAKEN FROM THE STYLE THE PAD REALLY IS
 *
 * The first version asked for ONE handle in handheld mode. Handheld has TWO
 * sensors - one per half - and `hidGetSixAxisSensorHandles` fails when the
 * count does not match the style. The handle was therefore zeroed, the sensor
 * "started" on nothing, and `hidGetSixAxisSensorStates` returned zero states
 * for ever after.
 *
 * NOT ONE of those three steps said a word. The log printed "motion sensors
 * started" while nothing had been obtained, and the dot sat in the middle -
 * which is exactly what was reported. The result codes are now read and
 * logged, one line each: a sensor that cannot be obtained must SAY so, because
 * the alternative is a feature that looks implemented and does nothing.
 *
 * The style is asked of the PAD rather than guessed: handheld, a Joy-Con pair,
 * or a Pro Controller are three different acquisitions and the console knows
 * which one it is in. */
HidSixAxisSensorHandle g_six[2];
int                    g_six_n       = 0;    /* handles actually obtained */
bool                   g_six_started = false;

void sixaxisStart(void *pad)
{
    if (g_six_started) return;
    PadState *p = (PadState *)pad;
    g_six_n = 0;

    const bool handheld = p && padIsHandheld(p);
    const u32  styles   = p ? padGetStyleSet(p) : 0;
    pmlog("[PM] manette : portable=%d styles=0x%08x", (int)handheld, (unsigned)styles);

    /* === PM6 - WE TRY, AND WE REPORT WHICH ONE ANSWERED ===
     *
     * The previous version picked ONE acquisition from the style and gave up if
     * it failed - which is a bet, and it lost: handheld was asked for one sensor
     * where it has two, and the whole feature was dead with "0 sensors" as its
     * only explanation.
     *
     * The combinations that exist are few and cheap to try. So we try them in
     * the order that is most likely for the way the console is being held, and
     * the log names the one that worked. That turns a guess into a measurement -
     * and if a future firmware changes the answer, the line says so instead of
     * the feature going quiet. */
    static const struct { const char *what; int n; HidNpadIdType id; HidNpadStyleTag style; }
    CANDIDATES[] = {
        { "portable x2",       2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld },
        { "portable x1",       1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld },
        { "paire Joy-Con x2",  2, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual  },
        { "paire Joy-Con x1",  1, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual  },
        { "manette Pro",       1, HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey  },
        { "Joy-Con gauche",    1, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyLeft  },
        { "Joy-Con droit",     1, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyRight },
    };
    const int N = (int)(sizeof CANDIDATES / sizeof CANDIDATES[0]);
    /* Handheld first when the console says it is handheld, otherwise start at
     * the detached ones: the order only decides which we try FIRST, never which
     * we accept - acceptance is the service's answer, not ours. */
    const int first = handheld ? 0 : 2;
    for (int k = 0; k < N && g_six_n == 0; k++) {
        const int i = (first + k) % N;
        const Result rc = hidGetSixAxisSensorHandles(&g_six[0], CANDIDATES[i].n,
                                                     CANDIDATES[i].id, CANDIDATES[i].style);
        if (R_SUCCEEDED(rc)) {
            g_six_n = CANDIDATES[i].n;
            pmlog("[PM] capteurs obtenus : %s (%d)", CANDIDATES[i].what, g_six_n);
        } else {
            pmdbg("[PM] %s : rc=0x%x", CANDIDATES[i].what, (unsigned)rc);
        }
    }

    for (int i = 0; i < g_six_n; i++) {
        const Result sr = hidStartSixAxisSensor(g_six[i]);
        if (R_FAILED(sr))
            pmlog("[PM] demarrage du capteur %d ECHOUE : rc=0x%x", i, (unsigned)sr);
    }
    g_six_started = (g_six_n > 0);
    if (!g_six_started)
        pmlog("[PM] NO sensor obtained - the gyroscope will not move");
}

void sixaxisStop()
{
    if (!g_six_started) return;
    for (int i = 0; i < g_six_n; i++) hidStopSixAxisSensor(g_six[i]);
    g_six_started = false;
    g_six_n = 0;
    pmlog("[PM] capteurs de mouvement arretes");
}

/* === PM8 2026-09-02 - BOTH JOY-CONS HAVE A GYROSCOPE, AND WE READ ONE ===
 *
 * The previous version returned on the FIRST handle that answered, with a
 * comment claiming that "which half answers depends on how the console is
 * held". That is true in handheld mode, where the pair reports as one. It is
 * FALSE for a detached pair - which is what the console reported
 * (`styles=0x4`, JoyDual): both sensors answer, every frame, so handle 0 always
 * won. Handle 0 is the LEFT Joy-Con. Turning the right one did nothing at all,
 * and nothing said why.
 *
 * The comment was not a description, it was a guess written in the voice of a
 * fact - which is worse than no comment, because it stops the next reader from
 * asking.
 *
 * We now read EVERY sensor and keep them apart. Which one drives the pointer is
 * a choice (`pad_mouse_gyro_source`), and its default has to be automatic: in
 * handheld both halves measure the same rotation, so either will do, while on a
 * detached pair the user aims with ONE hand and we cannot know which.
 *
 * AUTO IS STICKY. Taking the stronger of the two every frame makes the pointer
 * hand over mid-gesture when the resting hand twitches - the aim jumps and
 * nothing explains it. We keep the hand we are following until the other is
 * clearly more active, twice over, and only above the drift threshold. */
bool readGyro(float *gx, float *gy)
{
    HidSixAxisSensorState s;
    bool any = false;

    for (int i = 0; i < 2; i++) { g_gyro[i][0] = g_gyro[i][1] = 0.0f; g_gyro_live[i] = false; }
    for (int i = 0; i < g_six_n && i < 2; i++) {
        if (hidGetSixAxisSensorStates(g_six[i], &s, 1) > 0) {
            g_gyro[i][0] = s.angular_velocity.x;
            g_gyro[i][1] = s.angular_velocity.y;
            g_gyro_live[i] = true;
            any = true;
        }
    }
    if (!any) return false;

    /* PM9 - the choice and the combining are PURE and live in `pad_mouse.h`,
     * where `tests/test_pad_mouse.c` pins them. They used to be written here,
     * beside the HID calls, where nothing could check that the split had not
     * swapped its axes - a mistake that is invisible on reading. */
    pad_mouse_pick_gyro((int)Settings::instance().pad_mouse_gyro_source,
                        g_gyro[0][0], g_gyro[0][1], g_gyro[1][0], g_gyro[1][1],
                        g_gyro_live[0] ? 1 : 0,
                        (g_six_n > 1 && g_gyro_live[1]) ? 1 : 0,
                        &g_gyro_pick, gx, gy);
    return true;
}
#endif  /* __SWITCH__ */

void releaseButtons()
{
    for (int b = 0; b < 3; b++) {
        if (g_btn_down[b]) {
            shadow_input_post_mouse_button(b, false);
            g_btn_down[b] = false;
        }
    }
}

pad_mouse_cfg configFromSettings()
{
    const Settings &s = Settings::instance();
    pad_mouse_cfg c;
    c.sensitivity_pct = (int)s.pad_mouse_sens;
    /* The stick dead zone is the one already configured for the gamepad: it
     * describes the same hardware, and a second setting for the same physical
     * slack would be one more thing to keep in agreement - the B1 lesson. */
    c.deadzone_pct    = (int)s.pad_deadzone;
    c.invert_y        = s.pad_mouse_invert_y ? 1 : 0;
    c.scroll_step_pct = (int)s.pad_mouse_scroll_step;
    return c;
}

}  // namespace

/* PM3 - the pointing test. The dot lives in normalised coordinates so it does
 * not care what the video area measures, and it starts in the middle. */
static bool  g_test = false;
static float g_test_x = 0.5f, g_test_y = 0.5f;

/* PM5 - what the test page shows. Raw values, not a verdict: the page exists to
 * let the eye decide whether the sensor lives, and a boolean "it works" would
 * be our opinion rather than a measurement. */
static bool  g_gyro_ok = false, g_gyro_logged = false;
static float g_gyro_x = 0.0f, g_gyro_y = 0.0f;
static float g_last_sx = 0.0f, g_last_sy = 0.0f, g_last_scroll = 0.0f;

bool active() { return g_mode != Mode::Off; }
Mode mode()   { return g_mode; }

bool testMode() { return g_test; }

void setTestMode(bool on)
{
    if (on == g_test) return;
    g_test = on;
    g_test_x = g_test_y = 0.5f;
    if (on) {
        /* The test needs the module running, and the gyro needs its sensors.
         * Turning it on from the menu therefore ARMS the mouse - otherwise the
         * dot would sit still and the test would answer "the gyro does not
         * work" when the only thing not working was the test. */
        if (g_mode == Mode::Off) toggle();
    }
    /* Nothing is sent to the VM while the test runs, so any button we were
     * holding must be released now rather than at the end of the test. */
    releaseButtons();
    pmlog("[PM] test de pointage : %s", on ? "actif" : "arrete");
}

bool testPoint(float *nx, float *ny)
{
    if (!g_test) return false;
    if (nx) *nx = g_test_x;
    if (ny) *ny = g_test_y;
    return true;
}

void testRecenter() { g_test_x = g_test_y = 0.5f; }

Diag diag()
{
    Diag d;
#ifdef __SWITCH__
    d.handles = g_six_n;
#else
    d.handles = 0;
#endif
    d.answering = g_gyro_ok;
    d.gx = g_gyro_x;  d.gy = g_gyro_y;
#ifdef __SWITCH__
    d.g0x = g_gyro[0][0]; d.g0y = g_gyro[0][1];
    d.g1x = g_gyro[1][0]; d.g1y = g_gyro[1][1];
    d.live0 = g_gyro_live[0]; d.live1 = g_gyro_live[1];
    d.picked = g_gyro_pick;
#else
    d.g0x = d.g0y = d.g1x = d.g1y = 0.0f;
    d.live0 = d.live1 = false;
    d.picked = 0;
#endif
    d.sx = g_last_sx; d.sy = g_last_sy;
    d.scroll = g_last_scroll;
    for (int i = 0; i < 3; i++) d.btn[i] = g_btn_down[i];
    return d;
}

void setMode(Mode m)
{
    if (m == g_mode) return;
    /* Leaving: release what we hold, and give the sensors back. Both are
     * failures of the "resource taken and not returned" kind if skipped - one
     * leaves a button stuck in the remote machine, the other drains the
     * Joy-Con. */
    releaseButtons();
#ifdef __SWITCH__
    if (m != Mode::Gyro) sixaxisStop();
    /* PM4 - started on the first frame instead of here: the handles depend on the
     * pad's style, and this function has no pad. Arming from the menu without a
     * controller in hand would otherwise fix the wrong style for the session. */
#endif
    pad_mouse_reset(&g_state);
    /* The clock is re-armed on the next frame: keeping the old timestamp would
     * make the first frame integrate everything since the mode was last used. */
    g_last_us = 0;
    g_mode = m;
    pmlog("[PM] gamepad mouse: %s",
          m == Mode::Off ? "arretee" : (m == Mode::Stick ? "joysticks" : "gyroscope"));
}

void toggle()
{
    if (g_mode != Mode::Off) { setMode(Mode::Off); return; }
    const uint32_t pref = Settings::instance().pad_mouse_mode;
    setMode(pref == 2 ? Mode::Gyro : Mode::Stick);
}

void shutdown()
{
    g_test = false;
    setMode(Mode::Off);
}

void poll(void *pad, uint64_t now_us)
{
#if SHADOW_PADMOUSE_HAS_PAD
    if (g_mode == Mode::Off) return;
    PadState *p = (PadState *)pad;
    if (!p) return;

    /* First frame after arming: we have no previous instant, so we take a
     * reading and emit nothing. Integrating against `0` would give the pointer
     * the whole uptime of the console. */
    if (g_last_us == 0) { g_last_us = now_us; return; }
    uint32_t dt_us = (uint32_t)(now_us - g_last_us);
    /* A clock that goes backwards is not impossible here (applet resume): the
     * module clamps dt, but a negative difference wraps to something enormous
     * before it gets there. */
    if (now_us < g_last_us) dt_us = 0;
    g_last_us = now_us;

    const HidAnalogStickState l = padGetStickPos(p, 0);
    const HidAnalogStickState r = padGetStickPos(p, 1);
    const uint64_t btn = padGetButtons(p);

    pad_mouse_in in = {};
    /* HID sticks are -32768..32767 with y UP, which is exactly what the pure
     * module expects once normalised. */
    in.sx = (float)r.x / 32767.0f;
    in.sy = (float)r.y / 32767.0f;
    in.scroll = (float)l.y / 32767.0f;
    g_last_sx = in.sx; g_last_sy = in.sy; g_last_scroll = in.scroll;

#if SHADOW_PADMOUSE_GYRO
    if (g_mode == Mode::Gyro) {
        /* PM4 - started HERE, with the pad in hand: the handles depend on the
         * controller's style, and `setMode` has no pad to ask. */
        if (!g_six_started) sixaxisStart(p);
        float gx = 0.0f, gy = 0.0f;
        if (readGyro(&gx, &gy)) {
            in.gx = gx; in.gy = gy;
            g_gyro_ok = true;
            g_gyro_x = gx; g_gyro_y = gy;
            /* The FIRST reading of the session, with its raw values. One line,
             * and it settles the only two things this can be: a sensor that does
             * not answer, or one that answers on a scale we got wrong. On screen
             * they look identical. */
            if (!g_gyro_logged) {
                g_gyro_logged = true;
                pmlog("[PM] 1re lecture gyroscope : x=%.4f y=%.4f (unite non mesuree)",
                      (double)gx, (double)gy);
            }
        } else if (!g_gyro_logged) {
            static int said = 0;
            if (said++ == 60)
                pmlog("[PM] the gyroscope returns NO state - %d handle(s)", g_six_n);
        }
    }
#endif  /* SHADOW_PADMOUSE_GYRO */

    const pad_mouse_cfg cfg = configFromSettings();
    pad_mouse_out out = {};
    pad_mouse_step(&g_state, &cfg, (pad_mouse_mode_t)g_mode, &in, dt_us, &out);

    /* PM3 - in test mode NOTHING reaches the VM. The dot moves, the remote
     * machine does not: that is what makes the test safe to run in a game, and
     * what makes it answer one question at a time. The 1920x1080 divisor is a
     * reference frame, not the real desktop - the dot is normalised, so the only
     * thing it fixes is how far a given gesture travels across the test area,
     * and that has to match what the same gesture does in the VM. */
    if (g_test) {
        g_test_x += (float)out.dx / 1920.0f;
        g_test_y += (float)out.dy / 1080.0f;
        if (g_test_x < 0.0f) g_test_x = 0.0f;
        if (g_test_x > 1.0f) g_test_x = 1.0f;
        if (g_test_y < 0.0f) g_test_y = 0.0f;
        if (g_test_y > 1.0f) g_test_y = 1.0f;
        return;                       /* buttons included: nothing goes out */
    }

    if (out.dx || out.dy) shadow_input_post_mouse_move(out.dx, out.dy);
    for (int i = 0; i < out.notches; i++)  shadow_input_post_mouse_wheel(1);
    for (int i = 0; i > out.notches; i--)  shadow_input_post_mouse_wheel(-1);

    /* === THE BUTTONS ===
     * The two triggers are the clicks: they are where the index fingers already
     * are, and a mouse is held the same way. R is the middle button, which is
     * rare enough to deserve the least reachable of the three.
     *
     * Sent on the EDGE and held: a click is a press and a release, and a game
     * that reads "button down" needs the down to persist. Pulsing would make
     * dragging impossible - which is most of what a mouse does. */
    static const struct { uint64_t mask; int button; } MAP[3] = {
        { HidNpadButton_ZR, 0 },   /* left   */
        { HidNpadButton_ZL, 1 },   /* right  */
        { HidNpadButton_R,  2 },   /* middle */
    };
    for (int i = 0; i < 3; i++) {
        const bool down = (btn & MAP[i].mask) != 0;
        if (down != g_btn_down[MAP[i].button]) {
            g_btn_down[MAP[i].button] = down;
            shadow_input_post_mouse_button(MAP[i].button, down);
        }
    }
#else
    /* No console pad on this target: nothing to read. */
    (void)pad; (void)now_us;
#endif
}

}  // namespace padmouse
