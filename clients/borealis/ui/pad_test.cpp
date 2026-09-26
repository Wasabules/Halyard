/* ui::padtest - see the header. */

#include "pad_test.hpp"

#include "hold_exit.hpp"   /* HOLD-1: leaving this page works off console too */
#include "i18n.hpp"
#include "key_hint.hpp"    /* K21: name the key, not the button */
#include "pad_draw.hpp"
#include "paint.hpp"
#include "theme.hpp"
#include "type.hpp"
#include "touch.h"
#include "rear_label.hpp"
#include "settings.hpp"
#include "../device_caps.h"

extern "C" {
#include "../../../core/input/rear_touch_sce.h"
}

#if defined(__vita__) || defined(__psp2__)
#  include <psp2/ctrl.h>
#  include <cstring>
#endif

#ifdef __SWITCH__
#include <switch.h>
#endif

extern "C" {
#include "../../../core/protocol/ctrl_gamepad.h"
#include "../../../core/protocol/rumble_hid.h"
}

#include <math.h>
#include <stdio.h>

namespace ui {
namespace padtest {

namespace {

/* S87 - the instant at which to stop the test rumble. 0 = none running.
 * FILE-SCOPED rather than a function-level `static`: this is state that
 * outlives a call, and this repo has paid for four outages caused by tucking
 * that kind of thing inside a function. The difference is slim here - there is
 * only one tester - but the rule beats the exception. */
double rumble_until = 0.0;

/* What the CONSOLE reads, sampled now. */
pad::State readConsole()
{
    pad::State e;
#ifdef __SWITCH__
    static PadState p;
    static bool ready = false;
    if (!ready) { padConfigureInput(8, HidNpadStyleSet_NpadStandard); padInitializeAny(&p); ready = true; }
    padUpdate(&p);
    const uint64_t b = padGetButtons(&p);
    e.present = true;
    e.pressed[pad::A]       = (b & HidNpadButton_A)      != 0;
    e.pressed[pad::B]       = (b & HidNpadButton_B)      != 0;
    e.pressed[pad::X]       = (b & HidNpadButton_X)      != 0;
    e.pressed[pad::Y]       = (b & HidNpadButton_Y)      != 0;
    e.pressed[pad::L]       = (b & HidNpadButton_L)      != 0;
    e.pressed[pad::R]       = (b & HidNpadButton_R)      != 0;
    e.pressed[pad::ZL]      = (b & HidNpadButton_ZL)     != 0;
    e.pressed[pad::ZR]      = (b & HidNpadButton_ZR)     != 0;
    e.pressed[pad::MINUS]   = (b & HidNpadButton_Minus)  != 0;
    e.pressed[pad::PLUS]    = (b & HidNpadButton_Plus)   != 0;
    e.pressed[pad::STICK_L] = (b & HidNpadButton_StickL) != 0;
    e.pressed[pad::STICK_R] = (b & HidNpadButton_StickR) != 0;
    e.pressed[pad::UP]      = (b & HidNpadButton_Up)     != 0;
    e.pressed[pad::DOWN]    = (b & HidNpadButton_Down)   != 0;
    e.pressed[pad::LEFT]    = (b & HidNpadButton_Left)   != 0;
    e.pressed[pad::RIGHT]   = (b & HidNpadButton_Right)  != 0;
    /* The Switch triggers are ALL OR NOTHING: the gauge is full or empty. That
     * is the hardware, not an approximation on our side - and seeing it saves
     * you from hunting for an analog travel that does not exist. */
    e.zl = e.pressed[pad::ZL] ? 1.0f : 0.0f;
    e.zr = e.pressed[pad::ZR] ? 1.0f : 0.0f;
    const HidAnalogStickState l = padGetStickPos(&p, 0);
    const HidAnalogStickState r = padGetStickPos(&p, 1);
    e.lx = (float)l.x / 32767.0f; e.ly = (float)l.y / 32767.0f;
    e.rx = (float)r.x / 32767.0f; e.ry = (float)r.y / 32767.0f;

#elif defined(__vita__) || defined(__psp2__)
    /* PS VITA. The tester showed an EMPTY pad on hardware: this function was
     * `#ifdef __SWITCH__` from end to end, so everywhere else it returned a
     * default-constructed state with `present = false`. Reported from a real
     * console, 2026-09-13.
     *
     * WHAT THIS DEVICE DOES NOT HAVE, and how that is shown: a Vita 1000/2000
     * has no ZL/ZR (its L and R are the only shoulders, and they are digital)
     * and no stick clicks (L3/R3). Leaving those four dark forever would read
     * as "the test does not see them", which is the very failure this screen
     * exists to detect.
     *
     * This comment used to say `absent[]` said so - and there was no
     * `absent[]`, in this file or any other. The mechanism was described and
     * never written, so on hardware the four sat there permanently unlit,
     * exactly the reading it claimed to prevent. Corrected 2026-09-13 by
     * drawing the console instead of annotating it: `pad_draw.cpp` has a
     * PlayStation layout (`SHADOW_PAD_ART`) which simply does not put those
     * four controls on screen. A control the machine does not have is missing
     * from the picture rather than shown unpressed.
     *
     * On a PlayStation TV a DualShock 3/4 DOES provide all six, and the same
     * bits arrive here - which is why they are read rather than skipped. What
     * is absent is the HANDHELD's hardware, not the API's. */
    static bool sampling_set = false;
    if (!sampling_set) {
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
        sampling_set = true;
    }
    SceCtrlData d;
    memset(&d, 0, sizeof d);
    if (sceCtrlPeekBufferPositive(0, &d, 1) >= 0) {
        const unsigned b = d.buttons;
        e.present = true;
        /* MAPPED THE WAY BOREALIS MAPS, not by physical position. Borealis'
         * own PSV input (`psv_input.cpp`) binds BUTTON_A to CROSS, B to CIRCLE,
         * X to SQUARE and Y to TRIANGLE, and the rest of the application reads
         * that. A tester that disagreed with it would be worse than no tester:
         * it would report a defect in the mapping every time, and the first
         * thing anyone would do is go looking for one.
         *
         * The drawn pad is Switch-shaped, so on a Vita "A" lights on the right
         * while the finger is on the bottom button. That is a mismatch between
         * the DRAWING and this console, and it is the honest place to leave it
         * - the alternative hides a real disagreement to make a picture line
         * up. */
        e.pressed[pad::A]       = (b & SCE_CTRL_CROSS)    != 0;
        e.pressed[pad::B]       = (b & SCE_CTRL_CIRCLE)   != 0;
        e.pressed[pad::X]       = (b & SCE_CTRL_SQUARE)   != 0;
        e.pressed[pad::Y]       = (b & SCE_CTRL_TRIANGLE) != 0;
        e.pressed[pad::L]       = (b & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) != 0;
        e.pressed[pad::R]       = (b & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) != 0;
        e.pressed[pad::ZL]      = false;   /* no second shoulder pair */
        e.pressed[pad::ZR]      = false;
        e.pressed[pad::MINUS]   = (b & SCE_CTRL_SELECT)   != 0;
        e.pressed[pad::PLUS]    = (b & SCE_CTRL_START)    != 0;
        e.pressed[pad::STICK_L] = (b & SCE_CTRL_L3)       != 0;   /* PS TV only */
        e.pressed[pad::STICK_R] = (b & SCE_CTRL_R3)       != 0;
        e.pressed[pad::UP]      = (b & SCE_CTRL_UP)       != 0;
        e.pressed[pad::DOWN]    = (b & SCE_CTRL_DOWN)     != 0;
        e.pressed[pad::LEFT]    = (b & SCE_CTRL_LEFT)     != 0;
        e.pressed[pad::RIGHT]   = (b & SCE_CTRL_RIGHT)    != 0;
        e.zl = 0.0f;   /* digital shoulders: there is no travel to show */
        e.zr = 0.0f;
        /* The sticks arrive as bytes centred on 128. Y is inverted to the
         * display convention (positive = upwards), the same conversion the
         * Switch branch does through its own sign. */
        e.lx =  ((float)d.lx - 128.0f) / 127.0f;
        e.ly = -((float)d.ly - 128.0f) / 127.0f;
        e.rx =  ((float)d.rx - 128.0f) / 127.0f;
        e.ry = -((float)d.ry - 128.0f) / 127.0f;
    }
#endif
    return e;
}

/* What we ACTUALLY sent, read back from ctrl_gamepad's mirror. */
pad::State readSent()
{
    pad::State e;
    uint8_t axes[6] = {128, 128, 128, 128, 0, 0};
    uint16_t buttons = 0;
    uint8_t dpad = 0;
    ctrl_gamepad_last_sent(axes, &buttons, &dpad);

    /* A closed channel is NOT "everything released": it is "we are sending
     * nothing". The gamepad then draws faded, which is distinguishable at a
     * glance and saves you from hunting an encoding defect where there is
     * simply no channel. */
    e.present = ctrl_gamepad_active();

    static const struct { int shadow; pad::Button t; } MAP[] = {
        { SHADOW_PAD_CROSS, pad::B },      { SHADOW_PAD_CIRCLE,   pad::A },
        { SHADOW_PAD_SQUARE, pad::Y },      { SHADOW_PAD_TRIANGLE, pad::X },
        { SHADOW_PAD_L1,    pad::L },      { SHADOW_PAD_R1,       pad::R },
        { SHADOW_PAD_L3,    pad::STICK_L },{ SHADOW_PAD_R3,       pad::STICK_R },
        { SHADOW_PAD_SELECT,pad::MINUS },  { SHADOW_PAD_START,    pad::PLUS },
    };
    for (const auto &c : MAP)
        if (buttons & (uint16_t)(1u << c.shadow)) e.pressed[c.t] = true;

    e.pressed[pad::UP]    = (dpad & 0x01) != 0;
    e.pressed[pad::DOWN]  = (dpad & 0x02) != 0;
    e.pressed[pad::LEFT]  = (dpad & 0x04) != 0;
    e.pressed[pad::RIGHT] = (dpad & 0x08) != 0;

    /* The axes go out as bytes centred on 128: we map them back to [-1, 1].
     * This is exactly the scale G48 was fixing - seeing it on screen would have
     * shown the defect in one second.
     *
     * === MIND THE AXIS NUMBERING ===
     * It is COUNTER-INTUITIVE and it caught me out: in Shadow's order the RIGHT
     * stick comes first.
     *     0 = RX, 1 = RY, 2 = LX, 3 = LY
     * Reading `axes[0]` as the left stick therefore swapped the two on screen -
     * a defect reported on the very first run of the tester. We now index by
     * the constants, never by a bare number: `SHADOW_AXIS_LX` cannot pick the
     * wrong half. */
    e.lx =  ((float)axes[SHADOW_AXIS_LX] - 128.0f) / 127.0f;
    e.ly = -((float)axes[SHADOW_AXIS_LY] - 128.0f) / 127.0f;
    e.rx =  ((float)axes[SHADOW_AXIS_RX] - 128.0f) / 127.0f;
    e.ry = -((float)axes[SHADOW_AXIS_RY] - 128.0f) / 127.0f;
    e.zl = (float)axes[SHADOW_AXIS_L2] / 255.0f;
    e.zr = (float)axes[SHADOW_AXIS_R2] / 255.0f;
    return e;
}


/* === S87 - THE NUMERIC READOUT OF A STICK ===
 *
 * The drawing answers "is the stick moving?". It does NOT answer "does it come
 * back exactly to centre?", nor "does it reach the edge?", nor "is the dead
 * zone set right?" - three questions you ask of a drifting gamepad, and all
 * three need NUMBERS.
 *
 * The magnitude is given as a percentage because that is how the dead zone in
 * the neighbouring setting is expressed: the two then compare without mental
 * arithmetic. The angle is only shown above the noise floor - at rest it would
 * spin wildly and read like a fault. */
void metrics(NVGcontext *vg, float x, float y, float w,
             const char *name, float ax, float ay, bool click)
{
    if (isnan(ax)) ax = 0.0f;
    if (isnan(ay)) ay = 0.0f;

    const float amp = sqrtf(ax * ax + ay * ay);
    char line[96];
    if (amp > 0.06f) {
        /* atan2 returns [-pi, pi]; we map it to 0..359 degrees, zero pointing
         * UP and turning clockwise - the compass-rose convention, the one you
         * read without thinking about it. */
        float deg = atan2f(ax, ay) * 180.0f / 3.14159265f;
        if (deg < 0.0f) deg += 360.0f;
        snprintf(line, sizeof line, "X %+.2f   Y %+.2f   %3.0f %%   %3.0f deg",
                 (double)ax, (double)ay, (double)(amp * 100.0f), (double)deg);
    } else {
        snprintf(line, sizeof line, "X %+.2f   Y %+.2f   %3.0f %%",
                 (double)ax, (double)ay, (double)(amp * 100.0f));
    }

    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, type::SECONDARY);
    nvgFillColor(vg, click ? paint::accentVif : theme::label);
    nvgText(vg, x, y, name, nullptr);

    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, theme::value);
    nvgText(vg, x + w, y, line, nullptr);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
}

/* A TOUCH button. It uses no gamepad button at all, and that is the only
 * workable choice here: in a tester every button is already under observation -
 * giving one of them a second job would make the console buzz the moment you
 * checked that button.
 *
 * Returns true on RELEASE inside the frame, never on press: a finger that
 * slides off the button must be able to cancel, as everywhere else. */
bool touchButton(NVGcontext *vg, float x, float y, float w, float h,
                 const char *label, bool *inside)
{
    const ui_touch_t d = ui_touch_current();
    const bool over = d.active && d.x >= x && d.x <= x + w && d.y >= y && d.y <= y + h;

    bool fired = false;
    if (over)                        *inside = true;
    else if (*inside && !d.active) { *inside = false; fired = true; }
    else if (!d.active)               *inside = false;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, paint::CARD_RADIUS);
    nvgFillColor(vg, over ? paint::cardBgFocusTop : paint::cardBgTop);
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, paint::CARD_RADIUS);
    nvgStrokeWidth(vg, over ? 1.8f : 1.0f);
    nvgStrokeColor(vg, over ? paint::accentVif : paint::cardBorder);
    nvgStroke(vg);

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, type::BODY);
    nvgFillColor(vg, theme::title);
    nvgText(vg, x + w * 0.5f, y + h * 0.5f, label, nullptr);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    return fired;
}

}  // namespace

#if SHADOW_HAS_REAR_PAD

/* === THE REAR PANEL, DRAWN FROM THE SAME NUMBERS THE RECOGNISER IS FED ====
 *
 * A tester exists to separate causes. "The rear panel does not work" has four
 * of them: the panel reports nothing, the contact lands in a zone you did not
 * mean, the zone is unmapped, or the gesture is not the one you are making.
 * Drawing the contacts and the zones together answers all four at a glance -
 * you can see your own finger, which quarter it is in, what that quarter is
 * for, and whether the button lit up.
 *
 * The points come from `shadow_rear_touch_poll`'s own output rather than from
 * a second `sceTouchPeek` here. A second reading would be a second truth, and
 * the two would disagree exactly when it mattered - a frame apart, one finger
 * lifted in one and not the other. What is drawn IS what was judged.
 *
 * === WHY IT KEEPS ITS OWN CONFIG AND STATE ===
 *
 * The tester runs while `pad_forward` may also be polling, and the recogniser
 * is deliberately caller-owned state (`rear_touch.h`). Two callers therefore
 * need two states - sharing one would have each consume the other's transitions
 * and both would miss taps. The CONFIG is re-read from the settings every
 * frame, so a zone changed in the settings screen is already right when you
 * come back here. */
rear_touch_config rear_cfg;
rear_touch_state  rear_state;

void rearConfig(rear_touch_config *c)
{
    rear_touch_defaults(c);
    const Settings &s = Settings::instance();
    const uint32_t az[4] = { s.rear_zone_tl, s.rear_zone_tr,
                             s.rear_zone_bl, s.rear_zone_br };
    const uint32_t gz[4] = { s.rear_gest_tl, s.rear_gest_tr,
                             s.rear_gest_bl, s.rear_gest_br };
    for (int i = 0; i < REAR_ZONE_COUNT; i++) {
        c->action[i]  = (rear_action)(az[i] < (uint32_t)REAR_ACT_COUNT ? az[i] : 0);
        c->gesture[i] = (rear_gesture)(gz[i] <= (uint32_t)REAR_GEST_SLIDE ? gz[i] : 0);
    }
    c->two_finger = (rear_action)(s.rear_two_finger < (uint32_t)REAR_ACT_COUNT
                                  ? s.rear_two_finger : 0);
    c->slide_full = (float)s.rear_slide_pct / 100.0f;
    /* The tester ALWAYS runs the recogniser, even with the feature switched
     * off. That is the point: you come here to decide whether to switch it on,
     * and a panel that shows nothing until you have already committed cannot
     * help you decide. Nothing is sent to the VM from this page. */
    c->enabled = true;
}

/* One quarter: its bounds, its name, what it sends, and whether it is lit. */
void rearZone(NVGcontext *vg, float x, float y, float w, float h,
              int z, uint32_t act, bool lit, float pull)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 2.0f, y + 2.0f, w - 4.0f, h - 4.0f, 8.0f);
    nvgFillColor(vg, act == REAR_ACT_NONE ? nvgRGBA(255, 255, 255, 8)
                                          : nvgRGBA(120, 170, 255, lit ? 62 : 20));
    nvgFill(vg);
    nvgStrokeWidth(vg, 1.0f);
    nvgStrokeColor(vg, lit ? paint::accentVif : nvgRGBA(150, 176, 224, 46));
    nvgStroke(vg);

    /* An analog trigger shows its TRAVEL as a bar, because that is the thing
     * being tested: a digital button either is or is not pressed, but a trigger
     * that reaches only 60 % looks identical to a working one without this. */
    if ((act == REAR_ACT_ZL_ANALOG || act == REAR_ACT_ZR_ANALOG) && pull > 0.0f) {
        const float bh = 5.0f, by = y + h - 12.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 12.0f, by, (w - 24.0f) * pull, bh, bh * 0.5f);
        nvgFillColor(vg, paint::accentVif);
        nvgFill(vg);
    }

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, type::CAPTION);
    nvgFillColor(vg, theme::hint);
    nvgText(vg, x + w * 0.5f, y + h * 0.5f - 12.0f,
            ui::rearZoneLabel(z).c_str(), nullptr);
    nvgFontSize(vg, type::SECTION);
    nvgFillColor(vg, act == REAR_ACT_NONE ? theme::label
                                          : (lit ? paint::accentVif : theme::title));
    nvgText(vg, x + w * 0.5f, y + h * 0.5f + 10.0f,
            ui::rearActionShort(act).c_str(), nullptr);
}

/* Draws the panel and returns the height it used. */
float rearPanel(NVGcontext *vg, float x, float y, float w)
{
    rearConfig(&rear_cfg);
    rear_point pts[REAR_MAX_POINTS];
    size_t n = 0;
    const rear_touch_out o =
        shadow_rear_touch_poll(&rear_cfg, &rear_state, pts, &n);

    /* The panel is wider than it is tall, like the real one. Bounded so it
     * cannot crowd out the gamepad above it on a 544-line screen. */
    float pw = w - 96.0f;
    if (pw > 420.0f) pw = 420.0f;
    const float ph = pw * 0.42f;
    const float px = x + (w - pw) * 0.5f;
    float py = y;

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, type::CAPTION);
    nvgFillColor(vg, theme::hint);
    nvgText(vg, x + w * 0.5f, py, tr("pad/rear_test_desc").c_str(), nullptr);
    py += 16.0f;

    const float hw = pw * 0.5f, hh = ph * 0.5f;
    const uint32_t act[4] = { rear_cfg.action[0], rear_cfg.action[1],
                              rear_cfg.action[2], rear_cfg.action[3] };
    const bool lit[4] = {
        (act[0] == REAR_ACT_ZL && o.zl) || (act[0] == REAR_ACT_ZR && o.zr)
            || (act[0] == REAR_ACT_L3 && o.l3) || (act[0] == REAR_ACT_R3 && o.r3)
            || (act[0] == REAR_ACT_ZL_ANALOG && o.l2 > 0)
            || (act[0] == REAR_ACT_ZR_ANALOG && o.r2 > 0),
        (act[1] == REAR_ACT_ZL && o.zl) || (act[1] == REAR_ACT_ZR && o.zr)
            || (act[1] == REAR_ACT_L3 && o.l3) || (act[1] == REAR_ACT_R3 && o.r3)
            || (act[1] == REAR_ACT_ZL_ANALOG && o.l2 > 0)
            || (act[1] == REAR_ACT_ZR_ANALOG && o.r2 > 0),
        (act[2] == REAR_ACT_ZL && o.zl) || (act[2] == REAR_ACT_ZR && o.zr)
            || (act[2] == REAR_ACT_L3 && o.l3) || (act[2] == REAR_ACT_R3 && o.r3)
            || (act[2] == REAR_ACT_ZL_ANALOG && o.l2 > 0)
            || (act[2] == REAR_ACT_ZR_ANALOG && o.r2 > 0),
        (act[3] == REAR_ACT_ZL && o.zl) || (act[3] == REAR_ACT_ZR && o.zr)
            || (act[3] == REAR_ACT_L3 && o.l3) || (act[3] == REAR_ACT_R3 && o.r3)
            || (act[3] == REAR_ACT_ZL_ANALOG && o.l2 > 0)
            || (act[3] == REAR_ACT_ZR_ANALOG && o.r2 > 0),
    };
    for (int z = 0; z < 4; z++) {
        const float zx = px + (z & 1 ? hw : 0.0f);
        const float zy = py + (z >= 2 ? hh : 0.0f);
        const float pull = (act[z] == REAR_ACT_ZL_ANALOG) ? (float)o.l2 / 255.0f
                         : (act[z] == REAR_ACT_ZR_ANALOG) ? (float)o.r2 / 255.0f
                         : 0.0f;
        rearZone(vg, zx, zy, hw, hh, z, act[z], lit[z], pull);
    }

    /* The contacts, on top of the zones so a finger is never hidden by one. */
    for (size_t i = 0; i < n; i++) {
        const float cx = px + pts[i].x * pw, cy = py + pts[i].y * ph;
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 13.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 30));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 6.0f);
        nvgFillColor(vg, paint::accentVif);
        nvgFill(vg);
    }
    py += ph + 6.0f;

    /* The two-finger action and the count, spelled out: without the count you
     * cannot tell "the panel saw one finger" from "the second one was late",
     * which is the whole difference between the gesture firing and not. */
    char line[160];
    snprintf(line, sizeof line, "%s: %s   \u00b7   %u", tr("pad/rear_two").c_str(),
             ui::rearActionShort(rear_cfg.two_finger).c_str(), (unsigned)n);
    nvgFontSize(vg, type::CAPTION);
    nvgFillColor(vg, theme::hint);
    nvgText(vg, x + w * 0.5f, py + 8.0f, line, nullptr);
    py += 20.0f;

    /* Say plainly that nothing is being sent yet, when nothing is. Otherwise a
     * panel that lights up here and does nothing in a game is a mystery. */
    if (!Settings::instance().rear_touch) {
        nvgFillColor(vg, theme::label);
        nvgText(vg, x + w * 0.5f, py + 6.0f, tr("pad/rear_off_hint").c_str(), nullptr);
        py += 18.0f;
    }
    return py - y;
}

#endif  /* SHADOW_HAS_REAR_PAD */

bool draw(NVGcontext *vg, float x, float y, float w, float h, double t,
              Mode mode)
{
    /* A dimmed backdrop rather than an opaque one: over the stream we want to
     * keep seeing what we just paused; in the Gamepad screen the gradient is
     * already underneath. One rendering therefore suits both hosts. */
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillColor(vg, nvgRGBA(6, 10, 20, 226));
    nvgFill(vg);

    nvgFontFace(vg, theme::font());

    const pad::State rd = readConsole();

    /* Height reserved for the footer: the exit hint, plus the diagnostic when
     * there is one. Computed here so both layouts share it. */
    const float FOOTER = 76.0f;

    if (mode == Mode::Simple) {
        /* === S87 - A SINGLE GAMEPAD, LARGE ===
         *
         * Outside a session there is nothing to compare: the "sent" half would
         * be permanently dark. So we give the whole width to the one
         * measurement that exists, and the title moves up to screen-title size
         * - it is THE subject of the page, not a column caption. */
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(vg, type::SCREEN);
        nvgFillColor(vg, theme::title);
        nvgText(vg, x + w * 0.5f, y + 44.0f, tr("pad/read").c_str(), nullptr);

        nvgFontSize(vg, type::SECONDARY);
        nvgFillColor(vg, theme::hint);
        nvgText(vg, x + w * 0.5f, y + 72.0f, tr("pad/read_desc").c_str(), nullptr);

        /* The numeric readout and the rumble test take a strip under the
         * gamepad. It is sized FIRST, so that the gamepad gets everything that
         * is left - it is the thing you look at. */
        const float STRIP = 116.0f;
        const float pad_y = y + 92.0f;
        const float pad_h = h - 92.0f - STRIP - FOOTER;

        /* === TWO COLUMNS WHERE THERE IS A REAR PANEL =======================
         *
         * Stacking the panel under the gamepad does not fit: 544 lines already
         * carry a title, the gamepad, two stick readouts and a footer, and what
         * was left for the panel was smaller than a fingertip. The Vita's
         * screen is wide, though, and the two are the same hardware seen from
         * two sides - front on the left, back on the right - so side by side is
         * also the truer arrangement, not merely the one that fits. */
#if SHADOW_HAS_REAR_PAD
        const float col = w * 0.54f;
#else
        const float col = w;
#endif
        pad::draw(vg, x, pad_y, col, pad_h, rd, paint::accentVif, t);
#if SHADOW_HAS_REAR_PAD
        rearPanel(vg, x + col, pad_y + 10.0f, w - col);
#endif

        /* --- The numeric readout of both sticks -------------------------- */
        float bw = 520.0f;
        if (bw > col - 24.0f) bw = col - 24.0f;
        const float bx = x + (col - bw) * 0.5f;
        float by = pad_y + pad_h + 18.0f;

        metrics(vg, bx, by, bw, tr("pad/stick_l").c_str(),
                rd.lx, rd.ly, rd.pressed[pad::STICK_L]);
        by += 26.0f;
        metrics(vg, bx, by, bw, tr("pad/stick_r").c_str(),
                rd.rx, rd.ry, rd.pressed[pad::STICK_R]);
        by += 30.0f;

        /* --- The rumble test --------------------------------------------- */
        /* `inside` is a function-level `static`, and one of the rare cases this
         * repo accepts: it is not session state but the state of a GESTURE in
         * progress, only one tester is on screen at a time, and it resets
         * itself the moment the finger lifts - so on entering the mode as well
         * as on leaving it. Same reasoning as the long-press measurement
         * below. */
        /* NO MOTORS, NO TRIAL. The same rule as the settings screen: a button
         * that answers your press and produces nothing teaches you it works.
         * `rumble_until` and its shut-off below stay compiled either way - a
         * motor left running is worse than a missing button. */
#if SHADOW_HAS_RUMBLE
        static bool inside = false;
        const float btn_w = 260.0f, btn_h = 40.0f;
        if (touchButton(vg, x + (col - btn_w) * 0.5f, by, btn_w, btn_h,
                        tr("pad/rumble_test").c_str(), &inside)) {
            /* A SHORT pulse on both motors: we are checking that they respond,
             * not how they balance - that is the job of the four separate
             * tests in the settings screen, which do not stop on their own.
             * Switching off is armed by the timestamp below rather than by a
             * wait: sleeping here would freeze the display. */
            rumble_hid_apply(200, 200);
            rumble_until = t + 0.25;
        }
#else
        (void)by;
#endif
        if (rumble_until > 0.0 && t >= rumble_until) {
            rumble_until = 0.0;
            rumble_hid_apply(0, 0);
        }
    } else {
        /* NO REAR PANEL IN THE SPLIT VIEW, AND THAT IS THE RIGHT ANSWER.
         * This mode exists to compare what the console READS with what is SENT
         * to the machine, and a rear-panel button is sent - so it already
         * appears, as ZL, ZR or a stick click, in the right-hand column. Drawing
         * the panel again here would answer a question this mode is not asking,
         * and would take the width the comparison needs. */
        const pad::State sent = readSent();
        const float half = w * 0.5f;

        /* --- The two headings -------------------------------------------- */
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(vg, type::SECTION);
        nvgFillColor(vg, theme::title);
        nvgText(vg, x + half * 0.5f, y + 46.0f, tr("pad/read").c_str(), nullptr);
        nvgText(vg, x + half * 1.5f, y + 46.0f, tr("pad/sent").c_str(), nullptr);

        /* Under each title, what the column MEANS. Without that, "read" and
         * "sent" look too alike to know which one to blame. */
        nvgFontSize(vg, type::CAPTION);
        nvgFillColor(vg, theme::hint);
        nvgText(vg, x + half * 0.5f, y + 68.0f, tr("pad/read_desc").c_str(), nullptr);
        nvgText(vg, x + half * 1.5f, y + 68.0f,
                sent.present ? tr("pad/sent_desc").c_str()
                             : tr("pad/sent_no_session").c_str(), nullptr);

        /* --- The two gamepads -------------------------------------------- */
        const float pad_y = y + 88.0f;
        const float pad_h = h - 88.0f - FOOTER;
        pad::draw(vg, x, pad_y, half, pad_h, rd, paint::accentVif, t);
        /* S111 - the second gamepad has its own trail rings. */
        pad::draw(vg, x + half, pad_y, half, pad_h, sent,
                  sent.present ? paint::ledActive : paint::ledOff,
                  t, /*gamepad=*/1);

        /* The rule says these are TWO measurements, not one gamepad drawn
         * twice. */
        nvgBeginPath(vg);
        nvgMoveTo(vg, x + half, pad_y + 8.0f);
        nvgLineTo(vg, x + half, pad_y + pad_h - 8.0f);
        nvgStrokeWidth(vg, 1.0f);
        nvgStrokeColor(vg, nvgRGBA(150, 176, 224, 52));
        nvgStroke(vg);

        /* --- Channel diagnostic, spelled out ----------------------------- */
        char diag[128];
        ctrl_gamepad_diagnostic(diag, sizeof diag);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(vg, type::SECONDARY);
        nvgFillColor(vg, theme::hint);
        nvgText(vg, x + w * 0.5f, y + h - 48.0f, diag, nullptr);
    }

    /* --- Exit: the hint, then the bar while you hold -------------------- */
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, type::CAPTION);
    nvgFillColor(vg, theme::label);
    nvgText(vg, x + w * 0.5f, y + h - 26.0f,
            tr("pad/exit", hintGlyph("B")).c_str(), nullptr);

    /* === The long-press measurement ===
     * HOLD-1 2026-09-12: it used to read `rd.pressed[pad::B]`, which is the
     * CONSOLE pad - `readConsole()` is entirely inside `#ifdef __SWITCH__` and
     * reports nothing pressed anywhere else. So on desktop this page could not
     * be left either, exactly like the other two testers, and for a slightly
     * different reason: here the code was compiled, it just read a source that
     * is always empty off console.
     *
     * It now goes through the shared reader, which is libnx on console and
     * Borealis' unified state elsewhere - so a gamepad plugged into a desktop
     * works too, and so does Escape. Releasing still cancels, and the state
     * lives in `ui/hold_exit.hpp` at module scope rather than in a function
     * `static`. */
    float exit_progress = 0.0f;

    if (ui::holdExitStep(ui::HOLD_PAD, t, &exit_progress)) {
        /* NEVER leave a motor running on the way out: the protocol carries
         * no duration, so a rumble in flight would hold indefinitely. */
        if (rumble_until > 0.0) { rumble_until = 0.0; rumble_hid_apply(0, 0); }
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        return true;
    }

    if (exit_progress > 0.0f) {
        if (exit_progress > 1.0f) exit_progress = 1.0f;
        const float bw2 = 220.0f, bx2 = x + (w - bw2) * 0.5f, by2 = y + h - 12.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx2, by2, bw2, 4.0f, 2.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 34));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx2, by2, bw2 * exit_progress, 4.0f, 2.0f);
        nvgFillColor(vg, paint::accentVif);
        nvgFill(vg);
    }

    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    return false;
}

}  // namespace padtest
}  // namespace ui
