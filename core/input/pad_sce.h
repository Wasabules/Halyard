/* pad_sce - the PS Vita's buttons and sticks, in libnx's shape.
 *
 * === WHY libnx's SHAPE AND NOT A NEW ONE ==============================
 *
 * `pad_forward.cpp` reads the console's buttons and emits the CHANGES to the
 * VM. Its body is entirely generic once it holds a button MASK indexed by
 * `HID_BITS[]` - the mapping, the d-pad accumulation, the axes, the change
 * detection, none of that knows what a Switch is. Only the four calls that
 * FETCH the state did.
 *
 * So this header produces a libnx-shaped mask from `SceCtrl`, and the whole of
 * `pad_forward`'s logic then runs unchanged on both consoles. Inventing a
 * third vocabulary would have meant translating `HID_BITS[]` too, and a table
 * that exists twice is a table that drifts.
 *
 * === WHY IT LIVES IN core/ ============================================
 *
 * `pad_forward.cpp` is in `core/`, which must not depend on `clients/` - that
 * is the layering the 2026-09-12 restructure established, and the reason the
 * Borealis-backed shim in `clients/borealis/include/pad_compat.h` could not
 * simply be reused here. `core/` may talk to the platform SDK, and already
 * does on this console (the decoder, the audio output, the sockets), so
 * reading `SceCtrl` directly costs no upward arrow.
 *
 * `pad_compat.h` now includes THIS for buttons and sticks and keeps only what
 * genuinely needs Borealis: the touchscreen, whose panel-to-screen conversion
 * lives in `SceTouchPanelInfo` and is already written there.
 */
#pragma once

#if defined(__vita__) || defined(__psp2__)

#include <stdint.h>
#include <string.h>
#include <psp2/ctrl.h>

/* libnx bit POSITIONS, not invented ones: `devlink::injectedNpadMask()` speaks
 * libnx, and `HID_BITS[]` is written in these terms. */
enum {
    HidNpadButton_A      = 1u << 0,
    HidNpadButton_B      = 1u << 1,
    HidNpadButton_X      = 1u << 2,
    HidNpadButton_Y      = 1u << 3,
    HidNpadButton_StickL = 1u << 4,
    HidNpadButton_StickR = 1u << 5,
    HidNpadButton_L      = 1u << 6,
    HidNpadButton_R      = 1u << 7,
    HidNpadButton_ZL     = 1u << 8,
    HidNpadButton_ZR     = 1u << 9,
    HidNpadButton_Plus   = 1u << 10,
    HidNpadButton_Minus  = 1u << 11,
    HidNpadButton_Left   = 1u << 12,
    HidNpadButton_Up     = 1u << 13,
    HidNpadButton_Right  = 1u << 14,
    HidNpadButton_Down   = 1u << 15,
};
enum { HidNpadStyleSet_NpadStandard = 0 };

typedef struct { int32_t x, y; } HidAnalogStickState;
typedef struct { int unused; }   PadState;

static inline void padConfigureInput(int n, int style) { (void)n; (void)style; }

static inline void padInitializeAny(PadState *p)
{
    (void)p;
    /* ANALOG_WIDE, the same mode the pad tester sets: without it the sticks
     * report their centre forever and only the digital buttons move. */
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
}

static inline void padUpdate(PadState *p) { (void)p; }   /* peek is stateless */

static inline int shadow_pad_sce_read(SceCtrlData *d)
{
    memset(d, 0, sizeof *d);
    return sceCtrlPeekBufferPositive(0, d, 1) >= 0;
}

static inline uint64_t padGetButtons(PadState *p)
{
    (void)p;
    SceCtrlData d;
    if (!shadow_pad_sce_read(&d)) return 0;
    const unsigned b = d.buttons;
    uint64_t m = 0;
    /* MAPPED THE WAY BOREALIS MAPS, not by physical position: its own PSV input
     * binds A to CROSS, B to CIRCLE, X to SQUARE, Y to TRIANGLE, and the whole
     * application - the mapping screen included - reads that. Disagreeing here
     * would mean the tester and the stream report different buttons for the
     * same finger. */
    if (b & SCE_CTRL_CROSS)    m |= HidNpadButton_A;
    if (b & SCE_CTRL_CIRCLE)   m |= HidNpadButton_B;
    if (b & SCE_CTRL_SQUARE)   m |= HidNpadButton_X;
    if (b & SCE_CTRL_TRIANGLE) m |= HidNpadButton_Y;
    if (b & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) m |= HidNpadButton_L;
    if (b & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) m |= HidNpadButton_R;
    /* No ZL/ZR on this console: a handheld Vita has one shoulder pair. They are
     * left out rather than mapped onto L/R, which would send two buttons for
     * one finger. */
    if (b & SCE_CTRL_SELECT)   m |= HidNpadButton_Minus;
    if (b & SCE_CTRL_START)    m |= HidNpadButton_Plus;
    if (b & SCE_CTRL_L3)       m |= HidNpadButton_StickL;   /* PS TV pad only */
    if (b & SCE_CTRL_R3)       m |= HidNpadButton_StickR;
    if (b & SCE_CTRL_UP)       m |= HidNpadButton_Up;
    if (b & SCE_CTRL_DOWN)     m |= HidNpadButton_Down;
    if (b & SCE_CTRL_LEFT)     m |= HidNpadButton_Left;
    if (b & SCE_CTRL_RIGHT)    m |= HidNpadButton_Right;
    return m;
}

static inline HidAnalogStickState padGetStickPos(PadState *p, int stick)
{
    (void)p;
    HidAnalogStickState s = { 0, 0 };
    SceCtrlData d;
    if (!shadow_pad_sce_read(&d)) return s;
    const unsigned char rx = (stick == 0) ? d.lx : d.rx;
    const unsigned char ry = (stick == 0) ? d.ly : d.ry;
    /* 0..255 with 128 at rest -> libnx's +/-32767. Divided by 127, not 128:
     * 128 would leave full deflection one step short of the range the callers
     * threshold against. PSV3 is the counter-case - Borealis divided this same
     * travel by 255 and the stick read a permanent half-push. */
    s.x = (int32_t)(((int)rx - 128) * 32767 / 127);
    /* +Y is DOWN on this pad and UP in libnx. */
    s.y = (int32_t)(-(((int)ry - 128) * 32767 / 127));
    if (s.x >  32767) s.x =  32767;
    if (s.x < -32767) s.x = -32767;
    if (s.y >  32767) s.y =  32767;
    if (s.y < -32767) s.y = -32767;
    return s;
}

#endif /* __vita__ || __psp2__ */
