/* rumble_hid - the only place that talks to the motors.
 *
 * G57c, 2026-08-28. Extracted from stream_view.cpp so that the settings screen
 * can exercise the HARDWARE without the network: when nothing vibrates, you need
 * a way to tell "the protocol delivers nothing" from "the console refuses".
 *
 * CALL ONLY FROM THE UI THREAD. Borealis' rumble primitive starts with a
 * `padUpdate()` on the same PadState that thread refreshes every frame; calling
 * it elsewhere would be a concurrent write on the input state, and it would eat
 * button edges without raising a single error.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Applies both amplitudes, 0 to 255. Returns false if no handle could be
 * obtained. Takes the handles on the first call and logs the result of every
 * style it tries - without that, a failed initialisation is indistinguishable
 * from a silent channel, which has already cost one round trip. */
bool rumble_hid_apply(uint8_t basse, uint8_t haute);

/* Stops both motors. The protocol carries no duration: without an explicit
 * call, a rumble holds forever. */
void rumble_hid_stop(void);

#ifdef __cplusplus
}
#endif
