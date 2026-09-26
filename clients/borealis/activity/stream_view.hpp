// StreamView - draws the decoded YUV frames of the Shadow stream through nanovg.
//
// Architecture (post-optim #1+#4):
//   1. The session's decode thread calls pushYuvFrame(), which:
//      - copies the YUV planes into `writer_slot` (private to that thread)
//      - swaps with `shared_slot` under the mutex in microseconds
//        (vector::swap = pointer swap)
//      - returns at once, so it never blocks the decode thread
//   2. The Borealis thread (60Hz) calls draw(), which:
//      - swaps shared_slot into consumer_slot under the mutex, in microseconds
//      - does the YUV->RGBA conversion (NEON SIMD) outside the mutex
//      - uploads to nanovg and renders
//
// Why this beats the previous approach (converting under the mutex, on the
// producing thread):
//   - the producer never blocks for more than 1 ms, so the audio stops
//     crackling
//   - the conversion runs at screen rate (60Hz at most) instead of decoder rate
//     (which can burst to 100Hz), so no CPU is wasted
//   - no mutex contention during the conversion (10 ms) nor during the GPU
//     upload.

#pragma once

#include <string>

#include "../ui/hud.hpp"
#include "../ui/rate_meter.hpp"
#include "../../../core/protocol/audio_loss.h"   /* AUD-INS-1: the audio loss row's window */
#include "../ui/overlay_menu.hpp"
extern "C" {
#include "../ui/gestures.h"
}

#include <borealis.hpp>
#include <mutex>
#include <vector>
#include <deque>
#include <cstdint>

#include "clients/borealis/activity/gl_video_renderer.hpp"
#include "clients/borealis/activity/gxm_video_renderer.hpp"

class StreamView : public brls::Box {
public:
    StreamView();
    ~StreamView() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

    // Called from the decode thread. Copies the YUV planes, then swaps in
    // microseconds. NO YUV->RGBA conversion here (moved into draw(), on the
    // Borealis thread).
    /* `pts` used to be received by `stream_view_push_yuv` and THROWN AWAY
     * (`(void)pts`). L5 uses it as the KEY that finds the server timestamp of
     * the frame on the far side of the decoder - the only field that survives
     * the trip through libavcodec. */
    /* LAT-V1 - tell the platform media is playing (no dimming, no idle sleep). */
    void setStreamKeepAwake(bool active);
    int64_t pwr_next_check_us_ = 0;   /* LAT-V1: next clock read-back, per view */
    /* LAT-V2: draw() section profile (SHADOW_UI_FRAME_MS): 0 entry, 1 pop,
     * 2 video, 3 cursor, 4 metrics, 5 input, 7 rest, 8 whole draw, 9 frame. */
    double  prof_acc_[14] = {};
    int     prof_n_ = 0;
    int64_t prof_last_report_us_ = 0, prof_prev_entry_us_ = 0;

    void pushYuvFrame(int width, int height,
                      const uint8_t* data_y, int linesize_y,
                      const uint8_t* data_u, int linesize_u,
                      const uint8_t* data_v, int linesize_v,
                      int format, int64_t pts);

    // Stats
    uint32_t getFrameCount() const { return frames_received; }

private:
    // YUV frame: 2 supported formats.
    //   YUV420P (plain CPU decode): 3 separate Y/U/V planes.
    //   NV12 (output of the Tegra X1 NVDEC hardware): Y plane + interleaved UV.
    // std::vector gives an O(1) swap (pointer swap) under the mutex.
    struct YuvFrame {
        std::vector<uint8_t> y, u, v;
        /* === PS VITA: THE PLANES ARE NOT COPIED ===========================
         *
         * Measured on console: `sceAvcdecDecode` costs 3.8 ms while
         * `video/decode` -- which spans that callback -- costs 18.8. The 15 ms
         * between them is the 1.4 MB memcpy into the vectors above, and
         * `video/televerse` adds 7 more for the copy into the GXM texture. So
         * 22 ms went on copying a picture the silicon produces in 4.
         *
         * Here the planes STAY in the decoder's buffer, and it rotates four of
         * them (VITA_PIC_SLOTS): the presentation queue holds two at most,
         * plus the one being drawn, so a buffer is reused four pictures later,
         * long after it was displayed.
         *
         * A non-null `ext_y` means "read there, not in the vectors". The
         * vectors stay empty on this console, so VI2's recycling keeps
         * working: it moves empty vectors, which costs nothing. */
        const uint8_t *ext_y = nullptr;
        const uint8_t *ext_u = nullptr;
        int w = 0, h = 0;
        int ys = 0, us = 0, vs = 0;  // strides
        bool is_nv12 = false;        // true: u holds interleaved UV, v is empty
        /* K17 - FULL resolution chroma (4:4:4). The U and V planes are then as
         * large as the Y plane, not half of it. Rendering depends on this: it
         * computed `uv_w = w / 2` unconditionally, which stretched a quarter of
         * the chroma image across the whole surface. */
        bool chroma_pleine = false;
        /* Push timestamp, from latency_now_us(): taken once the planes are
         * copied, just before the queue lock (VI2), and 0 when SHADOW_LATENCE=0.
         * The draw subtracts it from the same clock for [L5] video/file-aff. */
        int64_t push_us = 0;
        /* L5 - SERVER timestamp of this frame, exactly as it came out of the
         * `VideoFrame` header. It travels WITH the frame rather than beside it:
         * the presentation queue drops frames (L3), so a shared variable would
         * describe a frame other than the one on screen. */
        uint32_t srv_stamp = 0;
    };

    // Frame pacing queue: we hold up to 2 frames to absorb NVDEC bursts. The
    // decode thread enqueues, the Borealis thread dequeues 1 frame per draw()
    // call, so presentation stays at a fixed 60Hz.
    //
    //   writer_slot  : buffer private to the decode thread (no lock on write)
    //   pending_queue: up to PACING_QUEUE_MAX frames waiting, under the lock
    //   consumer_slot: read by the Borealis thread, swapped in from pending_queue
    /* Raised from 2 to 4 (2026-05-06): the variance of the Shadow encoder
     * pushes bursts of 2-3 frames in under 16 ms followed by pauses. With
     * MAX=2 we dropped frames during the bursts, which was visibly jerky.
     * MAX=4 absorbs the bursts and adds at most ~33 ms of latency
     * (4 frames @ 60Hz). */
    /* L1 2026-08-22 - 4 -> 2: latency wins for gaming. At 60 Hz, four queued
     * frames means up to ~66 ms of delay on screen. Two are enough to smooth
     * out a burst without piling up. */
    static constexpr size_t PACING_QUEUE_MAX = 2;
    YuvFrame writer_slot;
    YuvFrame consumer_slot;
    std::deque<YuvFrame> pending_queue;
    std::mutex swap_mtx;
    /* === VI2 2026-09-11 - RECYCLED PLANE BUFFERS (SHADOW_PUSH_RECYCLE) ===
     * A frame whose pixels are no longer needed - dropped from a full
     * pending_queue, skipped by L3, or the consumer_slot being replaced -
     * parks its vectors here instead of freeing them, and pushYuvFrame hands
     * one set back to writer_slot, so the next copy lands in memory already
     * allocated. Guarded by swap_mtx on both threads, like pending_queue.
     * A member, not a static: this is view state. Reserved to PUSH_POOL_MAX at
     * construction, so a push_back under the lock never allocates; a frame
     * that does not fit is destroyed after the lock is released. */
    static constexpr size_t PUSH_POOL_MAX = 2;
    std::vector<YuvFrame> free_frames_;

    // RGBA buffer (private to the Borealis thread, needs no protection).
    // Used on the YUV420P fallback path (CPU NEON conversion). Unused for NV12
    // (Phase 2A - rendered directly by a GL shader).
    std::vector<uint8_t> rgba_buffer;
    int rgba_w = 0, rgba_h = 0;
    bool need_upload = false;
    int  nvg_image = -1;

    // Custom GL renderer for NV12 (Phase 2A - saves the NEON YUV->RGB pass).
    GLVideoRenderer gl_renderer;
#if SHADOW_HAS_GXM_VIDEO
    GxmVideoRenderer gxm_renderer;
#endif

    // Stats
    uint32_t frames_received = 0;
    uint32_t frames_drawn    = 0;
    /* L5 - clock of the draw-rate measurement. Distinct from `last_draw_us`,
     * which is only fed under `show_perf_stats && hud_.visible()`: a counter
     * that is only WRITTEN while someone is looking at it can say nothing
     * about a session where nobody opened the panel. */
    int64_t  lat_last_draw_us = 0;

    // Sliding stats window - each sample is one drawn frame. 60 samples = 1 s.
    // We track FPS, frame latency and inter-frame interval (jitter).
    static constexpr int STAT_WINDOW = 60;
    struct StatWindow {
        float samples[STAT_WINDOW] = {0};
        int   idx = 0;
        int   count = 0;
        void push(float v) {
            samples[idx] = v;
            idx = (idx + 1) % STAT_WINDOW;
            if (count < STAT_WINDOW) count++;
        }
        float mean() const {
            if (count == 0) return 0;
            float s = 0;
            for (int i = 0; i < count; i++) s += samples[i];
            return s / count;
        }
        float stddev() const {
            if (count < 2) return 0;
            float m = mean(), v = 0;
            for (int i = 0; i < count; i++) {
                float d = samples[i] - m;
                v += d * d;
            }
            return sqrtf(v / (count - 1));
        }
        float min_v() const {
            if (count == 0) return 0;
            float m = samples[0];
            for (int i = 1; i < count; i++) if (samples[i] < m) m = samples[i];
            return m;
        }
        float max_v() const {
            if (count == 0) return 0;
            float m = samples[0];
            for (int i = 1; i < count; i++) if (samples[i] > m) m = samples[i];
            return m;
        }
    };

    StatWindow stat_fps;       // instantaneous FPS
    /* VI5 2026-09-11 - the DISPLAY WAIT of each new picture (pushed -> popped),
     * the very sample [L5] video/file-aff gets: one per new picture, fed in
     * draw() whether the panel is open or not. It used to be the age of the
     * picture on screen, re-sampled on every draw while the panel was open. */
    StatWindow stat_latency;
    StatWindow stat_interval;  // ms between 2 draws

    int64_t last_draw_us = 0;

    /* The display refresh pacing (500 ms, without which the decimals dance
     * around) is now handled by ui::Hud itself. */

    // === Metrics panel and pause menu ===
    // Both are shared frameworks (clients/borealis/ui/): the view owns the object and
    // draws it, the content is declared elsewhere. The menu carries its own
    // actions - there is no longer an index enum to keep in sync with a switch
    // in the activity.
    bool            show_perf_stats = false;  // master switch of the panel
    ui::Hud         hud_;
    ui::OverlayMenu pause_menu_;
    bool            pad_test_ = false;
    bool            mouse_test_ = false;   /* PM5 */
    bool            net_test_   = false;   /* S119: the link's quality */
    /* Measured rates. They answer a question the panel used to blur together:
     * "frames per second" mixed the draw loop with the video, when a machine
     * can perfectly well render a 30 fps stream at 60 Hz. */
    uint32_t        frames_presented_ = 0;    // NEW frames actually put on screen
    uint64_t        nv12_seq_ = 0;            // HO-1: number of the picture in consumer_slot, the renderer's upload key
    ui::RateMeter   rate_received_;           // frames received from the server
    ui::RateMeter   rate_decoded_;            // frames out of the decoder
    ui::RateMeter   rate_presented_;          // new frames displayed
    ui::RateMeter   rate_bytes_;              // video bytes -> bitrate
    ui::RateMeter   rate_packets_;            // video packets
    ui::RateMeter   rate_audio_;              // Opus frames played
    ui::RateMeter   rate_audio_bytes_;        // Opus bytes -> bitrate
    float           hud_audio_kbps_  = 0.0f;
    audio_loss_win_t aud_loss_win_ = {};       // AUD-INS-1: last 10 s of (accepted, lost) audio frames
    float           hud_live_kbps_   = 0.0f;  // bitrate derived from rate_bytes_
    float           hud_pps_         = 0.0f;  // packet rate
    void            setupHud();               // declares the sections, once
    bool            devui_installed_ = false; // dev bar, per view

public:
    ui::Hud         &hud()       { return hud_; }
    ui::OverlayMenu &pauseMenu() { return pause_menu_; }

    /* === S73 2026-08-27 - THE GAMEPAD TESTER, FROM INSIDE THE STREAM ===
     *
     * It only lived in the Gamepad screen, hence OUTSIDE any session. Yet it
     * compares what the console reads with what we SEND - and what we send only
     * exists while a stream is running. Its right-hand half was therefore always
     * dark: the tester could not do the one thing it exists for.
     * It is now also an overlay of the stream, where the comparison finally
     * means something. */
    /* PM5 - the controller-mouse tester, hosted exactly like the gamepad one:
     * same shape, same exit, and the same reason for being reachable from the
     * pause menu - a sensitivity is judged with the controller in hand, during
     * a session. */
    bool mouseTestOpen() const { return mouse_test_; }
    void openMouseTest()       { mouse_test_ = true; pause_menu_.close(); }
    bool closeMouseTest()      { if (!mouse_test_) return false; mouse_test_ = false; return true; }

    /* S119 - the link-quality page, same pattern as the two testers. The
     * SAMPLING does not go through here: it runs in `draw` whether the page is
     * open or not, without which capacity would only be measured while someone
     * is looking at it - the defect this repo has fixed four times. */
    bool netTestOpen() const   { return net_test_; }
    void openNetTest()         { net_test_ = true; pause_menu_.close(); }
    bool closeNetTest()        { if (!net_test_) return false; net_test_ = false; return true; }

    bool padTestOpen() const { return pad_test_; }
    void openPadTest()       { pad_test_ = true; pause_menu_.close(); }
    bool closePadTest()       { if (!pad_test_) return false; pad_test_ = false; return true; }

    void openMenu()         { pause_menu_.open(); }
    void closeMenu()        { pause_menu_.close(); }
    bool isMenuOpen() const { return pause_menu_.isOpen(); }
    void menuUp()           { pause_menu_.up(); }
    void menuDown()         { pause_menu_.down(); }
    void menuLeft()         { pause_menu_.left(); }
    void menuRight()        { pause_menu_.right(); }
    void menuActivate()     { pause_menu_.activate(); }
    /* S106 - Y: the detail of the focused entry, if it has one. */
    bool menuDetail()       { return pause_menu_.detail(); }
    /* B: goes up one page, and only closes the menu at the root. */
    void menuBack()         { if (!pause_menu_.back()) pause_menu_.close(); }
    void menuToggleStats();    // toggle + persist through Settings

    /* L14 - wait for vsync during the stream only (`SHADOW_VSYNC=0`).
     * Trades ~8 ms against possible tearing; no effect by default. */
    void setStreamVsync(bool active);

    /* S93 - drawing area of the panel, remembered from the previous frame so a
     * finger can be turned into a block position. */
    float hud_zone_[4] = {0, 0, 0, 0};
    /* Phase of the placement gesture (0 press, 1 move, 2 release). A member and
     * not a function `static`: this is VIEW state, and that family has caused
     * enough failures in this repo already. */
    int   edit_phase_ = 2;
    /* S97 - the touch press hit a row exactly once; the release activates that
     * choice without questioning it again. */
    bool  menu_touch_hit_ = false;
    /* PM1 - edge detection for the L3+R3 combination, and the previous state of
     * the mouse mode. MEMBERS, not function `static`s: this is VIEW state, and
     * a `static` here would survive a second stream view in the same process -
     * the defect family this repo names first. */
    bool  pad_mouse_combo_ = false;
    bool  pad_mouse_was_active_ = false;
    /* S97 - held direction and next repeat, for the pause menu. */
    int     nav_dir_x_ = 0, nav_dir_y_ = 0;
    /* DEMO-1 - the pause menu opened once, N s into a demo stream. */
    double  demo_menu_t0_ = -1.0;
    bool    demo_menu_done_ = false;
    int64_t nav_next_ms_ = 0;


    /* Toggles one panel section and persists the choice. */
    void toggleHudSection(size_t i);
    /* Toggles one chart and persists the choice. */
    void toggleHudChart(size_t i);
    void toggleHudRow(size_t section, size_t line);
    bool perfStatsEnabled() const { return show_perf_stats; }

    /* S93 - position editing mode: blocks are grabbed with a finger. Public
     * because the pause menu drives them, like the neighbouring toggles. */
    void editPositions(bool on);
    bool editPositionsMode() const { return hud_.editing(); }
    void reinitPositions();
    /* Information rows of the menus. */
    std::string videoFormatText() const;
    std::string videoRateText() const;
    /* Cycles off -> absolute -> relative; `dir` = -1 goes back up. Persists. */
    void menuCycleTouchMode(int dir = +1);

    // === On-screen keyboard overlay ===
    /* The page goes back to LETTERS on every opening: reopening the keyboard to
     * type something and landing on the numeric pad because that is where you
     * were last time is a surprise, not a convenience. The modifiers are
     * disarmed for the same reason - a forgotten Ctrl would turn a keystroke
     * into a command. */
    void toggleKeyboard()       { kbd_open = !kbd_open; kbd_shift = false;
                                  kbd_last_touched = -1; kbd_page = 0;
                                  kbd_mod_ctrl = false; kbd_mod_alt = false;
                                  kbd_mod_ctrl_on = false; kbd_mod_alt_on = false;
                                  kbd_press_idx = -1; kbd_popup = -1; }

    /* === K8 - THE KEYBOARD HEIGHT IS SHARED ===
     * Two things depend on it: the drawing, and the guard in the touch block
     * that decides whether a finger belongs to the keyboard or to the stream.
     * Computing them separately would let them drift by a pixel on the first
     * tweak, and the symptom would be a dead band or a phantom click along the
     * border. One single source, therefore.
     *
     * The guard is evaluated BEFORE the drawing within the same frame: that is
     * why the value is COMPUTED rather than remembered at draw time - otherwise
     * it would be one frame stale and the first key pressed after opening the
     * keyboard would go to the stream instead. */
    static constexpr float KBD_FRACTION = 0.46f;
    static float kbdTop(float y, float height) { return y + height * (1.0f - KBD_FRACTION); }

    /* Runs the action bound to a gesture. Pulled out of the body of draw(): it
     * has eleven cases, and leaving them inline would make the touch loop
     * unreadable. */
    void executeGesture(const gesture_evt &e, bool has_session);
    bool isKeyboardOpen() const { return kbd_open; }

private:
    bool kbd_open       = false;
    int  kbd_page       = 0;    /* current tab: letters / nav / F / numpad */
    /* Finger tracking for the touch handling of the pause menu. */
    float menu_touch_x_ = 0.0f, menu_touch_y_ = 0.0f;
    bool  menu_touch_down_ = false;
    bool kbd_shift      = false;
    /* K1: indicator lights of the latched modifiers (Ctrl / Alt). */
    bool kbd_mod_ctrl_on = false;
    bool kbd_mod_alt_on  = false;
    int  kbd_last_touched = -1;
    /* === K9 - long press and accented variants ===
     * The press must be TRACKED over time, which edge detection could not do:
     * it only ever knew "a new key, or not". */
    int      kbd_press_idx = -1;   /* key under the finger since it went down */
    uint64_t kbd_press_us  = 0;    /* moment the finger went down */
    int      kbd_popup     = -1;   /* key whose variants are open */
    int      kbd_variant   = -1;   /* variant under the finger, -1 = none */
    /* A finger that slid below the keyboard band is NOT a lifted finger. As
     * long as it stays down, the gesture recognizer is frozen: otherwise it
     * reads a release, and a short release is a TAP, hence a click sent to the
     * VM that nobody asked for. */
    bool     touch_kbd_frozen_ = false;
    /* Armed modifiers. MEMBERS and not function `static`s: this repo has paid
     * for four failures caused by session state hidden in a `static`. Here the
     * symptom was twofold - an armed Ctrl outlived the session, and the
     * indicator (already a member) could disagree with the real state
     * depending on which page was displayed. */
    bool     kbd_mod_ctrl = false;
    bool     kbd_mod_alt  = false;

    // === Touch input -> mouse driver ===
    // Detects taps (single = click, two fingers = double click) and computes
    // either the deltas (relative) or the normalized position (absolute) to
    // drive shadow-input. State is sampled on every draw() (60Hz).
    int  touch_prev_count   = 0;
    int  touch_prev_x       = 0;
    int  touch_prev_y       = 0;
    uint64_t touch_press_us = 0;   // start of the current tap (0 = no finger)
    bool touch_drag_active  = false;  // true once we emitted mouse_button DOWN (long-press / drag mode)
    int  touch_total_dx     = 0;   // drag accumulated during a tap (tells tap from drag)
    int  touch_total_dy     = 0;

    /* Gesture recognition - a PURE module, tested offline
     * (tests/test_ui_gestes.c). Everything delicate lives there: the wobbling
     * finger count, the double-tap window, the latency it costs. */
    gestures_t gestures_{};
    bool     gestures_ready_ = false;

    // === Plus (Start) button - short vs long press ===
    // Short press (< 500 ms, release) -> sends Options/Start to the DS4 gamepad
    // Long press (>= 500 ms, hold)    -> opens the Shadow menu
    // Handled in draw(), by polling padGetButtons.
    bool      plus_was_pressed   = false;
    uint64_t  plus_pressed_us    = 0;
    bool      plus_long_consumed = false;  // a hold >= 500 ms already opened the menu
};

// C-callable API so that the decoder (pure C) can push frames to the active
// StreamView. One stream at a time (singleton).
extern "C" {
void stream_view_set_active(StreamView* view);
void stream_view_push_yuv(int width, int height,
                          const uint8_t* data_y, int linesize_y,
                          const uint8_t* data_u, int linesize_u,
                          const uint8_t* data_v, int linesize_v,
                          int format,
                          int64_t pts,
                          void* user);
}
