/* ui::nav - stack operations that a lock screen can hold back.
 *
 * === THE DEFECT THIS CLOSES ===
 *
 * The connection path drives the activity stack from a worker: it pushes
 * StreamActivity when the session comes up, and pops the connecting screen when
 * it ends. Those requests reach the UI thread through `Threading::sync`, which
 * runs them whenever it next drains - and that can be while a re-lock screen is
 * on top.
 *
 * `popActivity` always takes the TOP of the stack, whatever the caller meant.
 * So a pop aimed at the connecting screen removes the LOCK instead: the console
 * woke up, asked for a secret, and a background thread dismissed the question.
 * A push is the mirror image - StreamActivity lands on top of the lock and the
 * live session is on screen behind it.
 *
 * Neither is a race in the worker's own logic; the worker is right about what it
 * wants. What is wrong is doing it while the screen belongs to someone else.
 *
 * === HOW ===
 *
 * While a lock is demanding a secret, these operations are QUEUED instead of
 * run, and replayed in order once it has been answered AND has left the stack
 * (AF2 2026-09-10 - replayed under a lock still on top, a pop took the lock;
 * see LockActivity). The worker's intent is
 * preserved exactly - same operations, same order - it just happens a few
 * seconds later, which is the same thing that would have happened if the
 * console had been woken a moment later.
 *
 * The lock screen itself does NOT go through here: it must be able to push and
 * pop while it is the one blocking.
 */
#pragma once

#include <borealis.hpp>

namespace ui {
namespace nav {

/* Push / pop, held back while a lock is up. Call from the UI thread. */
void push(brls::Activity *activity);
void pop();

/* The lock screen raises this on the way in and lowers it on the way out;
 * lowering it replays whatever was held. Counted, so two locks cannot leave it
 * lowered while one is still on screen. */
void block();
void unblock();

/* True while operations are being held. */
bool blocked();

}  // namespace nav
}  // namespace ui
