/* rear_touch_sce.h - the PS Vita's rear panel, read and handed to rear_touch.h.
 *
 * This is the only part of the rear-touchpad feature that touches the console.
 * Everything that can be got wrong - what counts as a tap, how far a trigger
 * has been pulled, which finger is which - lives in `rear_touch.h`, where it is
 * arithmetic and has a test. What is left here is a panel read and a change of
 * units, which is exactly the split `pad_mouse.h` and `pad_mouse_hid.cpp` use.
 *
 * === WHY THE COORDINATES ARE ASKED FOR AND NOT ASSUMED ===
 *
 * The rear panel does NOT report in screen pixels, and its active area differs
 * from the front one and between models. `sceTouchGetPanelInfo` gives the real
 * bounds; hard-coding 1920x1088 would work on one machine and put every zone
 * boundary in the wrong place on another. The panel is asked once - the answer
 * cannot change while the console is on.
 *
 * Off the Vita this header compiles to a function that reports nothing, so the
 * caller carries no `#if` of its own.
 */
#ifndef REAR_TOUCH_SCE_H
#define REAR_TOUCH_SCE_H

#include "rear_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__vita__) || defined(__psp2__)

#include <psp2/touch.h>
#include <time.h>

/* The rear panel is port 1; the front one is 0. */
#ifndef SHADOW_REAR_PORT
#define SHADOW_REAR_PORT SCE_TOUCH_PORT_BACK
#endif

static inline uint32_t rear_touch_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* Reads the panel and steps the recogniser. `out_pts`/`out_n` optionally
 * receive the normalised contacts, so the pad tester can DRAW what the
 * recogniser is being fed - the same numbers, not a second reading that could
 * disagree with it. */
static inline rear_touch_out shadow_rear_touch_poll(const rear_touch_config *cfg,
                                                    rear_touch_state *st,
                                                    rear_point *out_pts,
                                                    size_t *out_n)
{
    if (out_n) *out_n = 0;

    /* Asked once: the active area cannot change while the console is on, and
     * asking per frame would be a syscall on the input path for a constant. */
    static int   inited = 0;
    static float ax0 = 0.0f, ay0 = 0.0f, aw = 1.0f, ah = 1.0f;
    if (!inited) {
        inited = 1;
        sceTouchSetSamplingState(SHADOW_REAR_PORT, SCE_TOUCH_SAMPLING_STATE_START);
        SceTouchPanelInfo info;
        if (sceTouchGetPanelInfo(SHADOW_REAR_PORT, &info) >= 0) {
            ax0 = (float)info.minAaX;
            ay0 = (float)info.minAaY;
            aw  = (float)(info.maxAaX - info.minAaX);
            ah  = (float)(info.maxAaY - info.minAaY);
            if (aw <= 1.0f) aw = 1.0f;
            if (ah <= 1.0f) ah = 1.0f;
        }
    }

    SceTouchData d;
    rear_point pts[REAR_MAX_POINTS];
    size_t n = 0;
    if (sceTouchPeek(SHADOW_REAR_PORT, &d, 1) >= 0) {
        for (int i = 0; i < (int)d.reportNum && n < REAR_MAX_POINTS; i++) {
            float x = ((float)d.report[i].x - ax0) / aw;
            float y = ((float)d.report[i].y - ay0) / ah;
            if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
            if (y < 0.0f) y = 0.0f; else if (y > 1.0f) y = 1.0f;
            pts[n].x  = x;
            pts[n].y  = y;
            pts[n].id = d.report[i].id;
            n++;
        }
    }
    if (out_pts && out_n) {
        for (size_t k = 0; k < n; k++) out_pts[k] = pts[k];
        *out_n = n;
    }
    return rear_touch_step(cfg, st, pts, n, rear_touch_now_ms());
}

#define SHADOW_HAS_REAR_TOUCH 1

#else   /* not a Vita: no panel, and the caller needs no #if for that */

static inline rear_touch_out shadow_rear_touch_poll(const rear_touch_config *cfg,
                                                    rear_touch_state *st,
                                                    rear_point *out_pts,
                                                    size_t *out_n)
{
    (void)cfg; (void)st; (void)out_pts;
    if (out_n) *out_n = 0;
    rear_touch_out o;
    o.zl = o.zr = o.l3 = o.r3 = false;
    o.l2 = o.r2 = 0;
    return o;
}

#define SHADOW_HAS_REAR_TOUCH 0

#endif

#ifdef __cplusplus
}
#endif
#endif /* REAR_TOUCH_SCE_H */
