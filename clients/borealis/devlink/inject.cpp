/* inject.cpp - the wiring of inject.h: one lock, one clock, two weak symbols.
 *
 * Everything that is a DECISION lives in inject.h, pure and tested offline.
 * What is here is what cannot be: a mutex, a monotonic clock, the mapping from
 * our command vocabulary to Borealis' enums, and the two entry points the
 * patched input managers call.
 */

#include "inject.hpp"
#include "inject.h"
#include "devcmd.h"

#include <borealis/core/input.hpp>

#include <chrono>
#include <cstring>
#include <mutex>

#ifdef __SWITCH__
#include <switch.h>
#endif

/* The header deliberately does not include Borealis, so this is where the two
 * are tied together. A Borealis that grows its enums fails HERE, at compile
 * time, instead of writing past the end of an array at 60 Hz. */
static_assert((size_t)brls::_BUTTON_MAX <= (size_t)INJECT_BUTTON_MAX,
              "INJECT_BUTTON_MAX is below brls::_BUTTON_MAX");
static_assert((size_t)brls::_AXES_MAX <= (size_t)INJECT_AXIS_MAX,
              "INJECT_AXIS_MAX is below brls::_AXES_MAX");

namespace devlink {
namespace {

std::mutex     g_lock;
inject_state_t g_state;
bool           g_ready = false;

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

inject_state_t &state()
{
    if (!g_ready) { inject_clear(&g_state); g_ready = true; }
    return g_state;
}

/* Our vocabulary -> Borealis' enum. The ORDER of `devcmd_button_t` is the
 * contract (devcmd.h says so), which is what lets the network path stop at an
 * index and never compare a string again. */
int brlsButton(int idx)
{
    switch (idx) {
    case DEVCMD_BTN_A:     return brls::BUTTON_A;
    case DEVCMD_BTN_B:     return brls::BUTTON_B;
    case DEVCMD_BTN_X:     return brls::BUTTON_X;
    case DEVCMD_BTN_Y:     return brls::BUTTON_Y;
    case DEVCMD_BTN_L:     return brls::BUTTON_LB;
    case DEVCMD_BTN_R:     return brls::BUTTON_RB;
    case DEVCMD_BTN_ZL:    return brls::BUTTON_LT;
    case DEVCMD_BTN_ZR:    return brls::BUTTON_RT;
    case DEVCMD_BTN_PLUS:  return brls::BUTTON_START;
    case DEVCMD_BTN_MINUS: return brls::BUTTON_BACK;
    default:               return -1;
    }
}

/* A direction presses BOTH the D-pad and the NAV button.
 *
 * They are not the same thing to Borealis: `BUTTON_NAV_*` drives focus movement,
 * `BUTTON_UP/DOWN/...` is the physical cross, and our screens are wired to the
 * cross through `ui::wireNavigation`. A physical controller raises both, so
 * raising only one would inject something no hardware can produce - and the
 * bug would show up on exactly one screen, at random. */
void pressDirection(inject_state_t &st, int dir, long long now, int ms)
{
    switch (dir) {
    case DEVCMD_NAV_UP:
        inject_press(&st, brls::BUTTON_UP, now, ms);
        inject_press(&st, brls::BUTTON_NAV_UP, now, ms);       break;
    case DEVCMD_NAV_DOWN:
        inject_press(&st, brls::BUTTON_DOWN, now, ms);
        inject_press(&st, brls::BUTTON_NAV_DOWN, now, ms);     break;
    case DEVCMD_NAV_LEFT:
        inject_press(&st, brls::BUTTON_LEFT, now, ms);
        inject_press(&st, brls::BUTTON_NAV_LEFT, now, ms);     break;
    case DEVCMD_NAV_RIGHT:
        inject_press(&st, brls::BUTTON_RIGHT, now, ms);
        inject_press(&st, brls::BUTTON_NAV_RIGHT, now, ms);    break;
    default: break;
    }
}

}  // namespace

bool pressButton(int devcmd_index, int ms)
{
    const int b = brlsButton(devcmd_index);
    if (b < 0) return false;
    std::lock_guard<std::mutex> v(g_lock);
    return inject_press(&state(), b, nowMs(), ms);
}

bool pressDirection(int devcmd_dir, int ms)
{
    if (devcmd_dir < 0 || devcmd_dir >= (int)DEVCMD_NAV_COUNT) return false;
    std::lock_guard<std::mutex> v(g_lock);
    pressDirection(state(), devcmd_dir, nowMs(), ms);
    return true;
}

bool moveStick(bool right, float x, float y, int ms)
{
    std::lock_guard<std::mutex> v(g_lock);
    const long long t = nowMs();
    inject_state_t &st = state();
    inject_axis(&st, right ? brls::RIGHT_X : brls::LEFT_X, x, t, ms);
    inject_axis(&st, right ? brls::RIGHT_Y : brls::LEFT_Y, y, t, ms);
    return true;
}

bool touch(float x0, float y0, float x1, float y1, int ms)
{
    std::lock_guard<std::mutex> v(g_lock);
    return inject_touch(&state(), x0, y0, x1, y1, nowMs(), ms);
}

void releaseAll()
{
    std::lock_guard<std::mutex> v(g_lock);
    inject_clear(&state());
}

unsigned long long injectedNpadMask()
{
#ifdef __SWITCH__
    std::lock_guard<std::mutex> v(g_lock);
    if (!g_ready) return 0;
    const long long t = nowMs();
    if (!inject_busy(&g_state, t)) return 0;

    bool b[INJECT_BUTTON_MAX];
    memset(b, 0, sizeof b);
    inject_sample(&g_state, t, b, INJECT_BUTTON_MAX, nullptr, 0, nullptr, nullptr);

    unsigned long long m = 0;
    if (b[brls::BUTTON_A])     m |= HidNpadButton_A;
    if (b[brls::BUTTON_B])     m |= HidNpadButton_B;
    if (b[brls::BUTTON_X])     m |= HidNpadButton_X;
    if (b[brls::BUTTON_Y])     m |= HidNpadButton_Y;
    if (b[brls::BUTTON_LB])    m |= HidNpadButton_L;
    if (b[brls::BUTTON_RB])    m |= HidNpadButton_R;
    if (b[brls::BUTTON_LT])    m |= HidNpadButton_ZL;
    if (b[brls::BUTTON_RT])    m |= HidNpadButton_ZR;
    if (b[brls::BUTTON_START]) m |= HidNpadButton_Plus;
    if (b[brls::BUTTON_BACK])  m |= HidNpadButton_Minus;
    if (b[brls::BUTTON_UP])    m |= HidNpadButton_Up;
    if (b[brls::BUTTON_DOWN])  m |= HidNpadButton_Down;
    if (b[brls::BUTTON_LEFT])  m |= HidNpadButton_Left;
    if (b[brls::BUTTON_RIGHT]) m |= HidNpadButton_Right;
    return m;
#else
    return 0;
#endif
}

bool injectedStick(bool right, int *x, int *y)
{
    std::lock_guard<std::mutex> v(g_lock);
    if (!g_ready) return false;
    const long long t = nowMs();
    const int ax = right ? brls::RIGHT_X : brls::LEFT_X;
    const int ay = right ? brls::RIGHT_Y : brls::LEFT_Y;
    if (g_state.axis_until[ax] <= t && g_state.axis_until[ay] <= t) return false;

    /* libnx reports a stick in -32767..32767, Borealis in -1..1. The conversion
     * lives here rather than in the pure module: it is a property of the API we
     * are feeding, not of the timing. */
    if (x) *x = (int)(g_state.axis_value[ax] * 32767.0f);
    if (y) *y = (int)(g_state.axis_value[ay] * 32767.0f);
    return true;
}

bool injectedTouch(float *x, float *y)
{
    std::lock_guard<std::mutex> v(g_lock);
    if (!g_ready) return false;
    return inject_sample(&g_state, nowMs(), nullptr, 0, nullptr, 0, x, y);
}

}  // namespace devlink

/* --- The two entry points the patched input managers call ------------------
 *
 * `extern "C"` and WEAK on the Borealis side: an unpatched Borealis links and
 * behaves exactly as before, which is what makes the patch archived in
 * patches/borealis/ optional rather than load-bearing for the build.
 *
 * Called on the RENDER thread, once per frame, under our lock. Both return
 * immediately when nothing is being injected - the only cost on a normal
 * session, where no script is attached, is one pass over two small arrays. */
extern "C" void halyard_inject_controller(bool *buttons, size_t nb,
                                                float *axes, size_t na)
{
    using namespace devlink;
    std::lock_guard<std::mutex> v(g_lock);
    if (!g_ready) return;                     /* nothing ever injected */
    const long long t = nowMs();
    if (!inject_busy(&g_state, t)) return;
    inject_sample(&g_state, t, buttons, nb, axes, na, nullptr, nullptr);
}

extern "C" bool halyard_inject_touch(float *x, float *y)
{
    using namespace devlink;
    std::lock_guard<std::mutex> v(g_lock);
    if (!g_ready) return false;
    return inject_sample(&g_state, nowMs(), nullptr, 0, nullptr, 0, x, y);
}
