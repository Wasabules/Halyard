/* LockView - the screen that stands between the console and the application.
 *
 * READ shadow/applock.h FIRST, in particular what this lock protects and what
 * it does not. Nothing on this screen should suggest more than that.
 *
 * === THE THREE WAYS IN, AND WHY THEY ARE ONE SCREEN ===
 *
 * PIN, password and pattern are not three screens but three modes of one,
 * because the parts that must not diverge are shared: the throttling, the
 * counter written before the check, the message, and the refusal to let B out.
 * Three screens would eventually enforce three slightly different throttles,
 * and the weakest of them would be the lock.
 *
 * Y cycles between the modes the install has armed. Only armed ones are
 * offered: a mode with no secret behind it would refuse every entry, and the
 * user would be trying to open a door that was never fitted.
 *
 * === THE DERIVATION RUNS ON A THREAD OF ITS OWN, AND WHY NOT THE SHARED ONE ===
 *
 * PBKDF2 takes a few hundred milliseconds by design. Run on the render thread it
 * would freeze the screen at the exact instant the user is waiting for an
 * answer, and a frozen screen reads as a crash.
 *
 * The obvious tool, `brls::Threading::async`, is WRONG HERE and it took an audit
 * to see why: Borealis runs every async task on ONE shared loop
 * (library/borealis/.../core/thread.cpp), sequentially - and
 * `connecting_activity.cpp` puts the whole streaming session on that loop, in a
 * call that blocks until the session ends. A derivation queued during a session
 * would therefore never run at all, and the re-lock screen that appears when the
 * console wakes mid-stream could never be opened. That is the exact opposite of
 * what a lock is for: it would lock the OWNER out of their own running session.
 *
 * So the check gets its own short-lived thread, JOINED in the destructor. Joining
 * costs at most the length of one derivation and it is what makes the lifetime
 * trivial: no thread can outlive this view, so nothing it writes can land in
 * freed memory. The mailbox stays because the result must still be consumed on
 * the render thread, in `paint`.
 *
 * === WHY B IS SWALLOWED ===
 *
 * `back()` always returns true. Borealis pops an activity when the screen lets
 * B through, and the whole point of this one is that it cannot be left. B is
 * given a meaning instead - erase a digit, clear the pattern - so the button
 * does something rather than appearing broken.
 */
#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include <functional>

#include "../ui/screen_base.hpp"

extern "C" {
#include "../../../core/services/applock_store.h"
}

class LockView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "lock"; }

    /* === ONE SCREEN, TWO JOBS ===
     *
     * Opening the lock and SETTING it are the same grid, the same keypad, the
     * same drawing. Two screens would eventually disagree about what counts as
     * a valid pattern - and the disagreement would only surface as "the pattern
     * I chose is refused", after the lock is already armed and the user is
     * locked out of their own console.
     *
     * So the geometry is shared and only the outcome differs: Unlock checks
     * against the card, Enrol asks twice and writes. */
    enum class Purpose { Unlock, Enrol };

    /* `method` is meaningful only for Enrol: which secret is being set. */
    explicit LockView(Purpose purpose = Purpose::Unlock, uint32_t method = 0);
    ~LockView() override;

    /* Called on the UI thread once the right secret has been given, or - in
     * Enrol - once the new one is written. `false` means the user left without
     * setting anything. The host
     * activity decides what "unlocked" means - open the application, or pop
     * itself off a running session. */
    void setOnUnlocked(std::function<void(bool)> fn) { on_unlocked_ = std::move(fn); }

    /* The message shown under the title. The host sets it so the same screen can
     * say "unlock" at startup and "the console woke up" on a re-lock. */
    void setSubtitle(std::string s) { subtitle_ = std::move(s); }

    bool up()      override;
    bool down()    override;
    bool left()    override;
    bool right()   override;
    bool activate() override;
    bool back()    override;
    /* X validates a pattern. A pattern has no natural end - you stop drawing -
     * so with a D-pad something has to say "that is the gesture"; lifting the
     * finger does it on a touchscreen. It is X and not PLUS because PLUS is
     * registered application-wide as "quit", and taking it over here would make
     * the lock screen the one place the application cannot be closed. */
    bool buttonX() override;
    bool buttonY() override;   /* switch method */

    void paint(NVGcontext *vg, float x, float y, float w, float h,
               double t) override;

private:
    enum class Mode { Pin, Pattern, Password };

    /* What the last attempt produced, handed back from the worker thread. The
     * view never touches the record from that thread. */
    struct Outcome {
        std::atomic<bool> ready{false};
        applock_result_t  result{APPLOCK_ERROR};
        int64_t           retry_in{0};
        /* The record AS THE WORKER LEFT IT - it carries the failure counter and
         * the deadline the attempt just wrote to the card.
         *
         * The worker gets a COPY and gives one back; it never touches
         * `record_`. Letting it write in place would have it racing `paint`,
         * which reads the same record sixty times a second to show the
         * countdown - a race whose visible symptom would be a countdown that
         * flickers, and whose invisible one is a torn 64-bit deadline. */
        applock_record_t  record{};
    };

    void  pickFirstMode();
    void  cycleMode(int dir);
    bool  modeArmed(Mode m) const;
    void  submit();                 /* hands the current entry to the worker */
    void  clearEntry();
    void  openKeyboard();           /* password: the system/IME text entry */
    void  pollTouch(float gx, float gy, float side, float px, float py, bool down);
    void  applyOutcome();

    void  drawPin(NVGcontext *vg, float cx, float top, float w, double t);
    void  drawPattern(NVGcontext *vg, float cx, float top, float w, double t);
    void  drawPassword(NVGcontext *vg, float cx, float top, float w, double t);

    applock_record_t record_{};
    bool             armed_ = false;

    const Purpose  purpose_;
    const uint32_t enrol_method_;
    /* Enrol asks TWICE. The first entry is kept here and compared against the
     * second: a lock set from a single entry is a lock set from a typo, and the
     * user finds out at the next launch, when it is too late to fix from
     * inside. */
    std::string    first_entry_;
    bool           awaiting_confirm_ = false;
    void           enrolSubmit(const std::string &plain);

    Mode  mode_   = Mode::Pin;
    int   cursor_ = 0;              /* cell under the D-pad, per mode */

    std::string   pin_;             /* digits typed so far */
    std::string   password_;        /* filled by the keyboard callback */
    unsigned char pattern_[9]{};
    int           pattern_len_ = 0;

    /* Touch/mouse drag state. `drawing_` is what tells a drag apart from a tap:
     * without it, a finger put down outside the grid and slid onto it would
     * start a pattern in the middle. */
    bool  drawing_    = false;
    bool  was_down_   = false;
    float last_px_ = 0, last_py_ = 0;   /* live segment endpoint, for the line */
    bool  have_point_ = false;

    std::vector<ui::Hint> hints_;    /* rebuilt when the mode or the entry changes */
    void rebuildHints();
    bool entryEmpty() const;
    /* What the footer was last built for. `paint` compares and rebuilds only on a
     * change: B's label depends on whether anything is entered, and rebuilding
     * three std::strings sixty times a second to answer a question whose answer
     * changes twice per entry would be wasteful. */
    int hint_key_ = -1;

    std::string subtitle_;
    std::string message_;
    bool        message_bad_ = false;
    /* True while the message is a countdown the header also shows live. Without
     * it the bottom line kept announcing "wait 30 s" long after the wait was
     * over, which reads as a lock that never lets go. */
    bool        message_is_wait_ = false;
    /* === ONE CLOCK, AND WHY THIS IS A REQUEST AND NOT AN INSTANT ===
     *
     * The refusal shake used to be stamped with `brls::getCPUTimeUsec()` and
     * compared against `paint`'s `t`, which comes from `std::chrono::
     * steady_clock` (ui/screen_base.cpp:40). Two epochs: the comparison was
     * meaningless, and on console the shake simply never played - the one piece
     * of feedback that needs neither sound nor reading.
     *
     * So the refusal RAISES A REQUEST and `paint` stamps it with the clock it
     * already has. One clock, by construction. */
    bool        shake_pending_ = false;
    double      shake_t_       = 0.0;   /* stamped in paint(); 0 = none */

    bool  busy_ = false;              /* a derivation is in flight */
    /* Latched once the right secret has been given. Every input path tests it:
     * the answer arrives on one frame and the host acts on the next, and a tap
     * landing in that gap would submit the still-buffered secret a second time -
     * pushing a second BootActivity. */
    bool  done_ = false;
    std::shared_ptr<Outcome>            outcome_;
    std::thread                         worker_;
    std::function<void(bool)>           on_unlocked_;
};
