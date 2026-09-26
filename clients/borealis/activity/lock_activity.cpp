/* See activity/lock_activity.hpp. */
#include "activity/lock_activity.hpp"

#include "lock_view.hpp"
#include "../ui/screen_base.hpp"
#include "../ui/nav_guard.hpp"

namespace {
/* === WHAT THIS COUNTS, AND THE BUG THAT TAUGHT US TO BE PRECISE ===
 *
 * It counts lock screens that are STILL DEMANDING A SECRET - not LockActivity
 * objects that exist.
 *
 * The difference killed the feature on its first day. The startup lock is
 * pushed FIRST and, having no `swapActivity` to use, is left underneath
 * BootActivity forever: it is never popped, so its destructor never runs. A
 * count kept by the destructor therefore stayed at 1 for the whole life of the
 * process, and the wake hook's `!lock_activity_is_up()` guard was false every
 * single time. The console re-locked on waking exactly never - which is the one
 * thing that separates this lock from theatre, since Borealis asks HOS not to
 * suspend us and HOME-out/HOME-in keeps the same process.
 *
 * So it is cleared when the screen is ANSWERED, not when it is destroyed. The
 * destructor still clears it, for the screen that is left rather than answered.
 *
 * Module state, not a function `static`: it is read by the focus hook and
 * written from two places, and this repo's most expensive defect family is
 * exactly session state hidden inside a function.
 *
 * A COUNT and not a flag: if two lock screens ever coexisted, a flag cleared by
 * the first would report "no lock" while the second is still on screen. */
int g_lock_screens_up = 0;
}

bool lock_activity_is_up() { return g_lock_screens_up > 0; }

LockActivity::LockActivity(std::function<void(bool)> on_done, std::string subtitle,
                           unsigned enrol_method)
    : on_done_(std::move(on_done)), subtitle_(std::move(subtitle)),
      enrol_method_(enrol_method)
{
    /* Counted only when it is a real lock. An ENROLMENT screen is not one: the
     * wake-up hook must still be able to push a lock over it, otherwise setting
     * a PIN would be a window during which the console cannot re-lock. */
    if (enrol_method_ == 0) {
        g_lock_screens_up++;
        counted_ = true;
        /* Hold the stack. The connection path drives it from a worker, and
         * `popActivity` always takes the TOP - so a pop meant for the
         * connecting screen would dismiss THIS one, without the secret. See
         * ui/nav_guard.hpp. */
        ui::nav::block();
        holding_ = true;
    }
}

void LockActivity::release()
{
    if (!counted_) return;
    counted_ = false;
    if (g_lock_screens_up > 0) g_lock_screens_up--;
}

void LockActivity::releaseStack(bool deferred)
{
    if (!holding_) return;
    holding_ = false;
    /* Replays whatever the connection path asked for while we held the screen.
     * Paired with the `block()` in the constructor. */
    if (deferred) brls::Threading::sync([]() { ui::nav::unblock(); });
    else          ui::nav::unblock();
}

LockActivity::~LockActivity()
{
    release();   /* the screen that is left rather than answered */
    /* AF2 - deferred. When the lock popped itself, this runs INSIDE Borealis'
     * pop callback: the lock already erased from the stack, the pop not yet
     * finished (it still has to return its input token). Replaying there would
     * push and pop from within Borealis' own bookkeeping; at the next drain the
     * stack is settled and its top is what the connection path meant.
     * `Threading::sync` is safe from anywhere on the UI thread: it appends
     * under a lock that the drain does not hold while it runs the tasks. */
    releaseStack(true);
}

brls::View *LockActivity::createContentView()
{
    view = new LockView(enrol_method_ ? LockView::Purpose::Enrol
                                      : LockView::Purpose::Unlock,
                        enrol_method_);
    view->setSubtitle(subtitle_);
    return view;
}

void LockActivity::onContentAvailable()
{
    /* `wireNavigation` wires the four directions, the triggers, A, B, X and Y -
     * everything this screen uses. Nothing is registered by hand here.
     *
     * B is NOT excluded from that wiring: `LockView::back()` always returns
     * true, which consumes the press before `wireNavigation` can pop the
     * activity. Taking B out instead would leave the button silent, and a
     * silent button reads as a frozen screen.
     *
     * PLUS is deliberately left alone. It is registered application-wide as
     * "quit" (main.cpp calls setGlobalQuit(true)), and registering it here
     * would REPLACE that - `View::registerAction` overwrites any action bound
     * to the same button. Being unable to close the application from the lock
     * screen would be a trap, and quitting is not a way past the lock: the next
     * launch asks again. */
    ui::wireNavigation(this, view);

    /* Cleared the moment the screen is answered, whatever the host then does
     * with the callback - it may push, pop, or nothing at all. Doing it here
     * rather than inside each host keeps the two from disagreeing. */
    auto done = on_done_;
    view->setOnUnlocked([this, done](bool ok) {
        release();
        if (done) done(ok);
        /* === AF2 2026-09-10 - THE REPLAY WAITS FOR THE LOCK TO LEAVE ===
         *
         * The stack used to be released inside `release()`, i.e. BEFORE
         * `done`. For the wake-up lock that is the wrong moment: the lock is
         * still the top of the stack. A held `pop` - the connection path going
         * back to the list after a session that ended during the sleep - took
         * the LOCK; then `done` popped again, and a FADE pop leaves the
         * activity in the stack until its fade ends, so it found the SAME
         * lock. The second `hide` finds the view already hidden and runs its
         * callback at once: the lock is deleted mid-fade, the first pop's
         * callback - the one that returns its input token - dies with it, and
         * the screen the connection path wanted gone is still there. Every
         * button dead, the AF6 shape. A held `push` was the mirror image:
         * StreamActivity landed on the lock, `done` popped the STREAM, and the
         * answered lock stayed on screen with all its inputs closed.
         * Reconstructed from Borealis' code by the 2026-09-10 shutdown audit
         * (application.cpp popActivity, view.cpp hide); on Switch only, where
         * the wake hook lives - SHADOW_DIAG_RELOCK_S (main.cpp) is the desktop
         * way to run it.
         *
         * So the host goes first. If it DISMISSED us - `popActivity` hides the
         * view at once, then fades - the destructor releases the stack once
         * Borealis has taken us out. If it left us in place - the startup lock
         * stays under BootActivity for ever and is never destroyed - nothing
         * will ever take us out, so it is now. */
        if (!isHidden()) releaseStack(false);
    });
}
