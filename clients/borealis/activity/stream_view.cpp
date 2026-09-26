// StreamView - implementation. See stream_view.hpp.

#include "../device_caps.h"
#include "../ui/haptics.hpp"
#include "clients/borealis/devlink/devlink.hpp"   /* DEVL-4: the stream describes itself too */
#include "clients/borealis/devlink/inject.hpp"
#include "../demo.hpp"                    /* DEMO-1 */    /* INJ-1: the stream reads libnx directly, so it reads this too */
#include "clients/borealis/activity/stream_view.hpp"
#include "clients/borealis/include/pad_compat.h"  /* the six libnx calls, off Switch */

#include "settings.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
#include "../../../core/common/log.h"      /* streaming log, WITHOUT any level filter */
/* S81 - log category of this module. See shadow/journal.h: it is declared
 * here, never inferred from the text of the messages. */
#define svlog(...) JOURNAL_INFO_(JOURNAL_CAT_UI, __VA_ARGS__)
#define svdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_UI, __VA_ARGS__)

#include "../../../core/common/stats.h"
#include "../../../core/input/shadow_input.h"
#include "../../../core/protocol/rumble_state.h"   /* G57: force feedback */
#include "../../../core/services/power_profile.h"   /* LAT-V1: clock read-back */
#include "../../../core/protocol/rumble_hid.h"
#include "../../../core/protocol/cursor_state.h"   /* CUR1 phase 2 2026-05-18 */
#include "../../../core/protocol/ctrl_gamepad.h"   /* gamepad: actions of the dev menu */
#include "../../../core/protocol/ctrl_session.h"   /* picture refresh request */
#include "../../../core/protocol/ctrl_session_glue.h"  /* actual codec, for the panel */
#include "../../../core/protocol/latency.h"      /* L5: instrumentation of the video path */
}

#include "../devui/devui.hpp"
#include "../ui/i18n.hpp"
#include "../ui/type.hpp"          /* typographic scale */
#include "../ui/pad_test.hpp"
#include "../ui/mouse_test.hpp"
#include "../ui/net_test.hpp"   /* S119 */

/* The decoder is set up for stereo (webrtc/audio.c) and the TOC seen on the
 * wire confirms it: CELT fullband, stereo. */
#define AUDIO_HUD_CHANNELS "stereo"
#include "../../../core/input/pad_forward.hpp"
#include "../../../core/input/pad_mouse_hid.hpp"   /* PM1 - the Joy-Cons as a mouse */
#include "../../../core/input/pad_map.hpp"   /* development menu bar */

/* Image stretching. The decision belongs to the setting (`stretch_image`); this
 * mirror exists only so the desktop menu bar and the diagnostic panel can read
 * back what the last frame drew, without either of them reaching into the
 * settings. Rewritten on every frame - never read it as the source of truth. */
static int g_stretch = 0;

/* Diagnostic marker for the cursor position the VM reports back (CUR1).
 * Hidden by default; SHADOW_CURSOR_DEBUG=1, or the development bar, shows it. */
static int g_cursor_debug = -1;

/* Synthetic arrow marking the position we send to the VM.
 *
 * It is indispensable on Switch, where no system pointer is drawn at all. On
 * the desktop it doubled the OS cursor - two arrows for one mouse - and since
 * it was drawn in logical units it grew with the window. Default: on for the
 * console, off for the desktop. SHADOW_CURSOR_ARROW forces either way. */
/* S113 - -1 = follow the env toggle, otherwise the setting. >= 0 = forced by
 * the pause menu toggle, for the current session only. */
static int g_cursor_arrow = -1;
/* EFFECTIVE value of the previous frame, so that the pause menu toggle mirrors
 * what is really drawn. Without it the menu said the arrow was off while the
 * setting had it on, and the first press did nothing visible: a toggle lying
 * about its own state. */
static int g_cursor_arrow_eff = 0;

#include "../gl_compat.h"
#if SHADOW_HAVE_GLAD
#include <glad/glad.h>
#endif
#if SHADOW_HAVE_DESKTOP_GL
#include <GLFW/glfw3.h>
#endif

#include "switch_compat.h"
#ifdef __SWITCH__
#include "switch_compat.h"
#ifdef __SWITCH__
#include <switch.h>
#endif
#endif

#include <atomic>
#include <cstring>
#include <algorithm>
#include <chrono>

#ifndef __SWITCH__
/* Whether the stream owns the keyboard, published by draw(): a session is up
 * and neither the pause menu nor the on-screen keyboard is open. */
static std::atomic<bool>  g_stream_focused{false};

#ifndef __SWITCH__
/* === K20 2026-09-10 - WHOSE KEYBOARD IS IT WHILE THE STREAM IS UP? ===
 *
 * Borealis calls this before folding the EXTENDED keyboard keys (X, Y, L, R,
 * F1, F2, Q, P) into the UI's button state — see the local patch in
 * `glfw_input.cpp`, archived under `patches/borealis/`.
 *
 * Those keys were promised by `docs/DESKTOP_KEYS.md` and mapped nowhere, so
 * the patch adds them. But the moment the stream has the focus they belong to
 * the VM: this file already forwards the physical keyboard there, and letting
 * Borealis act on them TOO would open a pause menu in the middle of a game —
 * `stream_activity` binds BUTTON_Y and BUTTON_START.
 *
 * The symbol is WEAK on the Borealis side: an unpatched Borealis links and
 * behaves exactly as before, which is what keeps `library/borealis/` a plain
 * upstream checkout. */
extern "C" bool halyard_ui_keys_blocked(void)
{
    return g_stream_focused.load(std::memory_order_relaxed);
}
#endif
#endif

/* === VI2 2026-09-11 - RECYCLE THE PLANE BUFFERS (SHADOW_PUSH_RECYCLE) ===
 * pushYuvFrame moved writer_slot's vectors into the queue and left writer_slot
 * with no capacity, so every picture allocated its planes afresh (1.38 MB at
 * 720p NV12) while its predecessors' were freed under swap_mtx - by the pop of
 * a full queue, by the L3 skips, and by the move-assign into consumer_slot.
 * Parked buffers now come back into writer_slot, so assign() lands in memory
 * already allocated; whatever the pool cannot take is destroyed after the
 * unlock.
 * Bench (wf_video/vi2_assess/bench_vi2.cpp - Windows/MinGW, 1280x720 NV12, the
 * real thread shape: the decode thread copies and enqueues, the UI thread pops
 * present-latest; arms interleaved, 5 x 700 frames, medians of the per-rep
 * percentiles, no overlap in any rep): push copy p50 362 -> 115 us, UI-thread
 * swap_mtx section p99 281 -> 2.9 us. About 0.25 ms of decode-thread work per
 * picture, and the old frame's free taken out of the lock the draw holds: ~2 %
 * of the ~29 ms chain, below what video/bout resolves. Measured on Windows
 * only, where NT heap page faults make the allocation expensive; the Switch
 * heap is pre-committed, so the gain there is probably smaller.
 * Legal with HO-1: the renderer's upload key is the picture NUMBER
 * (nv12_seq_), never a buffer address, and glTexSubImage2D has copied the
 * client memory when it returns (no pixel-unpack buffer on this path).
 * 0 restores today's allocation behaviour exactly. */
static int g_push_recycle = -1;
static bool push_recycle_on()
{
    if (g_push_recycle < 0) {
        const char *e = getenv("SHADOW_PUSH_RECYCLE");
        g_push_recycle = e ? atoi(e) : 1;
    }
    return g_push_recycle != 0;
}

StreamView::StreamView() {
    // Black background (while waiting for the first frame)
    this->setBackgroundColor(nvgRGB(0, 0, 0));
    this->setGrow(1.0f);  // fullscreen inside its parent
    // Without focusable=true, Application::currentFocus stays NULL and the
    // first button press crashes in setInputType -> currentFocus->onFocusGained.
    this->setFocusable(true);
    // We consume the whole stream: no focus highlight (blue frame) and no click
    // animation. The arrows / D-pad drive Shadow, they do not navigate
    // Borealis. The `registerAction` BUTTON_UP/DOWN already consume the event,
    // but we hide the focus rectangle too, for cleanliness.
    this->setHideHighlight(true);

    /* The mouse mode is persisted: we apply it as soon as the view is built,
     * otherwise it would only take effect after a trip through the pause menu -
     * and one only thinks of going there once the mouse has become unusable in
     * game. */
    shadow_input_set_mouse_relative(Settings::instance().mouse_relative ? 1 : 0);
    this->setHideClickAnimation(true);

    /* VI2 - room for the recycled plane buffers, reserved once so that a
     * push_back under swap_mtx never allocates. The toggle is read here, on the
     * UI thread, before stream_view_set_active() can hand this view a picture:
     * the decode thread then only ever reads a value already written. */
    free_frames_.reserve(PUSH_POOL_MAX);
    (void)push_recycle_on();

    // Restore the persisted stats toggle (Settings is loaded at app startup).
    show_perf_stats = Settings::instance().show_perf_stats;
    /* Declare the sections right at construction: the pause menu is filled in
     * by the activity before the first render, and it lists them. */
    setupHud();
    setStreamVsync(true);
    setStreamKeepAwake(true);
    /* S105 - while streaming, the remote machine drives the rumble motors. A
     * vibration coming from our UI would be blamed on the game: stay silent. */
    ui::haptics::setEnabled(false);
}

/* === L14 2026-08-29 - THE ONE REMAINING COST THAT IS THE SCREEN ITSELF ===
 *
 * `video/cadence` measures 17.4 ms at p50, at p90 AND at p99, with exactly 600
 * draws per ten-second window: 60.0 Hz, not one frame missed. So this is not a
 * defect to fix, it is the SCREEN PERIOD, and it costs what it costs - the
 * drawn frame waits for the next scan-out before it exists.
 *
 * Checked before touching anything: Borealis is already at the minimum. The
 * presentation chain has TWO buffers (`FRAMEBUFFERS_COUNT = 2`), not three, so
 * there is no extra frame of delay to remove, and the swap interval is already
 * 1. There is no setting left to win; the only lever is not waiting for the
 * scan-out at all.
 *
 * That is a TRADE, not a gain: with no wait, the frame changes mid-scan and the
 * seam is visible. The average gain is HALF a period - ~8 ms - because the swap
 * lands anywhere within the scan, not a whole period as one might assume. Eight
 * milliseconds against a visible tearing line: that is judged by eye, on moving
 * content, and nobody here can judge it on the user's behalf.
 *
 * Hence an OPTIONAL toggle, active only during the stream - a UI that tears
 * while a menu scrolls would be absurd - and restored to its normal value on
 * the way out. Default unchanged. */
void StreamView::setStreamVsync(bool active) {
    /* The setting decides; the environment variable overrides it, as everywhere
     * here - a campaign must be able to impose a value without going through
     * the UI. */
    bool wait_vsync = Settings::instance().vsync_stream;
    if (const char *e = getenv("SHADOW_VSYNC")) wait_vsync = (atoi(e) != 0);
    if (wait_vsync) return;                  /* original behaviour */
    auto *plat = brls::Application::getPlatform();
    if (!plat) return;
    auto *vid = plat->getVideoContext();
    if (!vid) return;
    vid->setSwapInterval(active ? 0 : 1);
    svlog("[L14] waiting on vsync %s during the stream (the trade: ~8 ms of "
         "moins, dechirure possible)", active ? "DESACTIVEE" : "retablie");
}

/* === LAT-V1 2026-09-26 - A STREAM IS MEDIA PLAYBACK ====================
 *
 * Nothing told the system a stream was playing. Without a button press it
 * applied its idle policy mid-stream: on the Vita the screen dims, and an
 * unattended session drew at 60 Hz for ~15 s of stream then at 46-48 Hz for
 * the rest, in every run of a 12-session campaign. Watching without touching
 * anything is a normal way to use a stream (a film, a cut-scene).
 * Borealis already carries the platform call: `sceKernelPowerTick` on every
 * frame on the Vita, `appletSetMediaPlaybackState` on the Switch, the
 * screensaver inhibit on a desktop. Held for the stream only, released on the
 * way out. `SHADOW_STREAM_KEEP_AWAKE=0` restores the old behaviour. */
void StreamView::setStreamKeepAwake(bool active) {
    const char *e = getenv("SHADOW_STREAM_KEEP_AWAKE");
    if (e && !atoi(e)) return;
    auto *plat = brls::Application::getPlatform();
    if (!plat) return;
    plat->disableScreenDimming(active, "stream", "Halyard");
    svlog("[LAT-V1] screen dimming %s for the stream",
          active ? "held off" : "restored");
}

StreamView::~StreamView() {
    /* S107 - the session adds itself to the play-time tally. Here rather than at
     * channel close: this is how long the user actually HAD the stream in front
     * of them, which is the question they are asking. */
    {
        session_stats_t ws;
        session_stats_get(&ws);
        if (ws.session_seconds > 0)
            Settings::instance().addSessionTime((uint32_t)ws.session_seconds);
    }
    ui::haptics::setEnabled(true);
    setStreamVsync(false);
    setStreamKeepAwake(false);
    /* PM1 - hands the six-axis sensors back and releases any mouse button we
     * were holding. Leaving the stream with a click down would leave it down in
     * the remote machine, with nothing left on this side able to release it;
     * leaving the sensors running would keep draining the Joy-Cons for the rest
     * of the application's life. */
    padmouse::shutdown();
    /* L9 - join the gamepad reader thread BEFORE anything else: HOS leaks the
     * handle of a thread that is never joined, and a leak costs a console
     * reboot. */
    padforward::stop();
    /* The development bar holds callbacks that capture this view: it must drop
     * them before the view goes away. */
    devui::clear(this);
}

void StreamView::menuToggleStats() {
    show_perf_stats = !show_perf_stats;
    Settings::instance().show_perf_stats = show_perf_stats;
    Settings::instance().save();
}

void StreamView::menuCycleTouchMode(int dir) {
    auto& s = Settings::instance();
    int next = (static_cast<int>(s.touch_mode) + (dir >= 0 ? 1 : 2)) % 3;
    s.touch_mode = static_cast<TouchMode>(next);
    s.save();
}

// YUV420P -> RGBA conversion (BT.601 limited range), Q8 fixed-point:
//   R = (298*(Y-16) + 409*(V-128) + 128) >> 8
//   G = (298*(Y-16) - 100*(U-128) - 208*(V-128) + 128) >> 8
//   B = (298*(Y-16) + 516*(U-128) + 128) >> 8
//
// NEON SIMD, 8 pixels per iteration on aarch64 -> ~6x faster than scalar C.
// Switch Tegra X1 (Cortex-A57) at 720p: 30-50 ms -> ~5-8 ms per frame.
// Without that speedup the conversion blocks the WebRTC thread, which gives
// jerky video plus crackling audio (the RTP audio queue backs up).

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>

// NV12 -> RGBA in NEON. Same as yuv420p_to_rgba but loads interleaved UV.
// The Tegra X1 NVDEC outputs NV12: data_uv = [U0,V0,U1,V1,U2,V2,...] x (h/2 rows).
static void nv12_to_rgba(int w, int h,
                          const uint8_t* y_plane,  int y_stride,
                          const uint8_t* uv_plane, int uv_stride,
                          uint8_t* rgba_out) {
    const int16x4_t k298   = vdup_n_s16(298);
    const int16x4_t k409   = vdup_n_s16(409);
    const int16x4_t k_n100 = vdup_n_s16(-100);
    const int16x4_t k_n208 = vdup_n_s16(-208);
    const int16x4_t k516   = vdup_n_s16(516);
    const int32x4_t k128_32 = vdupq_n_s32(128);
    const int16x8_t k_yoff = vdupq_n_s16(16);
    const int16x8_t k_uvof = vdupq_n_s16(128);
    const uint8x8_t k_alpha = vdup_n_u8(255);

    for (int row = 0; row < h; row++) {
        const uint8_t* yrow = y_plane + (size_t)row * y_stride;
        const uint8_t* uvrow = uv_plane + (size_t)(row / 2) * uv_stride;
        uint8_t* drow = rgba_out + (size_t)row * w * 4;

        int x = 0;
        for (; x + 8 <= w; x += 8) {
            uint8x8_t y8 = vld1_u8(yrow + x);
            // Load 16 interleaved UV bytes and de-interleave into 2 vectors of 8.
            // For 8 luma columns we need 4 UV pairs (8 bytes), but vld2 loads
            // 16 bytes (8 pairs) -> only the 4 low lanes are used.
            uint8x8x2_t uv = vld2_u8(uvrow + x);  // val[0]=U[0..7], val[1]=V[0..7]

            int16x8_t y_i = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(y8)), k_yoff);
            int16x8_t u_full = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(uv.val[0])), k_uvof);
            int16x8_t v_full = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(uv.val[1])), k_uvof);
            // Duplicate the 4 LOW chroma: [u0,u1,u2,u3] -> [u0,u0,u1,u1,u2,u2,u3,u3]
            int16x4_t u_lo4 = vget_low_s16(u_full);
            int16x4_t v_lo4 = vget_low_s16(v_full);
            int16x8_t u_dup = vcombine_s16(vzip1_s16(u_lo4, u_lo4),
                                            vzip2_s16(u_lo4, u_lo4));
            int16x8_t v_dup = vcombine_s16(vzip1_s16(v_lo4, v_lo4),
                                            vzip2_s16(v_lo4, v_lo4));

            int16x4_t y_lo = vget_low_s16(y_i),  y_hi = vget_high_s16(y_i);
            int16x4_t u_lo = vget_low_s16(u_dup), u_hi = vget_high_s16(u_dup);
            int16x4_t v_lo = vget_low_s16(v_dup), v_hi = vget_high_s16(v_dup);

            int32x4_t y298_lo = vmull_s16(y_lo, k298);
            int32x4_t y298_hi = vmull_s16(y_hi, k298);
            int32x4_t r_lo = vaddq_s32(vaddq_s32(y298_lo, vmull_s16(v_lo, k409)), k128_32);
            int32x4_t r_hi = vaddq_s32(vaddq_s32(y298_hi, vmull_s16(v_hi, k409)), k128_32);
            int32x4_t g_lo = vaddq_s32(vaddq_s32(y298_lo, vmull_s16(u_lo, k_n100)),
                                       vaddq_s32(vmull_s16(v_lo, k_n208), k128_32));
            int32x4_t g_hi = vaddq_s32(vaddq_s32(y298_hi, vmull_s16(u_hi, k_n100)),
                                       vaddq_s32(vmull_s16(v_hi, k_n208), k128_32));
            int32x4_t b_lo = vaddq_s32(vaddq_s32(y298_lo, vmull_s16(u_lo, k516)), k128_32);
            int32x4_t b_hi = vaddq_s32(vaddq_s32(y298_hi, vmull_s16(u_hi, k516)), k128_32);

            uint16x8_t r16 = vcombine_u16(vqshrun_n_s32(r_lo, 8), vqshrun_n_s32(r_hi, 8));
            uint16x8_t g16 = vcombine_u16(vqshrun_n_s32(g_lo, 8), vqshrun_n_s32(g_hi, 8));
            uint16x8_t b16 = vcombine_u16(vqshrun_n_s32(b_lo, 8), vqshrun_n_s32(b_hi, 8));
            uint8x8_t r = vqmovn_u16(r16);
            uint8x8_t g = vqmovn_u16(g16);
            uint8x8_t b = vqmovn_u16(b16);

            uint8x8x4_t rgba = { { r, g, b, k_alpha } };
            vst4_u8(drow + x * 4, rgba);
        }
        // Scalar tail
        for (; x < w; x++) {
            int yp = yrow[x] - 16;
            int up = uvrow[(x / 2) * 2]     - 128;  // U inside the interleaved UV
            int vp = uvrow[(x / 2) * 2 + 1] - 128;  // V
            int r = (298 * yp + 409 * vp + 128) >> 8;
            int g = (298 * yp - 100 * up - 208 * vp + 128) >> 8;
            int b = (298 * yp + 516 * up + 128) >> 8;
            drow[x * 4 + 0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            drow[x * 4 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            drow[x * 4 + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            drow[x * 4 + 3] = 255;
        }
    }
}

static void yuv420p_to_rgba(int w, int h,
                             const uint8_t* y_plane, int y_stride,
                             const uint8_t* u_plane, int u_stride,
                             const uint8_t* v_plane, int v_stride,
                             uint8_t* rgba_out) {
    // Constants as int16x4 (for vmull_s16 -> int32x4) and int32x4 (for the adds).
    const int16x4_t k298   = vdup_n_s16(298);
    const int16x4_t k409   = vdup_n_s16(409);
    const int16x4_t k_n100 = vdup_n_s16(-100);
    const int16x4_t k_n208 = vdup_n_s16(-208);
    const int16x4_t k516   = vdup_n_s16(516);
    const int32x4_t k128_32 = vdupq_n_s32(128);
    const int16x8_t k_yoff = vdupq_n_s16(16);
    const int16x8_t k_uvof = vdupq_n_s16(128);
    const uint8x8_t k_alpha = vdup_n_u8(255);

    for (int row = 0; row < h; row++) {
        const uint8_t* yrow = y_plane + (size_t)row * y_stride;
        const uint8_t* urow = u_plane + (size_t)(row / 2) * u_stride;
        const uint8_t* vrow = v_plane + (size_t)(row / 2) * v_stride;
        uint8_t* drow = rgba_out + (size_t)row * w * 4;

        int x = 0;
        for (; x + 8 <= w; x += 8) {
            uint8x8_t  y8  = vld1_u8(yrow + x);
            uint8x8_t  u8u = vld1_u8(urow + x / 2);  // 4 useful chroma in the low half
            uint8x8_t  v8u = vld1_u8(vrow + x / 2);

            int16x8_t y_i = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(y8)), k_yoff);
            int16x8_t u_full = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u8u)), k_uvof);
            int16x8_t v_full = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v8u)), k_uvof);
            // Duplicate chroma: [u0,u1,u2,u3,_,_,_,_] -> [u0,u0,u1,u1,u2,u2,u3,u3]
            int16x4_t u_lo4 = vget_low_s16(u_full);
            int16x4_t v_lo4 = vget_low_s16(v_full);
            int16x8_t u_dup = vcombine_s16(vzip1_s16(u_lo4, u_lo4),
                                            vzip2_s16(u_lo4, u_lo4));
            int16x8_t v_dup = vcombine_s16(vzip1_s16(v_lo4, v_lo4),
                                            vzip2_s16(v_lo4, v_lo4));

            // Math in int32 (vmull_s16 yields int32x4, no overflow possible)
            int16x4_t y_lo = vget_low_s16(y_i),  y_hi = vget_high_s16(y_i);
            int16x4_t u_lo = vget_low_s16(u_dup), u_hi = vget_high_s16(u_dup);
            int16x4_t v_lo = vget_low_s16(v_dup), v_hi = vget_high_s16(v_dup);

            int32x4_t y298_lo = vmull_s16(y_lo, k298);
            int32x4_t y298_hi = vmull_s16(y_hi, k298);

            // R = 298*y + 409*v + 128
            int32x4_t r_lo = vaddq_s32(vaddq_s32(y298_lo, vmull_s16(v_lo, k409)), k128_32);
            int32x4_t r_hi = vaddq_s32(vaddq_s32(y298_hi, vmull_s16(v_hi, k409)), k128_32);
            // G = 298*y - 100*u - 208*v + 128
            int32x4_t g_lo = vaddq_s32(vaddq_s32(y298_lo, vmull_s16(u_lo, k_n100)),
                                       vaddq_s32(vmull_s16(v_lo, k_n208), k128_32));
            int32x4_t g_hi = vaddq_s32(vaddq_s32(y298_hi, vmull_s16(u_hi, k_n100)),
                                       vaddq_s32(vmull_s16(v_hi, k_n208), k128_32));
            // B = 298*y + 516*u + 128
            int32x4_t b_lo = vaddq_s32(vaddq_s32(y298_lo, vmull_s16(u_lo, k516)), k128_32);
            int32x4_t b_hi = vaddq_s32(vaddq_s32(y298_hi, vmull_s16(u_hi, k516)), k128_32);

            // Saturate-shift-narrow: int32x4 -> uint16x4 (clamp 0..65535, shift 8)
            uint16x8_t r16 = vcombine_u16(vqshrun_n_s32(r_lo, 8), vqshrun_n_s32(r_hi, 8));
            uint16x8_t g16 = vcombine_u16(vqshrun_n_s32(g_lo, 8), vqshrun_n_s32(g_hi, 8));
            uint16x8_t b16 = vcombine_u16(vqshrun_n_s32(b_lo, 8), vqshrun_n_s32(b_hi, 8));
            // uint16x8 -> uint8x8, saturating at 255
            uint8x8_t r = vqmovn_u16(r16);
            uint8x8_t g = vqmovn_u16(g16);
            uint8x8_t b = vqmovn_u16(b16);

            uint8x8x4_t rgba = { { r, g, b, k_alpha } };
            vst4_u8(drow + x * 4, rgba);
        }
        // Scalar tail
        for (; x < w; x++) {
            int yp = yrow[x] - 16;
            int up = urow[x / 2] - 128;
            int vp = vrow[x / 2] - 128;
            int r = (298 * yp + 409 * vp + 128) >> 8;
            int g = (298 * yp - 100 * up - 208 * vp + 128) >> 8;
            int b = (298 * yp + 516 * up + 128) >> 8;
            drow[x * 4 + 0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            drow[x * 4 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            drow[x * 4 + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            drow[x * 4 + 3] = 255;
        }
    }
}

#else
// Scalar fallback for the host cross-build (no NEON on x86)
static inline uint8_t clip255(int v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}
static void nv12_to_rgba(int w, int h,
                          const uint8_t* y_plane,  int y_stride,
                          const uint8_t* uv_plane, int uv_stride,
                          uint8_t* rgba_out) {
    for (int row = 0; row < h; row++) {
        const uint8_t* yrow = y_plane + (size_t)row * y_stride;
        const uint8_t* uvrow = uv_plane + (size_t)(row / 2) * uv_stride;
        uint8_t* drow = rgba_out + (size_t)row * w * 4;
        for (int col = 0; col < w; col++) {
            int yp = yrow[col] - 16;
            int up = uvrow[(col / 2) * 2]     - 128;
            int vp = uvrow[(col / 2) * 2 + 1] - 128;
            int r = (298 * yp + 409 * vp + 128) >> 8;
            int g = (298 * yp - 100 * up - 208 * vp + 128) >> 8;
            int b = (298 * yp + 516 * up + 128) >> 8;
            drow[col * 4 + 0] = clip255(r);
            drow[col * 4 + 1] = clip255(g);
            drow[col * 4 + 2] = clip255(b);
            drow[col * 4 + 3] = 255;
        }
    }
}
static void yuv420p_to_rgba(int w, int h,
                             const uint8_t* y_plane, int y_stride,
                             const uint8_t* u_plane, int u_stride,
                             const uint8_t* v_plane, int v_stride,
                             uint8_t* rgba_out) {
    for (int row = 0; row < h; row++) {
        const uint8_t* yrow = y_plane + (size_t)row * y_stride;
        const uint8_t* urow = u_plane + (size_t)(row / 2) * u_stride;
        const uint8_t* vrow = v_plane + (size_t)(row / 2) * v_stride;
        uint8_t* drow = rgba_out + (size_t)row * w * 4;
        for (int col = 0; col < w; col++) {
            int yp = yrow[col] - 16;
            int up = urow[col / 2] - 128;
            int vp = vrow[col / 2] - 128;
            int r = (298 * yp + 409 * vp + 128) >> 8;
            int g = (298 * yp - 100 * up - 208 * vp + 128) >> 8;
            int b = (298 * yp + 516 * up + 128) >> 8;
            drow[col * 4 + 0] = clip255(r);
            drow[col * 4 + 1] = clip255(g);
            drow[col * 4 + 2] = clip255(b);
            drow[col * 4 + 3] = 255;
        }
    }
}
#endif

void StreamView::pushYuvFrame(int width, int height,
                              const uint8_t* data_y, int linesize_y,
                              const uint8_t* data_u, int linesize_u,
                              const uint8_t* data_v, int linesize_v,
                              int format, int64_t pts) {
    /* PM2 - the tracker's bounds are the REMOTE DESKTOP's, and this is where we
     * learn them: the decoded size is the resolution the server actually chose,
     * not the one we requested. Cheap enough to do every frame - two compares -
     * and doing it here means a mid-session resolution change is followed. */
    shadow_input_set_bounds(width, height);

    bool is_nv12 = (format == AV_PIX_FMT_NV12);
    /* === K17 2026-08-28 - 4:4:4 WAS REJECTED RIGHT HERE, SILENTLY ===
     * The guard only accepted YUV420P and NV12: a `yuv444p` frame was turned
     * away without a word. Observed symptom: normal video bitrate, decoding
     * without a single error (`linesize=[1280,1280,1280]`, i.e. three full
     * planes), and NO picture at all.
     * 4:4:4 also forces SOFTWARE decoding - the CUDA hardware fails during its
     * initialisation, exactly as it does in the official client. */
    const bool chroma_pleine = (format == AV_PIX_FMT_YUV444P);
    {   /* The format ACTUALLY received, logged once per value. Without it one
         * cannot tell "the frame never arrives" from "it arrives in another
         * format" - and that is exactly the ambiguity that blocks here. */
        static int seen = 0;
        if (format >= 0 && format < 32 && !(seen & (1 << format))) {
            seen |= (1 << format);
            svlog("[K17] pushYuvFrame recoit format=%d (444=%d, 420=%d, nv12=%d)",
                       format, (int)AV_PIX_FMT_YUV444P, (int)AV_PIX_FMT_YUV420P,
                       (int)AV_PIX_FMT_NV12);
        }
    }
    if (format != AV_PIX_FMT_YUV420P && !is_nv12 && !chroma_pleine) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            svlog("stream: pixel format %d unsupported - the picture is ignored", format);
        }
        return;
    }
    /* No 16 ms cap here: we push EVERY decoded frame into the pacing queue.
     * draw() consumes at the 60Hz vsync and drops the stale ones through
     * PACING_QUEUE_MAX. Capping here dropped frames out of a burst -> stutter. */

    // === Step 1: copy the YUV into writer_slot (private to the WebRTC thread, no lock) ===
    size_t y_sz = (size_t)linesize_y * height;
#if SHADOW_HAS_GXM_VIDEO
    /* Zero copy: see the comment on `ext_y` in stream_view.hpp. The non-NV12
     * path does not exist on this console -- the hardware decoder only ever
     * returns NV12 -- so there is nothing to convert in place. */
    if (is_nv12) {
        writer_slot.ext_y = data_y;
        writer_slot.ext_u = data_u;
        writer_slot.y.clear(); writer_slot.u.clear(); writer_slot.v.clear();
        writer_slot.vs = 0;
    } else
#endif
    {
    writer_slot.ext_y = nullptr; writer_slot.ext_u = nullptr;
    writer_slot.y.assign(data_y, data_y + y_sz);
    if (is_nv12) {
        // Interleaved UV: data_u holds [U0,V0,U1,V1,...] x (height/2 rows)
        size_t uv_sz = (size_t)linesize_u * (height / 2);
        writer_slot.u.assign(data_u, data_u + uv_sz);
        writer_slot.v.clear();
        writer_slot.vs = 0;
    } else {
        // YUV420P -> converted to NV12 IN PLACE so we can switch to the GPU
        // shader path (avoids the CPU yuv420p_to_rgba plus a 3.5 MB
        // nvgUpdateImage upload). Cost of the conversion: just interleaving
        // U/V - ~460 KB for 1280x720, far cheaper than YUV->RGBA plus 4x the
        // upload size.
        /* In 4:4:4 the chroma planes are as large as the Y plane. The rest of
         * the conversion is identical: we interleave U and V to feed the RG8
         * texture of the shader, which samples in normalized coordinates and
         * therefore copes with both resolutions. */
        int uv_w = chroma_pleine ? width  : width / 2;
        int uv_h = chroma_pleine ? height : height / 2;
        size_t uv_stride = (size_t)uv_w * 2;
        writer_slot.u.resize(uv_stride * uv_h);
        uint8_t *dst = writer_slot.u.data();
        for (int row = 0; row < uv_h; row++) {
            const uint8_t *u_src = data_u + row * linesize_u;
            const uint8_t *v_src = data_v + row * linesize_v;
            uint8_t *d = dst + row * uv_stride;
            for (int col = 0; col < uv_w; col++) {
                d[2 * col]     = u_src[col];
                d[2 * col + 1] = v_src[col];
            }
        }
        writer_slot.v.clear();
        writer_slot.vs = 0;
        linesize_u = (int)uv_stride;
        is_nv12 = true;  /* makes draw() switch to the GPU shader path */
    }
    }   /* end of the path WITH a copy; the dimensions below hold for both paths */
    writer_slot.w = width;
    writer_slot.h = height;
    writer_slot.ys = linesize_y;
    writer_slot.us = linesize_u;
    writer_slot.is_nv12 = is_nv12;
    writer_slot.chroma_pleine = chroma_pleine;
    /* K17 - three one-shot witnesses: they say WHERE the chain stops instead of
     * leaving it to be deduced. "pousse" without "rendu" = the frame never
     * reaches the display; both of them with no picture = the shader or the
     * dimensions. */
    if (chroma_pleine) {
        static int logged_push = 0;
        if (!logged_push) {
            logged_push = 1;
            svlog("[K17] 4:4:4 pousse : %dx%d ys=%d us=%d (uv pleine)",
                       width, height, linesize_y, linesize_u);
        }
    }
    /* L5 - we attach the server timestamp to the frame BEFORE it enters the
     * presentation queue. `pts` travelled through the decoder with it; it only
     * serves as a key, its numeric value is never read as a duration. Returns 0
     * when the match fails, and the module then ignores the sample rather than
     * inventing one. */
    writer_slot.srv_stamp = latency_enabled()
                               ? latency_video_stamp_for_pts(pts) : 0;
    /* === VI2 2026-09-11 - "PUSHED" MEANS READY TO QUEUE, NOT "ARRIVED" ===
     * This timestamp was taken at the top of the function, BEFORE the plane
     * copies. Those run on the decode thread inside the video/decode bracket -
     * the glue calls us synchronously from the decoder's output on the default
     * paths - so the copy was counted twice: in video/decode, and in
     * video/file-aff, defined as "pushed -> popped" (latency.h). Taken here,
     * the copy belongs to decode alone, and the wait for swap_mtx - which is
     * the queue - stays in file-aff. Every video/file-aff figure measured
     * before this change carries the copy: ~0.3-0.35 ms on Windows.
     * The clock is latency_now_us(), the one the draw subtracts it from; it
     * was steady_clock, which relied on both resolving to the same source.
     * push_us has no other reader, so SHADOW_LATENCE=0 takes no clock here. */
    writer_slot.push_us = latency_enabled() ? latency_now_us() : 0;

    // === Step 2: enqueue the frame into pending_queue (under a short lock) ===
    // When the queue is full we drop the OLDEST (latency-first rather than
    // smoothness-first). A max of 2 is enough to absorb the NVDEC bursts
    // without adding perceptible latency.
    YuvFrame spill;   /* VI2: a dropped frame the pool cannot take - freed after the unlock */
    {
        std::lock_guard<std::mutex> lock(swap_mtx);
        const bool recycle = push_recycle_on();   /* VI2 */
        if (pending_queue.size() >= PACING_QUEUE_MAX) {
            if (recycle) {   /* VI2: park its planes (O(1) move) instead of freeing them here */
                if (free_frames_.size() < PUSH_POOL_MAX)
                    free_frames_.push_back(std::move(pending_queue.front()));
                else
                    spill = std::move(pending_queue.front());
            }
            pending_queue.pop_front();   // drop the oldest
        }
        /* The member-wise moves below are O(1) - and so is a plain
         * std::move(frame): YuvFrame declares no copy, move or destructor, so
         * its implicit move operations move the vectors, which the VI2 pool
         * relies on. (This note used to say the vectors got copied; they do
         * not.) What these moves did NOT save is the next allocation:
         * writer_slot is left with no capacity - hence VI2's refill below. */
        YuvFrame f;
        f.y       = std::move(writer_slot.y);
        f.u       = std::move(writer_slot.u);
        f.v       = std::move(writer_slot.v);
        f.w       = writer_slot.w;
        f.h       = writer_slot.h;
        f.ys      = writer_slot.ys;
        f.us      = writer_slot.us;
        f.vs      = writer_slot.vs;
        f.is_nv12 = writer_slot.is_nv12;
        /* ADDED BY HAND, like everything else in this list -- and that is the
         * list's defect, not these two lines': it enumerates the fields
         * instead of moving the struct, so it silently loses every field added
         * after it. Forgotten once already, and the pointers arrived null in
         * the queue with the picture no longer drawing. */
        f.ext_y   = writer_slot.ext_y;
        f.ext_u   = writer_slot.ext_u;
        f.chroma_pleine = writer_slot.chroma_pleine;
        f.push_us = writer_slot.push_us;
        f.srv_stamp = writer_slot.srv_stamp;   /* L5 */
        pending_queue.push_back(std::move(f));
        /* VI2 - writer_slot's planes were just moved out: hand it a parked set
         * so the next assign() reuses its capacity. Only the planes - every
         * other field is rewritten before the next enqueue. */
        if (recycle && !free_frames_.empty()) {
            YuvFrame &r = free_frames_.back();
            writer_slot.y = std::move(r.y);
            writer_slot.u = std::move(r.u);
            writer_slot.v = std::move(r.v);
            free_frames_.pop_back();
        }
    }
    frames_received++;
}


/* --- information rows shown by the development menu bar --- */

std::string StreamView::videoFormatText() const
{
    if (consumer_slot.w <= 0 || consumer_slot.h <= 0) return ui::tr("hud/none");
    char buf[96];
    snprintf(buf, sizeof(buf), "%dx%d %s", consumer_slot.w, consumer_slot.h,
             consumer_slot.is_nv12 ? "NV12 (GPU)" : "YUV420P (CPU)");
    return buf;
}

/* UIFIX-1 2026-09-26 - a stage's name for a person, from the same order as
 * latency.c's k_names. A stage without a key (added later) shows its id. */
static std::string stageLabel(latency_stage_t e)
{
    static const char *const KEYS[] = {
        "hud/stage_burst", "hud/stage_hold", "hud/stage_dec_queue", "hud/stage_decode",
        "hud/stage_disp_queue", "hud/stage_upload", "hud/stage_cadence", "hud/stage_e2e",
        "hud/stage_input", "hud/stage_pad", "hud/stage_pad_rate", "hud/stage_audio_queue",
        "hud/stage_rx_pass",
    };
    const int i = (int)e;
    if (i >= 0 && i < (int)(sizeof KEYS / sizeof KEYS[0])) return ui::tr(KEYS[i]);
    return latency_stage_name(e);
}

std::string StreamView::videoRateText() const
{
    char fps[16];
    snprintf(fps, sizeof(fps), "%.1f", stat_fps.mean());
    return ui::tr("menu/info_fps_received", fps, frames_received);
}

/* Declaration of the metrics panel sections.
 *
 * Each section states WHAT IT MEASURES; layout, colours and sizing belong to
 * the framework (`ui::Hud`). The order fixes the bit of each section in the
 * persisted mask, so new ones go at the end. */
void StreamView::setupHud()
{
    hud_.addSection("perf", ui::tr("hud/perf"), [this](ui::HudBuilder &b) {
        /* Thresholds calibrated on what the server actually delivers (it caps
         * at 30 fps on a desktop): the old panel judged against 60 and showed
         * red during perfectly normal operation. */
        /* Four distinct rates. "Frames per second" meant nothing until you said
         * which one: the draw loop runs at screen rate even when the server
         * sends nothing, and a gap between "received" and "displayed" points at
         * a decoding problem rather than a network one. */
        const std::string per_s = ui::tr("hud/per_sec");
        float rx = rate_received_.value();
        ui::Grade fg = (rx >= 50.0f) ? ui::Grade::Good
                     : (rx >= 24.0f) ? ui::Grade::Warn : ui::Grade::Bad;
        b.gauge("received_rate", ui::tr("hud/received_rate").c_str(), rx / 60.0f, fg,
                "%.1f %s", rx, per_s.c_str());
        b.row("decoded_rate", ui::tr("hud/decoded_rate").c_str(), "%.1f %s",
              rate_decoded_.value(), per_s.c_str());
        b.row("displayed_rate", ui::tr("hud/displayed_rate").c_str(), "%.1f %s",
              rate_presented_.value(), per_s.c_str());
        b.row("render_rate", ui::tr("hud/render_rate").c_str(), "%.1f %s   %s %.1f",
              stat_fps.mean(), per_s.c_str(), ui::tr("hud/spread").c_str(),
              stat_fps.stddev());

        /* === VI5 2026-09-11 - DISPLAY WAIT, NOT "LATENCY" ===
         * This row showed the age of the picture on screen, re-sampled on
         * every draw (~11 ms for a true 3.6), graded against 30/80 ms that
         * nothing had calibrated, under `hud/latency` - the title of the
         * per-stage section right below. It now shows the [L5] video/file-aff
         * sample (fed in draw(), one per new picture) under its own label.
         * A picture waits about half a draw period for the next pass, so the
         * grade is relative to the measured draw interval: beyond twice it,
         * pictures are queuing behind the draw loop; beyond four times,
         * something is wrong. The id stays "latency": it keys the user's saved
         * hidden-row choice (ui::Hud), and a new id would unhide the row. */
        const std::string wait_label = ui::tr("hud/display_wait");
        if (!latency_enabled()) {
            b.row("latency", wait_label.c_str(), "%s", ui::tr("hud/latency_off").c_str());
        } else if (stat_latency.count == 0) {
            b.row("latency", wait_label.c_str(), "%s", ui::tr("hud/none").c_str());
        } else {
            const float l  = stat_latency.mean();
            const float iv = stat_interval.mean();
            if (iv > 0.0f) {
                const ui::Grade lg = (l <= 2.0f * iv) ? ui::Grade::Good
                                   : (l <= 4.0f * iv) ? ui::Grade::Warn : ui::Grade::Bad;
                b.graded("latency", wait_label.c_str(), lg, "%.1f ms   %s %.1f",
                         l, ui::tr("hud/spread").c_str(), stat_latency.stddev());
            } else {
                b.row("latency", wait_label.c_str(), "%.1f ms   %s %.1f",
                      l, ui::tr("hud/spread").c_str(), stat_latency.stddev());
            }
        }

        /* A small spread with a mean close to 16.7 = stable vsync; a large
         * spread betrays stutter rather than packet loss. */
        b.row("interval", ui::tr("hud/interval").c_str(), "%.1f ms   %s %.1f",
              stat_interval.mean(), ui::tr("hud/spread").c_str(),
              stat_interval.stddev());

        session_stats_t ws;
        session_stats_get(&ws);
        b.row("frames_received", ui::tr("hud/frames_received").c_str(),   "%u", frames_received);
        b.row("frames_decoded", ui::tr("hud/frames_decoded").c_str(), "%u", ws.h264_frames_decoded);
        b.row("frames_drawn", ui::tr("hud/frames_drawn").c_str(),"%u", frames_drawn);
        if (ws.h264_decode_errors > 0)
            b.graded("decode_errors", ui::tr("hud/decode_errors").c_str(), ui::Grade::Bad, "%u", ws.h264_decode_errors);
        if (ws.dec_queue_dropped > 0)   /* CONC-4: written at last */
            b.graded("frames_dropped", ui::tr("hud/frames_dropped").c_str(), ui::Grade::Warn, "%u", ws.dec_queue_dropped);
        b.row("session", ui::tr("hud/session").c_str(), "%d:%02d",
              ws.session_seconds / 60, ws.session_seconds % 60);
    });

    /* === L19 2026-08-29 - THE PERCENTILES, WHERE THE GAME IS PLAYED ===
     *
     * The latency stages only existed in the log, and nobody reads a log while
     * playing. Yet during a game is exactly when you want to know whether a
     * spike just went by.
     *
     * What matters here is NOT the mean: a path averaging 25 ms with a 99th
     * percentile at 200 ms plays worse than a tightly grouped 40 ms path. It is
     * the spikes that make you miss a shot. The p99 column is therefore the one
     * worth reading first, and the session worst answers "did it drop out at
     * any point since I started playing".
     *
     * The values come from the last COMPLETE measurement window and change all
     * at once every ten seconds. A partial window would give percentiles that
     * move on every frame, which would read as instability of the path when it
     * would only be the instrument filling up. */
    hud_.addSection("latence", ui::tr("hud/latency"), [this](ui::HudBuilder &b) {
        if (!latency_enabled()) {
            b.row("latency", ui::tr("hud/latency").c_str(), "%s", ui::tr("hud/latency_off").c_str());
            return;
        }
        b.row("latency_head", ui::tr("hud/latency_head").c_str(), "%s", "p50 / p90 / p99 / max");
        int seen = 0;
        for (int e = 0; e < LAT_NB; e++) {
            latency_report_t r;
            const int fed = latency_read((latency_stage_t)e, &r);
            /* VI4 2026-09-11 - a stage silent in the last window SAYS so, with
             * its session worst; it used to keep showing its last fed window's
             * figures while the log called it "never fed" (up to 110 s of a
             * 120 s outage in the bench). A stage never fed stays hidden: the
             * input stages are silent whenever nobody touches anything. */
            if (!fed && r.n_session == 0) continue;
            seen++;
            if (!fed) {
                b.row(latency_stage_name((latency_stage_t)e),
                      stageLabel((latency_stage_t)e).c_str(),
                      "%s   max %.1f ms", ui::tr("hud/latency_silent").c_str(),
                      r.worst_session_us / 1000.0);
                continue;
            }
            /* The SESSION worst, not the window worst: it is what keeps the
             * trace of a past drop-out, which a clean window would erase. */
            /* The row's identifier stays the measurement module's stable name
             * (what the log and the saved panel layout use); the LABEL is
             * translated - "video/televerse" meant nothing to a user. */
            b.row(latency_stage_name((latency_stage_t)e),
                  stageLabel((latency_stage_t)e).c_str(),
                  "%.1f / %.1f / %.1f / %.1f ms",
                  r.p50_us / 1000.0, r.p90_us / 1000.0,
                  r.p99_us / 1000.0, r.worst_session_us / 1000.0);
        }
        if (seen == 0)
            b.row("latency", ui::tr("hud/latency").c_str(), "%s",
                  ui::tr("hud/latency_wait").c_str());
    });

    hud_.addSection("net", ui::tr("hud/net"), [this](ui::HudBuilder &b) {
        session_stats_t ws;
        session_stats_get(&ws);
        float mbps = hud_live_kbps_ / 1000.0f;
        ui::Grade g = (mbps >= 4.0f) ? ui::Grade::Good
                    : (mbps >= 1.0f) ? ui::Grade::Warn : ui::Grade::Bad;
        /* Gauge scaled against the observed server ceiling (~14 Mbps), not
         * against the requested bitrate: the ceiling is what says whether we
         * have headroom. */
        b.gauge("bitrate", ui::tr("hud/bitrate").c_str(), mbps / 14.0f, g, "%.1f Mbps", mbps);
        b.row("packets_video", ui::tr("hud/packets_video").c_str(), "%u", ws.rtp_video_packets);
        /* Audio packets now have their own section: they used to come from a
         * DTLS channel that never received anything, and therefore showed a
         * perpetual zero next to video counters that were climbing. */
        if (ws.session_seconds > 0)
            b.row("packet_rate", ui::tr("hud/packet_rate").c_str(), "%.0f /s",
                  (float)ws.rtp_video_packets / (float)ws.session_seconds);

        /* === S117/S118 2026-09-03 - INTEGRITY, AT LAST ON SCREEN ===
         *
         * This section carried nothing but RATES. A user whose picture degraded
         * while playing could not see their own loss: they had to quit, pull the
         * log over FTP and read a statistics line. That happened, for real, on
         * 2026-09-03.
         *
         * The order is not cosmetic: loss first, because that is what you look
         * for when the picture degrades; the round trip after, because that is
         * the question you ask when it does NOT. */
        if (ws.chunks_expected > 0) {
            const float loss = 100.0f * (float)ws.chunks_missing
                                      / (float)ws.chunks_expected;
            /* The thresholds come from measurement: a healthy session stays under
             * 0.3 % (the server's resend noise); above 1 % the picture starts
             * showing pieces of the previous one. */
            const ui::Grade lg = (loss < 0.3f) ? ui::Grade::Good
                               : (loss < 1.0f) ? ui::Grade::Warn : ui::Grade::Bad;
            b.gauge("loss", ui::tr("hud/loss").c_str(), loss / 3.0f, lg,
                    "%.2f %%", loss);
            /* The lower bound is stated, not hidden. A picture whose opening chunk
             * is lost enters neither of the two counters above; its chunks land
             * here. Showing the rate alone would suggest a precision it does
             * not have. */
            if (ws.chunks_orphan_lost > 0)
                b.row("loss_extra", ui::tr("hud/loss_extra").c_str(), "%u",
                      ws.chunks_orphan_lost);
            if (ws.frames_trunc > 0)
                b.row("frames_trunc", ui::tr("hud/frames_trunc").c_str(), "%u",
                      ws.frames_trunc);
            /* The kernel counter separates OUR fault from the network's: non-zero,
             * it is our own buffers overflowing and the remedy is on our side.
             * Shown only when it is non-zero - a row that reads zero forever is
             * a row people stop reading. */
            if (ws.kernel_drops > 0)
                b.row("kernel_drops", ui::tr("hud/kernel_drops").c_str(), "%u",
                      ws.kernel_drops);
        }

        if (ws.ctrl_rtt_avg_us > 0) {
            const float rtt = ws.ctrl_rtt_us / 1000.0f;
            const ui::Grade rg = (rtt < 30.0f) ? ui::Grade::Good
                               : (rtt < 80.0f) ? ui::Grade::Warn : ui::Grade::Bad;
            /* "to the VM" and not "latency": this is an application-level TCP+TLS
             * round trip, not the latency of the video path, which is UDP and
             * may behave differently. The label carries the distinction because
             * nobody will go and read the struct's comment. */
            b.gauge("rtt", ui::tr("hud/rtt").c_str(), rtt / 100.0f, rg,
                    "%.0f ms", rtt);
            /* The p90 beside the mean: a good mean with a p90 at 200 ms FEELS bad,
             * and the mean alone hides it. */
            b.row("rtt_spread", ui::tr("hud/rtt_spread").c_str(),
                  "%.0f / %.0f ms   %s %.0f",
                  ws.ctrl_rtt_avg_us / 1000.0f, ws.ctrl_rtt_p90_us / 1000.0f,
                  ui::tr("hud/jitter").c_str(), ws.ctrl_rtt_jitter_us / 1000.0f);
        }
    });

    hud_.addSection("video", ui::tr("hud/video"), [this](ui::HudBuilder &b) {
        if (consumer_slot.w <= 0) { b.row("stream", ui::tr("hud/stream").c_str(), "%s", ui::tr("hud/none").c_str()); return; }
        b.row("definition", ui::tr("hud/definition").c_str(), "%dx%d", consumer_slot.w, consumer_slot.h);
        /* K18 - the CODEC and the decode mode. They were missing, and one had
         * to go and read the console log to learn that H.265 was running in
         * hardware. We show what the decoder IS DOING, not what was asked for:
         * the hardware fallback is sticky (S28), so the two can differ while
         * the setting never moves. */
        b.row("codec", ui::tr("hud/codec").c_str(), "%s  %s",
              ctrl_session_glue_codec(),
              ui::tr(ctrl_session_glue_hw() ? "hud/dec_hardware"
                                            : "hud/dec_software").c_str());
        b.row("format", ui::tr("hud/format").c_str(), "%s",
              consumer_slot.chroma_pleine ? "YUV444P (4:4:4)"
                                          : (consumer_slot.is_nv12 ? "NV12" : "YUV420P"));
        b.row("render", ui::tr("hud/render").c_str(), "%s", consumer_slot.is_nv12 ? ui::tr("hud/render_gl").c_str() : "nanovg");
    });

    hud_.addSection("input", ui::tr("hud/input"), [this](ui::HudBuilder &b) {
        std::string mode = ui::tr("menu/touch_off");
        switch (Settings::instance().touch_mode) {
            case TouchMode::Absolute: mode = ui::tr("menu/touch_absolute"); break;
            case TouchMode::Relative: mode = ui::tr("menu/touch_relative"); break;
            default: break;
        }
        b.row("touch", ui::tr("hud/touch").c_str(), "%s   %d", mode.c_str(), touch_prev_count);
        if (touch_prev_count > 0)
            b.row("position", ui::tr("hud/position").c_str(), "%d, %d", touch_prev_x, touch_prev_y);

        shadow_input_debug dbg = {};
        shadow_input_get_debug(&dbg);
        b.row("queue", ui::tr("hud/queue").c_str(), "%d", dbg.queue_depth);
        b.row("moves", ui::tr("hud/moves").c_str(), "%d", dbg.n_mouse_moves);
        b.row("clicks", ui::tr("hud/clicks").c_str(), "%d / %d", dbg.n_clicks_left, dbg.n_clicks_right);
        b.row("keys", ui::tr("hud/keys").c_str(), "%d", dbg.n_keypress);
        b.row("last_action", ui::tr("hud/last_action").c_str(), "%s", dbg.last_action);
    });

    hud_.addSection("advanced", ui::tr("hud/advanced"), [this](ui::HudBuilder &b) {
        b.row("stretch", ui::tr("hud/stretch").c_str(), "%s", g_stretch ? ui::tr("hud/on").c_str() : ui::tr("hud/off").c_str());
        b.row("gamepad", ui::tr("hud/gamepad").c_str(), "%s", ctrl_gamepad_active() ? ui::tr("hud/connected").c_str() : ui::tr("hud/absent").c_str());
        int probe = ctrl_gamepad_axis_probing();
        if (probe >= 0)
            b.graded("axis_probe", ui::tr("hud/axis_probe").c_str(), ui::Grade::Warn, "#%d", probe);
    });

    /* Audio section. The channel long seemed absent, and the counters that did
     * exist - "audio packets" - came from a DTLS channel that received nothing.
     * Everything here is measured on the stream actually played: the server
     * sends each frame twice, so showing the raw received packets would mislead
     * by a factor of two. */
    hud_.addSection("audio", ui::tr("hud/audio"), [this](ui::HudBuilder &b) {
        session_stats_t ws;
        session_stats_get(&ws);
        const std::string per_s = ui::tr("hud/per_sec");

        if (ws.opus_decoded == 0 && ws.rtp_audio_packets == 0) {
            b.row("audio_format", ui::tr("hud/audio_format").c_str(), "%s",
                  ui::tr("hud/audio_silent").c_str());
            return;
        }

        /* Format measured on the wire: CELT fullband, stereo, 10 ms frames at
         * 48 kHz (KB §3.26). */
        /* K18 - the audio codec is NEGOTIATED: "Opus 48 kHz" was hardcoded and
         * started lying as soon as High fidelity was selected. */
        b.row("audio_format", ui::tr("hud/audio_format").c_str(), "%s 48 kHz %s",
              ctrl_session_glue_codec_audio(),
              AUDIO_HUD_CHANNELS);

        float ar = rate_audio_.value();
        /* === AUD-INS-1 2026-09-11 - A FRAME RATE IS NOT A HEALTH GRADE ===
         * The bar still reads against 100 frames/s - real time, a frame lasting
         * 10 ms - but its colour no longer grades anything. The server sends
         * FRAMES, not time: a silent VM sends 2.5-5 a second and the official
         * client receives no more (KB §3.26, AUD14), so every quiet moment of a
         * game read red Bad - and AUD14 records this repo already mistaking such
         * a count for a collapse. Whether frames are MISSING is the
         * "audio_lost" row below. */
        b.gauge("audio_rate", ui::tr("hud/audio_rate").c_str(), ar / 100.0f, ui::Grade::Neutral,
                "%.0f %s", ar, per_s.c_str());
        b.row("audio_bitrate", ui::tr("hud/audio_bitrate").c_str(), "%.0f kbit/s", hud_audio_kbps_);
        /* AUD-INS-1 / AUD-DEDUP-3 2026-09-11 - frames the server sent that never
         * arrived in either copy (streaming/audio_loss.h). Shown only once
         * non-zero, like the other trouble rows (S119). Graded only when the
         * last 10 s expected 500 frames or more, i.e. while sound plays: on a
         * silent VM one lost frame in 50 would read 2 %. Below that, a neutral
         * count. The limit to keep in mind: a single lost copy reads 0, the 2x
         * redundancy hides it. */
        if (ws.opus_lost > 0) {
            uint32_t win_exp = 0, win_lost = 0;
            const audio_loss_grade_t lg = audio_loss_win_grade(&aud_loss_win_, &win_exp, &win_lost);
            if (lg == AUDIO_LOSS_GRADE_NONE)
                b.row("audio_lost", ui::tr("hud/audio_lost").c_str(), "%u", ws.opus_lost);
            else
                b.graded("audio_lost", ui::tr("hud/audio_lost").c_str(),
                         lg == AUDIO_LOSS_GRADE_BAD    ? ui::Grade::Bad
                         : lg == AUDIO_LOSS_GRADE_WARN ? ui::Grade::Warn : ui::Grade::Neutral,
                         "%u (%.1f %%)", ws.opus_lost, 100.0 * win_lost / win_exp);
        }

        if (ws.opus_dup_skipped > 0)
            b.row("audio_dup", ui::tr("hud/audio_dup").c_str(), "%u", ws.opus_dup_skipped);
        if (ws.opus_ring_full > 0)
            b.graded("audio_ring", ui::tr("hud/audio_ring").c_str(), ui::Grade::Warn, "%u",
                     ws.opus_ring_full);
        if (ws.opus_invalid > 0)
            b.graded("audio_invalid", ui::tr("hud/audio_invalid").c_str(), ui::Grade::Warn, "%u",
                     ws.opus_invalid);
        if (ws.opus_errors > 0)
            b.graded("audio_errors", ui::tr("hud/audio_errors").c_str(), ui::Grade::Bad, "%u",
                     ws.opus_errors);
    });


    /* Time series. A mean does not say whether the stream is steady or swinging
     * between two extremes; the curve does. The scale floor keeps a shiver on
     * an idle value from filling the whole frame. */
    hud_.addChart("received", ui::tr("hud/chart_received"), "",
                  [this] { return rate_received_.value(); }, 60.0f);
    hud_.addChart("displayed", ui::tr("hud/chart_displayed"), "",
                  [this] { return rate_presented_.value(); }, 60.0f);
    /* VI5 2026-09-11 - the display wait (see the "perf" row), no longer the
     * age of the picture on screen. The id stays "latency": the chart's bit in
     * the mask is its position, and its saved screen position is keyed by
     * this id. Scale floor 20 ms (about one 60 Hz period) instead of 60: the
     * value now sits near half a draw period, and 60 flattened it. */
    hud_.addChart("latency", ui::tr("hud/display_wait"), "ms",
                  [this] { return stat_latency.mean(); }, 20.0f);
    hud_.addChart("bitrate", ui::tr("hud/chart_bitrate"), "Mbps",
                  [this] { return hud_live_kbps_ / 1000.0f; }, 5.0f);
    hud_.addChart("packets", ui::tr("hud/chart_packets"), "/s",
                  [this] { return hud_pps_; }, 100.0f);
    hud_.addChart("interval", ui::tr("hud/chart_interval"), "ms",
                  [this] { return stat_interval.mean(); }, 40.0f);
    /* Added LAST: the order fixes the bit of each series in the persisted mask,
     * and inserting it in the middle would scramble the choices already saved
     * on the user's side. */
    hud_.addChart("audio", ui::tr("hud/chart_audio"), "kbit/s",
                  [this] { return hud_audio_kbps_; }, 128.0f);

    hud_.setRefreshMs((int)Settings::instance().hud_refresh_ms);
    hud_.setOpacity((int)Settings::instance().hud_opacity);

    uint32_t charts = Settings::instance().hud_charts;
    hud_.setChartMask(charts == Settings::CHARTS_UNSET ? 0x3u : charts);

    /* A zero mask means "never chosen": we then show the essentials rather than
     * an empty panel, which would look like a breakage. */
    uint32_t saved = Settings::instance().hud_sections;
    /* Default 0x27. A section's mask bit is its addSection index (perf 0,
     * latence 1, net 2, video 3, input 4, advanced 5, audio 6), so: perf,
     * latence, net and advanced - video and audio are OFF by default.
     * (AUD-INS-1 2026-09-11: this comment used to say "performance, network,
     * video - and audio", which the mask never did.) */
    hud_.setEnabledMask(saved ? saved : 0x27);
    hud_.setPositions(Settings::instance().hud_positions);
    hud_.setHiddenRows(Settings::instance().hud_rows);
}

/* S93 - entering and leaving the editing mode. We save ON THE WAY OUT and not
 * on every move: the settings file lives on the SD card, and writing it at
 * finger rate would wear it out for nothing. */
void StreamView::reinitPositions()
{
    hud_.resetPositions();
    Settings::instance().hud_positions.clear();
    Settings::instance().save();
}

void StreamView::editPositions(bool on)
{
    /* S105 - entering and leaving a mode is ANNOUNCED: it is a change of rules,
     * not a confirmation, and the long pattern says so without having to read
     * the screen. Allowed here even though a session is running: placement mode
     * already suspends forwarding to the machine, so no other motor is talking. */
    ui::haptics::setEnabled(true);
    ui::haptics::play(ui::haptics::Intent::Mode);
    if (!on) ui::haptics::setEnabled(false);
    hud_.setEditing(on);
    if (!on) {
        Settings::instance().hud_positions = hud_.positions();
        Settings::instance().save();
    }
}

void StreamView::toggleHudSection(size_t i)
{
    hud_.toggleSection(i);
    Settings::instance().hud_sections = hud_.enabledMask();
    Settings::instance().save();
}

void StreamView::toggleHudRow(size_t section, size_t line)
{
    hud_.toggleRow(section, line);
    Settings::instance().hud_rows = hud_.hiddenRows();
    Settings::instance().save();
}

void StreamView::toggleHudChart(size_t i)
{
    hud_.toggleChart(i);
    Settings::instance().hud_charts = hud_.chartMask();
    Settings::instance().save();
}

/* === G33 2026-08-22 — CAPTURE ON DEMAND (the DEVUI "Capture" buttons) ===
 * The user triggers it EXACTLY when they see the artefact; draw() then reads
 * back the video area as actually displayed (glReadPixels, after the shader) and
 * writes a PPM. n=1 one shot, n>1 a burst. Files:
 * /tmp/halyard/capture_*.ppm. */
static std::atomic<int> g_capture_frames{0};
extern "C" void stream_view_capture(int n) { g_capture_frames.store(n > 0 ? n : 1); }

/* === G46 2026-08-22 — KEYS THE LINUX DESKTOP INTERCEPTS ===
 *
 * GNOME/Ubuntu reserves Super (the "Windows key") at the compositor level: the
 * window NEVER receives the event, it opens Ubuntu's Activities instead of the
 * VM's Start menu. Same for Ctrl+Alt+Del (grabbed by the system) and partly
 * Alt+Tab. No application can reclaim these shortcuts under Wayland; the
 * official client has the same limitation and therefore offers to SEND them
 * explicitly. We expose the same actions in the menu, going through the usual
 * keyboard path (evdev scancodes). Press/release order is reversed for the
 * modifiers, like a real keyboard. */
extern "C" void stream_view_send_key(int evdev_code)
{
    if (evdev_code <= 0) return;
    shadow_input_post_scancode((uint16_t)evdev_code, true);
    shadow_input_post_scancode((uint16_t)evdev_code, false);
}

extern "C" void stream_view_send_combo(int mod1, int key, int mod2)
{
    if (mod1 > 0) shadow_input_post_scancode((uint16_t)mod1, true);
    if (mod2 > 0) shadow_input_post_scancode((uint16_t)mod2, true);
    if (key  > 0) { shadow_input_post_scancode((uint16_t)key, true);
                    shadow_input_post_scancode((uint16_t)key, false); }
    if (mod2 > 0) shadow_input_post_scancode((uint16_t)mod2, false);
    if (mod1 > 0) shadow_input_post_scancode((uint16_t)mod1, false);
}

/* === Executing a gesture (2026-08-27) ===
 *
 * The button codes are GLFW's - 0 left, 1 RIGHT, 2 middle - and the mapping to
 * the protocol index is the identity (ctrl_input_tcp.c, fixed 2026-08-21). The
 * middle click has never been verified against a VM: it is offered, not
 * attested.
 *
 * In ABSOLUTE mode every action first places the cursor at the position carried
 * by the event. For the second tap of a double click, that position is the
 * FIRST one's: Windows requires both clicks within a 4x4 pixel rectangle
 * (SM_CXDOUBLECLK), and a finger never lands that precisely - this realignment
 * is what was missing. */
void StreamView::executeGesture(const gesture_evt &e, bool has_session)
{
    if (e.what <= GESTURE_NONE || e.what >= GESTURE_COUNT) return;
    const Settings &cfg = Settings::instance();
    const GestureAction a = (GestureAction)cfg.gesture_action[e.what];
    if (a == GestureAction::None) return;

    auto place = [&] {
        if (has_session && cfg.touch_mode == TouchMode::Absolute)
            shadow_input_post_mouse_move_abs(e.x, e.y);
    };

    switch (a) {
        case GestureAction::LeftClick:
        case GestureAction::RightClick:
        case GestureAction::MiddleClick: {
            const int btn = (a == GestureAction::LeftClick) ? 0
                          : (a == GestureAction::RightClick)  ? 1 : 2;
            if (!has_session) break;
            place();
            shadow_input_emit_click(btn);
            break;
        }
        case GestureAction::DoubleClick:
            if (!has_session) break;
            place();
            shadow_input_emit_click(0);
            shadow_input_emit_click(0);
            break;
        case GestureAction::Drag:
            /* Held until the finger lifts, where the touch loop releases the
             * button. Emitted DURING the press - that is why the module reports
             * the long press without waiting for the release. */
            if (!has_session || touch_drag_active) break;
            place();
            shadow_input_post_mouse_button(0, true);
            touch_drag_active = true;
            break;
        case GestureAction::Keyboard:   toggleKeyboard(); break;
        case GestureAction::PauseMenu: openMenu();       break;
        /* evdev scancodes: ESC 1, TAB 15, left ALT 56, left META 125. */
        case GestureAction::Escape:   if (has_session) stream_view_send_key(1);   break;
        case GestureAction::Windows: if (has_session) stream_view_send_key(125); break;
        case GestureAction::AltTab:  if (has_session) stream_view_send_combo(56, 15, 0); break;
        default: break;
    }
}

void StreamView::draw(NVGcontext* vg, float x, float y, float width, float height,
                      brls::Style style, brls::FrameContext* ctx) {
    /* === LAT-V2 2026-09-26 - WHERE A FRAME OF THE STREAM GOES ============
     *
     * [UI7] says the whole Borealis frame costs 16.7 ms for the first ~15 s of
     * a Vita stream and 21-22 ms after, in every run of two 12-session
     * campaigns; clocks, idle policy, MSAA, vsync, the cursor sprite and the
     * display-queue depth were each ruled out. This splits the frame: the
     * sections of this draw, and "outside" = the rest of the Borealis frame
     * (other views, nanovg flush, GXM end/swap). Off unless
     * SHADOW_UI_FRAME_MS is set (the same switch as [UI7]); a 5 s summary. */
    static int prof_on = -1;
    if (prof_on < 0) { const char *e = getenv("SHADOW_UI_FRAME_MS"); prof_on = e ? (atoi(e) != 0) : 0; }
    struct DrawProf {
        StreamView *v; bool on; int64_t entry, t;
        void cp(int i) {
            if (!on) return;
            const int64_t n = latency_now_us();
            v->prof_acc_[i] += (double)(n - t) / 1000.0; t = n;
        }
        ~DrawProf() {
            if (!on) return;
            cp(7);                                         /* rest of draw */
            v->prof_acc_[8] += (double)(t - entry) / 1000.0; /* whole draw */
            v->prof_n_++;
        }
    } prof{this, prof_on != 0, 0, 0};
    if (prof.on) {
        const int64_t now = latency_now_us();
        if (prof_prev_entry_us_ > 0) prof_acc_[9] += (double)(now - prof_prev_entry_us_) / 1000.0;
        prof_prev_entry_us_ = now;
        prof.entry = prof.t = now;
        if (prof_last_report_us_ == 0) prof_last_report_us_ = now;
        if (now - prof_last_report_us_ >= 5000000 && prof_n_ > 0) {
            const double n = (double)prof_n_;
            svlog("[LAT-V2] ms/img sur %d : trame %.1f = draw %.1f (entree %.2f pop %.2f "
                  "video %.2f curseur %.2f compteurs %.2f [devui %.2f steady_now %.2f stats_get %.2f "
                  "debits %.2f nettest %.2f hud %.2f] entrees %.2f clavier+menu %.2f) + hors draw %.1f",
                  prof_n_, prof_acc_[9] / n, prof_acc_[8] / n, prof_acc_[0] / n,
                  prof_acc_[1] / n, prof_acc_[2] / n, prof_acc_[3] / n,
                  (prof_acc_[4] + prof_acc_[12] + prof_acc_[10] + prof_acc_[11] + prof_acc_[6] + prof_acc_[13]) / n,
                  prof_acc_[4] / n, prof_acc_[12] / n, prof_acc_[10] / n, prof_acc_[11] / n,
                  prof_acc_[6] / n, prof_acc_[13] / n,
                  prof_acc_[5] / n, prof_acc_[7] / n, (prof_acc_[9] - prof_acc_[8]) / n);
            for (double &a : prof_acc_) a = 0.0;
            prof_n_ = 0;
            prof_last_report_us_ = now;
        }
    }
    /* S90 - the equaliser follows the dock, DURING A SESSION TOO. This screen
     * does not go through `ui::Screen::draw`: without this line, docking the
     * console mid-game would keep the "handheld" preset, hence bass cut at
     * 160 Hz on a TV. The probe is just a boolean comparison as long as the mode
     * does not change. */
    Settings::followConsoleMode();

    /* DEVL-4 2026-09-12 - the stream describes itself too.
     *
     * Like the line above, this screen does NOT go through `ui::Screen::draw`,
     * so it would have stayed `ecran=inconnu` while every menu named itself -
     * and the stream is precisely where a script most needs to know it has
     * arrived. Published only on change, `nb` carrying the pause menu's depth so
     * a script can tell "in the game" from "in the pause menu" without a
     * screenshot. */
    {
        /* The name says WHICH page is in front, not how deep we are: a script
         * asserting "I am on the link-quality page" must not have to know that
         * it happens to be the third entry of the pause menu. */
        const char *page = mouse_test_ ? "test-souris"
                         : net_test_   ? "test-reseau"
                         : pad_test_   ? "test-manette"
                         : pause_menu_.isOpen() ? "menu-pause"
                                                : "flux";
        static const char *last_page = nullptr;
        if (page != last_page) {         /* literals: identity is enough */
            last_page = page;
            devlink::publishState(devlink::composeState(page, -1, 0, ""));
        }
    }

    // Background
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillColor(vg, nvgRGB(0, 0, 0));
    nvgFill(vg);

    /* The session timer ("T=1:04 pkts=...") used to sit permanently in the top
     * right of the picture, in green over the remote desktop. It was an
     * instrument for debugging the t=120 s freeze, kept long after. It has become
     * a row of the metrics panel, where you go when you want that figure. */

    /* VI1 2026-09-11 - the frozen-picture banner moved further down, after the
     * video (see "THE FROZEN-PICTURE BANNER"): drawn here, the picture covered
     * it. */

    prof.cp(0);   /* LAT-V2: entry work (settings, devlink state) */
    // === Step 1: pop the OLDEST frame from pending_queue (FIFO) ===
    // Frame pacing: we dequeue exactly 1 frame per draw (60Hz). When the queue
    // is empty we keep the previous frame (NVDEC not ready yet). With 2+ frames
    // buffered (a burst), we present them one per tick.
    bool got_new_yuv = false;
    /* VI2 - frames the pool cannot take, destroyed at the end of draw(), after
     * the unlock: at most the L3 skips (PACING_QUEUE_MAX - 1) plus the
     * consumer_slot being replaced. */
    YuvFrame spill[PACING_QUEUE_MAX];
    size_t   n_spill = 0;
    {
        std::lock_guard<std::mutex> lock(swap_mtx);
        if (!pending_queue.empty()) {
            /* VI2 - with SHADOW_PUSH_RECYCLE, a frame leaving the queue or the
             * slot parks its planes in free_frames_ (O(1) move) rather than
             * being freed under this lock. A frame with no buffer (the empty
             * slot before the first picture) has nothing to park, and would
             * only take a pool place from one that has. */
            const bool recycle = push_recycle_on();
            auto retire = [&](YuvFrame &&fr) {
                if (fr.y.capacity() == 0 && fr.u.capacity() == 0 && fr.v.capacity() == 0)
                    return;
                if (free_frames_.size() < PUSH_POOL_MAX)
                    free_frames_.push_back(std::move(fr));
                else if (n_spill < PACING_QUEUE_MAX)
                    spill[n_spill++] = std::move(fr);
                /* else it is freed here, under the lock, as before VI2 -
                 * unreachable while the queue holds at most PACING_QUEUE_MAX. */
            };
            /* === L3 2026-08-22 — SHOW THE NEWEST, NOT THE OLDEST ===
             * We popped one picture per draw, in arrival order. As soon as a
             * burst leaves two pictures queued, we therefore showed the STALE one
             * and kept the fresh one for the next draw: a free frame of latency
             * (~20 ms) every time. For gaming, the only picture that matters is
             * the last one: we drop the intermediates.
             * This only skips pictures ALREADY late - the perceived frame rate
             * does not drop, the latency does. `SHADOW_PRESENT_LATEST=0` restores
             * FIFO scrolling (useful to judge smoothness on slow content). */
            static int latest = -1;
            if (latest < 0) { const char *e = getenv("SHADOW_PRESENT_LATEST"); latest = e ? atoi(e) : 1; }
            if (latest) {
                static uint32_t skipped = 0;
                while (pending_queue.size() > 1) {
                    if (recycle) retire(std::move(pending_queue.front()));   /* VI2 */
                    pending_queue.pop_front();
                    if ((++skipped % 200) == 0)
                        brls::Logger::debug("[L3] {} stale pictures skipped at display", skipped);
                }
            }
            /* VI2 - park the outgoing picture's planes BEFORE the move-assign,
             * which would otherwise free them here, under the lock. Safe: the
             * renderer keys its upload on nv12_seq_, and it finished reading
             * these bytes - glTexSubImage2D copied them - in an earlier draw
             * on this same thread. */
            if (recycle) retire(std::move(consumer_slot));
            /* === VI3 2026-09-14 - THE QUEUE WAS EMPTY HERE, AND IT CANNOT BE ===
             *
             * A console dump puts `abort` directly above this line
             * (`deque::front()`, stream_view.cpp:1332) after ~50 s of stream.
             * By the code alone that is impossible: `pending_queue` is only
             * touched under `swap_mtx` - here and in `pushYuvFrame` - the
             * enclosing `if` tested `empty()`, and the L3 loop stops at
             * `size() > 1`. So either there is a hole that reading has not
             * found, or the deque is CORRUPT and the emptiness is a symptom of
             * something that overwrote it.
             *
             * The two cases need opposite fixes, and a silent guard would hide
             * which one is true. So: guard AND say so, capped at five lines.
             * If this ever prints, the queue really does empty and the hole is
             * findable. If the console still dies with this line never
             * printed, the deque was corrupt and the search belongs elsewhere
             * - which is just as useful an answer.
             *
             * Dropping the present costs one frame: the previous picture stays
             * on screen for another 16 ms. Dereferencing an empty deque costs
             * the session. */
            if (pending_queue.empty()) {
                static int told = 0;
                if (told < 5) {
                    told++;
                    brls::Logger::error("[VI3] pending_queue emptied between the "
                                        "guard and the pop - picture not presented "
                                        "(#{}/5)", told);
                }
            } else {
                consumer_slot = std::move(pending_queue.front());
                pending_queue.pop_front();
                got_new_yuv = true;
                nv12_seq_++;   /* HO-1: the renderer uploads when this number changes */
            }
            /* A NEW picture reaches the screen. `frames_drawn` counts draws,
             * not pictures: on a still desktop it keeps climbing at the display's
             * rate while nothing changes. */
            frames_presented_++;
        }
    }

    /* === L5 2026-08-29 — THE THREE MEASUREMENTS OF THE DISPLAY STAGE ===
     *
     * They are taken HERE, outside any display guard. `stat_latency`, which
     * already measured something close, was only fed under
     * `if (show_perf_stats && hud_.visible())`: a counter that is WRITTEN only
     * while you are looking at it says nothing about a normal session, and
     * leaves no trace in the log. That is the dead-counter failure, and this
     * repo has paid for seven of them. (VI5 2026-09-11: it is now fed below,
     * from the DISP_QUEUE sample itself.)
     *
     * Cost: two `clock_gettime` per draw, i.e. ~120 per second.
     *
     * 1. CADENCE - the interval between two draws. It is 16.7 ms as long as
     *    vsync holds; it is what tells you the UI has fallen to 30 fps, in which
     *    case EVERY item paced by the draw doubles (including input sampling)
     *    with nothing else reporting it.
     * 2. DISP_QUEUE - from the picture being pushed to it being popped. That is
     *    what waiting for the next draw pass costs. "Pushed" is the moment its
     *    planes are copied and it goes for the queue lock (VI2).
     * 3. E2E - from the SERVER stamp to this instant. See latency.h: only its
     *    variation means anything, there is no common clock. */
    if (latency_enabled()) {
        const int64_t lat_now_us = latency_now_us();
        /* LAT-V1: the clocks, read back every 10 s of stream (logged on change). */
        if (lat_now_us >= pwr_next_check_us_) {
            pwr_next_check_us_ = lat_now_us + 10 * 1000000LL;
            shadow_power_profile_check();
        }
        if (lat_last_draw_us > 0)
            latency_add(LAT_VID_CADENCE, lat_now_us - lat_last_draw_us);
        lat_last_draw_us = lat_now_us;
        if (got_new_yuv) {
            /* VI2 2026-09-11 - `push_us` comes from latency_now_us() too, so
             * this is one clock by construction. It used to come from
             * `steady_clock`, which relied on libstdc++ resolving it onto
             * `CLOCK_MONOTONIC`. */
            if (consumer_slot.push_us > 0) {
                const int64_t wait_us = lat_now_us - consumer_slot.push_us;
                latency_add(LAT_VID_DISP_QUEUE, wait_us);
                /* === VI5 2026-09-11 - THE PANEL'S ROW IS THIS VERY SAMPLE ===
                 * The panel pushed `now - push_us` on EVERY draw while it was
                 * open, re-sampling the same ageing picture: it read file-aff
                 * plus half of (picture period - draw period), ~11 ms on the
                 * baseline against a true 3.6 ms, under the title of the
                 * per-stage section right below it. One sample per NEW
                 * picture now, the one [L5] video/file-aff gets, so the two
                 * cannot disagree - and it is fed whether the panel is open or
                 * not. */
                const float wait_ms = (float)wait_us / 1000.0f;
                if (wait_ms >= 0.0f && wait_ms < 5000.0f) stat_latency.push(wait_ms);
            }
            latency_video_displayed(consumer_slot.srv_stamp, lat_now_us);
        }
    }

    // === Step 2: YUV -> RGBA conversion outside the mutex (YUV420P CPU NEON path) ===
    // For NV12 we skip this step - direct GPU shader render further down.
    if (got_new_yuv && !consumer_slot.is_nv12
            && consumer_slot.w > 0 && consumer_slot.h > 0) {
        rgba_buffer.resize((size_t)consumer_slot.w * consumer_slot.h * 4);
        rgba_w = consumer_slot.w;
        rgba_h = consumer_slot.h;
        yuv420p_to_rgba(consumer_slot.w, consumer_slot.h,
                         consumer_slot.y.data(), consumer_slot.ys,
                         consumer_slot.u.data(), consumer_slot.us,
                         consumer_slot.v.data(), consumer_slot.vs,
                         rgba_buffer.data());
        need_upload = true;
    }

    prof.cp(1);   /* LAT-V2: pop + latency stamps + CPU conversion */
    // === Step 3: nanovg upload + fetch the handles for rendering ===
    int img = nvg_image;
    int img_w = rgba_w;
    int img_h = rgba_h;
    if (need_upload && !rgba_buffer.empty() && rgba_w > 0 && rgba_h > 0) {
        if (img < 0) {
            img = nvg_image = nvgCreateImageRGBA(vg, rgba_w, rgba_h,
                                                  NVG_IMAGE_NEAREST,
                                                  rgba_buffer.data());
        } else {
            nvgUpdateImage(vg, img, rgba_buffer.data());
        }
        need_upload = false;
    }

    // Letterboxing (common to both paths)
    // SHADOW_STRETCH=1: fills the whole window (= distorts the aspect ratio, but
    // avoids black bars when the stream is a 1920x540 crop shown in a 16:9
    // viewport).
    /* === 2026-09-02 - A SETTING, NOT A FROZEN `static` ===
     *
     * This block used to read `SHADOW_STRETCH` ONCE, into a function `static`,
     * on the first frame drawn - the repo's most expensive defect family, and
     * here it had two consequences. The reading was frozen before the first
     * picture arrived, and the only way to change it afterwards was the desktop
     * menu bar (`devui/dev_menu.cpp`), which is excluded from the Switch build.
     * On console the behaviour was therefore reachable by no means at all.
     *
     * Read on every frame now: the toggle is turned WHILE looking at the
     * picture, which is the only way to judge whether the distortion is worth
     * the black bars it removes. The variable keeps priority, like everywhere
     * here, so a campaign can impose it without going through the UI. */
    int stretch_enabled = Settings::instance().stretch_image ? 1 : 0;
    if (const char *e = getenv("SHADOW_STRETCH")) stretch_enabled = (atoi(e) == 1);
    g_stretch = stretch_enabled;   /* the desktop menu bar reads it back */
    /* Scale of the surfaces we draw by hand.
     *
     * Borealis works in a logical frame of fixed width: enlarging the window
     * enlarges everything drawn into it, and the panel took up the same fraction
     * of a screen twice the size. On desktop we therefore cancel the window's
     * scale to keep an apparent size constant; on Switch the window never moves
     * and that correction would only shrink everything. The user's chosen
     * magnification multiplies the whole thing. */
    {
        float k = (float)Settings::instance().ui_scale / 100.0f;
        if (k <= 0.0f) k = 1.0f;
#if !SHADOW_UI_FIXED_WINDOW
        float ws_ui = brls::Application::windowScale;
        if (ws_ui > 0.0f) k /= ws_ui;
#endif
        hud_.setScale(k);
        /* Read on every frame: the setting is changed from the pause menu, and
         * it must be visible while being turned - otherwise you adjust blind and
         * come back three times. */
        hud_.setOpacity((int)Settings::instance().hud_opacity);
        pause_menu_.setScale(k);
        devui::setScale(k);
    }

    /* The bar takes the top: the video starts below it, otherwise it would pass
     * behind and the menu's clicks would land on the picture. On Switch
     * `barHeight()` returns 0 and the calculation vanishes. */
    const float bar_h = devui::barHeight();
    /* The pause menu is modal: it covers the whole view, bar included. So we
     * keep the original dimensions before reserving the bar. */
    const float view_x = x, view_y = y, view_w = width, view_h = height;
    y      += bar_h;
    height -= bar_h;

    auto letterbox = [&](int sw, int sh, float &dx, float &dy, float &dw, float &dh) {
        if (stretch_enabled) {
            dx = x; dy = y; dw = width; dh = height;
            return;
        }
        float dst_aspect = width / height;
        float src_aspect = (float)sw / (float)sh;
        dx = x; dy = y; dw = width; dh = height;
        if (src_aspect > dst_aspect) {
            dh = width / src_aspect;
            dy = y + (height - dh) / 2.0f;
        } else {
            dw = height * src_aspect;
            dx = x + (width - dw) / 2.0f;
        }
    };

    // === Phase 2A - GPU shader path for NV12 ===
    // Renders directly through a custom GLSL fragment shader (NV12->RGB on the
    // GPU). Avoids the NEON CPU conversion (~5-8 ms) and the RGBA upload
    // (3.5 MB -> 1.4 MB Y+UV).
    if (consumer_slot.is_nv12 && consumer_slot.w > 0 && consumer_slot.h > 0) {
        float dx, dy, dw, dh;
        letterbox(consumer_slot.w, consumer_slot.h, dx, dy, dw, dh);

#if SHADOW_HAS_GXM_VIDEO
        /* === THE VITA DRAWS HERE, AND DELIBERATELY BEFORE THE GL BLOCK =====
         *
         * Inside the nanovg frame - no `nvgEndFrame`, no viewport, no
         * `windowScale`. The rect below is already in the coordinate space
         * nanovg is drawing in, which is the space `dx/dy/dw/dh` were computed
         * in; V1's multiplication exists only because the GL shader converts
         * pixels to NDC itself and therefore needs the FRAMEBUFFER size.
         * Applying it here would enlarge the picture by the scale factor
         * twice.
         *
         * The GL path below is left untouched and unreachable here: without a
         * loader `vp_w`/`vp_h` stay 0 and its own guard skips it. */
        {
            const int64_t lat_tex0 = latency_enabled() ? latency_now_us() : 0;
            /* A non-null `ext_y` means the planes still live in the decoder's
             * buffer and were never copied (see stream_view.hpp). */
            const uint8_t *py = consumer_slot.ext_y ? consumer_slot.ext_y
                                                    : consumer_slot.y.data();
            const uint8_t *pu = consumer_slot.ext_u ? consumer_slot.ext_u
                                                    : consumer_slot.u.data();
            if (py && pu)
                gxm_renderer.render(vg, consumer_slot.w, consumer_slot.h,
                                    py, consumer_slot.ys, pu, consumer_slot.us,
                                    dx, dy, dw, dh, nv12_seq_);
            if (lat_tex0)
                latency_add(LAT_VID_UPLOAD, latency_now_us() - lat_tex0);
        }
#endif

        // Fetch the viewport, for the pixel->NDC conversion in the shader
        int vp_w = 0, vp_h = 0;
#if SHADOW_HAVE_GLAD
        GLint vp[4] = {0,0,0,0};
        glGetIntegerv(GL_VIEWPORT, vp);
        vp_w = vp[2]; vp_h = vp[3];
#endif
        /* Without a GL loader this stays 0 and the block below is skipped -
         * which is right: the renderer refuses too, and drawing nothing is the
         * honest outcome until a GXM renderer exists. */

        if (vp_w > 0 && vp_h > 0) {
            /* === V1 2026-08-21 — THE VIDEO DID NOT FOLLOW THE ZOOM ===
             * `dx/dy/dw/dh` come from the view's rect, so in Borealis LOGICAL
             * pixels, whereas `vp_w/vp_h` is the framebuffer in REAL pixels. As
             * the shader converts pixel -> NDC using the viewport, enlarging the
             * window enlarged the viewport without enlarging the rect: the video
             * stayed at its size in an ever-bigger window. So we convert to real
             * pixels. (Same factor as the cursor below, which does the inverse:
             * it divides the raw position by `windowScale`.) */
            float ws = brls::Application::windowScale;
            if (ws <= 0) ws = 1.0f;
            nvgEndFrame(vg);
            /* L5 - the CPU cost of uploading both textures and drawing the
             * quad. This is NOT GPU time: there is no `glFinish` on this path,
             * and adding one to measure would cost more than what it measures
             * (it would serialise the loop on the GPU every frame). What IS
             * measured - the time the UI thread spends in GL calls - is exactly
             * what delays the next draw, hence the useful part. */
            /* VI3 2026-09-11 - the lazy init (shader compile, 15-40 ms) ran
             * inside the timed region and pinned video/televerse's session
             * worst at 23-25 ms. A no-op once done. If it fails, render()
             * could only fail the same way - after compiling a second time -
             * so it is skipped. */
            if (gl_renderer.init()) {
                const int64_t lat_tex0 = latency_enabled() ? latency_now_us() : 0;
                gl_renderer.render(consumer_slot.w, consumer_slot.h,
                                    consumer_slot.y.data(), consumer_slot.ys,
                                    consumer_slot.u.data(), consumer_slot.us,
                                    dx * ws, dy * ws, dw * ws, dh * ws, vp_w, vp_h,
                                    consumer_slot.chroma_pleine, nv12_seq_);
                if (lat_tex0)
                    latency_add(LAT_VID_UPLOAD, latency_now_us() - lat_tex0);
            }
            if (consumer_slot.chroma_pleine) {
                static int dit_rendu = 0;
                if (!dit_rendu) {
                    dit_rendu = 1;
                    svlog("[K17] 4:4:4 rendu : %dx%d us=%d dst=%dx%d",
                               consumer_slot.w, consumer_slot.h,
                               consumer_slot.us, (int)(dw * ws), (int)(dh * ws));
                }
            }

            /* SHADOW_DUMP_RENDER=1 - reads back the video area AFTER the
             * shader (what is REALLY on screen) and writes it as a PPM. Compare
             * it with the decoded picture: if they differ, the rendering adds
             * corruption; otherwise the screen shows the decode faithfully. */
            {
                static int rchk=0, ron=0; static uint32_t rn=0;
                if(!rchk){rchk=1; const char*e=getenv("SHADOW_DUMP_RENDER"); ron=(e&&atoi(e)==1);}
                rn++;
                bool env_shot = ron && (rn==300||rn==1000||rn==2000||rn==3000);
                bool req_shot = g_capture_frames.load() > 0;   /* G33: DEVUI button */
                if (env_shot || req_shot) {
                        static uint32_t cap_idx = 0;
                        int rw=(int)(dw*ws), rh=(int)(dh*ws);
                        int rx=(int)(dx*ws), ry=(int)(vp_h-(dy*ws)-rh);
                        /* clamp to the framebuffer, so we never read out of bounds */
                        if (rx<0) rx=0; if (ry<0) ry=0;
                        if (rx+rw>vp_w) rw=vp_w-rx; if (ry+rh>vp_h) rh=vp_h-ry;
                        if (rw>0 && rh>0) {
                            std::vector<uint8_t> px((size_t)rw*rh*3);
                            /* PACK_ALIGNMENT=1: otherwise every row is padded to
                             * a multiple of 4 bytes and the dump shears into
                             * horizontal stripes (a read-back artefact). */
#if SHADOW_HAVE_GLAD
                            glPixelStorei(GL_PACK_ALIGNMENT, 1);
                            glReadPixels(rx, ry, rw, rh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
#else
                            /* No GL loader (Vita/GXM): devlink's `shot` has no
                             * way to read the framebuffer back here yet. */
                            std::fill(px.begin(), px.end(), (unsigned char)0);
#endif
                            char path[256];
                            if (req_shot) snprintf(path,sizeof(path),"/tmp/halyard/capture_%03u.ppm", cap_idx++);
                            else          snprintf(path,sizeof(path),"/tmp/halyard/render_%04u.ppm", rn);
                            FILE*f=fopen(path,"wb");
                            if(f){ fprintf(f,"P6\n%d %d\n255\n",rw,rh);
                                   for(int y2=rh-1;y2>=0;y2--) fwrite(px.data()+(size_t)y2*rw*3,1,(size_t)rw*3,f);
                                   fclose(f);
                                   if (req_shot) { fprintf(stderr,"[G33] capture -> %s (%dx%d)\n", path, rw, rh);
                                                   brls::Logger::info("Capture: {} ({}x{})", path, rw, rh); } }
                        }
                        if (req_shot) g_capture_frames--;
                }
            }
            /* === V2 2026-08-21 — EVERYTHING SHIFTED AFTER A RESIZE ===
             * Borealis opens its frame with nvgBeginFrame(...) THEN
             * nvgScale(windowScale): its views draw in LOGICAL pixels. Closing
             * the frame to render the video in OpenGL loses that transform; it
             * must be restored identically, otherwise everything that comes
             * after (Shadow cursor, HUD, menu bar, widgets) is drawn in REAL
             * pixels. At 1280 wide, windowScale is 1 and the bug is invisible -
             * it only appears on the first resize, on all three at once. */
            float sf = (float)brls::Application::getPlatform()
                           ->getVideoContext()->getScaleFactor();
            if (sf <= 0) sf = 1.0f;
            nvgBeginFrame(vg, (float)vp_w, (float)vp_h, sf);
            nvgScale(vg, ws, ws);
            frames_drawn++;
        }
    }
    // === Classic nanovg path (YUV420P CPU decode fallback) ===
    else if (img >= 0 && img_w > 0 && img_h > 0) {
        float dx, dy, dw, dh;
        letterbox(img_w, img_h, dx, dy, dw, dh);
        NVGpaint paint = nvgImagePattern(vg, dx, dy, dw, dh, 0.0f, img, 1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, dx, dy, dw, dh);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
        frames_drawn++;
    } else {
        /* Waiting screen. It used to show "Waiting for video... (received=0)":
         * a debugging counter, and a zero that makes it look like something is
         * broken when the machine is simply preparing the stream. A title, a
         * sentence, and three dots that advance - enough to see the application
         * is not frozen. */
        const float cx_w = x + width / 2.0f;
        const float cy_w = y + height / 2.0f;

        nvgFontFace(vg, "regular");
        nvgFontSize(vg, ui::type::SECTION);
        nvgFillColor(vg, nvgRGBA(238, 240, 246, 255));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, cx_w, cy_w - 14.0f, ui::tr("stream/waiting").c_str(), nullptr);

        nvgFontSize(vg, ui::type::SECONDARY);
        nvgFillColor(vg, nvgRGBA(158, 165, 182, 255));
        nvgText(vg, cx_w, cy_w + 16.0f, ui::tr("stream/waiting_hint").c_str(), nullptr);

        /* Three dots lighting up in turn, one second per cycle. */
        {
            int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
            const int lit = (int)((ms / 330) % 3);
            for (int d = 0; d < 3; d++) {
                nvgBeginPath(vg);
                nvgCircle(vg, cx_w - 14.0f + (float)d * 14.0f, cy_w + 46.0f, 3.5f);
                nvgFillColor(vg, (d == lit) ? nvgRGBA(126, 148, 190, 255)
                                            : nvgRGBA(90, 96, 112, 255));
                nvgFill(vg);
            }
        }
    }

    prof.cp(2);   /* LAT-V2: video render */
    // === Shadow cursor overlay (synthetic) ===
    // We draw an arrow at the position WE send to Shadow. Since Shadow does not
    // render the OS pointer into its video stream (it pushes the bitmap
    // separately on the shadow-cursor channel - which we do not overlay yet),
    // this crosshair is the visual feedback: where the mouse IS on the Shadow VM.
    // Coordinates in pixels of the source resolution (1280x720 by default).
    {
        int csrc_w = 0, csrc_h = 0;
        if (consumer_slot.is_nv12 && consumer_slot.w > 0) {
            csrc_w = consumer_slot.w; csrc_h = consumer_slot.h;
        } else if (img_w > 0) {
            csrc_w = img_w; csrc_h = img_h;
        } else {
            csrc_w = 1280; csrc_h = 720;
        }
        float cdx, cdy, cdw, cdh;
        letterbox(csrc_w, csrc_h, cdx, cdy, cdw, cdh);
        int cx_src = 640, cy_src = 360;
        shadow_input_get_cursor_pos(&cx_src, &cy_src);
        float scale_x = cdw / (float)csrc_w;
        float scale_y = cdh / (float)csrc_h;
        float cx = cdx + cx_src * scale_x;
        float cy = cdy + cy_src * scale_y;

        /* === S113 2026-08-29 - THE CHOICE BELONGS TO THE USER ===
         *
         * S77 cut BOTH of our cursor renderings, and it was right on a Windows
         * DESKTOP: the stream already carries the pointer the server composites
         * in, at the right place. But that is not true everywhere - when the VM
         * composites nothing (full screen, some games), cutting both leaves NO
         * pointer at all, and a console has no system cursor to compensate. The
         * client cannot know which of the two cases it is in.
         *
         * Three sources, chosen in Settings > Picture > Cursor. The environment
         * toggle stays AUTHORITATIVE (the A/B campaigns use it), and the pause
         * menu can force the arrow for the current session.
         *
         * The setting is re-read ON EVERY FRAME. Freezing it in a `static` would
         * make it SESSION state - the defect family that has already produced
         * four failures here - and, concretely, changing it would have no effect
         * before the application restarts. The cost is reading one field of a
         * singleton, once per frame. */
        static int g_arrow_env = -3;          /* -3 = not read yet, -2 = unset */
        if (g_arrow_env == -3) {
            const char *e = getenv("SHADOW_CURSOR_ARROW");
            g_arrow_env = e ? atoi(e) : -2;
        }
        const uint32_t cursor_src = Settings::instance().cursor_source;
        const int cursor_arrow = (g_cursor_arrow >= 0) ? g_cursor_arrow
                               : (g_arrow_env >= 0)    ? g_arrow_env
                               : (cursor_src == 2 ? 1 : 0);
        g_cursor_arrow_eff = cursor_arrow;

        /* === S54 2026-08-26 — THE REAL POINTER OF THE REMOTE MACHINE ===
         *
         * The comment above was right: "it pushes the bitmap separately on the
         * shadow-cursor channel - which we do not overlay yet". We do now. The
         * `:base+20` channel carries the pointer image (KB §3.37); it had been
         * arriving all along and the video decoder was rejecting it.
         *
         * The POSITION, though, does not come from the server: it only sends the
         * shape (measured - 55 type 0x02 frames, all zeroes). So we draw the
         * remote image at the position WE track, which is exactly what the
         * synthetic arrow did.
         *
         * Fallback: as long as no image has arrived - the first few seconds, or
         * if the channel does not open - the synthetic arrow stays. An
         * approximate pointer beats no pointer.
         * `SHADOW_CURSOR_SPRITE=0` forces the synthetic arrow. */
        bool sprite_drawn = false;
        {
            static int g_sprite = -3;          /* see cursor_arrow above */
            if (g_sprite == -3) {
                const char *e = getenv("SHADOW_CURSOR_SPRITE");
                /* S77: see the SHADOW_CURSOR_ARROW comment. The video stream
                 * already carries the pointer Windows composites in; also drawing
                 * the image received on `:base+20` at OUR estimated position gave
                 * two cursors drifting apart. `=1` restores the sprite. */
                g_sprite = e ? atoi(e) : -2;
            }
            const int sprite_on = (g_sprite >= 0) ? g_sprite
                                                  : (cursor_src == 1 ? 1 : 0);
            cursor_image_t ci;
            if (sprite_on && cursor_state_get_image(&ci)
                && ci.format == 2 /* BGRA 32 bits */
                && ci.width > 0 && ci.height > 0) {

                /* nanovg wants RGBA; the wire carries BGRA. The conversion is
                 * redone only when the image changes - a cursor arrives about
                 * every five seconds, but we draw at 60 Hz. */
                static int          tex = 0;
                static uint32_t     tex_w = 0, tex_h = 0;
                static size_t       tex_fingerprint = 0;
                std::vector<uint8_t> rgba(ci.width * ci.height * 4);
                for (uint32_t y = 0; y < ci.height; y++) {
                    const uint8_t *src = ci.pixels + (size_t)y * ci.stride;
                    uint8_t *dst = rgba.data() + (size_t)y * ci.width * 4;
                    for (uint32_t x = 0; x < ci.width; x++) {
                        dst[x*4+0] = src[x*4+2];   /* R <- B */
                        dst[x*4+1] = src[x*4+1];   /* G */
                        dst[x*4+2] = src[x*4+0];   /* B <- R */
                        dst[x*4+3] = src[x*4+3];   /* alpha */
                    }
                }
                size_t fingerprint = ci.size;
                for (size_t i = 0; i < rgba.size(); i += 97) fingerprint += rgba[i] * (i + 1);
                if (tex == 0 || tex_w != ci.width || tex_h != ci.height) {
                    if (tex != 0) nvgDeleteImage(vg, tex);
                    tex = nvgCreateImageRGBA(vg, (int)ci.width, (int)ci.height,
                                             0, rgba.data());
                    tex_w = ci.width; tex_h = ci.height; tex_fingerprint = fingerprint;
                } else if (fingerprint != tex_fingerprint) {
                    nvgUpdateImage(vg, tex, rgba.data());
                    tex_fingerprint = fingerprint;
                }

                if (tex != 0) {
                    /* Constant size on screen, like the arrow: without this the
                     * pointer would grow with the window. */
                    float ck = 1.0f;
#if !defined(__SWITCH__)
                    float ws_c = brls::Application::windowScale;
                    if (ws_c > 0.0f) ck = 1.0f / ws_c;
#endif
                    const float w = ci.width * ck, h = ci.height * ck;
                    const float px = cx - ci.hot_x * ck;   /* hotspot onto the position */
                    const float py = cy - ci.hot_y * ck;
                    NVGpaint p = nvgImagePattern(vg, px, py, w, h, 0.0f, tex, 1.0f);
                    nvgBeginPath(vg);
                    nvgRect(vg, px, py, w, h);
                    nvgFillPaint(vg, p);
                    nvgFill(vg);
                    sprite_drawn = true;
                }
            }
        }

        // Small arrow: a white triangle outlined in black, hotspot at the
        // top-left corner (= the clickable position). Fixed size in screen pixels
        // so it stays legible.
        if (cursor_arrow && !sprite_drawn) {
        nvgSave(vg);
        /* Same scale correction as the panel: without it the arrow is drawn in
         * logical units and grows with the window. We bring it back to a constant
         * on-screen size, around its hotspot. */
        {
            float ck = 1.0f;
#if !defined(__SWITCH__)
            float ws_c = brls::Application::windowScale;
            if (ws_c > 0.0f) ck = 1.0f / ws_c;
#endif
            if (ck != 1.0f) {
                nvgTranslate(vg, cx, cy);
                nvgScale(vg, ck, ck);
                nvgTranslate(vg, -cx, -cy);
            }
        }
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx,        cy);
        nvgLineTo(vg, cx,        cy + 22);
        nvgLineTo(vg, cx + 6.5f, cy + 17);
        nvgLineTo(vg, cx + 11,   cy + 25);
        nvgLineTo(vg, cx + 14,   cy + 23);
        nvgLineTo(vg, cx + 9,    cy + 15);
        nvgLineTo(vg, cx + 16,   cy + 14);
        nvgClosePath(vg);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgFill(vg);
        nvgStrokeColor(vg, nvgRGB(0, 0, 0));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
        nvgRestore(vg);
        }

        /* CUR1 phase 2 2026-05-18 - a second marker = the position the Shadow
         * VM sends back on the cursor channel. It shows the sync/desync between
         * what we send (the white crosshair above) and what Shadow understands
         * (the cyan circle below). Shown only once at least one cursor update
         * has arrived. */
        /* This marker served to compare what we send with what the VM
         * understands, while the cursor channel was being decoded. The channel
         * works now, and all that is left is a blue circle in the middle of the
         * picture, meaningless to someone looking at their desktop: hidden by
         * default. SHADOW_CURSOR_DEBUG=1 turns it back on, as does the dev
         * bar. */
        if (g_cursor_debug < 0) {
            const char *e = getenv("SHADOW_CURSOR_DEBUG");
            g_cursor_debug = e ? atoi(e) : 0;
        }
        int sh_x = 0, sh_y = 0; bool sh_has = false;
        cursor_state_get_position(&sh_x, &sh_y, &sh_has);
        if (sh_has && g_cursor_debug) {
            float sh_cx = cdx + sh_x * scale_x;
            float sh_cy = cdy + sh_y * scale_y;
            nvgBeginPath(vg);
            nvgCircle(vg, sh_cx, sh_cy, 8.0f);
            nvgStrokeColor(vg, nvgRGBA(0, 200, 255, 220));
            nvgStrokeWidth(vg, 2.0f);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgCircle(vg, sh_cx, sh_cy, 2.5f);
            nvgFillColor(vg, nvgRGBA(0, 200, 255, 220));
            nvgFill(vg);
        }
    }

    /* === VI1 2026-09-11 - THE FROZEN-PICTURE BANNER, ABOVE THE PICTURE ===
     * Shown whatever show_perf_stats says: the user MUST know the stream is
     * dead, so as not to wait indefinitely. It never showed, for two reasons at
     * once - nothing had written rtp_video_stuck_secs since S112, and it was
     * drawn at the top of draw(), BEFORE the video, whose quad and image then
     * covered it. So during a video outage (69 in 1523 s on console Wi-Fi, one
     * of 143 s) the user saw a still picture and nothing else. Now written by
     * the session loop and drawn here: after the video and the cursor, below
     * the HUD and the pause menu. Only once this view has shown a picture, so a
     * value left from the previous session cannot draw on the next one's
     * waiting screen. The message no longer promises a reconnection at 30 s:
     * the native path has none. */
    {
        session_stats_t ws_freeze = {0};
        session_stats_get(&ws_freeze);
        if (consumer_slot.w > 0 && ws_freeze.rtp_video_stuck_secs >= 5) {
            const std::string banner_s =
                ui::tr("stream/frozen", ws_freeze.rtp_video_stuck_secs);
            float bw = 600.f, bh = 50.f;
            float bx = x + (width - bw) / 2;
            float by = y + 20;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx, by, bw, bh, 8.0f);
            nvgFillColor(vg, nvgRGBA(220, 140, 40, 220));   /* orange */
            nvgFill(vg);
            nvgFontSize(vg, ui::type::SECTION);
            nvgFontFace(vg, "regular");
            nvgFillColor(vg, nvgRGB(255, 255, 255));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, bx + bw / 2, by + bh / 2, banner_s.c_str(), nullptr);
        }
    }

    /* the bar is drawn LAST, so it stays above the video */
    devui::draw(vg, x, y - bar_h, width);


    prof.cp(3);   /* LAT-V2: cursor + banners */
    // === Metrics panel ===
    // The sections' contents are declared once in setupHud(); here we only feed
    // the rolling averages and ask for the render.
    /* === D1 2026-08-21 — DEVELOPMENT MENU BAR ===
     * On PC the client was driven by shortcuts inherited from the Switch (HOLD
     * F2 for the menu): hard to discover, and it cost time on every test. Here
     * we lend the bar what it needs to read and change the view's state;
     * everything else (drawing, hover, checkboxes, menu contents) lives in
     * clients/borealis/devui/ and does not compile for the Switch. */
    {
        /* Reinstalled for EVERY view.
         *
         * This block used to be guarded by a static boolean: the bar therefore
         * kept the callbacks of the FIRST stream view. On the second connection
         * the first view was destroyed but the bar kept calling it -
         * `MenuBar::layout()` measures every entry on every frame, including the
         * information rows, which query the view. Hence a crash on reconnection,
         * never on the first connection. */
        if (!devui_installed_) {
            devui_installed_ = true;
            StreamView *self = this;
            devui::Host host;
            host.metricsEnabled = [self] { return self->perfStatsEnabled(); };
            host.toggleMetrics  = [self] { self->menuToggleStats(); };
            host.stretchEnabled = [] { return g_stretch != 0; };
            /* Writes the SETTING: `g_stretch` is now recomputed on every
             * frame, so flipping it alone would last exactly one frame. */
            host.toggleStretch  = [] {
                Settings &c = Settings::instance();
                c.stretch_image = !c.stretch_image;
                c.save();
            };
            host.openShadowMenu = [self] { if (!self->isMenuOpen()) self->openMenu(); };
            host.toggleKeyboard = [self] { self->toggleKeyboard(); };
            host.videoFormat    = [self] { return self->videoFormatText(); };
            host.videoRate      = [self] { return self->videoRateText(); };
            host.cursorArrow       = [] { return g_cursor_arrow_eff > 0; };
            /* We force the inverse of what is DISPLAYED, not of what is stored:
             * the override stays -1 until someone touches the toggle. */
            host.toggleCursorArrow = [] { g_cursor_arrow = g_cursor_arrow_eff > 0 ? 0 : 1; };
            host.cursorDebug       = [] { return g_cursor_debug > 0; };
            host.toggleCursorDebug = [] { g_cursor_debug = (g_cursor_debug > 0) ? 0 : 1; };
            host.hud              = &self->hud();
            host.toggleHudSection = [self](size_t i) { self->toggleHudSection(i); };
            host.toggleHudChart   = [self](size_t i) { self->toggleHudChart(i); };
            devui::install(host, this);
        }
    }

    /* S94 - placement mode forces the display on. Without this, entering
     * placement with the panel off gives a screen where nothing moves and from
     * which you cannot find your way out: an invisible mode is a broken mode. */
    /* === S117 2026-09-03 - BITRATE IS MEASURED EVEN WITH THE PANEL CLOSED ===
     *
     * The seven rate counters lived INSIDE the guard below, so the client
     * stopped measuring its own bitrate the moment the panel was closed. This is
     * the fourth time this repo has paid for the same mistake: `[L5]` fixed it
     * for latency (see the comment in stream_view.hpp), `buf_drop_count` existed
     * in no public structure, `stat_latency` was fed only with the panel open -
     * and here is the bitrate.
     *
     * It is not theoretical: a link-quality tester, or simply the question "what
     * bitrate am I actually receiving", cannot be answered from a number that
     * only exists while someone is looking at it. Sampling costs two
     * subtractions per counter per picture.
     *
     * What STAYS inside the guard: the drawing, the geometry, and the
     * measurements that only mean anything to the drawing (the render-rate
     * spread). */
    {
        prof.cp(4);   /* LAT-V2: metrics entry (devui) */
        auto now_tp = std::chrono::steady_clock::now();
        prof.cp(12);  /* LAT-V2: steady_clock::now() alone */
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             now_tp.time_since_epoch()).count();
        session_stats_t ws;
        session_stats_get(&ws);
        prof.cp(10);  /* LAT-V2: session_stats_get */
        rate_received_.sample(frames_received, now_us);
        rate_decoded_.sample(ws.h264_frames_decoded, now_us);
        rate_presented_.sample(frames_presented_, now_us);
        rate_bytes_.sample(ws.rtp_video_bytes, now_us);
        rate_packets_.sample(ws.rtp_video_packets, now_us);
        rate_audio_.sample(ws.opus_decoded, now_us);
        rate_audio_bytes_.sample(ws.rtp_audio_bytes, now_us);
        /* AUD-INS-1 2026-09-11 - the loss row's 10 s window is fed HERE, panel
         * open or closed (S117): a window fed only while the panel draws would
         * open empty every time. Accepted frames are rtp_audio_packets: it
         * travels in the same NET merge as opus_lost, so the pair is one
         * snapshot whatever SHADOW_AUDIO_STATS_TICK says. */
        audio_loss_win_sample(&aud_loss_win_, now_us / 1000, ws.rtp_audio_packets, ws.opus_lost);
        hud_audio_kbps_ = rate_audio_bytes_.value() * 8.0f / 1000.0f;
        hud_live_kbps_  = rate_bytes_.value() * 8.0f / 1000.0f;
        hud_pps_        = rate_packets_.value();
        prof.cp(11);  /* LAT-V2: rate meters + audio loss window */

        /* S119: the path's capacity accumulates WHILE playing, with the page
         * closed. An estimate that only built itself when the page was opened
         * would never gather enough samples to conclude - and that would be the
         * fifth time this repo measured only what it was looking at. */
        ui::nettest::sample(now_us / 1000000.0);
        prof.cp(6);   /* LAT-V2: nettest::sample */
    }

    if ((show_perf_stats || hud_.editing()) && hud_.visible()) {
        auto now_tp = std::chrono::steady_clock::now();
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             now_tp.time_since_epoch()).count();

        if (last_draw_us > 0) {
            float interval_ms = (now_us - last_draw_us) / 1000.0f;
            if (interval_ms > 0.1f && interval_ms < 1000.0f) {
                stat_interval.push(interval_ms);
                stat_fps.push(1000.0f / interval_ms);
            }
        }
        last_draw_us = now_us;
        /* VI5 2026-09-11 - `stat_latency` is no longer fed here, once per draw
         * with the age of the picture on screen: the L5 block feeds it once
         * per new picture (see there). */

        /* Rates and bitrate, all measured the same way: from the counters'
         * real variations (see ui::RateMeter). */
        session_stats_t ws;
        session_stats_get(&ws);   /* the rates are already sampled above */

        /* S93 - the area will be used to turn a finger into a block position.
         * Keeping it here rather than recomputing it on the touch side avoids
         * two truths for one rectangle. */
        hud_zone_[0] = x; hud_zone_[1] = y;
        hud_zone_[2] = width; hud_zone_[3] = height;
        hud_.draw(vg, x, y, width, height);
    }

    prof.cp(13);  /* LAT-V2: HUD block (perf overlay) */
    // === Touch input -> mouse driver ===
    // Active only when the overlay keyboard and the menu are closed. Modes:
    //   Off       : ignored (touch only serves the overlay keyboard)
    //   Relative  : touch delta = mouse delta (trackpad style)
    //   Absolute  : touch position -> absolute mouse_move
    //
    // Gestures (touchpad convention):
    //   - 1 finger, short tap (< 350 ms, < 40 px) -> left click
    //   - 1 finger, two taps close together       -> double click (the 2nd click
    //     is REALIGNED onto the 1st one's position, otherwise Windows separates
    //     them)
    //   - 2 fingers, vertical slide               -> wheel
    //   - 2 fingers, short tap with no slide      -> right click
    //   - Drag/long hold -> only the mouse_move follows the finger, no held
    //     button DOWN (= drag-select unsupported until the separate
    //     mousedown/mouseup format has been reverse-engineered). A sliding
    //     finger simply moves the cursor, with no selection.
#include "switch_compat.h"
/* Was `#ifdef __SWITCH__` until 2026-09-13, which is how the Vita ended up
 * with no touchscreen and no pause-menu navigation during a stream: the
 * whole block was compiled out. The logic inside is neutral - only six
 * libnx calls were not, and `include/pad_compat.h` supplies them. */
#if SHADOW_HAS_STREAM_INPUT
    /* === Touch INSIDE the pause menu (2026-08-25) ===
     * The menu is drawn in immediate mode with nanovg: it is not a Borealis
     * view, so Borealis's touch system does not see it. It could therefore only
     * be navigated with buttons. Here we route the presses to it, bringing the
     * raw point back into view space exactly as the library does (division by
     * `windowScale`, see switch_input.cpp).
     *
     * We act on RELEASE and not on press: that leaves room to put a finger down
     * then slide off a row to cancel, and it avoids firing twice if the finger
     * trembles. */
    /* === K10 2026-08-27 — THE MENU COMES BEFORE THE KEYBOARD ===
     * The guard used to be `isMenuOpen() && !kbd_open`: opening the menu while
     * the keyboard was already up made the menu UNUSABLE by touch, since its
     * touch handling stayed off. And the keyboard kept testing its keys - it was
     * drawn UNDERNEATH but still caught the presses. Two surfaces fought over
     * the same finger, and the wrong one won.
     * The menu now has priority on both sides: it receives the touch, and the
     * keyboard stops being drawn and tested while the menu is open.
     * `kbd_open` is KEPT: the keyboard comes back as it was when the menu
     * closes, which is what you expect when you go and change a setting in the
     * middle of typing. */
    /* === S97 2026-08-29 — THE STICK NAVIGATES, AND A HELD PRESS REPEATS ===
     *
     * The pause menu was driven by Borealis actions: D-PAD ONLY, and one event
     * per press. Two gaps reported together - the left stick did nothing, and
     * you had to tap the direction row by row, whereas the settings screens do
     * scroll when you hold.
     *
     * So we read the state on every frame and apply the same rule as everywhere
     * else: one immediate step, then repetition after a guard delay. The delay
     * exists so the first press stays PRECISE - without it, aiming at a row
     * becomes impossible.
     *
     * The stick is treated as a definite direction beyond a threshold: a stick
     * at rest drifts by a few units, and with no threshold the menu would run
     * away on its own. */
    if (isMenuOpen()) {
        PadState nav_pad;
        padConfigureInput(8, HidNpadStyleSet_NpadStandard);
        padInitializeAny(&nav_pad);
        padUpdate(&nav_pad);
        /* INJ-1: the stream reads libnx DIRECTLY (K22 says so above), so the
         * hook placed inside Borealis never reaches it. Second reader, same
         * synthetic input - without this, every menu would move while the
         * stream stayed inert, which reads as "injection does not work" rather
         * than "it works in one of the two places the app reads input". */
        const uint64_t nb = padGetButtons(&nav_pad) | devlink::injectedNpadMask();
        HidAnalogStickState st = padGetStickPos(&nav_pad, 0);
        { int ix = 0, iy = 0;
          if (devlink::injectedStick(false, &ix, &iy)) { st.x = ix; st.y = iy; } }
        const int32_t THRESHOLD = 16000;   /* ~half the travel */

        int dx = 0, dy = 0;
        if ((nb & HidNpadButton_Up)    || st.y >  THRESHOLD) dy = -1;
        if ((nb & HidNpadButton_Down)  || st.y < -THRESHOLD) dy = +1;
        if ((nb & HidNpadButton_Left)  || st.x < -THRESHOLD) dx = -1;
        if ((nb & HidNpadButton_Right) || st.x >  THRESHOLD) dx = +1;

        const int64_t now_ms = (int64_t)(armTicksToNs(armGetSystemTick()) / 1000000ULL);
        if (dx == 0 && dy == 0) {
            nav_dir_x_ = nav_dir_y_ = 0;
            nav_next_ms_ = 0;
        } else if (dx != nav_dir_x_ || dy != nav_dir_y_) {
            nav_dir_x_ = dx; nav_dir_y_ = dy;
            nav_next_ms_ = now_ms + 380;   /* guard before repeating */
            if (dy < 0) menuUp(); else if (dy > 0) menuDown();
            if (dx < 0) menuLeft(); else if (dx > 0) menuRight();
        } else if (now_ms >= nav_next_ms_) {
            nav_next_ms_ = now_ms + 90;    /* repeat rate */
            if (dy < 0) menuUp(); else if (dy > 0) menuDown();
            if (dx < 0) menuLeft(); else if (dx > 0) menuRight();
        }

        HidTouchScreenState mts = {0};
        const int mn = (hidGetTouchScreenStates(&mts, 1) > 0) ? (int)mts.count : 0;
        /* No `/ windowScale` here any more. This was the only one of the four
         * touch call sites in this file that divided; the other three compare
         * the raw value against view rectangles, i.e. logical coordinates. On
         * the Switch the two conventions are indistinguishable - its panel only
         * exists in handheld, where windowScale is exactly 1 - so the division
         * was a no-op that read like a rule. On a console whose scale is not 1
         * it put the menu selection somewhere else than the finger. */
        if (mn == 1) {
            menu_touch_x_ = (float)mts.touches[0].x;
            menu_touch_y_ = (float)mts.touches[0].y;
            menu_touch_down_ = true;
            /* === S97 2026-08-29 - THE POSITION TEST HAPPENS ONLY ONCE ===
             *
             * S96 set the selection on press then RE-TESTED it on release. But
             * setting the selection can SCROLL the menu: the rows move between
             * the two, and the second test landed on the neighbouring row - "it
             * opens the menu underneath".
             *
             * One single measurement, on press. The release merely activates
             * what was selected; it re-discovers nothing. */
            if (!menu_touch_hit_)
                menu_touch_hit_ = pause_menu_.selectAt(menu_touch_x_, menu_touch_y_);
        } else if (mn == 0 && menu_touch_down_) {
            menu_touch_down_ = false;
            if (menu_touch_hit_) pause_menu_.activate();
            menu_touch_hit_ = false;
        }
        /* The menu consumes the touch: we do not forward it to the remote
         * desktop while it is open. */
    } else {
        menu_touch_down_ = false;
        menu_touch_hit_  = false;
    }

    /* === K8 2026-08-27 — THE STREAM STAYS DRIVABLE WITH THE KEYBOARD OPEN ===
     * The guard was `!kbd_open`: opening the keyboard cut ALL touch, including
     * on the top half of the screen where the stream stays perfectly visible.
     * You therefore could not click into the field you wanted to type in without
     * closing the keyboard and reopening it. Now only the KEYBOARD BAND is
     * reserved; above it, the finger drives the mouse. */
    /* === S94 2026-08-29 — PLACEMENT IS HANDLED FIRST, AND SEPARATELY ===
     *
     * This interception used to live INSIDE the touch block below and escape it
     * with a `return`. Two consequences, both found in use: the buttons read
     * further down were never reached - hence a held Start that did not leave
     * the mode - and placement did not work at all when the stream's touch mode
     * was set to "off", since this block's guard excluded it.
     *
     * So it lives here: before, and outside any guard that does not concern it.
     * The only legitimate blocker is the menu being open. */
    if (hud_.editing() && !isMenuOpen()) {
        HidTouchScreenState ets = {0};
        const int en = (hidGetTouchScreenStates(&ets, 1) > 0) ? (int)ets.count : 0;
        const float etx = (en > 0) ? (float)ets.touches[0].x : 0.0f;
        const float ety = (en > 0) ? (float)ets.touches[0].y : 0.0f;
        const int phase = (en > 0) ? ((edit_phase_ == 2) ? 0 : 1) : 2;
        if (en > 0 || edit_phase_ != 2)
            hud_.touch(etx, ety, phase,
                         hud_zone_[0], hud_zone_[1], hud_zone_[2], hud_zone_[3]);
        Settings &cfg = Settings::instance();
        bool to_save = false;
        if (phase == 2 && edit_phase_ != 2) {
            /* On RELEASE only: writing the settings file at the finger's rate
             * would wear the card for nothing. */
            cfg.hud_positions = hud_.positions();
            to_save = true;
        }
        if (hud_.takeMaskChanges()) {
            cfg.hud_sections = hud_.enabledMask();
            cfg.hud_charts   = hud_.chartMask();
            to_save = true;
        }
        if (to_save) cfg.save();
        edit_phase_ = phase;
    }

    if (!isMenuOpen() && !hud_.editing()
            && Settings::instance().touch_mode != TouchMode::Off) {
        HidTouchScreenState ts = {0};
        int n = (hidGetTouchScreenStates(&ts, 1) > 0) ? (int)ts.count : 0;

        /* A finger PLACED ON THE KEYBOARD does not belong to the stream. We
         * REMOVE it from the count rather than ignore it: otherwise the gesture
         * recogniser would see a finger stay in the air indefinitely, and the
         * next gesture would start from a false state. With the count falling to
         * zero, it sees a clean release.
         *
         * The filter applies to EVERY finger: resting a thumb on the keyboard
         * and sliding the index above it must keep moving the mouse. */
        const int n_raw = n;
        if (kbd_open) {
            const float limit = kbdTop(y, height);
            unsigned kept = 0;
            for (unsigned i = 0; i < (unsigned)n && i < 16; i++)
                if ((float)ts.touches[i].y < limit)
                    ts.touches[kept++] = ts.touches[i];
            n = (int)kept;
        }

        /* === K11 2026-08-27 — CROSSING THE BAND IS NOT A RELEASE ===
         *
         * The filter above made the count fall to zero while the finger was
         * STILL down. My own comment congratulated itself on it - "with the count
         * falling to zero, it sees a clean release" - without seeing that this is
         * exactly the defect: a clean release IS a tap. So a LEFT CLICK was sent
         * to the VM as soon as a finger slid towards the keyboard, and another on
         * the way back. Intermittent on top of that: a slow crossing (more than
         * 350 ms) did not click.
         *
         * So we freeze the recogniser until the REAL lift, the one where the RAW
         * count falls to zero. The freeze also closes three defects of the same
         * family: a two-finger gesture straddling the boundary that triggered the
         * one-finger action, a finger vibrating on the border that locked
         * `max_fingers` at 2 (frozen pointer then right click), and a fresh
         * gesture restarting at the crossing point. */
        if (n_raw > n && !touch_kbd_frozen_) {
            gestures_cancel(&gestures_);
            touch_kbd_frozen_ = true;
        }
        if (touch_kbd_frozen_) {
            if (n_raw == 0) touch_kbd_frozen_ = false;
            else             n = 0;
        }

        int tx = (n > 0) ? (int)ts.touches[0].x : 0;
        int ty_t = (n > 0) ? (int)ts.touches[0].y : 0;
        /* Y of the CENTRE of the two fingers, not of the first: on a slight
         * pinch, finger 0 can rise while the centre falls, and the scroll would
         * go the wrong way. */
        const int cy = (n >= 2)
                     ? (((int)ts.touches[0].y + (int)ts.touches[1].y) / 2) : ty_t;

        const uint64_t now_us = (uint64_t)armTicksToNs(armGetSystemTick()) / 1000ULL;

        Settings &cfg = Settings::instance();

        /* The configuration is RE-READ on every frame: changing a mapping in
         * the settings must take effect without restarting the session. */
        gestures_config gc;
        gestures_defaults(&gc);
        for (int d = 1; d <= 3; d++)
            gc.double_distinct[d] =
                (cfg.gesture_action[GESTURE_DOUBLE(d)] != (uint8_t)GestureAction::None);
        if (!gestures_ready_) { gestures_init(&gestures_, &gc); gestures_ready_ = true; }
        else                { gestures_.cfg = gc; }

        gesture_evt ev[4];
        int detents = 0;
        const int nev = gestures_update(&gestures_, n, tx, ty_t, cy, now_us, ev, 4, &detents);

        const TouchMode mode = cfg.touch_mode;
        const bool has_session = shadow_input_session_active();

        /* --- Pointer movement: ONE finger alone --- */
        if (n == 1 && gestures_.max_fingers == 1) {
            const int dx = (touch_prev_count == 1) ? (tx - touch_prev_x) : 0;
            const int dy = (touch_prev_count == 1) ? (ty_t - touch_prev_y) : 0;
            if (has_session) {
                if (mode == TouchMode::Relative && (dx || dy)) {
                    /* Sensitivity: a finger travelling 100 px used to send
                     * 100, on top of which the game applies its own - hence
                     * impossible aiming. The REMAINDER of the division is carried
                     * from one frame to the next: without it, at 50 %, any
                     * movement of less than two pixels would be lost, and that is
                     * precisely the movement of fine aiming. */
                    const int sens_pct = (int)cfg.touch_sensitivity;
                    static int rem_x = 0, rem_y = 0;
                    const int nx = dx * sens_pct + rem_x;
                    const int ny = dy * sens_pct + rem_y;
                    const int sx = nx / 100, sy = ny / 100;
                    rem_x = nx - sx * 100;
                    rem_y = ny - sy * 100;
                    if (sx || sy) shadow_input_post_mouse_move(sx, sy);
                } else if (mode == TouchMode::Absolute) {
                    shadow_input_post_mouse_move_abs(tx, ty_t);
                }
            }
        }

        /* --- Wheel --- */
        if (detents != 0 && has_session) {
            int dir = (detents > 0) ? 1 : -1;
            if (cfg.touch_scroll_invert) dir = -dir;
            const int nb = (detents > 0) ? detents : -detents;
            for (int i = 0; i < nb; i++) shadow_input_post_mouse_wheel(dir);
        }

        /* --- Actions --- */
        for (int i = 0; i < nev; i++) executeGesture(ev[i], has_session);

        /* --- End of a drag --- */
        if (n == 0 && touch_drag_active) {
            /* The button release is sent EVEN with no session: otherwise a
             * button stays held on the VM's side and everything becomes a
             * selection - defect C2, already paid for once. */
            shadow_input_post_mouse_button(0, false);
            touch_drag_active = false;
        }

        touch_prev_count = n;
        touch_prev_x = tx;
        touch_prev_y = ty_t;
    }
#endif  /* SHADOW_HAS_STREAM_INPUT */

#if SHADOW_HAVE_DESKTOP_GL   /* GLFW: desktop only - see gl_compat.h */
    /* === DEMO-1 2026-09-26 - A PAUSE MENU A SCRIPT CAN REACH ===
     *
     * On desktop only the developer menu bar opens the pause menu (the F2 long
     * press exists on console only, and the keyboard belongs to the VM while
     * streaming - K20), and devlink injects nothing into this view off console.
     * The demo mode hides that bar, so a scripted capture had no way in:
     * SHADOW_DEMO_MENU_AT_S=N opens the menu once, N seconds into a demo
     * stream. Demo mode only - a real session never opens a menu by itself. */
    if (!demo_menu_done_ && demo::enabled()) {
        const char *at = getenv("SHADOW_DEMO_MENU_AT_S");
        const double now_s = glfwGetTime();
        if (!at || atof(at) <= 0) {
            demo_menu_done_ = true;
        } else if (demo_menu_t0_ < 0) {
            demo_menu_t0_ = now_s;
        } else if (now_s - demo_menu_t0_ >= atof(at)) {
            demo_menu_done_ = true;
            if (!isMenuOpen()) openMenu();
        }
    }
    /* === K22 2026-09-10 - THE PAUSE MENU WAS UNUSABLE ON DESKTOP ===
     *
     * Reported in use: the arrow keys did not move in the menu, and neither did
     * clicks. Not a regression - an ABSENCE: the menu's navigation (S97) and
     * its touch selection live in the `#ifdef __SWITCH__` block above, and read
     * `padGetButtons`, `padGetStickPos` and `hidGetTouchScreenStates` - three
     * libnx APIs that do not exist here. On PC the menu opened and stayed
     * inert.
     *
     * What hid the defect: Borealis' BUTTON_UP/DOWN actions are registered in
     * `stream_activity`, and they CONSUME the key - without acting, because S97
     * moved the real movement into `draw()` to handle the stick and the
     * repeat. The keyboard therefore looked taken into account, and did
     * nothing. That consumption is still what we want: it keeps a press from
     * counting twice now that we act here.
     *
     * Same constants as S97 - 380 ms of guard, then 90 ms repeats - so the menu
     * behaves the same on both platforms; the first press must stay PRECISE,
     * or aiming at a line becomes impossible.
     *
     * The mouse plays the finger, with the S97 rule that matters most: the
     * position is measured ONLY ON PRESS. Selecting can scroll the menu;
     * re-testing on release would land on the neighbouring line, which is
     * exactly the "it opens the menu below" defect S97 fixed on console. */
    if (isMenuOpen()) {
        GLFWwindow *mw = glfwGetCurrentContext();
        if (mw) {
            int dx = 0, dy = 0;
            if (glfwGetKey(mw, GLFW_KEY_UP)    == GLFW_PRESS) dy = -1;
            if (glfwGetKey(mw, GLFW_KEY_DOWN)  == GLFW_PRESS) dy = +1;
            if (glfwGetKey(mw, GLFW_KEY_LEFT)  == GLFW_PRESS) dx = -1;
            if (glfwGetKey(mw, GLFW_KEY_RIGHT) == GLFW_PRESS) dx = +1;

            const int64_t now_ms = (int64_t)(glfwGetTime() * 1000.0);
            if (dx == 0 && dy == 0) {
                nav_dir_x_ = nav_dir_y_ = 0;
                nav_next_ms_ = 0;
            } else if (dx != nav_dir_x_ || dy != nav_dir_y_) {
                nav_dir_x_ = dx; nav_dir_y_ = dy;
                nav_next_ms_ = now_ms + 380;
                if (dy < 0) menuUp(); else if (dy > 0) menuDown();
                if (dx < 0) menuLeft(); else if (dx > 0) menuRight();
            } else if (now_ms >= nav_next_ms_) {
                nav_next_ms_ = now_ms + 90;
                if (dy < 0) menuUp(); else if (dy > 0) menuDown();
                if (dx < 0) menuLeft(); else if (dx > 0) menuRight();
            }

            double mx = 0, my = 0;
            glfwGetCursorPos(mw, &mx, &my);
            const float sc = (brls::Application::windowScale > 0.0f)
                           ? brls::Application::windowScale : 1.0f;
            const bool down =
                glfwGetMouseButton(mw, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (down) {
                menu_touch_x_ = (float)(mx / sc);
                menu_touch_y_ = (float)(my / sc);
                menu_touch_down_ = true;
                if (!menu_touch_hit_)
                    menu_touch_hit_ = pause_menu_.selectAt(menu_touch_x_, menu_touch_y_);
            } else if (menu_touch_down_) {
                menu_touch_down_ = false;
                if (menu_touch_hit_) pause_menu_.activate();
                menu_touch_hit_ = false;
            }
        }
    } else {
        menu_touch_down_ = false;
        menu_touch_hit_  = false;
    }
#endif

    // === Plus button (Start): short press -> DS4 Options; long press -> menu ===
#include "switch_compat.h"
#if SHADOW_HAS_STREAM_INPUT
    /* === S76 2026-08-28 — THE PAUSE MENU CUTS GAMEPAD FORWARDING ===
     *
     * Touch was already guarded by `isMenuOpen()` (K8/K10), but NOT this block:
     * `padforward::poll()` and the Plus/Minus buttons kept forwarding while you
     * navigated the menu. Every press therefore acted TWICE - in the menu and in
     * the game behind it.
     *
     * We first release what is held ON THE VM'S SIDE (`relacherTout()`),
     * otherwise the button held at the moment of opening would stay held in the
     * game for as long as you read the menu. `reset()` alone would not do: it
     * zeroes the LOCAL state without emitting anything.
     *
     * On closing, `g_primed` having fallen back to false, the first `poll()`
     * pass re-emits the full state: nothing to rebuild.
     *
     * We cut the FORWARDING, not the READING: edge tracking for Plus and Minus
     * keeps running, otherwise it would stay frozen on the state at the moment
     * the menu opened, and the first press after closing would be
     * misinterpreted. */
    const bool s_menu = isMenuOpen();
    /* === S96 2026-08-29 — PLACEMENT SILENCES THE GAMEPAD, LIKE THE MENU ===
     *
     * Placement mode let the gamepad through to the remote machine: holding
     * Start to leave it therefore ALSO sent a 500 ms Start into the game -
     * pause, menu, or worse depending on the title. The pause menu already had
     * this treatment (S76); the mode added afterwards did not, because the
     * condition was written `s_menu` everywhere instead of naming what it meant:
     * "the UI has the input".
     *
     * Hence this name, used everywhere `s_menu` was guarding the INPUT PATH. The
     * other uses of `s_menu` - the ones that really are about the menu - are
     * left as they are. */
    const bool ui_has_input = s_menu || hud_.editing();
    {
        static bool ui_had_input = false;
        if (ui_has_input && !ui_had_input) {
            /* Release BEFORE going silent: without this the VM would keep the
             * button held at the moment of entry pressed, and the game would see
             * it held for the whole time spent in the menu or placing blocks
             * (S76). */
            padforward::releaseAll();
            rumble_state_stop();       /* G57: and the motors along with the buttons */
        }
        ui_had_input = ui_has_input;
    }
    {
        static PadState plus_pad;
        static bool plus_pad_init = false;
        if (!plus_pad_init) {
            padConfigureInput(8, HidNpadStyleSet_NpadStandard);
            padInitializeAny(&plus_pad);
            plus_pad_init = true;
        }
        padUpdate(&plus_pad);
        const uint64_t plus_held = padGetButtons(&plus_pad) | devlink::injectedNpadMask();
        bool pressed = (plus_held & HidNpadButton_Plus) != 0;   /* INJ-1 */
        uint64_t now_us = (uint64_t)armTicksToNs(armGetSystemTick()) / 1000ULL;
        const uint64_t LONG_HOLD_US = 500'000;  /* 500 ms hold = open the menu */

        /* === S75 2026-08-27 — START MUST FOLLOW THE PRESS, LIKE SELECT ===
         *
         * It went out as an 80 ms PULSE on release: the game therefore always
         * saw the same duration, whatever yours was. Select goes through
         * continuous forwarding and respects the press - hence the difference
         * visible in the tester.
         *
         * We now send the PRESS on the falling edge and the RELEASE on the
         * rising edge, exactly like the other buttons. One constraint remains,
         * and it is inherent: "+" also opens the menu. A LONG press therefore
         * releases the button at the moment the menu opens, so as not to leave it
         * stuck down in the game while you navigate. The game then sees a 500 ms
         * Start - that is the price of the double role, and the only one we can
         * pay. */
        const int plus_target = padmap::target(padmap::Btn::Plus);
        const bool plus_forwardable = (plus_target >= 0 && plus_target <= SHADOW_PAD_GUIDE)
                                        && !ui_has_input;   /* S76, S96 */

        if (pressed && !plus_was_pressed) {
            plus_pressed_us = now_us;
            plus_long_consumed = false;
            if (plus_forwardable) ctrl_gamepad_button(plus_target, true);
        } else if (pressed && plus_was_pressed && !plus_long_consumed
                    && (now_us - plus_pressed_us) >= LONG_HOLD_US) {
            /* === S94 2026-08-29 — IN EDIT MODE, A LONG START CONFIRMS ===
             * Edit mode takes all the touch input: there is therefore no way out
             * by finger, and reopening the menu to untick a toggle would be
             * absurd since the menu is what you closed to get in. We reuse the
             * gesture that already has that role - a held Start - which avoids
             * inventing one nobody would guess.
             * Tested BEFORE the menu opens: in edit mode a long Start confirms
             * and opens nothing. */
            if (hud_.editing()) {
                editPositions(false);
                plus_long_consumed = true;
                if (plus_forwardable) ctrl_gamepad_button(plus_target, false);
                plus_was_pressed = pressed;
                return;
            }
            if (!isMenuOpen()) openMenu();
            plus_long_consumed = true;
            /* The menu is opening: we RELEASE, otherwise the button would stay
             * pressed in the game while you navigate. */
            if (plus_forwardable) ctrl_gamepad_button(plus_target, false);
        } else if (!pressed && plus_was_pressed) {
            /* Release: we only send the release if the long press has not
             * already done so, otherwise we would emit twice. */
            if (!plus_long_consumed) {
                if (plus_forwardable) ctrl_gamepad_button(plus_target, false);
            }
        }
        plus_was_pressed = pressed;

        /* === K5 2026-08-27 — SELECT: SHORT = THE REAL BUTTON, LONG = KEYBOARD ===
         * "-" toggled the keyboard on press, so the Select button NEVER reached
         * the remote machine - and many games use it (map, inventory, view
         * change). We give it the same mechanics as "+", which already works
         * this way: short press = button forwarded, long press = our UI. */
        static bool     minus_prev = false;
        static uint64_t minus_start_us = 0;
        static bool     minus_long_used = false;
        const bool minus = ((padGetButtons(&plus_pad) | devlink::injectedNpadMask())
                            & HidNpadButton_Minus) != 0;   /* INJ-1 */

        if (minus && !minus_prev) {
            minus_start_us = now_us;
            minus_long_used = false;
        } else if (minus && minus_prev && !minus_long_used
                   && (now_us - minus_start_us) >= LONG_HOLD_US) {
            toggleKeyboard();
            minus_long_used = true;
        } else if (!minus && minus_prev && !minus_long_used) {
            int t = padmap::target(padmap::Btn::Minus);
            if (!ui_has_input && t >= 0 && t <= SHADOW_PAD_GUIDE)   /* S76, S96 */
                ctrl_gamepad_button_pulse(t);
        }
        minus_prev = minus;

        /* === PM1 2026-09-02 - THE JOY-CONS AS A MOUSE ===
         *
         * THE COMBINATION IS THE TWO STICK CLICKS, TOGETHER. Every other button
         * on this controller already has a job in the remote machine, and the
         * two that do not - "+" and "-" - are taken by the pause menu and the
         * keyboard, each with a short and a long press. L3+R3 is the one gesture
         * left that no game asks for by accident and that no hand performs by
         * accident either.
         *
         * Edge-triggered on the moment BOTH are down: holding them must toggle
         * once, not sixty times a second. And the two clicks are swallowed while
         * the combination fires, otherwise the game receives an L3 as the mouse
         * turns on - which on most games means crouch or sprint. */
        {
            const uint64_t both = HidNpadButton_StickL | HidNpadButton_StickR;
            const bool combo = (padGetButtons(&plus_pad) & both) == both;
            if (combo && !pad_mouse_combo_) padmouse::toggle();
            pad_mouse_combo_ = combo;
        }

        /* THE MOUSE CONSUMES THE CONTROLLER. In this mode the buttons ARE the
         * mouse: forwarding them to the VM at the same time would make the game
         * react to every click, and the sticks would aim while the pointer
         * moves. `releaseAll` on the transition is what stops a button held at
         * that instant from staying down in the remote machine for good - the
         * same reasoning as S76 for the UI. */
        if (padmouse::active()) {
            if (!pad_mouse_was_active_) padforward::releaseAll();
            /* PM6 - the test page steps the module itself, from where its
             * readings are needed. Stepping it here too would split each frame's
             * dt between two calls and halve the pointer's speed exactly while
             * you are trying to judge it. */
            if (!padmouse::testMode()) padmouse::poll(&plus_pad, now_us);
        } else if (!ui_has_input) {
            /* Forward the other buttons to the Shadow gamepad, following the
             * mapping the user configured. */
            padforward::poll();   /* S76, S96: the UI consumes the gamepad */
        }
        pad_mouse_was_active_ = padmouse::active();

        /* === G57 2026-08-28 — FORCE FEEDBACK ===
         * The VM has been sending it all along (measured: 407 messages in one
         * session, 244 with BOTH motors active) and we were throwing it away.
         *
         * The hardware is driven by `rumble_hid.c`, extracted so the settings
         * screen can exercise it WITHOUT the wire: when nothing vibrates, you
         * need to be able to tell "the protocol delivers nothing" from "the
         * console refuses".
         *
         * We only apply ON CHANGE: the server emits 33 messages per second, and
         * one IPC round trip per frame for an unchanged value would be free of
         * benefit and costly. */
        {
            static int swap = -1;
            if (swap < 0) {
                const char *e = getenv("SHADOW_RUMBLE_SWAP");
                swap = e ? atoi(e) : 0;
            }
            uint8_t rb = 0, rh = 0;
            if (rumble_state_get(&rb, &rh)) {
                if (ui_has_input) { rb = 0; rh = 0; }   /* S76, S96 */
                rumble_hid_apply(swap ? rh : rb, swap ? rb : rh);
            }
        }
    }
#endif  /* SHADOW_HAS_STREAM_INPUT */

    // === Linux desktop input → shadow-input ===
    //
    // Architecture - chained GLFW callbacks (NOT button polling):
    //   - mouse buttons + scroll + keys -> glfwSet*Callback. GLFW emits ONE
    //     event per edge (PRESS / RELEASE) -> we post ONE Shadow message per
    //     event. Clean edge detection, like the browser we reverse-engineered.
    //   - cursor position -> polled in draw() (inter-frame delta, aggregated at
    //     60 Hz)
    //
    // No cursor capture (GLFW_CURSOR_DISABLED). Too fragile under X11 (missed
    // releases -> stuck buttons). The cursor stays free, deltas are computed
    // while it is inside the window. In future we could add an optional
    // FPS-capture toggle on a key.
    //
    // The existing Borealis callbacks are chained through the pointers saved on
    // the first install (otherwise UI keyboard nav / IME / list scrolling
    // break). Installed once for the lifetime of the app.
#if SHADOW_HAVE_DESKTOP_GL   /* GLFW: desktop only - see gl_compat.h */
    static bool s_cb_installed = false;
    static GLFWmousebuttonfun s_prev_btn_cb = nullptr;
    static GLFWkeyfun         s_prev_key_cb = nullptr;
    static GLFWscrollfun      s_prev_scroll_cb = nullptr;
    static bool s_btn_held[3] = {false, false, false};
    static bool s_key_held[GLFW_KEY_LAST + 1] = {false};
    // Stream rect + VM dimensions, updated on every draw() - shared with the
    // GLFW callbacks so a synchronous cursor position can be re-posted just
    // before each button event (the click wire format does not carry X/Y, it is
    // the last known move position that counts).
    static float s_rect_x = 0, s_rect_y = 0, s_rect_w = 0, s_rect_h = 0;
    static int s_vm_w = 1920, s_vm_h = 1080;

    GLFWwindow *win = glfwGetCurrentContext();
    if (!win) goto skip_linux_input;

    if (!s_cb_installed) {
        s_cb_installed = true;
        // Force the cursor visible - an earlier call (legacy capture mode,
        // Borealis pointer lock for scrolling, or inherited X11 state) may have
        // left GLFW_CURSOR at HIDDEN/DISABLED over the stream view. We restore
        // it here once and for all, when the callbacks are installed (= the
        // first stream frame).
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        // Explicit system cursor (otherwise X11 can inherit a transparent
        // cursor from the parent / window manager).
        static GLFWcursor *s_arrow = nullptr;
        if (!s_arrow) s_arrow = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
        if (s_arrow) glfwSetCursor(win, s_arrow);
        // These callbacks ignore the stream context - they always chain into
        // Borealis. The "send to Shadow?" condition is evaluated inside, on
        // every event, against the global state (a session is up + no
        // menu/keyboard open). See shadow_focused_now() below.
        struct LinuxInputCb {
            // evdev mapping (the Linux scancodes Shadow expects).
            static uint16_t glfw_to_evdev(int glfw_key) {
                switch (glfw_key) {
                    case GLFW_KEY_A: return 30; case GLFW_KEY_B: return 48;
                    case GLFW_KEY_C: return 46; case GLFW_KEY_D: return 32;
                    case GLFW_KEY_E: return 18; case GLFW_KEY_F: return 33;
                    case GLFW_KEY_G: return 34; case GLFW_KEY_H: return 35;
                    case GLFW_KEY_I: return 23; case GLFW_KEY_J: return 36;
                    case GLFW_KEY_K: return 37; case GLFW_KEY_L: return 38;
                    case GLFW_KEY_M: return 50; case GLFW_KEY_N: return 49;
                    case GLFW_KEY_O: return 24; case GLFW_KEY_P: return 25;
                    case GLFW_KEY_Q: return 16; case GLFW_KEY_R: return 19;
                    case GLFW_KEY_S: return 31; case GLFW_KEY_T: return 20;
                    case GLFW_KEY_U: return 22; case GLFW_KEY_V: return 47;
                    case GLFW_KEY_W: return 17; case GLFW_KEY_X: return 45;
                    case GLFW_KEY_Y: return 21; case GLFW_KEY_Z: return 44;
                    case GLFW_KEY_1: return 2;  case GLFW_KEY_2: return 3;
                    case GLFW_KEY_3: return 4;  case GLFW_KEY_4: return 5;
                    case GLFW_KEY_5: return 6;  case GLFW_KEY_6: return 7;
                    case GLFW_KEY_7: return 8;  case GLFW_KEY_8: return 9;
                    case GLFW_KEY_9: return 10; case GLFW_KEY_0: return 11;
                    case GLFW_KEY_ENTER: return 28; case GLFW_KEY_ESCAPE: return 1;
                    case GLFW_KEY_BACKSPACE: return 14; case GLFW_KEY_TAB: return 15;
                    case GLFW_KEY_SPACE: return 57; case GLFW_KEY_DELETE: return 111;
                    case GLFW_KEY_INSERT: return 110; case GLFW_KEY_HOME: return 102;
                    case GLFW_KEY_END: return 107; case GLFW_KEY_PAGE_UP: return 104;
                    case GLFW_KEY_PAGE_DOWN: return 109;
                    case GLFW_KEY_LEFT: return 105; case GLFW_KEY_RIGHT: return 106;
                    case GLFW_KEY_UP: return 103;   case GLFW_KEY_DOWN: return 108;
                    case GLFW_KEY_LEFT_SHIFT: return 42; case GLFW_KEY_RIGHT_SHIFT: return 54;
                    case GLFW_KEY_LEFT_CONTROL: return 29; case GLFW_KEY_RIGHT_CONTROL: return 97;
                    case GLFW_KEY_LEFT_ALT: return 56; case GLFW_KEY_RIGHT_ALT: return 100;
                    case GLFW_KEY_LEFT_SUPER: return 125;
                    /* G46: the RIGHT Super key was missing - when the desktop
                     * does not grab it (X11, or the GNOME shortcut disabled), it
                     * must go through like the left one. 126 = KEY_RIGHTMETA. */
                    case GLFW_KEY_RIGHT_SUPER: return 126;
                    case GLFW_KEY_F1: return 59; case GLFW_KEY_F2: return 60;
                    case GLFW_KEY_F3: return 61; case GLFW_KEY_F4: return 62;
                    case GLFW_KEY_F5: return 63; case GLFW_KEY_F6: return 64;
                    case GLFW_KEY_F7: return 65; case GLFW_KEY_F8: return 66;
                    case GLFW_KEY_F9: return 67; case GLFW_KEY_F10: return 68;
                    case GLFW_KEY_F11: return 87; case GLFW_KEY_F12: return 88;
                    case GLFW_KEY_MINUS: return 12; case GLFW_KEY_EQUAL: return 13;
                    case GLFW_KEY_LEFT_BRACKET: return 26; case GLFW_KEY_RIGHT_BRACKET: return 27;
                    case GLFW_KEY_BACKSLASH: return 43; case GLFW_KEY_SEMICOLON: return 39;
                    case GLFW_KEY_APOSTROPHE: return 40; case GLFW_KEY_GRAVE_ACCENT: return 41;
                    case GLFW_KEY_COMMA: return 51; case GLFW_KEY_PERIOD: return 52;
                    case GLFW_KEY_SLASH: return 53;
                    default: return 0;
                }
            }

            static bool shadow_focused_now() {
                /* === K21 2026-09-10 - THE PAUSE MENU DID NOT TAKE THE
                 * KEYBOARD BACK ===
                 *
                 * This function tested only the SESSION, and yet its comment
                 * claimed the opposite: "the menu and the keyboard are
                 * overlays, we let them intercept before us". Nothing let them
                 * intercept anything - these GLFW callbacks are installed on
                 * the window, they go through no Borealis overlay. With the
                 * pause menu open, every key therefore ALSO went to the VM:
                 * you navigated the menu and typed into the game at the same
                 * time. Reported in use, reproduced, fixed.
                 *
                 * `g_stream_focused` already carries exactly the right
                 * condition - session active AND no overlay open (see where
                 * `draw` writes it) - and the cursor polling had relied on it
                 * all along. The keyboard, the clicks and the wheel now line
                 * up with the mouse.
                 *
                 * A possible one-frame lag is harmless here: the value is
                 * rewritten on every `draw`, and a key arriving within 16 ms
                 * of the menu opening is not what separates a usable menu from
                 * an unusable one. */
                return g_stream_focused.load(std::memory_order_relaxed);
            }

            static void mouse_btn(GLFWwindow *w, int button, int action, int mods) {
                /* D1 - the development menu bar sees the clicks BEFORE the VM:
                 * without this, clicking in a menu would also send the click to
                 * Windows, which would lose focus and close the menu. */
                if (button == 0) {
                    float lx = 0, ly = 0;
                    devui::pointerLogical(lx, ly);
                    if (devui::onMouseButton(lx, ly, action == GLFW_PRESS)) {
                        if (s_prev_btn_cb) s_prev_btn_cb(w, button, action, mods);
                        return;
                    }
                }
                if (shadow_focused_now() && button >= 0 && button <= 2) {
                    bool pressed = (action == GLFW_PRESS);
                    if (pressed != s_btn_held[button]) {
                        // shadow_input.c now patches X/Y into the click
                        // templates from g_cursor_x/y -> the position is already
                        // right thanks to the cursor poll on every draw().
                        shadow_input_post_mouse_button(button, pressed);
                        s_btn_held[button] = pressed;
                    }
                }
                if (s_prev_btn_cb) s_prev_btn_cb(w, button, action, mods);
            }

            static void key(GLFWwindow *w, int gkey, int scancode, int action, int mods) {
                if (shadow_focused_now() && gkey >= 0 && gkey <= GLFW_KEY_LAST) {
                    if (action == GLFW_PRESS || action == GLFW_RELEASE) {
                        bool pressed = (action == GLFW_PRESS);
                        uint16_t evdev = glfw_to_evdev(gkey);
                        if (evdev && pressed != s_key_held[gkey]) {
                            shadow_input_post_scancode(evdev, pressed);
                            s_key_held[gkey] = pressed;
                        }
                    }
                    // GLFW_REPEAT: ignored (Shadow handles its own repeat on
                    // the Windows VM side, from the initial DOWN).
                }
                if (s_prev_key_cb) s_prev_key_cb(w, gkey, scancode, action, mods);
            }

            static void scroll(GLFWwindow *w, double xoff, double yoff) {
                if (shadow_focused_now() && yoff != 0.0) {
                    shadow_input_post_mouse_wheel(yoff > 0 ? 1 : -1);
                }
                if (s_prev_scroll_cb) s_prev_scroll_cb(w, xoff, yoff);
            }
        };

        s_prev_btn_cb    = glfwSetMouseButtonCallback(win, &LinuxInputCb::mouse_btn);
        s_prev_key_cb    = glfwSetKeyCallback(win, &LinuxInputCb::key);
        s_prev_scroll_cb = glfwSetScrollCallback(win, &LinuxInputCb::scroll);
    }

    // Cursor position - ABSOLUTE MODE.
    // We map the cursor position (relative to the stream view rect, in window
    // pixels) onto the resolution of the Shadow VM stream. That is what the
    // browser does on its <canvas>: the user moves their PC cursor over the
    // canvas, and the Windows cursor goes to the corresponding position,
    // normalised to the streamed resolution.
    //
    // Why not deltas: with deltas the positions diverge (OS mouse gain,
    // non-linear acceleration, imperfect capture of the window enter/leave
    // frames). In absolute mode: 1:1 guaranteed as long as you are on the rect.
    //
    // Outside the stream rect (cursor over header/footer/menu) -> we send
    // nothing, the Shadow cursor stays at its last position.
    {
        // Compute the EFFECTIVE video rect (= the real Shadow stream area, not
        // the StreamView rect). If the window's aspect differs from the stream's
        // (e.g. a 16:9 stream in a 4:3 window), black bars appear and the cursor
        // mapping must ignore them. Identical to the `letterbox` lambda above.
        int sw = (consumer_slot.w > 0) ? consumer_slot.w : 1920;
        int sh = (consumer_slot.h > 0) ? consumer_slot.h : 1080;
        float vid_x = x, vid_y = y, vid_w = width, vid_h = height;
        if (width > 0 && height > 0 && sw > 0 && sh > 0) {
            float dst_aspect = width / height;
            float src_aspect = (float)sw / (float)sh;
            if (src_aspect > dst_aspect) {
                /* Stream wider than the window -> top+bottom bars */
                vid_h = width / src_aspect;
                vid_y = y + (height - vid_h) / 2.0f;
            } else {
                /* Stream taller than the window -> left+right bars */
                vid_w = height * src_aspect;
                vid_x = x + (width - vid_w) / 2.0f;
            }
        }

        // Update the shared rect/VM for the GLFW callbacks (sync_cursor_now) -
        // uses the VIDEO rect (letterboxed), not the StreamView rect.
        s_rect_x = vid_x; s_rect_y = vid_y; s_rect_w = vid_w; s_rect_h = vid_h;
        s_vm_w = sw;
        s_vm_h = sh;

        bool stream_focused = shadow_input_session_active() && !isMenuOpen() && !kbd_open;
        g_stream_focused.store(stream_focused, std::memory_order_relaxed);
        if (stream_focused && vid_w > 0 && vid_h > 0) {
            float lcx = 0, lcy = 0;
            devui::pointerLogical(lcx, lcy);
            double cx = lcx, cy = lcy;

            // glfwGetCursorPos returns RAW window pixels. Borealis works in
            // LOGICAL pixels (= raw / windowScale) - the rect
            // (x, y, width, height) that `draw()` receives is logical. Without
            // this conversion: a systematic offset on HiDPI or on a window
            // fractionnally scaled. cf. brls Application::windowScale.

            // Cursor position RELATIVE to the real video area (post-letterbox).
            // Outside it (on the black bars or Borealis chrome) = nothing sent.
            float rel_x = (float)cx - vid_x;
            float rel_y = (float)cy - vid_y;
            bool inside = (rel_x >= 0 && rel_x < vid_w
                           && rel_y >= 0 && rel_y < vid_h);

            /* hovering the bar: we do not move the VM's cursor */
            if (devui::hovers((float)cx, (float)cy)) inside = false;
            if (inside) {
                int vm_w = s_vm_w;
                int vm_h = s_vm_h;

                float nx = rel_x / vid_w;
                float ny = rel_y / vid_h;
                int tx = (int)(nx * vm_w);
                int ty = (int)(ny * vm_h);
                if (tx < 0)        tx = 0;
                if (tx >= vm_w)    tx = vm_w - 1;
                if (ty < 0)        ty = 0;
                if (ty >= vm_h)    ty = vm_h - 1;

                static int s_last_tx = -1, s_last_ty = -1;
                if (tx != s_last_tx || ty != s_last_ty) {
                    shadow_input_post_mouse_move_abs(tx, ty);
                    s_last_tx = tx;
                    s_last_ty = ty;
                }
            }
        } else {
            // Force-release everything held (otherwise the UPs never arrive if
            // the session ends with a button pressed).
            for (int i = 0; i < 3; i++) {
                if (s_btn_held[i] && shadow_input_session_active()) {
                    shadow_input_post_mouse_button(i, false);
                }
                s_btn_held[i] = false;
            }
            for (int k = 0; k <= GLFW_KEY_LAST; k++) s_key_held[k] = false;
        }
    }

    skip_linux_input:;
#endif

    prof.cp(5);   /* LAT-V2: input (touch, pad, Start) */
    // === On-screen keyboard ===
    /* === K8 2026-08-27 — PAGES, AND THE STREAM STAYS DRIVABLE ===
     *
     * Three defects fixed at once:
     *
     *  1. Keys were missing - Page Up / Page Down first, but also Home, End,
     *     Insert, the context menu key and the WHOLE numeric keypad.
     *  2. Everything fitted on a single board. Letters, function keys and
     *     navigation fought over six rows, so every key was tiny and nothing was
     *     where you expect it. The keyboard now has FOUR pages, with tabs:
     *     fingers get normal-sized keys again, and each page looks like the
     *     region of the keyboard it represents (the numeric keypad has the shape
     *     of a keypad).
     *  3. The area of the stream still visible above the keyboard was DEAD:
     *     touch was cut entirely there as soon as the keyboard opened. You could
     *     not click into the field you wanted to type in without closing the
     *     keyboard. See `kbdTop()` and the touch block's guard.
     *
     * THE SCANCODE IS PHYSICAL, never the character. The key labelled "A" on an
     * AZERTY and the one labelled "Q" on a QWERTY are the SAME key and carry the
     * same scancode 16; it is the layout configured INSIDE THE VM that decides
     * which character is produced. The AZERTY and QWERTY tables therefore carry
     * exactly the same scancodes - only the labels and the row order change, so
     * that you find your own keyboard.
     *
     * Pages 1 to 3 are COMMON to both layouts: an arrow key, F5 or the keypad's
     * 7 sit in the same place on every keyboard in the world.
     */
    if (kbd_open && !isMenuOpen()) {
        /* scancode 0 = SPACER: takes up room and draws nothing. It gives the
         * arrow block its inverted-T shape and the numeric keypad its grid -
         * without it everything would be left-aligned and unreadable. */
        /* The scancodes are those of linux/input-event-codes.h (KEY_Q = 16,
         * KEY_ESC = 1, ...). Two runs are NOT contiguous and have already caused
         * a failure once: F1..F10 are 59..68 but F11 and F12 are 87 and 88; and
         * the numeric keypad is not in digit order (7 8 9 = 71 72 73, but
         * 1 2 3 = 79 80 81). */
        struct KeyDef { const char *label; uint16_t scancode; int row; float w_units; };

        static const KeyDef PAGE_LETTERS_AZERTY[] = {
            {"&", 2, 0, 1}, {"é", 3, 0, 1}, {"\"", 4, 0, 1}, {"'", 5, 0, 1},
            {"(", 6, 0, 1}, {"-", 7, 0, 1}, {"è", 8, 0, 1}, {"_", 9, 0, 1},
            {"ç", 10, 0, 1}, {"à", 11, 0, 1}, {"<-", 14, 0, 2},
            {"a", 16, 1, 1}, {"z", 17, 1, 1}, {"e", 18, 1, 1}, {"r", 19, 1, 1},
            {"t", 20, 1, 1}, {"y", 21, 1, 1}, {"u", 22, 1, 1}, {"i", 23, 1, 1},
            {"o", 24, 1, 1}, {"p", 25, 1, 1}, {"^", 26, 1, 1},
            {"q", 30, 2, 1}, {"s", 31, 2, 1}, {"d", 32, 2, 1}, {"f", 33, 2, 1},
            {"g", 34, 2, 1}, {"h", 35, 2, 1}, {"j", 36, 2, 1}, {"k", 37, 2, 1},
            {"l", 38, 2, 1}, {"m", 39, 2, 1}, {"Entree", 28, 2, 2},
            {"Maj", 42, 3, 2}, {"w", 44, 3, 1}, {"x", 45, 3, 1}, {"c", 46, 3, 1},
            {"v", 47, 3, 1}, {"b", 48, 3, 1}, {"n", 49, 3, 1}, {",", 50, 3, 1},
            {";", 51, 3, 1}, {":", 52, 3, 1}, {"Suppr", 111, 3, 1},
            /* Ctrl/Alt/Shift are LATCHING modifiers: you arm them, then you
             * type. That is what makes Ctrl+C and Alt+Tab possible. */
            {"Echap", 1, 4, 1}, {"Tab", 15, 4, 1}, {"Ctrl", 29, 4, 1}, {"Alt", 56, 4, 1},
            {"Espace", 57, 4, 4},
            {"←", 105, 4, 1}, {"↑", 103, 4, 1}, {"↓", 108, 4, 1}, {"→", 106, 4, 1},
        };
        /* QWERTY: the same scancodes, put back in their places. The digit row
         * becomes 1..0, and w/z swap positions. */
        static const KeyDef PAGE_LETTERS_QWERTY[] = {
            {"1", 2, 0, 1}, {"2", 3, 0, 1}, {"3", 4, 0, 1}, {"4", 5, 0, 1},
            {"5", 6, 0, 1}, {"6", 7, 0, 1}, {"7", 8, 0, 1}, {"8", 9, 0, 1},
            {"9", 10, 0, 1}, {"0", 11, 0, 1}, {"<-", 14, 0, 2},
            {"q", 16, 1, 1}, {"w", 17, 1, 1}, {"e", 18, 1, 1}, {"r", 19, 1, 1},
            {"t", 20, 1, 1}, {"y", 21, 1, 1}, {"u", 22, 1, 1}, {"i", 23, 1, 1},
            {"o", 24, 1, 1}, {"p", 25, 1, 1}, {"[", 26, 1, 1},
            {"a", 30, 2, 1}, {"s", 31, 2, 1}, {"d", 32, 2, 1}, {"f", 33, 2, 1},
            {"g", 34, 2, 1}, {"h", 35, 2, 1}, {"j", 36, 2, 1}, {"k", 37, 2, 1},
            {"l", 38, 2, 1}, {";", 39, 2, 1}, {"Enter", 28, 2, 2},
            {"Shift", 42, 3, 2}, {"z", 44, 3, 1}, {"x", 45, 3, 1}, {"c", 46, 3, 1},
            {"v", 47, 3, 1}, {"b", 48, 3, 1}, {"n", 49, 3, 1}, {"m", 50, 3, 1},
            {",", 51, 3, 1}, {".", 52, 3, 1}, {"Del", 111, 3, 1},
            {"Esc", 1, 4, 1}, {"Tab", 15, 4, 1}, {"Ctrl", 29, 4, 1}, {"Alt", 56, 4, 1},
            {"Space", 57, 4, 4},
            {"←", 105, 4, 1}, {"↑", 103, 4, 1}, {"↓", 108, 4, 1}, {"→", 106, 4, 1},
        };

        /* NAVIGATION page. The arrow block keeps its INVERTED-T shape, the only
         * one the hand recognises without looking. The six movement keys are
         * grouped as on a full keyboard.
         * Page Up / Page Down were missing entirely: that request is what
         * triggered this rework. */
        static const KeyDef PAGE_NAV[] = {
            {"Inser", 110, 0, 1}, {"Debut", 102, 0, 1}, {"Pg↑", 104, 0, 1},
            {"", 0, 0, 1},
            {"Echap", 1, 0, 1}, {"Tab", 15, 0, 1}, {"<-", 14, 0, 1},
            {"Suppr", 111, 1, 1}, {"Fin", 107, 1, 1}, {"Pg↓", 109, 1, 1},
            {"", 0, 1, 1},
            {"Win", 125, 1, 1}, {"Menu", 127, 1, 1}, {"Entree", 28, 1, 1},
            {"", 0, 2, 1}, {"↑", 103, 2, 1}, {"", 0, 2, 1},
            {"", 0, 2, 1},
            {"Ctrl", 29, 2, 1}, {"Alt", 56, 2, 1}, {"Maj", 42, 2, 1},
            {"←", 105, 3, 1}, {"↓", 108, 3, 1}, {"→", 106, 3, 1},
            {"", 0, 3, 1},
            {"Espace", 57, 3, 3},
        };

        /* FUNCTION page. F1..F10 follow on (59..68) but F11 and F12 are 87 and
         * 88: the run is NOT contiguous, and assuming it once cost a session. */
        static const KeyDef PAGE_FN[] = {
            {"F1", 59, 0, 1}, {"F2", 60, 0, 1}, {"F3", 61, 0, 1},
            {"F4", 62, 0, 1}, {"F5", 63, 0, 1}, {"F6", 64, 0, 1},
            {"F7", 65, 1, 1}, {"F8", 66, 1, 1}, {"F9", 67, 1, 1},
            {"F10", 68, 1, 1}, {"F11", 87, 1, 1}, {"F12", 88, 1, 1},
            {"Echap", 1, 2, 1}, {"Tab", 15, 2, 1}, {"Win", 125, 2, 1},
            {"Menu", 127, 2, 1}, {"Ctrl", 29, 2, 1}, {"Alt", 56, 2, 1},
            {"Maj", 42, 3, 1}, {"Entree", 28, 3, 1}, {"<-", 14, 3, 1},
            {"Espace", 57, 3, 3},
        };

        /* NUMERIC KEYPAD page, laid out like a real keypad. The nine digits are
         * attested byte-exact by two independent captures of the official
         * client; the keypad's Enter and divide are EXTENDED keys (K7) and
         * therefore go out with their flag, without which they would be the main
         * Enter and the ordinary "/" key. */
        static const KeyDef PAGE_KEYPAD[] = {
            {"Verr", 69, 0, 1}, {"/", 98, 0, 1}, {"*", 55, 0, 1}, {"-", 74, 0, 1},
            {"7", 71, 1, 1}, {"8", 72, 1, 1}, {"9", 73, 1, 1}, {"+", 78, 1, 1},
            {"4", 75, 2, 1}, {"5", 76, 2, 1}, {"6", 77, 2, 1}, {"", 0, 2, 1},
            {"1", 79, 3, 1}, {"2", 80, 3, 1}, {"3", 81, 3, 1}, {"Ent", 96, 3, 1},
            {"0", 82, 4, 2}, {".", 83, 4, 1}, {"", 0, 4, 1},
        };

        const bool qwerty = (Settings::instance().kb_layout == Settings::KbLayout::Qwerty);
        /* === K11 — ONE SCANCODE, ONE LABEL ===
         * The common pages (navigation, function keys, keypad) were written in
         * French, while the QWERTY letters page said "Esc", "Shift", "Enter".
         * The same scancode therefore carried TWO names depending on the tab,
         * which is exactly what a keyboard must never do. The special keys now
         * follow the chosen layout, everywhere. */
        auto label = [&](const KeyDef &k) -> const char * {
            if (!qwerty) return k.label;
            switch (k.scancode) {
                case 1:   return "Esc";
                case 14:  return "<-";
                case 15:  return "Tab";
                case 28:  return "Enter";
                case 42:  return "Shift";
                case 57:  return "Space";
                case 69:  return "Num";
                case 96:  return "Ent";
                case 102: return "Home";
                case 104: return "Pg^";
                case 107: return "End";
                case 109: return "Pgv";
                case 110: return "Ins";
                case 111: return "Del";
                case 125: return "Win";
                case 127: return "Menu";
                default:  return k.label;
            }
        };

        struct Page { const char *name; const KeyDef *k; int n; int rows; };
        /* The tab names go through i18n: they are INTERFACE TEXT, not key
         * labels. The strings are kept alive for the duration of the draw -
         * taking the .c_str() of a temporary inside the array would leave a
         * dangling pointer. */
        const std::string tab_letters = qwerty ? ui::tr("kbd/tab_qwerty")
                                               : ui::tr("kbd/tab_azerty");
        const std::string tab_nav = ui::tr("kbd/tab_nav");
        const std::string tab_fn  = ui::tr("kbd/tab_fn");
        const std::string tab_pad = ui::tr("kbd/tab_pad");
        const Page PAGES[] = {
            { tab_letters.c_str(),
              qwerty ? PAGE_LETTERS_QWERTY : PAGE_LETTERS_AZERTY,
              qwerty ? (int)(sizeof PAGE_LETTERS_QWERTY / sizeof PAGE_LETTERS_QWERTY[0])
                     : (int)(sizeof PAGE_LETTERS_AZERTY / sizeof PAGE_LETTERS_AZERTY[0]),
              5 },
            { tab_nav.c_str(), PAGE_NAV, (int)(sizeof PAGE_NAV / sizeof PAGE_NAV[0]), 4 },
            { tab_fn.c_str(),  PAGE_FN,  (int)(sizeof PAGE_FN  / sizeof PAGE_FN[0]),  4 },
            { tab_pad.c_str(), PAGE_KEYPAD,(int)(sizeof PAGE_KEYPAD/ sizeof PAGE_KEYPAD[0]), 5 },
        };
        const int N_PAGES = (int)(sizeof PAGES / sizeof PAGES[0]);
        if (kbd_page < 0 || kbd_page >= N_PAGES) kbd_page = 0;

        const KeyDef *KEYS = PAGES[kbd_page].k;
        const int N_KEYS   = PAGES[kbd_page].n;
        const int N_ROWS   = PAGES[kbd_page].rows;

        const float kbd_h = height * KBD_FRACTION;
        const float kbd_y = kbdTop(y, height);
        const float kbd_pad = 6.0f;
        /* The tabs take a fixed band at the top of the keyboard. It is
         * PROPORTIONAL to the keyboard and not to the screen: otherwise it would
         * eat all the room on a five-row page. */
        const float tabs_h = kbd_h * 0.15f;
        const float keys_h = kbd_h - tabs_h;
        const float row_h = (keys_h - kbd_pad * (N_ROWS + 1)) / N_ROWS;

        nvgBeginPath(vg);
        nvgRect(vg, x, kbd_y, width, kbd_h);
        nvgFillColor(vg, nvgRGBA(15, 20, 30, 220));
        nvgFill(vg);

        int touch_x = -1, touch_y = -1;
        bool touch_active = false;
#include "switch_compat.h"
#if SHADOW_HAS_STREAM_INPUT
        /* === K11 — THE KEYBOARD LOOKS FOR THE FINGER THAT IS ON IT ===
         * It only read `ts.touches[0]`. But since K8 it is LEGITIMATE to have a
         * finger down in the stream area while the keyboard is open: that finger
         * occupies slot 0 for as long as it stays down, and the keyboard then
         * went COMPLETELY MUTE - no key containing that point, so nothing was
         * ever typed. The two blocks were asymmetric: the stream protected
         * itself from the keyboard's fingers, the keyboard did not protect
         * itself from the stream's.
         * So we take the first finger located INSIDE the keyboard band. */
        HidTouchScreenState ts = {0};
        if (hidGetTouchScreenStates(&ts, 1) > 0 && ts.count > 0) {
            for (unsigned i = 0; i < ts.count && i < 16; i++) {
                if ((float)ts.touches[i].y >= kbd_y) {
                    touch_x = (int)ts.touches[i].x;
                    touch_y = (int)ts.touches[i].y;
                    touch_active = true;
                    break;
                }
            }
        }
#endif  /* SHADOW_HAS_STREAM_INPUT */

        /* --- Onglets --- */
        int tab_hit = -1;
        {
            const float ow = (width - kbd_pad * (N_PAGES + 1)) / N_PAGES;
            float ox = x + kbd_pad;
            for (int p = 0; p < N_PAGES; p++) {
                const bool ici = touch_active && touch_x >= ox && touch_x < ox + ow
                                 && touch_y >= kbd_y + kbd_pad
                                 && touch_y < kbd_y + tabs_h;
                if (ici) tab_hit = p;
                nvgBeginPath(vg);
                nvgRoundedRect(vg, ox, kbd_y + kbd_pad, ow,
                               tabs_h - kbd_pad * 1.5f, 6.0f);
                nvgFillColor(vg, (p == kbd_page) ? nvgRGB(70, 140, 220)
                                                 : nvgRGB(38, 46, 62));
                nvgFill(vg);
                nvgFontSize(vg, (tabs_h - kbd_pad * 1.5f) * 0.52f);
                nvgFontFace(vg, "regular");
                nvgFillColor(vg, (p == kbd_page) ? nvgRGB(255, 255, 255)
                                                 : nvgRGB(170, 180, 200));
                nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgText(vg, ox + ow / 2,
                        kbd_y + kbd_pad + (tabs_h - kbd_pad * 1.5f) / 2,
                        PAGES[p].name, nullptr);
                ox += ow + kbd_pad;
            }
        }

        /* === K9 2026-08-27 — ACCENTED VARIANTS ON A LONG PRESS ===
         *
         * Like a phone keyboard: you hold "e", a strip appears with é è ê ë, you
         * slide onto one and release.
         *
         * TWO CONSTRAINTS THAT SHAPE EVERYTHING ELSE:
         *
         * 1. WE SEND SCANCODES, NOT CHARACTERS. There is no "send ê" in this
         *    protocol: a key is a physical POSITION, and it is the layout
         *    configured INSIDE THE VM that decides the character. The variants
         *    are therefore produced as on a real French keyboard - by a DEAD KEY
         *    followed by the letter: `^` (scancode 26) then `a` gives â; `¨`
         *    (Shift+26) then `a` gives ä. Those that have their own key on
         *    AZERTY (é è ç à ù) go out directly.
         *    Accepted consequence: this only works if the VM is set to French.
         *    That is already the assumption of the whole AZERTY page - its labels
         *    would be wrong otherwise - so the variants are offered there only.
         *    On QWERTY there is no standard dead key, and guessing between US,
         *    US-International and UK would produce random characters: better to
         *    offer nothing.
         *
         * 2. THE KEY GOES OUT ON RELEASE, no longer on press. That is required to
         *    be able to slide onto a variant before choosing. The change applies
         *    to ALL keys, not only those that have variants: deferring only some
         *    would make a lag noticeable on the vowels alone, that is, on the
         *    most-typed keys. It is also how every touch keyboard behaves, and it
         *    brings two things along the way - sliding off a key CANCELS the
         *    keystroke, and running a finger across the keyboard no longer types
         *    everything it passes over, which edge detection did.
         */
        struct Variant { const char *label; uint16_t dead; bool dead_shift; uint16_t base; };
        /* `dead` = dead key to strike BEFORE the letter (0 = none).
         * `dead_shift` = that dead key needs Shift (the diaeresis is Shift+^).
         * AZERTY scancodes: ^/¨ = 26, é = 3, è = 8, ç = 10, à = 11, ù = 40. */
        static const Variant VAR_A[] = { {"à",0,false,11}, {"â",26,false,16}, {"ä",26,true,16} };
        static const Variant VAR_E[] = { {"é",0,false,3},  {"è",0,false,8},
                                          {"ê",26,false,18},{"ë",26,true,18} };
        static const Variant VAR_I[] = { {"î",26,false,23},{"ï",26,true,23} };
        static const Variant VAR_O[] = { {"ô",26,false,24},{"ö",26,true,24} };
        static const Variant VAR_U[] = { {"ù",0,false,40}, {"û",26,false,22},{"ü",26,true,22} };
        static const Variant VAR_C[] = { {"ç",0,false,10} };
        static const Variant VAR_Y[] = { {"ÿ",26,true,21} };
        struct VarSet { uint16_t base; const Variant *v; int n; };
        static const VarSet VARIANT_SETS[] = {
            {16, VAR_A, 3}, {18, VAR_E, 4}, {23, VAR_I, 2}, {24, VAR_O, 2},
            {22, VAR_U, 3}, {46, VAR_C, 1}, {21, VAR_Y, 1},
        };
        /* Returns a key's variant set, or nullptr. Variants are only offered on
         * the letters page in AZERTY (see constraint 1). */
        auto variants_of = [&](int idx, int *n) -> const Variant * {
            *n = 0;
            if (kbd_page != 0 || qwerty || idx < 0 || idx >= N_KEYS) return nullptr;
            for (const VarSet &j : VARIANT_SETS)
                if (j.base == KEYS[idx].scancode) { *n = j.n; return j.v; }
            return nullptr;
        };

        /* --- Keys --- */
        float row_units[8] = {0};
        for (int i = 0; i < N_KEYS; i++)
            if (KEYS[i].row >= 0 && KEYS[i].row < 8)
                row_units[KEYS[i].row] += KEYS[i].w_units;

        int hit_idx = -1;
        float hit_x = 0, hit_y = 0, hit_w = 0;   /* geometry, to place the strip */
        for (int row = 0; row < N_ROWS; row++) {
            if (row_units[row] <= 0.0f) continue;
            const float row_y = kbd_y + tabs_h + kbd_pad + row * (row_h + kbd_pad);
            /* K11 - A KEY DOES NOT SPREAD OUT. The width used to be "all the
             * room divided by the number of units": on the numeric keypad, four
             * units per row gave 312 x 49 px cells, anything but a keypad. Worse,
             * the rows of the letters page (11 units against 12) did not even
             * line up with each other.
             * So we cap it at about twice the height - the proportion of a real
             * key - and we CENTRE the row. */
            const float unit_max = row_h * 2.0f;
            float unit_w = (width - kbd_pad * (row_units[row] + 1)) / row_units[row];
            if (unit_w > unit_max) unit_w = unit_max;
            const float row_w = unit_w * row_units[row] + kbd_pad * (row_units[row] - 1);
            float cx = x + (width - row_w) * 0.5f;
            for (int i = 0; i < N_KEYS; i++) {
                if (KEYS[i].row != row) continue;
                const float kw = unit_w * KEYS[i].w_units + (KEYS[i].w_units - 1) * kbd_pad;
                if (KEYS[i].scancode == 0) { cx += kw + kbd_pad; continue; }  /* espaceur */
                const bool is_hit = touch_active && touch_x >= cx && touch_x < cx + kw
                              && touch_y >= row_y && touch_y < row_y + row_h;
                if (is_hit) { hit_idx = i; hit_x = cx; hit_y = row_y; hit_w = kw; }
                if (i == kbd_press_idx) { hit_x = cx; hit_y = row_y; hit_w = kw; }
                int nv = 0;
                const bool has_variants = (variants_of(i, &nv) != nullptr);
                const NVGcolor fill =
                      (i == kbd_press_idx) ? nvgRGB(70, 140, 220)
                    : (KEYS[i].scancode == 42 && kbd_shift)      ? nvgRGB(140, 90, 50)
                    : (KEYS[i].scancode == 29 && kbd_mod_ctrl_on)? nvgRGB(140, 90, 50)
                    : (KEYS[i].scancode == 56 && kbd_mod_alt_on) ? nvgRGB(140, 90, 50)
                    : nvgRGB(50, 60, 80);
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cx, row_y, kw, row_h, 6.0f);
                nvgFillColor(vg, fill);
                nvgFill(vg);
                nvgFontSize(vg, row_h * 0.45f);
                nvgFontFace(vg, "regular");
                nvgFillColor(vg, nvgRGB(240, 240, 245));
                nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgText(vg, cx + kw / 2, row_y + row_h / 2, label(KEYS[i]), nullptr);
                /* A discreet dot marks a key that hides variants.
                 * Without it the feature would be invisible: nobody holds a key
                 * at random to see what happens. */
                if (has_variants) {
                    nvgBeginPath(vg);
                    nvgCircle(vg, cx + kw - row_h * 0.16f, row_y + row_h * 0.18f, 2.0f);
                    nvgFillColor(vg, nvgRGBA(255, 255, 255, 110));
                    nvgFill(vg);
                }
                cx += kw + kbd_pad;
            }
        }

        /* --- Tracking the press, and opening the variants strip --- */
        /* PORTABLE clock: `armGetSystemTick` only exists on Switch and broke the
         * desktop build - and the desktop is where features are validated before
         * being ported. `steady_clock` is monotonic everywhere, and microsecond
         * precision is three orders of magnitude beyond what a long press
         * needs. */
        const uint64_t now_us = (uint64_t)
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        const uint64_t LONG_PRESS_US = 400000;   /* 0.4 s, as on a phone */

        if (touch_active && kbd_press_idx < 0 && kbd_popup < 0 && tab_hit < 0
                && hit_idx >= 0) {
            kbd_press_idx = hit_idx;          /* finger down */
            kbd_press_us  = now_us;
        }
        /* === K9b — CANCELLATION IS DECIDED DURING THE PRESS, NOT ON RELEASE ===
         * The first version checked `hit_idx == kbd_press_idx` ON RELEASE to
         * know whether the finger had stayed on the key. But the hit test
         * requires `touch_active`: on the release frame there is no finger left,
         * so `hit_idx` is -1 and the comparison was ALWAYS false. The keyboard
         * stopped sending anything at all.
         * So we decide while the finger is still there: if it leaves the key, we
         * cancel immediately. The variants strip is the exception - the finger is
         * precisely meant to leave the key to go and choose. */
        if (touch_active && kbd_press_idx >= 0 && kbd_popup < 0
                && hit_idx != kbd_press_idx)
            kbd_press_idx = -1;
        if (kbd_press_idx >= 0 && kbd_popup < 0
                && (now_us - kbd_press_us) >= LONG_PRESS_US) {
            int nv = 0;
            if (variants_of(kbd_press_idx, &nv)) kbd_popup = kbd_press_idx;
        }

        /* --- The variants strip, drawn over the keys --- */
        int nv = 0;
        const Variant *vs = (kbd_popup >= 0) ? variants_of(kbd_popup, &nv) : nullptr;
        if (vs) {
            const float vw = hit_w > 0 ? hit_w : row_h;
            const float bw = vw * (float)nv + kbd_pad * (float)(nv + 1);
            /* Centred on the key, but PULLED BACK inside the screen: on "a", at
             * the left edge, a centred strip would run off the frame and half the
             * variants would be unreachable. */
            float bx = hit_x + vw * 0.5f - bw * 0.5f;
            if (bx < x + kbd_pad) bx = x + kbd_pad;
            if (bx + bw > x + width - kbd_pad) bx = x + width - kbd_pad - bw;
            const float bh = row_h * 1.15f;
            const float by = hit_y - bh - kbd_pad;

            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx - 3.0f, by - 3.0f, bw + 6.0f, bh + 6.0f, 10.0f);
            nvgFillColor(vg, nvgRGBA(10, 14, 22, 245));
            nvgFill(vg);

            /* K9b - do NOT clear the variant when the finger has just left.
             * Hover can only be computed while `touch_active`; on the release
             * frame there is no finger left, and resetting to -1 here wiped the
             * choice just before it was read. The strip appeared, you slid onto
             * it, and nothing came out. */
            if (touch_active) kbd_variant = -1;
            float vx = bx + kbd_pad;
            for (int k = 0; k < nv; k++) {
                const bool over = touch_active && touch_x >= vx && touch_x < vx + vw
                                    && touch_y >= by && touch_y < by + bh;
                if (over) kbd_variant = k;
                nvgBeginPath(vg);
                nvgRoundedRect(vg, vx, by, vw, bh, 8.0f);
                nvgFillColor(vg, over ? nvgRGB(70, 140, 220) : nvgRGB(46, 56, 74));
                nvgFill(vg);
                nvgFontSize(vg, bh * 0.5f);
                nvgFontFace(vg, "regular");
                nvgFillColor(vg, nvgRGB(240, 240, 245));
                nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgText(vg, vx + vw / 2, by + bh / 2, vs[k].label, nullptr);
                vx += vw + kbd_pad;
            }
        }

        /* --- Frappe, AU RELACHEMENT --- */
        /* K11 - the modifiers are MEMBERS. They used to be function-level
         * `static`s: an armed Ctrl therefore survived the SESSION CLOSING and
         * came back armed on the next one, exactly the failure family this repo
         * has paid for four times. The indicator was already a member, hence a
         * second possible inconsistency between what is shown and what acts. */
        auto strike = [&](uint16_t code, bool maj) {
            if (maj) shadow_input_post_scancode(42, true);
            shadow_input_post_scancode(code, true);
            shadow_input_post_scancode(code, false);
            if (maj) shadow_input_post_scancode(42, false);
        };

        if (tab_hit >= 0 && kbd_press_idx < 0 && kbd_popup < 0) {
            /* The tabs, though, stay on press: they do not change the VM's
             * contents, only the page shown, and waiting for the release would
             * make a tab feel unresponsive. */
            if (kbd_last_touched != -1000 - tab_hit) {
                kbd_page = tab_hit;
                kbd_last_touched = -1000 - tab_hit;
            }
        } else if (!touch_active && kbd_press_idx >= 0) {
            const int idx = kbd_press_idx;
            if (vs && kbd_variant >= 0 && kbd_variant < nv) {
                /* Chosen variant: the dead key, then the letter. Shift applies to
                 * the LETTER (to get Â), not to the dead key, which has its own
                 * need for Shift to produce the diaeresis. */
                const Variant &v = vs[kbd_variant];
                if (v.dead) strike(v.dead, v.dead_shift);
                strike(v.base, kbd_shift);
                kbd_shift = false; kbd_mod_ctrl = false; kbd_mod_alt = false;
            } else if (kbd_popup >= 0) {
                /* Strip open but released beside it: we CANCEL. Sending the base
                 * letter would be the opposite of what the gesture does. */
            } else {
                /* The finger never left the key: the guard above
                 * aurait remis kbd_press_idx a -1 sinon. */
                const uint16_t code = KEYS[idx].scancode;
                if (code == 42 || code == 29 || code == 56) {
                    if (code == 42) kbd_shift = !kbd_shift;
                    if (code == 29) kbd_mod_ctrl  = !kbd_mod_ctrl;
                    if (code == 56) kbd_mod_alt   = !kbd_mod_alt;
                } else {
                    /* === K1 2026-08-22 - REAL MODIFIERS, COMBINABLE ===
                     * BEFORE: only Shift existed, as a one-shot toggle, and it
                     * went through `keypress_shifted` - so NO real combination
                     * was possible, neither Ctrl+C, nor Alt+Tab, nor
                     * Ctrl+Alt+Del. That approach is refuted; do not go back to
                     * it.
                     * NOW: we hold each modifier DOWN while it is armed, strike
                     * the key, then release in reverse order - what a real
                     * keyboard does. */
                    if (kbd_mod_ctrl)  shadow_input_post_scancode(29, true);
                    if (kbd_mod_alt)   shadow_input_post_scancode(56, true);
                    if (kbd_shift) shadow_input_post_scancode(42, true);
                    shadow_input_post_scancode(code, true);
                    shadow_input_post_scancode(code, false);
                    if (kbd_shift) shadow_input_post_scancode(42, false);
                    if (kbd_mod_alt)   shadow_input_post_scancode(56, false);
                    if (kbd_mod_ctrl)  shadow_input_post_scancode(29, false);
                    kbd_shift = false; kbd_mod_ctrl = false; kbd_mod_alt = false;
                }
            }
            /* Slid off the key with no strip open: nothing. That is the
             * cancellation, and it comes for free with the release. */
            kbd_mod_ctrl_on = kbd_mod_ctrl; kbd_mod_alt_on = kbd_mod_alt;
            kbd_press_idx = -1;
            kbd_popup     = -1;
            kbd_variant  = -1;
        } else if (!touch_active) {
            kbd_last_touched = -1;
            kbd_press_idx = -1;
            kbd_popup     = -1;
            kbd_variant  = -1;
        }
    }

    // === Menu de pause ===
    // The contents are declared by the activity (only it knows what "Quit"
    // means); the view merely draws them, on top of everything else.
    /* === S73 - THE GAMEPAD TESTER, ON TOP OF THE STREAM ===
     * It is here, and only here, that the "read / sent" comparison means
     * anything: the gamepad channel only exists during a session. It is drawn
     * after the menu, whose place it takes. */
    if (pad_test_) {
        const double tnow = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        /* Closing by holding B, measured in ui/pad_test: that module already
         * reads the button's real state in order to draw it. The previous version
         * asked the button handler to "arm" the measurement - but that handler is
         * guarded by "is the menu open?", and opening the tester CLOSES the menu:
         * the arming never happened. */
        if (ui::padtest::draw(vg, view_x, view_y, view_w, view_h, tnow))
            pad_test_ = false;
    } else if (net_test_) {
        /* S119 - same host, same way out as the two testers. It REPLACES the
         * menu rather than covering it: a page of figures has to be readable,
         * and the menu would sit on top of the very numbers you came for. */
        const double tnow_net = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (ui::nettest::draw(vg, view_x, view_y, view_w, view_h, tnow_net))
            net_test_ = false;
    } else if (mouse_test_) {
        /* PM5 - same host, same exit. It takes the menu's place rather than
         * covering it: a page that shows figures has to be readable, and the
         * menu would sit on the very numbers you opened it for. */
        const double tnow = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (ui::mousetest::draw(vg, view_x, view_y, view_w, view_h, tnow))
            mouse_test_ = false;
    } else {
        pause_menu_.draw(vg, view_x, view_y, view_w, view_h);
    }
}

// === API C-callable ===

static StreamView* g_active_view = nullptr;
static std::mutex  g_active_mtx;

extern "C" void stream_view_set_active(StreamView* view) {
    std::lock_guard<std::mutex> lock(g_active_mtx);
    g_active_view = view;
}

extern "C" void stream_view_push_yuv(int width, int height,
                                      const uint8_t* data_y, int linesize_y,
                                      const uint8_t* data_u, int linesize_u,
                                      const uint8_t* data_v, int linesize_v,
                                      int format,
                                      int64_t pts,
                                      void* user) {
    (void)user;
    // Holds the lock across the call: avoids a use-after-free if the activity is
    // destroyed between copying the pointer and invoking it. Serialises the
    // pushes, but that is fine for a 30 fps stream.
    std::lock_guard<std::mutex> lock(g_active_mtx);
    if (g_active_view) {
        g_active_view->pushYuvFrame(width, height, data_y, linesize_y,
                                     data_u, linesize_u, data_v, linesize_v,
                                     format, pts);
    }
}
