/* ui::shutdown - the state of the shutdown, so that it can be SHOWN.
 *
 * === UX10 2026-09-10 - WE USED TO CLOSE IN THE DARK ===
 *
 * Measured on Windows on 2026-09-10, window closed mid-session: the process
 * lived on for 2988 ms after the window had gone. That time is not wasted - it
 * unregisters the eight streams, joins the receive threads, then DELETES our
 * clients on the server (two DELETEs, ~2 s on their own) so the session is not
 * left as a ghost on the VM. Good work, entirely invisible: the user sees a
 * window vanish and an application lingering in the task manager.
 *
 * And 3 s was not the worst case. Those two requests carried the common HTTP
 * timeout, `SHADOW_HTTP_TOTAL_TIMEOUT` = 30 s: with the network gone at the
 * moment of closing, the shutdown could stretch towards a minute (AF3, the same
 * day, now bounds requests at exit to a 1.5 s grace - see shadow/http.c). On
 * console the HOME menu does not wait - HOS kills the application, and a thread
 * killed outright leaks its handles, which this repo pays for with a reboot
 * (CLAUDE.md, the thread lifecycle rules).
 *
 * === WHY A COUNTER AND NOT A DELAY ===
 *
 * The screen must go when the work is DONE, not after a duration picked by
 * guesswork. Each shutdown task announces itself (`beginTask`) and withdraws
 * (`endTask`); the screen lives as long as any remain. With no session open the
 * counter is zero and the shutdown is immediate - the common case, which must
 * cost nothing.
 *
 * The ceiling stays, but as a SAFETY NET, not as the mechanism: a task that
 * never withdraws must not hold the application for ever.
 *
 * Pure, no I/O, no allocation: callable from any thread, including while the
 * application is being torn down.
 */
#pragma once

#include <atomic>
#include <cstring>

namespace ui {
namespace shutdown {

/* The label is copied into a fixed buffer: a `std::string` shared between the
 * session thread that writes it and the render thread that reads it would need
 * a lock, and a shutdown path is the last place to take one. The last byte is
 * never written, so a reader always finds a terminator - at worst it reads a
 * label mid-change for one frame. */
inline constexpr int STEP_MAX = 96;

struct State {
    std::atomic<bool>   active{false};
    std::atomic<int>    pending{0};
    std::atomic<bool>   stepDirty{false};
    /* The start time lives HERE, not in a function `static` at the caller:
     * this repo names that defect family at the top of its CLAUDE.md, and an
     * exception of the "but shutdown only happens once" kind is exactly the
     * argument that let it settle in every previous time. */
    std::atomic<double> startedAt{0.0};
    char                step[STEP_MAX] = {0};
};

inline State &state()
{
    static State s;
    return s;
}

/* Puts the application into shutdown. Idempotent: two requests close together
 * (a double click on the cross) restart nothing. */
inline bool begin(double now)
{
    bool expected = false;
    if (!state().active.compare_exchange_strong(expected, true)) return false;
    state().startedAt.store(now, std::memory_order_relaxed);
    return true;
}

/* Seconds since `begin`. `now` comes from the caller: this module knows no
 * clock, which keeps it testable and usable from any thread. */
inline double elapsed(double now)
{
    return now - state().startedAt.load(std::memory_order_relaxed);
}

inline bool active() { return state().active.load(std::memory_order_relaxed); }

/* Publishes the current step. Truncates rather than refuses: a label too long
 * is a display defect, not a reason to lose the information. */
inline void setStep(const char *s)
{
    if (!s) return;
    State &st = state();
    std::strncpy(st.step, s, STEP_MAX - 1);
    st.step[STEP_MAX - 1] = 0;
    st.stepDirty.store(true, std::memory_order_release);
}

inline const char *step() { return state().step; }

/* A shutdown task starts / ends. Call in pairs. */
inline void beginTask() { state().pending.fetch_add(1, std::memory_order_relaxed); }
inline void endTask()
{
    if (state().pending.fetch_sub(1, std::memory_order_relaxed) <= 0)
        state().pending.store(0, std::memory_order_relaxed);   /* never negative */
}

inline int pending() { return state().pending.load(std::memory_order_relaxed); }

/* Is anything left to wait for? */
inline bool finished() { return pending() <= 0; }

}  // namespace shutdown
}  // namespace ui
