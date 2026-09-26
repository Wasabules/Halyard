/* padforward - forwards the console's buttons to the Shadow gamepad.
 *
 * On desktop, ctrl_gamepad reads a gamepad plugged in over USB (evdev). On
 * Switch there was nothing: the Joy-Cons drove the UI, never the VM. This module
 * fills that gap by reading the HID state every frame and emitting only the
 * CHANGES - the gamepad channel is UDP and the protocol sends one message per
 * transition, not a periodic full state.
 *
 * The button mapping comes from padmap, hence from what the user configured in
 * the settings.
 */
#pragma once

namespace padforward {

/* Call every frame during the stream. With no active gamepad session the call
 * does nothing. On platforms other than Switch it is an empty stub: the desktop
 * already has its evdev reader. */
void poll();

/* Forces the whole state to be resent on the next frame (after a mapping change,
 * or when a session reconnects). */
void reset();

/* Releases everything held ON THE VM'S SIDE, then zeroes the local state. Use it
 * when suspending forwarding while keeping the session alive - the pause menu
 * being open, for instance. `reset()` alone would leave the button pressed. */
void releaseAll();

/* Stops the gamepad reading thread (L9). Call it when the stream view is
 * destroyed. Without an explicit join, HOS leaks the handle and the console has
 * to be rebooted to get it back. */
void stop();

}  // namespace padforward
