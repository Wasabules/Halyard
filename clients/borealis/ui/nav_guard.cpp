/* ui::nav - see nav_guard.hpp. */
#include "nav_guard.hpp"

#include <deque>

#include "../../../core/common/log.h"
#include "shutdown.hpp"   /* UX10 */
#define nvlog(...) JOURNAL_INFO_(JOURNAL_CAT_UI, __VA_ARGS__)

namespace ui {
namespace nav {

namespace {

struct Op {
    bool              is_push;
    brls::Activity   *activity;   /* push only */
};

/* Module state, not a function `static`: read and written from two files, and
 * this repo's most expensive defect family is session state hidden inside a
 * function. */
std::deque<Op> g_held;
int            g_blocks = 0;

/* === AF2 (second half) 2026-09-10 - A REPLAY RUNS EACH OPERATION TO THE END ===
 *
 * Measured on desktop with SHADOW_DIAG_RELOCK_S: a session that ends under the
 * lock asks for TWO pops - the stream view, then the connecting screen, 1.3 s
 * apart when nothing holds them. Replayed, they ran in the same instant, and a
 * FADE pop leaves its activity in the stack until the fade ends: the second pop
 * found the stream view AGAIN. The log said it plainly - "ecran-connexion
 * redevient visible" twice, never "liste-VM" - and the application sat on the
 * connecting screen with every key dead: the AF6 shape (the second `hide` runs
 * its callback at once, and the first pop's callback, the one that returns its
 * input token, dies with the view). Waiting for the lock to leave the stack
 * (LockActivity) was necessary; it was not enough.
 *
 * So a replay goes WITHOUT transition. With NONE, `popActivity` has removed its
 * activity before it returns and `pushActivity` hands its input token back at
 * once, so each operation finds the stack the previous one left - which is
 * what they would have found had they run when they were asked for. NONE on the
 * push matters as much: a FADE push popped straight away would be deleted
 * before its fade-in returns the token. Operations nothing held keep their
 * transitions. */
void run(const Op &op)
{
    if (op.is_push) brls::Application::pushActivity(op.activity, brls::TransitionAnimation::NONE);
    else            brls::Application::popActivity(brls::TransitionAnimation::NONE);
}

}  // namespace

bool blocked() { return g_blocks > 0; }

/* === UX10 2026-09-10 - WHILE THE APPLICATION CLOSES, THE STACK STAYS PUT ===
 *
 * Reported in use: one frame of the stream showing at the very end of the
 * shutdown. That is literally what happened. The end of a session pops to go
 * back to the list (`[NAV] depilement vue-flux depuis le fil de travail`), and
 * since UX10 the screen on top is no longer the stream view: it is the
 * SHUTDOWN SCREEN. The pop removed it, uncovering the stream view for one frame
 * before the application left.
 *
 * Holding would not have been enough - held operations are REPLAYED when the
 * lock opens, and here nothing will open: we are quitting. So they are
 * ignored, openly, and the log says so.
 *
 * The activity of an ignored `push` is not destroyed. Pushed, Borealis would
 * own it; deleting it here instead would run a destructor that assumes a
 * content view that was never built. A few bytes abandoned a second before the
 * process exits are the lesser of the two risks. */
static bool closing() { return ui::shutdown::active(); }

void push(brls::Activity *activity)
{
    if (!activity) return;
    if (closing()) {
        nvlog("[nav] empilement ignore — fermeture en cours");
        return;
    }
    if (blocked()) {
        nvlog("[nav] push held back - a lock is asking for a secret");
        g_held.push_back({ true, activity });
        return;
    }
    brls::Application::pushActivity(activity);
}

void pop()
{
    if (closing()) {
        nvlog("[nav] depilement ignore — fermeture en cours");
        return;
    }
    if (blocked()) {
        nvlog("[nav] pop held back - a lock is asking for a secret");
        g_held.push_back({ false, nullptr });
        return;
    }
    brls::Application::popActivity();
}

void block() { g_blocks++; }

void unblock()
{
    if (g_blocks > 0) g_blocks--;
    if (g_blocks > 0) return;

    /* Replayed in the order they were asked for. The queue is emptied BEFORE
     * anything runs: a replayed operation can push a screen that itself asks
     * for one, and iterating over a container being appended to is the kind of
     * bug that only shows up under exactly this timing. */
    std::deque<Op> pending;
    pending.swap(g_held);
    if (!pending.empty())
        nvlog("[nav] lock opened - %zu operation(s) resumed", pending.size());
    for (const Op &op : pending) run(op);
}

}  // namespace nav
}  // namespace ui
