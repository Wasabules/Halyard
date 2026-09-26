/* pad_compat - the handful of libnx input calls the in-stream code uses, on a
 * console that is not the Switch.
 *
 * === WHY A SHIM AND NOT A PORT ==========================================
 *
 * `stream_view.cpp` carries 274 lines of in-stream input - pause-menu
 * navigation, touch-as-mouse - inside one `#ifdef __SWITCH__`. Reported from a
 * real Vita 2026-09-13: the touchscreen did nothing during a stream. It was
 * not a mapping problem; the whole block was compiled out.
 *
 * Those 274 lines are almost entirely neutral logic. Their libnx surface is
 * SIX entry points and FOUR button constants - the d-pad and the left stick,
 * for menu navigation, plus three touch reads. Rewriting the logic to a new
 * abstraction would mean re-deriving behaviour that is already correct and
 * already tested on one console; supplying the six calls costs a page and
 * leaves the logic byte-identical on both.
 *
 * WHAT BACKS THEM. Borealis already reads this hardware, and - the part worth
 * stating - it CONVERTS the touch panel through `SceTouchPanelInfo`'s active
 * area, into `Application::contentWidth/Height`. That is the same 1280x720
 * space libnx reports in on the Switch, so the coordinates pass straight
 * through and the logic above cannot tell the two consoles apart. Writing that
 * conversion again here is the duplicated path this repo keeps paying for.
 *
 * THE BUTTON BITS ARE libnx's REAL VALUES, deliberately: `padGetButtons()` is
 * OR'd with `devlink::injectedNpadMask()`, which speaks libnx. Inventing
 * distinct bits here would work until the day devlink injects a d-pad press.
 */
#pragma once

#include "../device_caps.h"

#if SHADOW_HAS_STREAM_INPUT && !defined(__SWITCH__)

#include <cstdint>
#include <cstring>
#include <ctime>
#include <vector>

#include <borealis/core/application.hpp>

/* Buttons, sticks and the libnx bit positions come from `core/input/pad_sce.h`,
 * which reads `SceCtrl` directly. They used to be defined HERE too, backed by
 * Borealis' unified controller state - two implementations of `padGetButtons`
 * with the same name in two translation units, which is the drift this port has
 * been paying for all day. One reader, used by both layers. */
#include "../../../core/input/pad_sce.h"

/* One finger is all the callers read (`touches[0]`, `touches[1].y`), but the
 * array is sized as libnx sizes it so an indexing mistake lands in memory that
 * exists rather than past the end. */
struct HidTouchState { uint32_t x, y; };
struct HidTouchScreenState {
    int32_t       count;
    HidTouchState touches[16];
};

static inline brls::InputManager* shadow_pad_im()
{
    return brls::Application::getPlatform()
               ? brls::Application::getPlatform()->getInputManager()
               : nullptr;
}

/* The monotonic clock the block times its gestures with. libnx counts 19.2 MHz
 * ticks and converts; both call sites here are `armTicksToNs(armGetSystemTick())`
 * - never a raw tick - so the pair can simply speak nanoseconds and the
 * conversion becomes the identity. Anything that ever reads a tick on its own
 * would break the build here rather than silently get a wrong unit. */
static inline uint64_t armGetSystemTick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline uint64_t armTicksToNs(uint64_t ns) { return ns; }

static inline int hidGetTouchScreenStates(HidTouchScreenState* out, size_t n)
{
    if (!out || n == 0) return 0;
    std::memset(out, 0, sizeof *out);
    brls::InputManager* im = shadow_pad_im();
    if (!im) return 0;
    std::vector<brls::RawTouchState> states;
    im->updateTouchStates(&states);
    /* === THE SPACE: LOGICAL, LIKE ui/touch.h =============================
     *
     * Two goes at this on 2026-09-13, and the second one is the one that
     * matters. libnx reports PANEL pixels, so the obvious reading is that this
     * header should too - and the menu path does divide by
     * `Application::windowScale` to reach the logical frame. But it is the ONLY
     * one of the four call sites that does: the touch-as-mouse path, the edge
     * gesture and the keyboard filter all compare the raw value against VIEW
     * rectangles, which are logical.
     *
     * The Switch code is therefore inconsistent with itself, and could not
     * notice: its touchscreen only exists in handheld, where the framebuffer is
     * 1280x720 and `windowScale` is exactly 1 - dividing by it or not is the
     * same thing there. On a console whose scale is 0.75 the two conventions
     * finally disagree, and the first fix here picked the minority one: the
     * cursor then landed at 0.75x the finger, aligned at the top-left corner
     * and drifting further away towards the right edge. Reported exactly like
     * that from hardware.
     *
     * So: logical, the same frame `ui/touch.h` hands out, and the menu's lone
     * division is removed rather than three call sites changed to match it. */    int k = 0;
    for (size_t i = 0; i < states.size() && k < 16; i++) {
        if (!states[i].pressed) continue;
        out->touches[k].x = (uint32_t)states[i].position.x;
        out->touches[k].y = (uint32_t)states[i].position.y;
        k++;
    }
    out->count = k;
    return 1;                      /* one state read, as libnx returns */
}

#endif /* SHADOW_HAS_STREAM_INPUT && !__SWITCH__ */
