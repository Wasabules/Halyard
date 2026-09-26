/* ui::pointer — see pointer.hpp. */
#include "pointer.hpp"

#include "paint.hpp"

namespace ui {
namespace pointer {

namespace {

struct State {
    bool   on        = false;
    bool   pressed   = false;
    double click_t   = 0.0;   /* instant of the last press, 0 = none */
    float  x = 0, y = 0;
};
State g;

const double RIPPLE_S = 0.45;   /* how long the ripple lives after a click */

}  // namespace

void setEnabled(bool on) { g.on = on; }
bool enabled()           { return g.on; }

bool state(float &x, float &y, bool &pressed)
{
#if !defined(__SWITCH__)
    brls::RawMouseState raw{};
    brls::Application::getPlatform()->getInputManager()->updateMouseStates(&raw);
    /* === 2026-09-02 - THE SCALE WAS APPLIED TWICE ===
     *
     * This used to divide by `windowScale`, on the stated grounds that the
     * position came in the WINDOW's frame. It does not: GLFWInputManager already
     * returns `x * scaleFactor / Application::windowScale`
     * (library/borealis/.../glfw/glfw_input.cpp:367), i.e. the logical frame we
     * draw in. Dividing again put the dot at `position / windowScale²`.
     *
     * It was invisible where it was looked at - `windowScale` is the window's
     * width over 1280, so exactly 1.0 in the default window, where a squared
     * one is still one. Maximise to 1920 and the dot sat at two thirds of the
     * distance to the cursor. */
    x = raw.position.x;
    y = raw.position.y;
    pressed = raw.leftButton;
    return true;
#else
    (void)x; (void)y; (void)pressed;
    return false;   /* console: the finger is read through ui/touch.h */
#endif
}

void draw(NVGcontext *vg, double t)
{
    if (!g.on || !vg) return;

#if !defined(__SWITCH__)
    bool pressed = false;
    if (!state(g.x, g.y, pressed)) return;
    if (pressed && !g.pressed) g.click_t = t;   /* rising edge: the click INSTANT */
    g.pressed = pressed;
#else
    (void)t;
    return;
#endif

    const float R = 11.0f;

    /* The ripple first, UNDER the disc: drawn on top it would hide the dot at
     * the exact moment you need to see it. */
    if (g.click_t > 0.0 && t - g.click_t < RIPPLE_S) {
        const float u = (float)((t - g.click_t) / RIPPLE_S);
        const float r = R + u * 26.0f;
        nvgBeginPath(vg);
        nvgCircle(vg, g.x, g.y, r);
        nvgStrokeColor(vg, nvgRGBAf(0.38f, 0.62f, 1.0f, (1.0f - u) * 0.75f));
        nvgStrokeWidth(vg, 2.5f * (1.0f - u) + 0.5f);
        nvgStroke(vg);
    }

    /* A dark halo under the disc: on a light image a light dot alone vanishes.
     * It costs one pass and keeps the pointer readable on any background —
     * which is what a system cursor gets from its outline. */
    nvgBeginPath(vg);
    nvgCircle(vg, g.x, g.y, R + 2.0f);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 90));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, g.x, g.y, g.pressed ? R : R - 3.0f);
    if (g.pressed) nvgFillColor(vg, nvgRGBA(96, 158, 255, 235));
    else           nvgFillColor(vg, nvgRGBA(220, 232, 255, 150));
    nvgFill(vg);

    /* At rest a ring rather than a solid disc: the two states are then told
     * apart even on a frozen frame, which is what matters for a screenshot. */
    if (!g.pressed) {
        nvgBeginPath(vg);
        nvgCircle(vg, g.x, g.y, R);
        nvgStrokeColor(vg, nvgRGBA(220, 232, 255, 210));
        nvgStrokeWidth(vg, 1.6f);
        nvgStroke(vg);
    }
}

}  // namespace pointer
}  // namespace ui
