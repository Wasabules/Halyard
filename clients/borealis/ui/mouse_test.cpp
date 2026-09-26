/* ui::mousetest - see the header. */
#include "mouse_test.hpp"
#include "../devlink/inject.hpp"   /* INJ-1: synthetic input is read here too */
#include "theme.hpp"
#include "i18n.hpp"
#include "settings.hpp"
#include "../../../core/input/pad_mouse_hid.hpp"
#include "../../../core/input/pad_mouse.h"        /* PM7 - the scale the readouts are shown on */

#include <borealis.hpp>

#include "hold_exit.hpp"   /* HOLD-1: leaving this page works off console too */
#include "key_hint.hpp"    /* K21: name the key, not the button */

#include <cstdio>
#include <string>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace ui {
namespace mousetest {
namespace {

/* A labelled bar, -1..1 or 0..1, with the figure beside it. The bar is what
 * says "it moves"; the figure is what says "by how much". Neither alone
 * answers the question this page exists for. */
void bar(NVGcontext *vg, float x, float y, float w, const char *label,
         float v, float range, bool bipolar)
{
    const float h = 10.0f;
    nvgFontSize(vg, 15.0f);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 150));
    nvgText(vg, x, y + h * 0.5f, label, nullptr);

    const float bx = x + 132.0f, bw = w - 132.0f - 96.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bx, y, bw, h, h * 0.5f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 26));
    nvgFill(vg);

    float n = (range > 0.0f) ? v / range : 0.0f;
    if (n >  1.0f) n =  1.0f;
    if (n < -1.0f) n = -1.0f;
    if (n != n)    n =  0.0f;            /* a NaN must not draw off screen */

    if (bipolar) {
        const float mid = bx + bw * 0.5f;
        const float len = n * bw * 0.5f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, len >= 0.0f ? mid : mid + len, y,
                       len >= 0.0f ? len : -len, h, h * 0.5f);
    } else {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx, y, bw * (n < 0.0f ? -n : n), h, h * 0.5f);
    }
    nvgFillColor(vg, nvgRGBA(120, 200, 255, 220));
    nvgFill(vg);

    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.4f", (double)v);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 210));
    nvgText(vg, x + w, y + h * 0.5f, buf, nullptr);
}

void line(NVGcontext *vg, float x, float y, const char *label,
          const std::string &value, NVGcolor col)
{
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 150));
    nvgText(vg, x, y, label, nullptr);
    nvgFillColor(vg, col);
    nvgText(vg, x + 132.0f, y, value.c_str(), nullptr);
}

}  // namespace

bool draw(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    /* The page OWNS the test while it is up: it turns it on when it opens and
     * off when it leaves. Leaving it to the caller would let the two hosts do it
     * differently, and one of them would eventually leave it running.
     *
     * === PM6 2026-09-02 - AND IT DRIVES THE MODULE ITSELF ===
     *
     * The first version relied on `StreamView` calling `padmouse::poll()` once
     * per frame. That is the ONLY caller there is - so opening this page from
     * the settings, where no stream view exists, left the module never polled:
     * no sensor was ever obtained, and the page reported "Sensors 0 / Answering
     * no". Which is exactly what it was built to report, except that it was
     * describing OUR wiring rather than the console's hardware.
     *
     * A page whose readings depend on another screen being drawn is a page that
     * lies about a different subsystem the day that screen is not there. It
     * polls here, from where the readings are needed, and the stream stands
     * aside while it is up (`padmouse::testMode()`), so the module is stepped
     * exactly once per frame in both hosts. */
    if (!padmouse::testMode()) padmouse::setTestMode(true);

    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillColor(vg, nvgRGBA(12, 14, 20, 242));
    nvgFill(vg);

    const padmouse::Diag d = padmouse::diag();
    const Settings &st = Settings::instance();
    const bool gyro = (padmouse::mode() == padmouse::Mode::Gyro);

    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFontSize(vg, 22.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 235));
    nvgText(vg, x + 28.0f, y + 20.0f, ui::tr("menu/pad_mouse_test").c_str(), nullptr);

    /* --- The pointer area, on the left -------------------------------- */
    const float pad = 28.0f;
    const float aw = w * 0.52f, ah = h - 108.0f;
    const float ax = x + pad, ay = y + 60.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, ax, ay, aw, ah, 8.0f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 12));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    /* A cross at the centre and a quarter grid: a pointer without a reference
     * cannot be judged for drift, and drift is the first thing a gyroscope
     * shows. */
    nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 26));
    for (int i = 1; i < 4; i++) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, ax + aw * 0.25f * i, ay);
        nvgLineTo(vg, ax + aw * 0.25f * i, ay + ah);
        nvgMoveTo(vg, ax, ay + ah * 0.25f * i);
        nvgLineTo(vg, ax + aw, ay + ah * 0.25f * i);
        nvgStroke(vg);
    }

    float nx = 0.5f, ny = 0.5f;
    padmouse::testPoint(&nx, &ny);
    const float px = ax + nx * aw, py = ay + ny * ah;
    nvgBeginPath(vg);
    nvgCircle(vg, px, py, 16.0f);
    nvgStrokeColor(vg, nvgRGBA(255, 210, 0, 235));
    nvgStrokeWidth(vg, 3.0f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, px, py, 4.5f);
    nvgFillColor(vg, nvgRGBA(255, 210, 0, 235));
    nvgFill(vg);

    /* --- The chain, on the right -------------------------------------- */
    const float rx = ax + aw + 24.0f;
    const float rw = x + w - pad - rx;
    float ry = ay + 6.0f;

    line(vg, rx, ry, ui::tr("menu/pad_mouse_mode").c_str(),
         gyro ? ui::tr("menu/pad_mouse_gyro") : ui::tr("menu/pad_mouse_stick"),
         nvgRGBA(255, 255, 255, 235));
    ry += 26.0f;

    if (gyro) {
        /* STAGE 1 - were any sensor handles obtained? Zero here and nothing
         * downstream can possibly move: it names its own stage, which a dot
         * could not. */
        char hb[48];
        std::snprintf(hb, sizeof hb, "%d", d.handles);
        line(vg, rx, ry, ui::tr("menu/pm_handles").c_str(), hb,
             d.handles > 0 ? nvgRGBA(120, 230, 140, 235) : nvgRGBA(255, 120, 120, 235));
        ry += 26.0f;

        /* STAGE 2 - does one of them answer? A handle can be obtained and still
         * return no state. */
        line(vg, rx, ry, ui::tr("menu/pm_answering").c_str(),
             d.answering ? ui::tr("menu/pm_yes") : ui::tr("menu/pm_no"),
             d.answering ? nvgRGBA(120, 230, 140, 235) : nvgRGBA(255, 120, 120, 235));
        ry += 30.0f;

        /* STAGE 3 - the values themselves, on the scale a hand actually
         * produces (PAD_MOUSE_GYRO_MAX / 4 = a brisk turn), so the bar moves
         * when you move rather than sitting at zero. */
        bar(vg, rx, ry, rw, ui::tr("menu/pm_yaw").c_str(),
            d.gy, PAD_MOUSE_GYRO_MAX * 0.25f, true); ry += 24.0f;
        bar(vg, rx, ry, rw, ui::tr("menu/pm_pitch").c_str(),
            d.gx, PAD_MOUSE_GYRO_MAX * 0.25f, true); ry += 26.0f;

        /* === PM7 - THE ASSUMPTION, MADE FALSIFIABLE ===
         * The raw figure means nothing until its unit is known, and we do not
         * know it: we ASSUME turns per second (see pad_mouse.h). So the page
         * also shows what that assumption implies in degrees per second - and
         * that is a number a hand can check. Turn the controller a quarter turn
         * in one second: if it reads about 90, the assumption holds. If it reads
         * 5000, the unit is radians per second and the scale is wrong by 2*pi.
         *
         * A diagnostic page that only showed the raw value would leave the
         * question open for ever; this one closes it with a gesture. */
        {
            char db[80];
            std::snprintf(db, sizeof db, "%+.0f / %+.0f", (double)(d.gy * 360.0f),
                          (double)(d.gx * 360.0f));
            line(vg, rx, ry, ui::tr("menu/pm_degs").c_str(), db,
                 nvgRGBA(255, 255, 255, 210));
            ry += 26.0f;
        }

        /* === PM8 - THE TWO SENSORS, SIDE BY SIDE ===
         * "Both Joy-Cons have a gyroscope" is a claim; these two lines are the
         * measurement. They also show WHICH one is currently driving the
         * pointer, which is the only way to tell "the right Joy-Con has no
         * sensor" from "we are not reading it" - two very different things that
         * look identical from behind a pointer. */
        {
            char lb[80], rb2[80];
            std::snprintf(lb, sizeof lb, "%+.0f / %+.0f  %s",
                          (double)(d.g0y * 360.0f), (double)(d.g0x * 360.0f),
                          d.live0 ? (d.picked == 0 ? "<--" : "") : "-");
            std::snprintf(rb2, sizeof rb2, "%+.0f / %+.0f  %s",
                          (double)(d.g1y * 360.0f), (double)(d.g1x * 360.0f),
                          d.live1 ? (d.picked == 1 ? "<--" : "") : "-");
            line(vg, rx, ry, ui::tr("menu/pm_left").c_str(), lb,
                 d.live0 ? nvgRGBA(255, 255, 255, 210) : nvgRGBA(255, 120, 120, 200));
            ry += 24.0f;
            line(vg, rx, ry, ui::tr("menu/pm_right").c_str(), rb2,
                 d.live1 ? nvgRGBA(255, 255, 255, 210) : nvgRGBA(255, 120, 120, 200));
            ry += 30.0f;
        }
    } else {
        bar(vg, rx, ry, rw, "stick X", d.sx, 1.0f, true); ry += 24.0f;
        bar(vg, rx, ry, rw, "stick Y", d.sy, 1.0f, true); ry += 30.0f;
    }

    bar(vg, rx, ry, rw, ui::tr("menu/pm_scroll").c_str(), d.scroll, 1.0f, true);
    ry += 32.0f;

    /* The buttons, as the MODULE sees them - not as the pad reports them. What
     * matters here is what would have been sent, and those are two different
     * things the day a mapping goes wrong. */
    {
        static const char *NAMES[3] = { "ZR", "ZL", "R" };
        float bx = rx;
        for (int i = 0; i < 3; i++) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx, ry - 10.0f, 54.0f, 26.0f, 6.0f);
            nvgFillColor(vg, d.btn[i] ? nvgRGBA(120, 200, 255, 210)
                                      : nvgRGBA(255, 255, 255, 24));
            nvgFill(vg);
            nvgFontSize(vg, 15.0f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, d.btn[i] ? nvgRGBA(0, 0, 0, 235) : nvgRGBA(255, 255, 255, 170));
            nvgText(vg, bx + 27.0f, ry + 3.0f, NAMES[i], nullptr);
            bx += 62.0f;
        }
        ry += 34.0f;
    }

    {
        char sb[64];
        std::snprintf(sb, sizeof sb, "%u %%", (unsigned)st.pad_mouse_sens);
        line(vg, rx, ry, ui::tr("menu/pad_mouse_sens").c_str(), sb,
             nvgRGBA(255, 255, 255, 210));
    }

    /* --- Hints and the exit hold --------------------------------------- */
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 150));
    nvgText(vg, x + w * 0.5f, y + h - 26.0f,
            ui::tr("menu/pm_hint", ui::hintGlyph("B")).c_str(), nullptr);

#ifdef __SWITCH__
    static PadState p;
    static bool ready = false;
    if (!ready) { padConfigureInput(8, HidNpadStyleSet_NpadStandard); padInitializeAny(&p); ready = true; }
    padUpdate(&p);
    const uint64_t btn = padGetButtons(&p) | devlink::injectedNpadMask();   /* INJ-1 */
    if (btn & HidNpadButton_X) padmouse::testRecenter();

    /* PM6 - the module is stepped HERE. Its own clock, because `t` is a double
     * in seconds and the module wants microseconds since the previous frame -
     * and because the arithmetic it protects (a bounded dt) deserves the same
     * clock the rest of the input path uses. */
    padmouse::poll(&p, (uint64_t)armTicksToNs(armGetSystemTick()) / 1000ULL);
#endif

    /* HOLD-1 2026-09-12 - the hold used to be measured inside the block above,
     * so off console it was not compiled and this page could not be left at
     * all. What stays behind the `#ifdef` is what really is console-only: the
     * gyro stepping and the X recentre. Releasing still cancels. */
    float progress = 0.0f;
    if (ui::holdExitStep(ui::HOLD_MOUSE, t, &progress)) {
#ifdef __SWITCH__
        padmouse::setTestMode(false);
#endif
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        return true;
    }
    if (progress > 0.0f) {
        if (progress > 1.0f) progress = 1.0f;
        const float bw2 = 220.0f, bx2 = x + (w - bw2) * 0.5f, by2 = y + h - 12.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx2, by2, bw2, 4.0f, 2.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 34));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx2, by2, bw2 * progress, 4.0f, 2.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 200));
        nvgFill(vg);
    }

    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    return false;
}

}  // namespace mousetest
}  // namespace ui
