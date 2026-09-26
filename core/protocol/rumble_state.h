/* rumble_state - the last rumble received, from the session thread to the UI
 * thread.
 *
 * G57, 2026-08-28. Channel `:base+13` has been sending us force feedback all
 * along and we were throwing it away. This module is the handoff between the two
 * threads, built on the pattern of `cursor_state.h`: written from the UDP
 * receive thread, read from the UI thread, guarded by a mutex.
 *
 * WHY A STATE AND NOT A QUEUE. The protocol carries no duration: the official
 * client calls SDL with `duration_ms = 0`, meaning "never expires". A rumble is
 * therefore a STATE that holds until the next message, and the measured rate is
 * 33.5 messages per second at the median, 48 at the peak. Stacking those
 * messages would make no sense: only the last one counts. Last value wins.
 *
 * WHY THE UI THREAD AND NOT THE SESSION THREAD. Calling the hardware from the
 * receive thread would be a silent defect, not a style violation:
 * `SwitchInputManager::sendRumbleRaw` starts with `padUpdate()` on the SAME
 * `PadState` that the UI thread refreshes every frame. Calling it elsewhere is a
 * concurrent write on Borealis' input state, and it would swallow button edges
 * without reporting a single error.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Written from the session thread, on every decoded kVibration. */
void rumble_state_set(uint8_t id, uint8_t basse, uint8_t haute);

/* Read from the UI thread, once per frame.
 *
 * Returns true if the value CHANGED since the last read: that is what lets the
 * caller touch the hardware only on a real change, instead of 60 times a second
 * for nothing. The amplitudes are always returned, changed or not. */
bool rumble_state_get(uint8_t *basse, uint8_t *haute);

/* Resets to zero AND flags a change, so that the next read shuts the motors
 * down. Call it when the rumble must stop without the server having asked: pause
 * menu opened, session over, controller unplugged.
 *
 * This is NOT a finishing touch: since the protocol carries no duration, the
 * last message of a session is often non-zero - `04 00 07 ff 00` was seen at the
 * end of a log, big motor at full power. Without an explicit shutdown the
 * controller would keep vibrating until you put it down. */
void rumble_state_stop(void);

/* Number of messages received since the session started, for diagnostics.
 * SESSION state: reset to zero by `rumble_state_stop` at startup. */
uint32_t rumble_state_recus(void);

#ifdef __cplusplus
}
#endif
