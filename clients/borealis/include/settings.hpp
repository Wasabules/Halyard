// Settings - persisted on the SD card in /switch/halyard/settings.txt
// A simple format: one "key=value" line per option. Avoids cJSON to stay light.

#pragma once

#include "../device_caps.h"

#include <cstdint>
#include <string>

/* S90 - forward declaration: the full header (`streaming/eq.h`) is only pulled
 * in by the .cpp files that really manipulate bands. */
struct eq_band_s;

enum class TouchMode {
    Off = 0,        // Touch is not used for the cursor (the overlay keyboard only)
    Absolute = 1,   // Touch position → mouse position absolue
    Relative = 2,   // Touch delta → mouse delta (trackpad style)
};

/* === Actions assignable to a touch gesture (2026-08-27) ===
 *
 * `None` has TWO meanings depending on the row, and that is deliberate:
 *   - on a tap or a long press: do nothing;
 *   - on a DOUBLE tap: do nothing OF ITS OWN, hence let the single tap repeat.
 *     That is the default case, and the only one that adds NO latency - as soon
 *     as a double has an action of its own, the single tap has to wait out the
 *     window (0.4 s) to know whether a double is coming. The settings screen says
 *     so on the row concerned. */
enum class GestureAction : uint8_t {
    None = 0,
    LeftClick,
    RightClick,
    MiddleClick,
    DoubleClick,
    Drag,          /* holds the left button down until release */
    Keyboard,      /* virtual keyboard */
    PauseMenu,
    Escape,
    Windows,
    AltTab,
    Count
};

/* Indexed by `gesture_t` from ui/gestures.h - same order, same size
 * (GESTURE_COUNT). Index 0 (GESTURE_NONE) is never read; it is there so the
 * indices line up with no offset to compute. */
#define SETTINGS_GESTURES_COUNT 10

class Settings {
public:
    // Shows the perf overlay in the top right during the stream
    bool show_perf_stats = false;

    /* === L14 2026-08-29 - WAITING FOR THE SCAN-OUT, OR NOT, DURING THE STREAM ===
     *
     * `video/cadence` is measured at exactly 60.0 Hz, without a single missed
     * frame: the screen's period is the first cost of the video chain, and
     * Borealis is already at its minimum (two presentation buffers, swap
     * interval 1). The only lever left is to stop waiting.
     *
     * This is not a free gain: the picture changes mid-scan and the tear shows.
     * The average gain is HALF a period (~8 ms) - the switch lands anywhere in
     * the scan. It has to be judged by eye, on moving content; hence a setting
     * rather than a default.
     *
     * Applies ONLY during the stream: a UI that tears while scrolling a menu
     * would be absurd. */
    bool vsync_stream = true;

    /* Fills the whole screen instead of keeping the aspect ratio.
     *
     * It existed only as `SHADOW_STRETCH`, read ONCE into a function `static` on
     * the first frame, and toggled from `devui/dev_menu.cpp` - which is excluded
     * from the Switch build. So on console the behaviour was reachable by no
     * means at all: neither a menu nor, in practice, the variable, since the
     * value was frozen before the first picture arrived.
     *
     * Off by default: distorting the picture is a deliberate trade, worth it
     * against the black bars of a 1920x540 crop and wrong the rest of the
     * time. */
    bool stretch_image = false;

    /* === THE APPLICATION LOCK RE-ARMS WHEN THE CONSOLE WAKES ===
     *
     * Borealis asks HOS not to suspend us (`AppletFocusHandlingMode_NoSuspend`),
     * so pressing HOME and coming back keeps the SAME process and the same main
     * loop. A lock that only gated startup would therefore be answered once and
     * never asked again for the whole life of the process - HOME out, HOME in,
     * and you are inside somebody else's session.
     *
     * On by default: a lock that can be walked past is worse than no lock,
     * because it is trusted. The setting exists because re-entering a pattern
     * every time you check a notification is a real cost, and it is the owner's
     * to weigh - not ours.
     *
     * Does nothing when no lock is configured. */
    bool lock_on_wake = true;

    // Switch touchscreen mode for driving the cursor
    TouchMode touch_mode = TouchMode::Relative;

    // UI language. Empty = follow the console (or BOREALIS_LANG on the
    // desktop). Otherwise a Borealis locale code ("fr", "en-US", ...).
    // Applied at startup: the catalogues are loaded once.
    std::string language;

    // Mapping of console buttons -> Shadow gamepad, "3,2,1,..." in
    // padmap::Btn order. Empty = the default table.
    std::string pad_map;
    bool        pad_invert_y = false;   // vertical stick axis
    uint32_t    pad_deadzone = 10;      // stick dead zone, as a percentage

    /* === THE REAR TOUCHPAD, PS VITA ===================================
     *
     * The console has no ZL, no ZR and no stick clicks - four of the sixteen
     * buttons a game can bind, and the four a PC game puts aiming, sprinting
     * and melee on. The rear panel sits under the fingers that would press
     * them.
     *
     * Stored as small integers rather than as the enums of `rear_touch.h`,
     * because this file is read from and written to a text file on a memory
     * card: a number survives a header being reordered, an enum name does not.
     * The mapping is in `rear_touch.h`, which is also where the meaning lives.
     *
     * OFF by default. It changes what the pad does, and a control that starts
     * doing something you did not ask for is worse than one you have to find. */
    bool     rear_touch = false;
    uint32_t rear_zone_tl = 5;     // rear_action: 5 = ZL analog
    uint32_t rear_zone_tr = 6;     //              6 = ZR analog
    uint32_t rear_zone_bl = 0;     // the lower half is where the palms rest:
    uint32_t rear_zone_br = 0;     // unmapped by default, deliberately
    uint32_t rear_gest_tl = 2;     // rear_gesture: 2 = slide
    uint32_t rear_gest_tr = 2;
    uint32_t rear_gest_bl = 0;     //               0 = tap
    uint32_t rear_gest_br = 0;
    uint32_t rear_two_finger = 4;  // 4 = R3, the click a right stick carries
    uint32_t rear_slide_pct = 28;  // travel for a full pull, % of the panel

    /* === PM1 2026-09-02 - THE JOY-CONS AS A WIRELESS MOUSE ===
     *
     * Two ways of pointing, because neither is right for everything: the sticks
     * are steadier and need no space, the gyroscope is far more precise for
     * aiming but asks you to move the console. The console cannot know which one
     * the remote content wants, so it is a choice - the same reasoning as the
     * cursor source (S113).
     *
     * `pad_mouse_mode` is the PREFERRED mode, not the active one. Turning the
     * mouse on and off - by the button combination or the pause menu - does not
     * rewrite it: coming back you get the mode you chose, not the last one that
     * happened to be running. The active mode is session state and lives in
     * `padmouse`, where it belongs.
     *
     * 0 = sticks, 1 = sticks, 2 = gyroscope. Zero is not "off": whether the
     * mouse is running is not a setting, it is a gesture. */
    uint32_t pad_mouse_mode = 1;

    /* 10..400 %. The reference (100) crosses a 1920-wide screen in about two
     * seconds at full deflection. */
    uint32_t pad_mouse_sens = 100;

    /* Inverts the vertical axis, for the pointer only. Separate from
     * `pad_invert_y`, which describes the sticks FORWARDED to the VM: the two
     * answer different questions and sharing one field would mean changing an
     * aiming preference by changing a mouse one. */
    bool     pad_mouse_invert_y = false;

    /* Deflection-seconds for one wheel notch. 100 = one second at full
     * deflection; lower scrolls faster. */
    uint32_t pad_mouse_scroll_step = 100;

    /* === PM8 2026-09-02 - WHICH JOY-CON AIMS ===
     *
     * BOTH Joy-Cons carry a gyroscope - that is how a game uses one in each
     * hand. We were reading only the first sensor the service handed us, which
     * on a detached pair is always the LEFT one: turning the right Joy-Con did
     * nothing, and nothing said why.
     *
     * 0 automatic  the one that is moving, with hysteresis
     * 1 left       2 right
     * 3 both       the AVERAGE of the two, not their sum: held as a pair the
     *              sensors measure the SAME rotation, so summing would double a
     *              gesture that has not doubled
     * 4 split      right turns for the horizontal, left tilts for the vertical
     * 5 split      the other way round
     *
     * Automatic is the default and it is not a dodge: in handheld mode both
     * halves measure the same rotation, so the question does not arise, and on a
     * detached pair the user aims with one hand that we cannot name in advance.
     * The explicit choices exist because "the one that moves" is a heuristic,
     * and a heuristic that guesses wrong needs an off switch.
     *
     * The pairings (3, 4, 5) need a detached pair. Asked for with a single
     * Joy-Con they DEGRADE to whichever sensor answers - a setting that cannot
     * be honoured must fall back, not turn the feature off in silence. */
    uint32_t pad_mouse_gyro_source = 0;

    // Refresh rate of the panel's text, in milliseconds.
    // The measurements are moving averages: reformatting them every frame
    // would make the decimals dance. 500 ms is readable, 250 is snappier.
    uint32_t hud_refresh_ms = 500;
    /* S97 - opacity of the measurement blocks, 10..100 %. A hundred by default:
     * the panel exists to be read, and a translucent default would make one
     * doubt the display before knowing a setting exists. */
    uint32_t hud_opacity = 100;

    /* S105 - strength of the UI's haptic feedback, 0..100. Zero = off. Sixty by
     * default: enough to be felt, little enough not to turn navigation into a
     * shudder. No effect during the stream, where the remote machine already
     * drives the motors. */
    uint32_t haptics_strength = 60;

    /* S110 - demo pointer (desktop): replaces the cursor with a disc that marks the
     * click, so screen captures show the GESTURE and not only its result. Off by
     * default: it is a demonstration tool, not an everyday setting. */
    bool demo_pointer = false;

    /* S113 2026-08-29 - WHICH CURSOR TO DRAW, AND WHY IT HAS TO BE A CHOICE
     *
     * Three possible sources, and none of them is right in every case:
     *   0 = NONE. The video stream already carries the pointer Windows composites,
     *       in the right place since it comes from the server. That has been the
     *       default since S77, where the user reported "two overlapping cursors"
     *       and turning OUR two renderings off left exactly one.
     *   1 = THE IMAGE RECEIVED on `:base+20`, the VM's real pointer bitmap. It is
     *       drawn at the position WE track, so it separates from the composited
     *       cursor as soon as a game recentres the mouse (SHADOW_INPUT_ABS). But
     *       when the VM composites nothing - full screen, some games - it is the
     *       ONLY pointer available.
     *   2 = A synthetic ARROW. A crude fallback, useful when the `:base+20`
     *       channel does not open.
     *
     * Why a setting and not a default: S77 decided on ONE machine and ONE piece
     * of content. The right choice depends on what runs inside the VM, which the
     * client cannot know. The `SHADOW_CURSOR_SPRITE` / `_ARROW` toggles already
     * existed but are environment variables - hence UNREACHABLE on console, where
     * there is no shell. This is exactly the "a capability that exists with no
     * setting" family fixed on 2026-08-27 for six others. The variables keep
     * priority when they are defined. */
    /* The VM's REAL pointer, which the `:base+20` channel has been delivering
     * since §3.37. Off by default made sense while the images were being
     * discarded; now that they are decoded, a remote desktop with no pointer is
     * simply unusable - you cannot see what you are about to click. */
    uint32_t cursor_source = 1;   /* 0 = none, 1 = the received image, 2 = an arrow */

    // Magnification of the metrics panel and the stream menus, as a
    // percentage. 100 = the reference size (that of a 1280-wide window).
    /* === THE PAUSE MENU ON A 544-LINE PANEL ==========================
     * 100 % is right for a television and for a 720p handheld; on the Vita's
     * 960x544 it fills the screen. 75 % is the first rung of the same ladder
     * the settings screen offers, so it is a value the user can see and undo.
     * It drives the HUD, the pause menu and the dev bar together - they are
     * one overlay and must stay in proportion. */
#if defined(__vita__) || defined(__psp2__)
    uint32_t ui_scale = 75;
#else
    uint32_t ui_scale = 100;
#endif

    // Active charts, one bit per series. The sentinel tells "never
    // chosen" apart from "all off", which is a legitimate setting.
    static const uint32_t CHARTS_UNSET = 0xFFFFFFFFu;
    uint32_t hud_charts = CHARTS_UNSET;

    // Visible sections of the metrics panel, one bit per section in
    // declaration order (StreamView::setupHud). 0 = never chosen, and we
    // then fall back on a default set.
    uint32_t hud_sections = 0;

    /* S93 - position of each panel block, "id:x,y;...", as fractions of the video
     * area. Empty = the default layout (stacked on the right). Fractions rather
     * than pixels: the console draws at 720p in handheld mode and 1080p on the
     * dock, and pixels would send the blocks off screen when the mode changes. */
    std::string hud_positions;
    /* S106 - hidden rows of the panel, "sectionId/rowId;...". The identifiers are
     * explicit and untranslated: changing language must not forget the choices,
     * and neither must a rewording. */
    std::string hud_rows;

    /* === S107 2026-08-29 - PLAY TIME, BECAUSE IT IS CAPPED ===
     *
     * Shadow limits the number of hours. Not knowing where you stand from the
     * console is a real gap, not a comfort.
     *
     * THIS COUNT IS OURS, NOT SHADOW'S. It only sees the sessions launched from
     * this console: playing from a PC or the browser does not increment it. And
     * if Shadow bills VM-ON time rather than client-session time, the two will
     * diverge. The screen says so rather than let it pass for an official
     * reading - a number you believe is official and is not is worse than no
     * number.
     *
     * The periods carry THEIR DATE: a monthly counter that does not know when it
     * was reset keeps running after a skipped month, and two months then add up
     * without anyone seeing it. */
    uint32_t time_total_s = 0;
    uint32_t time_month_s  = 0;
    uint32_t time_day_s  = 0;
    uint32_t time_day    = 0;   /* AAAAMMJJ de `temps_jour_s` */
    uint32_t time_month    = 0;   /* AAAAMM   de `temps_mois_s` */

    /* Adds a finished session, and rolls the periods over if the date has
     * changed since the last one. Saves. */
    void addSessionTime(uint32_t seconds);

    // B1 2026-05-18 - Quality settings (= the Q1 params, persisted).
    // 0 = use the defaults (= except the resolution, which has an explicit one).
    /* === 20 Mb/s ON A CONSOLE, AND THE NUMBER IS MEASURED ============
     * `0` means "let the client decide", which on these two consoles is not a
     * decision anyone made. Above ~25 Mb/s the picture breaks up in play
     * (KB §9, DEBIT-1: 100 Mb/s breaks, 25 holds) because the server probes
     * beyond what the link carries and loses on every probe. 20 sits under that
     * ceiling with margin, and is what both consoles should ask for unless the
     * user says otherwise. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
    uint32_t max_bitrate_mbps = 20;
#else
    uint32_t max_bitrate_mbps = 0;    // 0 → desktop default 20 Mbps. Sliders : 5..150
#endif
    /* 60, not 144: the Vita's panel runs at 59.94 Hz and the Switch's at 60.
     * Asking for 144 spends encoder, link and decoder on pictures no panel can
     * show - and the frame rate travels ONLY in the channel announcement
     * (CFG-4), so a wrong value is paid for the whole session. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
    uint32_t target_fps       = 60;
#else
    uint32_t target_fps       = 0;    // 0 -> desktop default 144. Selectors: 30/60/90/120/144
#endif
    uint32_t codec            = 0;    // 0 → desktop default 2 (H.264?). 1=H.265?, 3=AV1?
    /* === G56 2026-08-28 - GAMEPAD TYPE ANNOUNCED TO THE VM ===
     * The wire enum, CONFIRMED on console (Steam shows "Xbox 360 Controller" with
     * value 0): Xbox360=0, XboxOne=1, Dualshock4=2, ControllerLeft=3,
     * ControllerRight=4, Default=5. The server refuses anything above 5 - it then
     * returns a kReply with its error flag and creates NO device at all.
     * Default 0: that is XInput, the stack best supported by Windows games, and
     * it is what Shadow's "Improved compatibility" setting does. We used to
     * announce 2 (DualShock 4) - an artefact of the guided capture of 26/08, made
     * with a DS4, not a choice. The Switch is neither. */
    /* === G57d 2026-08-28 - RUMBLE STRENGTH, 0 to 100 % ===
     * 0 = motors off. The protocol sends only amplitudes (bytes 3 and 4, full
     * scale 0-255) and NO duration: turning it off therefore cannot be done by
     * ignoring the messages, a zero has to be sent, otherwise a rumble in
     * progress would hold indefinitely. That is why the scaling lives in
     * `rumble_hid_apply` and not in its callers. */
    uint32_t rumble_strength = 100;
    uint32_t gamepad_type     = 0;
    uint32_t profile_id       = 0;    // 0 → desktop default 1 (speed). 2=reliability/quality
    /* 720p on a console. `auto_resolution` already clamped a handheld to it at
     * connect time, but the SETTING read 1080p - so the screen said one thing
     * and the session did another, which is the shape of defect this repository
     * spends its sessions chasing. PS TV (docked) keeps the clamp's exemption
     * through `auto_resolution`. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
    uint32_t display_width    = 1280;
    uint32_t display_height   = 720;
#else
    uint32_t display_width    = 1920; // 1280/1600/1920/2560
    uint32_t display_height   = 1080; // 720/900/1080/1440
#endif

    // === Ajouts 2026-08-25 ===

    /* === B4 2026-09-02 - VIDEO TRANSPORT: UDP OR TCP ===
     *
     * The official client's `reliability` profile. It was reachable only through
     * `SHADOW_VIDEO_NET_TCP`, i.e. by dropping a file on the SD card - which on
     * a console means not at all. It is a real choice (KB §9, K15/K16), so it
     * gets a real setting.
     *
     * WHAT IT COSTS, AND WHY THE DEFAULT IS UDP. In TCP mode the server refuses
     * `EnableDynamicBitrate`: the ADAPTIVE BITRATE IS OFF, and the stream runs
     * at whatever was negotiated. Application-level retransmission (`rG`),
     * the SUFP chunking and the UDP receive buffer all become inert - TCP
     * retransmits by itself. Measured against it: UDP loss on this console is
     * 0.008 to 0.099 %, about one truncated frame a minute, with no receive
     * overflow at all.
     *
     * What it buys: no loss at all, on a link where loss is what breaks the
     * picture. Worth trying on a saturated Wi-Fi; not worth leaving on by
     * default. */
    bool video_tcp = false;

    // Forwarding the gamepad to the VM. Opt-in: the transfer is not validated
    // end to end (the server acknowledges, Steam sees nothing), and a wrongly
    // announced gamepad flips games into gamepad mode, where they then ignore
    // keyboard and mouse.
    /* === ON BY DEFAULT WHERE THE PAD IS THE MACHINE ==================
     * G45's rule - announce a gamepad only if there is one - was written for a
     * desktop, where announcing a pad nobody has flips games into gamepad mode
     * and makes them ignore the keyboard. A Switch and a Vita ARE a gamepad:
     * `ctrl_gamepad_present()` returns true there unconditionally, and the
     * question the rule guards against cannot arise.
     * Off, the setting sets SHADOW_GAMEPAD_PLUG=0, which FORBIDS the
     * announcement - so the console shipped with its own pad refused. */
#if defined(__SWITCH__) || defined(__vita__) || defined(__psp2__)
    bool gamepad_enabled = true;
#else
    bool gamepad_enabled = false;
#endif

    // (The "follow the console / force a language" choice already exists:
    //  an empty `language` = follow. No second flag, so as not to create two
    //  sources of truth for the same question.)

    // Virtual keyboard layout. Changes the displayed keys AND the emitted
    // scancodes: an A key on AZERTY is physically the Q key, and it is the
    // physical SCANCODE the VM expects.
    enum class KbLayout : uint32_t { Azerty = 0, Qwerty = 1 };
    KbLayout kb_layout = KbLayout::Azerty;

    // Resolution following the console's mode: 1080p on the dock, 720p in
    // handheld. The built-in screen is 1280x720 - asking for 1080p in handheld
    // encodes and then transmits twice as many pixels for nothing.
    bool auto_resolution = true;

    // A separate bitrate per link. The Switch's Wi-Fi tops out well below the
    // dock adapter's Ethernet, and a single setting forces a choice between
    // saturating one and throttling the other. 0 = follow
    // `max_bitrate_mbps`.
    bool     bitrate_per_link  = false;
    uint32_t bitrate_wifi_mbps = 15;
    uint32_t bitrate_eth_mbps  = 40;

    // Remote mouse mode.
    //
    // false (default) = ABSOLUTE positions: correct on a desktop, where the
    //   cursor must land exactly where you point.
    // true = relative DELTAS: indispensable as soon as a game captures the mouse.
    //   A game like Skyrim recentres the cursor every frame; our absolute
    //   positions, computed from our own tracking which ignores that
    //   recentring, then produce enormous and random offsets - the mouse goes
    //   "a hundred times too fast in every direction".
    //
    // Adjustable mid-game from the pause menu: that is where you notice it, and
    // quitting the game to go into the settings would make no sense.
    // SHADOW_INPUT_ABS keeps priority when it is defined.
    bool mouse_relative = false;

    // UDP registration on the input channel `:base+13`.
    //
    // Video (`+10`) and cursor (`+30`) both send the 25-byte registration
    // packet carrying the authentication hash; the input channel is the ONLY
    // one that sends nothing. The default was set on a hypothesis that was never
    // confirmed - "our unexpected packet invalidates the channel" - and since
    // then the server acknowledges the gamepad without ever creating a device in
    // the remote machine.
    //
    // If the server needs that registration to associate our source port with
    // the session, our gamepad packets arrive anonymous and are dropped: that
    // matches the symptom exactly. To try: turn it on, and check that mouse and
    // keyboard keep working - that is what
    // l'hypothese d'origine redoutait.
    bool udp_register_input = false;

    // Sensitivity of touch pointing, as a percentage.
    //
    // In relative mode, a finger travelling 100 pixels sends a movement of 100 -
    // to which the game applies its OWN sensitivity on top. On a 6-inch screen
    // the gesture is short, so the effective gain is enormous and aiming becomes
    // impossible: that is the "the mouse goes a hundred times too fast" observed
    // in Skyrim, once relative mode was already on.
    //
    // 100 = one finger pixel for one mouse pixel. Below that the gesture is
    // damped. The remainder of the division is carried over from one frame to the
    // next, without which small movements would be purely and simply lost - and
    // that is what matters for fine aiming.
    uint32_t touch_sensitivity = 100;   // 10 a 200



    /* Direction of two-finger scrolling. By default we follow the TOUCHSCREEN
     * convention: the content follows the finger, so sliding up moves down
     * through the document. People used to the Windows touchpad expect the
     * opposite - it is a 50/50 split, hence a toggle rather than an imposed
     * choice. */
    bool touch_scroll_invert = false;

    /* === K14 - AUDIO QUALITY: it is a CODEC, not a bitrate ===
     * "Standard" = Opus (lossy, ~100 kbit/s), "High fidelity" = FLAC (lossless,
     * ~800-900 kbit/s). The account already allows it.
     * The codec is negotiated AT SESSION OPEN: changing this setting only takes
     * effect on the next connection, and the screen says so. */
    bool audio_hifi = false;

    /* === K17 - 4:4:4 COLOUR ("high color fidelity") ===
     * Field 5 of the video announcement, a BOOLEAN. There is no "chroma" field on
     * the wire at all: it is this one bit.
     * IMPOSSIBLE ON SWITCH, and this is not a setting to be found: the Tegra X1's
     * NVDEC decodes 4:4:4 on NO codec, and the installed FFmpeg does not even
     * offer the pixel format. So the default stays false, and the screen explains
     * it rather than letting someone try. */
    bool color_444 = false;

    /* Gesture -> action mapping. The default values reproduce a touchpad's
     * convention: one finger clicks, two fingers right-click, a long press
     * drags. */
    uint8_t gesture_action[SETTINGS_GESTURES_COUNT] = {
        (uint8_t)GestureAction::None,        /* 0 — inutilise */
        (uint8_t)GestureAction::LeftClick,  /* tape 1 doigt */
        (uint8_t)GestureAction::None,        /* double, 1 finger (= repeats the tap) */
        (uint8_t)GestureAction::Drag,     /* long 1 doigt */
        (uint8_t)GestureAction::RightClick,   /* tape 2 doigts */
        (uint8_t)GestureAction::None,        /* double 2 doigts */
        (uint8_t)GestureAction::None,        /* long 2 doigts */
        (uint8_t)GestureAction::Keyboard,     /* tape 3 doigts */
        (uint8_t)GestureAction::None,        /* double 3 doigts */
        (uint8_t)GestureAction::PauseMenu,   /* long 3 doigts */
    };

    /* Output volume, as a percentage. Above 100 it is a BOOST: the signal exceeds
     * what the source contained and the loud passages clip. That is deliberate -
     * a remote machine whose Windows mixer is low stays inaudible otherwise - but
     * it is why 100 remains the default.
     * Applied in webrtc/audio.c, bounded by streaming/audio_gain.h. */
    uint32_t audio_volume = 100;        // 0 a 300

    /* Connect by itself when the account has only one machine. Handy day to day,
     * annoying as soon as you want to reach the settings or change machine: you
     * are back in a session before you could click. */
    bool auto_connect = true;

    /* === Behaviour on LOSS OF FOCUS (2026-08-27) ===
     * `AppletFocusState != InFocus` does not only mean "the console is asleep":
     * pressing HOME causes the same thing. Until now any loss of focus cut the
     * session without saying anything - surprising the first time, and invisible
     * since it was not configurable.
     * `false` = we TRY to keep the session. It is a bet: HOS can kill a
     * backgrounded application, and resuming is not guaranteed. Hence the
     * cautious default. */
    bool cut_when_unfocused = true;

    /* Automatic reconnection when the stream dies without being asked to. The
     * native path - the console's - had NONE: only the abandoned WebRTC branch
     * had one. And the video UDP sometimes dies mid-session with the server
     * alive (KB §3.34). */
    bool auto_reconnect = true;
    uint32_t reconnect_max = 3;      /* attempts before giving up */

    /* HARDWARE video decoding (NVDEC). The biggest performance lever, and it had
     * no setting: it could only be changed by an action in the development menu
     * that dropped a marker file. */
    bool hw_decode = true;

    /* Hardware audio decoding (the `hwopus` service). Deliberately opt-in: the two
     * references in the field decode Opus in software, and the gain is paid for
     * with an IPC round trip per packet. See KB A5. */
    bool hw_opus = false;

    /* === S90 2026-08-29 - TONE CORRECTION FOR THE MACHINE'S SOUND ===
     *
     * TWO presets, and that is the point: the problem to correct is not the same
     * depending on the console's mode. On the dock the sound goes to a TV or an
     * amplifier that needs nothing - hence `Flat` by default. In handheld it
     * comes out of two speakers a few centimetres across, incapable of
     * reproducing the bass and clipping when you send them any.
     *
     * A single preset would force a re-choice on every mode change, that is,
     * never. The values are those of `eq_preset_t` (streaming/eq.h).
     *
     * The custom bands are stored separately and SHARED by both modes:
     * duplicating five bands per mode would mean ten rows to set for a user who
     * wants five. So the "Custom" preset is chosen per mode, but it designates
     * the same setting. */
    uint32_t eq_preset_docked     = 0;   /* EQ_PRESET_FLAT */
    uint32_t eq_preset_handheld = 1;   /* EQ_PRESET_HANDHELD */
    /* Five bands: "type,freq,q,gain" separated by semicolons. Empty = never set,
     * and we then start from the handheld preset as a starting point. */
    std::string eq_bands;
    /* Automatic attenuation so no setting can clip. Configurable, because a user
     * who boosts the bass sometimes REALLY wants it louder, and will lower the
     * volume themselves. */
    bool eq_auto_trim = true;

    /* === S88 2026-08-29 - THE UI SOUNDS ===
     *
     * On by default: a console UI that does not answer the finger seems dead, and
     * that is exactly the feedback these sounds provide. But they are heard, so
     * they can be turned off - and the volume is SEPARATE from the stream's: one
     * sets the game's sound and the menus' sound for reasons that have nothing to
     * do with each other.
     *
     * 70 % and not 100: these sounds accompany, they do not announce themselves.
     * The default should be the level at which you stop noticing them. */
    bool     sounds          = true;
    uint32_t sounds_volume   = 70;      /* 0 a 100 */

    /* === S81 2026-08-29 - LOG LEVEL ===
     *
     * Values from `journal_severity_t` (shadow/journal.h): 1 = WARN (failures
     * only), 2 = INFO (default: the milestones and the failures), 3 = DEBUG
     * (periodic measurements, experiments), 4 = TRACE (per packet and per frame).
     *
     * The default is not "everything": the log grows by several megabytes per
     * session, written to an SD card, for a user who plays and will never
     * diagnose anything. Nor is it "nothing": the session milestones fit in a few
     * dozen lines and are exactly what you ask someone to send you when it does
     * not work.
     *
     * `SHADOW_JOURNAL_NIVEAU` keeps priority - the repo's rule for every toggle:
     * `env.txt` serves the A/B runs, and a UI setting must not silently override
     * an experiment in progress. */
    uint32_t log_level = 2;

    /* Command channel for the development machine. It used to be armed by dropping
     * a file over FTP; this toggle allows doing it without a computer. */
    bool dev_channel = false;
    /* True as soon as `dev_channel` has been read from the file or deliberately
     * changed. While it is false we do NOT touch the destination file: otherwise
     * a `false` default would disarm, on the first launch, a channel already in
     * place - which happened, and cut the diagnosis exactly when it was
     * useful. */
    bool dev_channel_known = false;

    /* CANAL-1: when no choice is on file, believe what is on disk - `logsink.txt`
     * present means the channel is armed. Called by `load()`, on BOTH its paths
     * (a settings file that exists, and none at all). */
    void adoptDevChannelFromDisk();

    /* === Translating settings into environment toggles ===
     *
     * Three capabilities - hardware video decoding, hardware Opus decoding, the
     * development channel - live in C that reads them with `getenv` and caches
     * them in a `static`. Plumbing them one by one down to those modules would
     * couple the C to our C++ object for three booleans.
     *
     * So we reuse the repo's experiment mechanism: the settings SET the
     * variables, and the C keeps reading them knowing nothing.
     *
     * To be called VERY EARLY, before the first read: those `getenv` calls are
     * cached on their first invocation, and setting the variable afterwards would
     * have no effect - a setting you believe is applied and is not is worse than
     * no setting at all.
     *
     * `env.txt` keeps PRIORITY: it serves the A/B runs, and a UI setting must not
     * silently override an experiment in progress. */
    void applyToggles() const;

    /* S90 - applies the equaliser matching the console's current mode. */
    void applyEq() const;

    /* === S90b - READING THE BANDS LIVES HERE, AND NOWHERE ELSE ===
     *
     * Three places need it: `applyEq`, the settings screen, and the pause menu.
     * Copying the string parsing into each is a guarantee that one day one of the
     * three will read a format the other two no longer write - and this repo has
     * paid for that mistake three times in a single day (the bounded text, the
     * command parser, the focus guard).
     *
     * `eqReadBands` fills EQ_BANDS entries and returns how many are useful. Zero
     * means "never set": the caller then starts from the handheld preset rather
     * than from silence. */
    int  eqReadBands(struct eq_band_s *out) const;
    void eqWriteBands(const struct eq_band_s *bands);

    /* === B1 2026-09-02 - PUSH THE BITRATE THAT IS ACTUALLY IN FORCE ===
     *
     * Resolves the per-link rule (`streaming/bitrate.h`) against the console's
     * current link and hands the result to the running session. Does nothing
     * when there is no session.
     *
     * It exists because the same quantity was applied by two different paths:
     * the quality screen sent its value live, the two per-link rates waited for
     * the next connection, and the pause menu wrote a third field. Three
     * behaviours for one number, none of them stated on screen - which is what
     * "nothing is coherent" meant. Every screen now calls this.
     *
     * NOT to be called every frame: it reads the network service through
     * `device::linkType()`. Call it when a setting CHANGES. */
    void applyBitrateLive();

    /* The value in force right now, "Auto" left as 0. The screens display it,
     * `applyBitrateLive` sends it, and the connect path uses the same one. */
    uint32_t effectiveBitrateMbps() const;

    /* S90 - to be called every frame: does nothing until the mode changes. That is
     * what makes the preset follow when the console is docked or undocked,
     * without which the application would have to be relaunched. */
    static void followConsoleMode();

    /* === RESET TO THE DEFAULT VALUES ===
     *
     * A session of trials leaves a configuration nobody can name any more, and
     * until now the only way back was to delete the file from a computer - which
     * means in practice that nobody went back, and that the next measurement ran
     * on an unknown configuration. That is the same disease as a phantom
     * measurement: you believe you are testing one thing and you are testing
     * two.
     *
     * The USE COUNTERS are kept. They are a log, not a preference: erasing the
     * hours played to change a bitrate back would be a surprise, and the kind
     * that cannot be undone. Everything else goes back to the value a fresh
     * install has.
     *
     * Saves and applies at once, so the state on screen is the state in
     * force. */
    void resetToDefaults();

    // Singleton
    static Settings& instance();

    // Loads from the SD card; resets to the defaults when absent or unparsable.
    void load();

    // Saves to the SD card. Best-effort, errors ignored.
    void save() const;

private:
    Settings() = default;
};
