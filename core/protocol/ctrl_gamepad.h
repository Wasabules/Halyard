/* ctrl_gamepad - gamepad over the native channel `:base+13`.
 *
 * Format decoded on 2026-08-21 (KB.md §3.25), guided capture plus decryption
 * 238/238 then 2598/2598: chacha20-poly1305 UDP packets `[ct 14][nonce 12]
 * [tag 16]`, encrypted with the upstream key (the one from OUR OWN Encryption
 * request, see KB.md §3.23 - not the one in the reply).
 *
 * 14-byte payload: `04 [b1] [type] ...`
 *   type 00 = button : @11 = identifier, @13 = 1 pressed / 0 released
 *   type 01 = axis   : @11 = index, @12 = value 0-255, @13 = @12 + 128
 *   type 02 = d-pad  : value at @3
 *   type 05 = keepalive (zeroed body, ~7 s)
 * The very first message of a session, `04 01 03 02 ...`, is the plug-in
 * announcement - the native equivalent of the web path's
 * `GamepadPluggedInputV4Model`, which makes the VM create a virtual gamepad.
 */
#pragma once


/* Linkage guard: this header is included from C++ files (Borealis interface).
 * Without it, every include site has to remember to wrap it in `extern "C"`
 * itself - forgetting once gives a link error on a mangled symbol, far from the
 * cause. The guard belongs to the header. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "encryption.h"

/* Button identifiers, full table established by 100% correlation */
enum {
    SHADOW_PAD_SQUARE = 0, SHADOW_PAD_TRIANGLE = 1, SHADOW_PAD_CROSS = 2,
    SHADOW_PAD_CIRCLE = 3, SHADOW_PAD_R3 = 4, SHADOW_PAD_L3 = 5,
    SHADOW_PAD_R1 = 6, SHADOW_PAD_L1 = 7, SHADOW_PAD_START = 8,
    SHADOW_PAD_SELECT = 9, SHADOW_PAD_GUIDE = 10
};

/* Axis indices (value 0-255, 128 is centre for the sticks) */
/* G2 2026-08-21 - the vertical indices were SWAPPED. The correlation put them
 * almost neck and neck (spread 24.7 for one, 25.6 for the other, against 0.1
 * for the horizontal ones) and I had separated them by elimination - the wrong
 * way round. Reported symptom: moving the right stick vertically moved the left
 * stick. SHADOW_PAD_SWAP_Y=1 swaps them back if needed. */
enum {
    SHADOW_AXIS_RX = 0, SHADOW_AXIS_RY = 1, SHADOW_AXIS_LX = 2,
    SHADOW_AXIS_LY = 3, SHADOW_AXIS_L2 = 4, SHADOW_AXIS_R2 = 5
};

/* Attaches the channel: UDP socket already connected to :base+13, plus the
 * session cipher. Sends the plug-in announcement. */
void ctrl_gamepad_attach(int udp_fd, shadow_cipher *cipher);
void ctrl_gamepad_detach(void);
bool ctrl_gamepad_active(void);

/* G45: true if a PHYSICAL gamepad is plugged in (evdev probe, rescanned on each
 * call so hotplug is picked up). ctrl_gamepad_active() only says whether the
 * network channel is open - the menu used to conflate the two. */
bool ctrl_gamepad_present(void);

/* G55 - summary of the downstream side of :base+13, printed even when it is
 * zero. Defined in ctrl_session.c, which owns the counters. */
void ctrl_session_log_gamepad_rx(void);

int ctrl_gamepad_plug(void);

/* G51 - presses a button and releases it ~80 ms later (SHADOW_PAD_PULSE_MS).
 * Use this for any button sent as a pulse: back-to-back press and release leave
 * within the same microsecond and no game ever sees them. */
int ctrl_gamepad_button_pulse(int button_id);

/* What was REALLY sent to the remote machine - axes 0..5 as bytes
 * as they went out (128 = centre), a button mask (bit N = Shadow
 * identifier N), a d-pad mask. Meant for the test screen, which shows it beside what
 * the console READS: it is their divergence that reveals a defect between
 * reading and sending. */
void ctrl_gamepad_last_sent(uint8_t axes[6], uint16_t *buttons, uint8_t *dpad);

/* One readable line describing the state of the gamepad channel: channel open
 * or not, plug-in announcement sent or not (and when), send counters. This is
 * what answers "the gamepad is no longer declared after reconnecting". Meant for
 * the pause menu; it logs nothing itself. */
void ctrl_gamepad_diagnostic(char *buf, size_t cap);

/* P6: repeats the plug-in announcement during the first seconds of a session
 * (the server may not be ready when the first one goes out). */
void ctrl_gamepad_replug_tick(void);
int ctrl_gamepad_button(int button_id, bool pressed);
int ctrl_gamepad_axis(int axis_idx, uint8_t value);
/* SRV6 2026-10-02 - bytes 12..13 of an axis message are ONE little-endian
 * int16, not a value plus a second representation: the 8-bit call above encodes
 * `257*value + 32768` without having meant to. This variant writes the full
 * 16 bits the wire carries, for a caller that has them (the Switch pad). No
 * caller yet - the pad pipeline is uint8_t end to end; see the comment on
 * ctrl_gamepad_axis in ctrl_gamepad.c. */
int ctrl_gamepad_axis16(int axis_idx, int16_t value);
int ctrl_gamepad_dpad(uint8_t value);

/* Local evdev reader (Linux): reads the gamepad plugged into this machine and
 * forwards it. SHADOW_GAMEPAD=1 enables it. On Switch it will be the libnx pad
 * that calls the functions above. */
/* Wire self-test with no gamepad: SHADOW_GAMEPAD_SELFTEST=1 */
int  ctrl_gamepad_selftest(void);
/* Probe: sweeps ONE axis index in a loop. SHADOW_PAD_AXIS_PROBE=<0..5> */
int  ctrl_gamepad_axis_probe(void);
/* Sweeps a single axis index in a loop (negative index = stop); returns the
 * index being probed, or -1. Driven from the developer menu bar. */
int  ctrl_gamepad_axis_sweep(int idx);
int  ctrl_gamepad_axis_probing(void);
int  ctrl_gamepad_start_local_reader(volatile int *abort_flag);
void ctrl_gamepad_stop_local_reader(void);

#ifdef __cplusplus
}
#endif
