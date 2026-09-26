/* LockActivity - hosts the LockView.
 *
 * === WHY IT IS PUSHED AND NEVER SWAPPED ===
 *
 * Borealis has no `swapActivity`: the stack API is exactly push and pop. So the
 * lock follows the shape this application already uses - BootActivity is pushed
 * first and stays underneath the VM list forever - and does the same:
 *
 *   - at startup, the lock is pushed FIRST and pushes BootActivity on top of
 *     itself once opened. It stays underneath, harmless, and `popActivity`
 *     refuses to empty the stack, so nothing can ever surface it by accident.
 *
 *   - on a re-lock (the console woke up), it is pushed ON TOP of whatever was
 *     showing and pops ITSELF once opened, which puts the user back exactly
 *     where they were - mid-stream included. What the connection path asked
 *     for meanwhile is replayed only once it has LEFT the stack (AF2).
 *
 * The two cases differ only in what happens on success, which is why that is a
 * callback and not a mode.
 */
#pragma once

#include <borealis.hpp>
#include <functional>
#include <string>

class LockView;

class LockActivity : public brls::Activity {
public:
    /* `on_done` runs on the UI thread: with true once the right secret is given
     * (or the new one written), with false when the user left an enrolment
     * without setting anything.
     *
     * `enrol_method` is 0 to OPEN the lock, or one of APPLOCK_PIN /
     * APPLOCK_PASSWORD / APPLOCK_PATTERN to SET that one. */
    explicit LockActivity(std::function<void(bool)> on_done,
                          std::string subtitle = std::string(),
                          unsigned enrol_method = 0);
    ~LockActivity() override;

    brls::View *createContentView() override;
    void onContentAvailable() override;

private:
    /* Clears this screen's contribution to the "a lock is demanding a secret"
     * count, once. Called when the screen is answered AND from the destructor:
     * the startup lock is answered but never destroyed, the abandoned enrolment
     * is destroyed but never answered. */
    void release();

    /* AF2 2026-09-10 - gives back this screen's hold on the navigation stack,
     * once, which replays what the connection path asked for meanwhile. Split
     * from `release()` because a lock that pops itself needs the two at
     * DIFFERENT moments: the count when it is answered, the stack only once it
     * has left. `deferred` queues the replay to the next UI drain. */
    void releaseStack(bool deferred);

    LockView *view = nullptr;
    bool counted_ = false;
    bool holding_ = false;   /* holds a ui::nav::block() */
    std::function<void(bool)> on_done_;
    std::string subtitle_;
    unsigned    enrol_method_;
};

/* True while a lock screen is up. Guards the re-lock hook against pushing a
 * second one when the console wakes twice - which it does, because opening the
 * HOME menu and closing it fires the same event as a sleep and a wake. */
bool lock_activity_is_up();
