#pragma once
/* === WHAT THIS DEVICE CAN ACTUALLY DO ==================================
 *
 * Created 2026-09-13, when the PS Vita build showed a settings panel with five
 * rows behind which there was nothing: hardware-decode choice, haptics, the
 * demo pointer, cut-on-focus-loss and lock-on-wake. An inert row is not a
 * cosmetic problem - the repo already recorded the reasoning twice, for
 * `SET_HW_AUDIO` and `SET_POINTER`: "offering a setting with nothing behind it
 * casts doubt on all the others". On the Vita five of them did that at once.
 *
 * WHY A HEADER AND NOT AN `#ifdef __vita__` AT EACH SITE. This port has paid
 * for the subtractive condition over and over - `#else`, `!__SWITCH__`,
 * `#ifndef __SWITCH__`, each one meaning "desktop" and each one true only
 * while there were exactly two targets. `SET_POINTER` below is one of those:
 * it read `#if !defined(__SWITCH__)`, which silently turned the row on for the
 * Vita the day the Vita existed. The rule that survives a third target is to
 * NAME THE CAPABILITY and let each platform answer yes or no.
 *
 * Which is also why these are 0/1 macros and not `#ifdef`: `#if
 * SHADOW_HAS_RUMBLE` on a typo'd name is a silent 0, but `-Wundef` makes it a
 * warning, whereas a mistyped `#ifdef` is silently false forever.
 *
 * "Has" here means REACHABLE FROM THIS BINARY, not "the silicon could". Vita
 * suspend/resume exists in hardware; what does not exist is our code for it -
 * the focus and wake settings are consumed only inside `main.cpp`'s
 * `#ifdef __SWITCH__` applet hook. So the day someone wires
 * `sceAppUtil`/power callbacks, they flip ONE macro here and the two rows come
 * back, instead of hunting for the sites that hid them. */

/* A vibration motor. The PS Vita has none at all (the PS TV pairs a DS3 that
 * does, but nothing in this binary drives it), and off console
 * `rumble_hid_apply` is the stub that logs once and returns false. */
#if defined(__SWITCH__)
#  define SHADOW_HAS_RUMBLE 1
#else
#  define SHADOW_HAS_RUMBLE 0
#endif

/* A CHOICE between hardware and software video decoding. On the Vita there is
 * no choice: `SCE_VIDEODEC_TYPE_HW_AVCDEC` is the only decoder, the vitasdk's
 * libavcodec ships no H.264 at all, and a 444 MHz Cortex-A9 would not decode
 * 720p in software if it did - so `h264_decoder_create` returns before any
 * `SHADOW_HWACCEL` is read. The toggle switched nothing. */
#if defined(__vita__) || defined(__psp2__)
#  define SHADOW_HAS_DECODER_CHOICE 0
#else
#  define SHADOW_HAS_DECODER_CHOICE 1
#endif

/* A drawn mouse pointer driven by a real mouse. Desktop only: the console
 * builds have a touchscreen and a stick, which the gestures page covers. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
#  define SHADOW_HAS_DEMO_POINTER 0
#else
#  define SHADOW_HAS_DEMO_POINTER 1
#endif

/* "The user left the app": libnx's `appletHook` FocusState, read only inside
 * main.cpp's own `#ifdef __SWITCH__`. */
#if defined(__SWITCH__)
#  define SHADOW_HAS_APPLET_EVENTS 1
#else
#  define SHADOW_HAS_APPLET_EVENTS 0
#endif

/* "The console woke up", which re-arms the app lock. A SEPARATE macro from the
 * one above even though both come from the same applet hook, and the reason is
 * worth the four extra lines: the desktop has no wake event, but it does have
 * `SHADOW_DIAG_RELOCK_S` (AF2), the diagnostic that pushes one wake lock on
 * demand - and it refuses unless this setting is on. Folding the two settings
 * into one capability would have hidden the only switch that arms that
 * campaign off console, i.e. quietly disabled an existing test path while
 * tidying a menu. The Vita has neither the event nor the diagnostic. */
#if defined(__vita__) || defined(__psp2__)
#  define SHADOW_HAS_WAKE_LOCK 0
#else
#  define SHADOW_HAS_WAKE_LOCK 1
#endif

/* === WHICH GAMEPAD THE TESTER DRAWS ====================================
 *
 * The tester's whole purpose is to compare what the CONSOLE reads against what
 * we SEND (see pad_draw.hpp), and it does that by drawing the pad. Drawing a
 * Switch on a PS Vita defeats the comparison at the first glance: reported from
 * a real console 2026-09-13, the reader sees A/B/X/Y where the hardware is
 * marked with four symbols, plus ZL/ZR and two clickable sticks that this
 * machine does not have at all. You cannot check "the console read what I
 * pressed" against a picture of a different console.
 *
 * A style, not a platform, so the desktop build can pick either one and a
 * future target only has to answer this question. */
/* === WHAT THE PANEL AND THE DECODER ALREADY DECIDE =====================
 *
 * The Vita's screen is 960x544 at 60 Hz and its hardware decoder picks a
 * geometry off the descending ladder in `h264_decoder.c` by what it can
 * actually allocate. Offering a resolution and a frame-rate choice there is
 * offering to set a value the console then overrides - a control that lies is
 * worse than a control that is absent, because the user believes it. */
#if defined(__vita__) || defined(__psp2__)
#  define SHADOW_HAS_MODE_CHOICE 0
#else
#  define SHADOW_HAS_MODE_CHOICE 1
#endif

/* 4:4:4 chroma. Neither console can: the Switch's hardware path was already
 * established as unable (KB K13/K14), and the Vita's `SceAvcdec` returns NV12,
 * which is 4:2:0 by definition. The setting stays on desktop, where the server
 * grant is the only limit. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
#  define SHADOW_HAS_CHROMA_444 0
#else
#  define SHADOW_HAS_CHROMA_444 1
#endif

/* Gyroscope aiming. The Switch reads it through `hidGetSixAxisSensorStates`.
 * The Vita DOES have a motion sensor - this is not a hardware limit - but
 * nothing here is wired to `SceMotion`, so the mode would arm and never move
 * the pointer. Absent is honest; present and inert is not. A machine that wires
 * SceMotion gains the setting by answering 1 here. */
#if defined(__SWITCH__)
#  define SHADOW_HAS_GYRO 1
#else
#  define SHADOW_HAS_GYRO 0
#endif

/* === A REAR TOUCH PANEL =============================================
 *
 * The PS Vita has one, and it is the answer to the four buttons the console
 * physically lacks (ZL, ZR, L3, R3). No other target here has one - the Switch
 * has a FRONT touchscreen, which is a different thing in every way that
 * matters: you can see it, and your fingers are not already resting on it.
 *
 * Named for the capability rather than the platform, so the day a target with
 * a rear panel appears it answers 1 and nothing else changes. */
#if defined(__vita__) || defined(__psp2__)
#  define SHADOW_HAS_REAR_PAD 1
#else
#  define SHADOW_HAS_REAR_PAD 0
#endif

/* === A WINDOW THAT NEVER MOVES =====================================
 *
 * A desktop window can be resized, and Borealis works in a logical frame of
 * fixed width: enlarging the window enlarges everything drawn into it, so the
 * UI scale has to be divided by `windowScale` to keep an apparent size
 * constant. On a console the panel is the window and that correction is not
 * merely useless - it is wrong, because `windowScale` is not 1 there.
 *
 * This was written `#if !defined(__SWITCH__)`, which meant "a desktop" only
 * while the Switch was the only console. On the Vita's 960x544 panel the
 * division therefore MAGNIFIED the pause menu instead of leaving it alone.
 * Naming the capability is the fix the repository already names for this
 * family: do not subtract the platforms. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
#  define SHADOW_UI_FIXED_WINDOW 1
#else
#  define SHADOW_UI_FIXED_WINDOW 0
#endif

#define SHADOW_PAD_ART_NINTENDO    1
#define SHADOW_PAD_ART_PLAYSTATION 2

#if defined(__vita__) || defined(__psp2__)
#  define SHADOW_PAD_ART SHADOW_PAD_ART_PLAYSTATION
#else
#  define SHADOW_PAD_ART SHADOW_PAD_ART_NINTENDO
#endif

/* === WHO DRAWS THE VIDEO ===============================================
 *
 * Two renderers, and they are not variants of one design (see
 * gxm_video_renderer.hpp): the GL one leaves nanovg and issues its own
 * shader; the GXM one stays inside nanovg because the Vita's texture unit
 * converts YUV for free.
 *
 * Named as a capability rather than tested by platform, and the CMake is the
 * reason it matters here: `file(GLOB_RECURSE)` hands every `clients/*.cpp` to
 * every target, so `gxm_video_renderer.cpp` is compiled on the desktop and on
 * the Switch too. It has to yield no symbol there, and this is the switch that
 * makes it so. */
#if defined(__vita__) || defined(__psp2__)
#  define SHADOW_HAS_GXM_VIDEO 1
#else
#  define SHADOW_HAS_GXM_VIDEO 0
#endif

/* === IN-STREAM INPUT ====================================================
 *
 * Pause-menu navigation and touch-as-mouse WHILE the stream is up. It sat
 * behind `#ifdef __SWITCH__` in `stream_view.cpp` - 274 lines - so on the Vita
 * the touchscreen did nothing during a stream and the pause menu could not be
 * navigated. The logic is neutral; only six libnx calls were not, and
 * `include/pad_compat.h` supplies those. Named for what it IS, so a target
 * that gains the six calls gains the feature by answering here. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
#  define SHADOW_HAS_STREAM_INPUT 1
#else
#  define SHADOW_HAS_STREAM_INPUT 0
#endif
