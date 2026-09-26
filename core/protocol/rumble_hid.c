/* rumble_hid.c - see rumble_hid.h. */

#include "rumble_hid.h"

#include "../common/log.h"

/* S81 - this module's log category. See shadow/journal.h: the category is
 * declared here, never inferred from the text of the messages. */
#define rhlog(...) JOURNAL_INFO_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)
#define rhdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_GAMEPAD, __VA_ARGS__)

#include <stdlib.h>

#ifdef __SWITCH__
#include <switch.h>

static bool                     g_prises = false;
static bool                     g_tentee = false;
static HidVibrationDeviceHandle g_poignees[2];

static bool prendre(void)
{
    if (g_prises)  return true;
    if (g_tentee)  return false;   /* a failure is not retried on every frame */
    g_tentee = true;

    /* === THE STYLE IS A *TAG*, NOT A *SET* (G57b) ===
     * Passing `HidNpadStyleSet_NpadFullCtrl` - an OR of three bits - where the
     * signature expects a single bit compiles without a word of warning and then
     * returns an error. So we try the three real styles, most likely first. */
    static const struct { HidNpadIdType id; HidNpadStyleTag style; const char *name; } essais[] = {
        { HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, "portable"    },
        { HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey,  "manette Pro" },
        { HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual,  "Joy-Cons"    },
    };
    for (size_t k = 0; k < sizeof essais / sizeof essais[0]; k++) {
        const Result rc = hidInitializeVibrationDevices(g_poignees, 2,
                                                        essais[k].id, essais[k].style);
        rhlog("[G57] poignees de vibration (%s) : rc=0x%08x",
                   essais[k].name, (unsigned)rc);
        if (R_SUCCEEDED(rc)) { g_prises = true; return true; }
    }
    rhlog("[G57] NO style returned a handle - rumble is impossible");
    return false;
}

/* === G57d - INTENSITY IS SET HERE, AND NOWHERE ELSE ===
 * This is the single gateway to the motors: putting the scaling in the callers
 * would force every one of them to remember it, and the next one would forget.
 *
 * At 0% we still EMIT zeros, we do not skip the call: the protocol carries no
 * duration, so a rumble already under way would hold forever if we simply went
 * quiet. "Stop" is an order, not a silence.
 *
 * `SHADOW_RUMBLE_PCT` is pushed by the settings screen
 * (Settings::applyToggles) and still wins if it is set by hand.
 *
 * === B3 2026-09-02 - NO `static` CACHE HERE, AND THAT IS THE WHOLE POINT ===
 *
 * It had one, read on the FIRST call. The intensity is a UI setting, so the
 * first call happened before the user had touched it - the first interface
 * haptic, or the first rumble of the session - and the value was frozen for the
 * life of the process. Changing the intensity then changed NOTHING until the
 * application was restarted, which is exactly how it was reported: "I change
 * the intensity and I feel no difference on the real trials."
 *
 * This is the defect family CLAUDE.md names first - session state in a function
 * `static` - and `ctrl_session.c` already carries the identical fix, in the same
 * words, for `SHADOW_UDP_REG_13`: "the UI can change this setting between two
 * connections of the SAME process, and a static would keep the first value".
 *
 * Re-reading costs a `getenv` plus an `atoi` per rumble message. The gamepad
 * emits a few per second; the video path emits none. Caching bought nothing
 * measurable and cost the setting. */
static uint8_t scale(uint8_t v)
{
    const char *e = getenv("SHADOW_RUMBLE_PCT");
    int pct = e ? atoi(e) : 100;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)(((unsigned)v * (unsigned)pct) / 100u);
}

bool rumble_hid_apply(uint8_t basse, uint8_t haute)
{
    if (!prendre()) return false;
    basse = scale(basse);
    haute = scale(haute);

    /* FIXED FREQUENCIES, only the amplitudes vary. The protocol carries
     * amplitudes ONLY, and 160/320 Hz is the pair Borealis itself treats as the
     * resting point - that is the useful band of a linear actuator. Deriving the
     * frequency from the amplitude, as Borealis' `sendRumble()` does, would give
     * 10 Hz at 20% power: nothing you can feel. */
    HidVibrationValue v[2] = {0};
    for (int k = 0; k < 2; k++) {
        v[k].amp_low   = (float)basse / 255.0f;
        v[k].freq_low  = 160.0f;
        v[k].amp_high  = (float)haute / 255.0f;
        v[k].freq_high = 320.0f;
    }
    const Result rc = hidSendVibrationValues(g_poignees, v, 2);

    /* The witness that was missing: `hidGetActualVibrationValue` returns what
     * the CONSOLE really applies. It is the only point in the chain where we
     * step outside our own assumptions. */
    static uint32_t n = 0;
    if (++n <= 5 || (n % 200) == 0) {
        HidVibrationValue reel = {0};
        const Result rl = hidGetActualVibrationValue(g_poignees[0], &reel);
        rhlog("[G57] send #%u low=%u high=%u -> rc=0x%08x | "
                   "applique amp_low=%.3f amp_high=%.3f (rc=0x%08x)",
                   n, (unsigned)basse, (unsigned)haute, (unsigned)rc,
                   (double)reel.amp_low, (double)reel.amp_high, (unsigned)rl);
    }
    return R_SUCCEEDED(rc);
}

void rumble_hid_stop(void) { (void)rumble_hid_apply(0, 0); }

#else  /* ===== hors Switch ===== */

/* The desktop build can NOT serve as a witness: `GLFWInputManager::sendRumble`
 * has an EMPTY body and `USE_SDL2` is OFF. Silence there would prove nothing, so
 * we say so rather than let it look like a failure. */
bool rumble_hid_apply(uint8_t basse, uint8_t haute)
{
    static int dit = 0;
    if (!dit++) rhlog("[G57] rumble ignored: this binary is not the console "
                           "(basse=%u haute=%u)", (unsigned)basse, (unsigned)haute);
    return false;
}
void rumble_hid_stop(void) { }

#endif
