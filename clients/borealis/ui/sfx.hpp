/* ui::sfx - the interface sounds.
 *
 * === WHY THIS MODULE EXISTS (S88, 2026-08-29) ===
 *
 * Borealis has a sound API (`brls::AudioPlayer`, `SOUND_FOCUS_CHANGE`...) and it
 * is of no use here: its Switch implementation is ENTIRELY commented out in our
 * vendored copy. It used to mount `qlaunch`'s romfs to pull the console's SYSTEM
 * sounds from it - which, on top of being disabled, would amount to shipping
 * Nintendo assets. On desktop it falls back to a player that does nothing.
 *
 * So we have our own layer, with our own files.
 *
 * === THE CONSTRAINT THAT DICTATES THE WHOLE DESIGN ===
 *
 * The console has only ONE audio output, and during a session the stream owns it
 * (`media/audio.c`: `audout` on console, ALSA on desktop). Two producers piling
 * their buffers into the same queue get played ONE AFTER THE OTHER: a 100 ms
 * interface sound would delay the game's audio by that much, and would be heard
 * as a hiccup.
 *
 * Interface sounds are therefore SILENT during the stream. This is not a
 * limitation we put up with, it is the right behaviour: beeping over someone's
 * game would be worse than saying nothing. The device is opened lazily on the
 * first sound and closed after two seconds of inactivity.
 *
 * === AUDC-1 / OUT-2 2026-09-11 - THE STREAM CLAIMS THE OUTPUT ===
 *
 * This header used to say those two seconds left the device free "by the time a
 * session starts", and that play() did nothing while the stream owned the
 * output. Neither held. libnx has ONE IAudioOut per process - both of us call
 * `audoutStartAudioOut()` with no handle - and a reconnection plays Failed and
 * opens the stream one second later, inside the two seconds: HOS either refuses
 * the second Start (no sound for the whole session) or accepts it, and our idle
 * close then STOPS the shared output a second in. Silent either way, on every
 * automatic reconnection, with the default settings. And `release()` cut the
 * Connected chime it was meant to let through.
 *
 * So the stream now CLAIMS the output: `StreamClaim` around the session call.
 * The thread closes the device, acknowledges, and stays closed until the claim
 * ends; `play()` does nothing meanwhile. Offline bench (this module built for
 * __SWITCH__ against a mock of libnx's IAudioOut, both models of a second
 * Start): a reconnected session heard 0 % (Start refused) or 3-33 % (accepted)
 * before, 100 % bit-exact after; tests/test_sfx_handoff.cpp keeps it that way.
 * `SHADOW_SFX_HANDOFF=0` restores the previous behaviour.
 *
 * === WHAT THE MODULE DOES ABOUT MISSING FILES ===
 *
 * Nothing. A sound with no file is a call that does nothing. That is what makes
 * it possible to add the fourteen files ONE BY ONE, listening as they land,
 * instead of waiting for the whole set to exist before hearing anything.
 *
 * The files live in `resources/sfx/` (so `romfs:/sfx/` on console) and their
 * specification - format, duration, character - is in `docs/UI_SOUNDS.md`.
 */
#pragma once

#include <cstdint>

namespace ui {
namespace sfx {

/* Events, not files: a call site states what HAPPENS, and the module decides
 * which sound that produces. That is what lets the palette change - merging two
 * sounds, adding one - without touching the screens. */
enum class Sound {
    Navigation = 0,   /* the cursor moves within a list */
    NavigationEdge,   /* top or bottom of the list reached */
    Value,            /* left/right changes a value */
    Section,          /* L/R switches section */
    Confirm,          /* A */
    Back,             /* B */
    ToggleOn,
    ToggleOff,
    Open,             /* a page is pushed */
    Close,            /* a page is popped */
    Connected,        /* the session is established */
    Failed,           /* the connection failed */
    Alert,            /* an incident during a session - silent while a StreamClaim
                         holds the output (AUDC-1); no call site plays it yet */
    Startup,          /* application launch */
    Count
};

/* Loads whatever is present. Call it once, at startup - but forgetting to is not
 * fatal: `play()` loads on demand if it has to. */
void init();

/* Plays `s`. No effect if sounds are muted, if the file is missing, or if the
 * stream owns the audio output (a `StreamClaim` is alive - AUDC-1: that last
 * clause had no code behind it before 2026-09-11). NEVER blocks: an interface
 * sound that made the interface wait would be a regression all by itself. */
void play(Sound s);

/* Settings. `volume` is a percentage, 0 to 100 - separate from the stream's
 * volume, which it does not touch. */
void setEnabled(bool on);
void setVolume(uint32_t percent);
bool enabled();

/* Lets the sounds in flight finish, then closes the output on the thread's next
 * idle pass instead of two seconds later. Called right after the Connected
 * chime. AUDC-1 / OUT-2 2026-09-11: it used to CUT every voice, microseconds
 * after that play() - 0 of the chime's 34 464 frames ever reached the output.
 * Handing the output to the stream is `StreamClaim`'s job now. With
 * SHADOW_SFX_HANDOFF=0 it cuts the voices again. */
void release();

/* === AUDC-1 - THE STREAM TAKES THE OUTPUT, AND GIVES IT BACK ===
 *
 * suspend_for_stream(): from now on play() does nothing, and the thread closes
 * the output and does not reopen it. BLOCKS until the thread has done so and
 * acknowledged - a mere "is it closed" check races an iteration already inside
 * the open - polling in 10 ms slices for 100 ms at most (KB §7.3). Returns
 * false, after logging it, if the thread did not answer in time; the stream
 * goes ahead anyway. Call it on a worker thread, never on the UI thread.
 * resume_after_stream(): the UI sounds may play again.
 * Both are no-ops with SHADOW_SFX_HANDOFF=0. Prefer `StreamClaim`, which cannot
 * forget the second call: a missed resume would leave the interface mute for
 * the rest of the process. */
bool suspend_for_stream();
void resume_after_stream();

/* Scope guard: the stream owns the audio output for the guard's lifetime. Wrap
 * the session call in it (connecting_activity.cpp) - the output then comes back
 * on every path out, before anything plays the next Failed. */
class StreamClaim {
public:
    StreamClaim() : handed_(suspend_for_stream()) {}
    ~StreamClaim() { resume_after_stream(); }
    StreamClaim(const StreamClaim &) = delete;
    StreamClaim &operator=(const StreamClaim &) = delete;
    /* false if the output was not handed back within 100 ms (already logged). */
    bool handed() const { return handed_; }
private:
    bool handed_;
};

/* Stops the thread and releases everything. Call before `exit()` - on console a
 * thread killed brutally leaks its handles, and a leak costs a reboot. */
void close();

}  // namespace sfx
}  // namespace ui
