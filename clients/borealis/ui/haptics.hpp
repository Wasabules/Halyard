/* ui::haptics — the interface's vibration feedback.
 *
 * === WHY INTENT, AND NOT EVENT ===
 *
 * Wiring a vibration to every action gives ten patterns that feel alike, and the
 * hand stops telling them apart within a minute: the feedback becomes background
 * noise, which is to say nothing at all. So callers declare what a gesture
 * MEANS, and this module picks the pattern. Two intents cannot end up sharing a
 * vibration by accident, and adding one forces you to say how it differs from
 * the others.
 *
 * === WHAT DESERVES A VIBRATION, AND WHAT DOES NOT ===
 *
 * Sound already covers navigation and confirmation. Vibration is added only
 * where sound is not enough:
 *
 *   Limit     — you are pushing against something. This is a SENSATION; no sound
 *               replaces it, and it is the only feedback that says "no point
 *               insisting" without taking up the ear.
 *   Toggle    — two patterns differing by the DOMINANT motor, not by duration:
 *               nobody compares two 35 ms durations, but everyone tells a low
 *               rumble from a high one.
 *   Key       — on-screen keyboard. At the edge of perception: a key that thumps
 *               is tiring within ten words.
 *   Mode      — the only LONG pattern, which is what takes it out of the
 *               confirmation family: it announces a change of rules, not a
 *               success. Confusing the two would be worse than staying silent.
 *
 * NO vibration on the click sent to the remote machine: the game already drives
 * the motors, and two sources blur together — an interface cue would be read as
 * a game event.
 *
 * === DURING STREAMING, WE STAY QUIET ===
 *
 * The VM owns the motors in session. An interface vibration mid-game would be
 * attributed to the game, which is exactly the misreading we are avoiding.
 * `haptics::setEnabled(false)` for the duration of the stream.
 */
#pragma once

namespace ui {
namespace haptics {

enum class Intent {
    Limit,        /* end of a list, value at its maximum */
    ToggleOn,
    ToggleOff,
    Key,          /* on-screen keyboard */
    Mode,         /* entering or leaving a mode */
};

/* Strength as a percentage (0 = off). Read from settings, re-read live. */
void setStrength(int percent);

/* Silences or restores all feedback — the stream uses this. */
void setEnabled(bool on);

/* Fires this intent's pattern. Does nothing when strength is zero, when
 * feedback is disabled, or when a pattern is already running: two overlapping
 * vibrations make one mush, and the second message is lost either way. */
void play(Intent what);

/* Call once per frame with the screen's clock. This is what TURNS THE MOTORS
 * OFF: without this heartbeat a pattern would stay on forever — precisely the
 * failure nobody forgives in haptic feedback. */
void tick(double t);

}  // namespace haptics
}  // namespace ui
