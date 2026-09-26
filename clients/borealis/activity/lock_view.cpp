/* LockView - see lock_view.hpp, and shadow/applock.h for the threat model. */
#include "lock_view.hpp"

#include "../ui/i18n.hpp"
#include "../ui/paint.hpp"
#include "../ui/theme.hpp"
#include "../ui/sfx.hpp"
#include "../ui/haptics.hpp"
#include "../ui/lock_grid.h"
#include "../ui/touch.h"
#include "../ui/pointer.hpp"
#include "../device_mode.hpp"

#include "../../../core/common/log.h"
#define alockwarn(...) JOURNAL_WARN_(JOURNAL_CAT_UI, __VA_ARGS__)

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <cmath>
#include <cstring>
#include <cstdio>
#include <ctime>

namespace {

/* The pattern grid is 3x3, the PIN pad 3 columns by 4 rows: 1..9, then erase,
 * 0 and validate. That last row is why the pad is not simply a 3x3 - a keypad
 * with no visible way to erase makes people restart the whole entry. */
constexpr int PAD_COLS = 3, PAD_ROWS = 4;
constexpr int PAD_ERASE = 9, PAD_ZERO = 10, PAD_OK = 11;

constexpr float HIT_FRAC = 0.38f;   /* see ui_lock_grid_hit on why below 0.5 */
constexpr double SHAKE_S = 0.45;

int64_t now_seconds()
{
    return (int64_t)time(nullptr);
}

/* The digit a PIN-pad cell carries, or -1 for the three control cells. */
int pad_digit(int cell)
{
    if (cell >= 0 && cell <= 8) return cell + 1;   /* 1..9 in reading order */
    if (cell == PAD_ZERO) return 0;
    return -1;
}

}  // namespace

LockView::LockView(Purpose purpose, uint32_t method)
    : purpose_(purpose), enrol_method_(method)
{
    outcome_ = std::make_shared<Outcome>();
    armed_   = applock_store_load(&record_) != 0;
    if (purpose_ == Purpose::Enrol) {
        /* The mode is imposed by whoever opened the screen: you came here to set
         * a PIN, not to choose between three things again. */
        mode_ = (method == APPLOCK_PATTERN)  ? Mode::Pattern
              : (method == APPLOCK_PASSWORD) ? Mode::Password
                                             : Mode::Pin;
        clearEntry();
    } else {
        pickFirstMode();
    }
    rebuildHints();
}

LockView::~LockView()
{
    /* Joined, not abandoned. It costs at most one derivation (a few hundred
     * milliseconds) and it is what makes the lifetime trivial: after this line
     * no thread exists that could write into a destroyed view. A detached
     * thread would also leak its handle on HOS, which CLAUDE.md records as
     * needing a console reboot to clear. */
    if (worker_.joinable()) worker_.join();
}

/* ── Les modes ────────────────────────────────────────────────────────────── */

bool LockView::modeArmed(Mode m) const
{
    /* While enrolling, the requested mode is the only one available and it is
     * available whether or not a secret exists yet - that is the point. */
    if (purpose_ == Purpose::Enrol) return m == mode_;
    const uint32_t u = applock_usable_methods(&record_);
    switch (m) {
        case Mode::Pin:      return (u & APPLOCK_PIN) != 0;
        case Mode::Password: return (u & APPLOCK_PASSWORD) != 0;
        case Mode::Pattern:  return (u & APPLOCK_PATTERN) != 0;
    }
    return false;
}

void LockView::pickFirstMode()
{
    /* Only an ARMED mode is ever shown. A mode with no secret behind it would
     * refuse every entry, and the user would be trying to open a door that was
     * never fitted. */
    const Mode order[] = { Mode::Pin, Mode::Pattern, Mode::Password };
    for (Mode m : order) {
        if (modeArmed(m)) { mode_ = m; break; }
    }
    clearEntry();
}

void LockView::cycleMode(int dir)
{
    const Mode order[] = { Mode::Pin, Mode::Pattern, Mode::Password };
    int idx = 0;
    for (int i = 0; i < 3; i++) if (order[i] == mode_) idx = i;
    for (int step = 0; step < 3; step++) {
        idx = (idx + (dir >= 0 ? 1 : 2)) % 3;
        if (modeArmed(order[idx])) break;
    }
    if (order[idx] == mode_) return;      /* only one mode armed: nothing to do */
    mode_ = order[idx];
    clearEntry();
    rebuildHints();
    ui::sfx::play(ui::sfx::Sound::Section);
}

void LockView::clearEntry()
{
    pin_.clear();
    password_.clear();
    pattern_len_ = 0;
    cursor_      = (mode_ == Mode::Pattern) ? 4 : 0;
    drawing_     = false;
    have_point_  = false;
}

/* True when there is nothing left to erase - which is what decides B's meaning. */
bool LockView::entryEmpty() const
{
    switch (mode_) {
        case Mode::Pin:      return pin_.empty();
        case Mode::Pattern:  return pattern_len_ == 0;
        case Mode::Password: return true;   /* nothing is buffered on this screen */
    }
    return true;
}

void LockView::rebuildHints()
{
    hints_.clear();
    switch (mode_) {
        case Mode::Pin:
            hints_.push_back({ "A", ui::tr("lock/hint_press") });
            break;
        case Mode::Pattern:
            hints_.push_back({ "A", ui::tr("lock/hint_node") });
            hints_.push_back({ "X", ui::tr("lock/hint_validate") });
            break;
        case Mode::Password:
            hints_.push_back({ "A", ui::tr("lock/hint_type") });
            break;
    }

    /* === B HAS ONE MEANING AT A TIME, AND THE FOOTER SAYS WHICH ===
     *
     * It used to be listed twice while enrolling: the mode added "Erase" (or
     * "Start over") and the enrolment added "Cancel", so the footer showed two
     * B buttons doing different things. The BEHAVIOUR was already right -
     * `back()` erases while there is something to erase and leaves otherwise -
     * but a footer that contradicts itself is worse than one that says nothing:
     * the user cannot tell which of the two will happen.
     *
     * So the hint follows the state. With something entered, B erases; with the
     * entry empty, B cancels (enrolment) or does nothing worth announcing
     * (unlock, where the screen cannot be left at all). */
    if (!entryEmpty()) {
        hints_.push_back({ "B", ui::tr(mode_ == Mode::Pattern ? "lock/hint_clear"
                                                              : "lock/hint_erase") });
    } else if (purpose_ == Purpose::Enrol) {
        hints_.push_back({ "B", ui::tr("lock/hint_cancel") });
    }

    if (purpose_ == Purpose::Enrol) return;   /* no method switch while setting one */

    /* Offered only when there is somewhere to go: a hint for a button that does
     * nothing is worse than no hint. */
    int armed = 0;
    if (modeArmed(Mode::Pin)) armed++;
    if (modeArmed(Mode::Pattern)) armed++;
    if (modeArmed(Mode::Password)) armed++;
    if (armed > 1) hints_.push_back({ "Y", ui::tr("lock/hint_method") });
}

/* ── La verification ──────────────────────────────────────────────────────── */

void LockView::submit()
{
    if (busy_ || done_) return;

    /* Build the plaintext for the mode. The pattern goes through its canonical
     * form FIRST - the same bytes the secret was created from - otherwise the
     * gesture that set the pattern would not match itself. */
    std::string plain;
    uint32_t method = 0;
    if (mode_ == Mode::Pin) {
        if (!applock_pin_valid(pin_.c_str())) {
            message_ = ui::tr("lock/too_short");
            message_bad_ = true;
            return;
        }
        plain = pin_;
        method = APPLOCK_PIN;
    } else if (mode_ == Mode::Password) {
        if (!applock_password_valid(password_.c_str())) {
            message_ = ui::tr("lock/too_short");
            message_bad_ = true;
            return;
        }
        plain = password_;
        method = APPLOCK_PASSWORD;
    } else {
        unsigned char canon[APPLOCK_PATTERN_MAX];
        char enc[APPLOCK_PATTERN_MAX + 1];
        const int n = applock_pattern_canonical(pattern_, (size_t)pattern_len_,
                                                canon, sizeof canon);
        if (n < 0 || !applock_pattern_valid(canon, (size_t)n) ||
            applock_pattern_bytes(canon, (size_t)n, enc, sizeof enc) < 0) {
            message_ = ui::tr("lock/too_short");
            message_bad_ = true;
            clearEntry();
            return;
        }
        plain = enc;
        method = APPLOCK_PATTERN;
    }

    if (purpose_ == Purpose::Enrol) { enrolSubmit(plain); return; }

    busy_ = true;
    message_.clear();

    /* Off the render thread, on a thread of our own - see the header for why the
     * shared Borealis loop cannot be used here. 60 000 rounds measured at 55 ms
     * on an x86_64 laptop, so a few hundred on the console. */
    auto outcome = outcome_;
    const applock_record_t snapshot = record_;   /* by value: see Outcome::record */
    const int64_t now = now_seconds();

    if (worker_.joinable()) worker_.join();   /* cannot happen while busy_, but free */
    worker_ = std::thread([outcome, snapshot, method, plain, now]() {
        applock_record_t local = snapshot;
        int64_t retry = 0;
        const applock_result_t r =
            applock_store_verify(&local, method, plain.c_str(), now, &retry);
        outcome->record   = local;
        outcome->result   = r;
        outcome->retry_in = retry;
        /* Published LAST, and the only atomic in the group: the release on this
         * store pairs with the acquire on `paint`'s load, which is what makes
         * the three plain writes above visible to the reader. */
        outcome->ready.store(true);
    });
}

void LockView::enrolSubmit(const std::string &plain)
{
    if (!awaiting_confirm_) {
        first_entry_     = plain;
        awaiting_confirm_ = true;
        message_     = ui::tr("lock/confirm");
        message_bad_ = false;
        clearEntry();
        rebuildHints();
        return;
    }

    if (plain != first_entry_) {
        /* Start over from the FIRST entry, not from the confirmation: the user
         * does not know which of the two was the typo, and asking them to
         * confirm a first entry they may have got wrong would arm a lock nobody
         * can open. */
        awaiting_confirm_ = false;
        first_entry_.clear();
        message_     = ui::tr("lock/mismatch");
        message_bad_ = true;
        shake_pending_ = true;
        ui::sfx::play(ui::sfx::Sound::Failed);
        clearEntry();
        rebuildHints();
        return;
    }

    /* The two entries agree. Derive and write. This is slow, like a check, and
     * for once blocking is acceptable: the screen has nothing else to do and
     * the user has just pressed the last key of a deliberate action. */
    applock_record_t rec;
    if (!applock_store_load(&rec)) applock_record_init(&rec);

    /* === THE MASTER KEY: INHERITED, OR CREATED ONCE ===
     *
     * Arming a SECOND method must seal the SAME master key, not a new one: a new
     * one would leave the token sealed under a key nobody holds any more, and
     * adding a convenience would silently throw the session away.
     *
     * It is available because enrolment is reachable only from Settings, i.e.
     * only after the lock has been opened this session - which is what loaded
     * it. The only case with nothing to inherit is the FIRST method ever armed;
     * there we make one, and the caller re-seals the token under it. */
    /* === WHY EACH FAILURE NAMES ITSELF (2026-09-02) ===
     *
     * All four ways this can fail used to show the same sentence, "cannot write
     * to the card". When a user hit it there was no way to tell whether the
     * random generator, the derivation, the sealing or the write had refused -
     * four causes, one symptom, and they call for opposite fixes. That is the
     * PM5 lesson applied to an error message: show the STAGE, not a verdict.
     *
     * The step also goes to the log, so a pulled journal answers it without the
     * user having to read anything off the screen. */
    auto fail = [this](const char *step) {
        alockwarn("[lock] enrolment FAILED at step: %s", step);
        message_ = ui::tr("lock/card_error") + " (" + step + ")";
        message_bad_ = true;
        awaiting_confirm_ = false;
        first_entry_.clear();
        clearEntry();
    };

    if (!applock_master_ready() && !applock_master_create()) { fail("master"); return; }

    /* Arming a method clears any lockout in force: the owner has just proved
     * they are the owner, and leaving them to serve a wait earned before the
     * lock even existed would be absurd.
     *
     * Cleared BEFORE the write and not after: `applock_store_rewrap` already
     * saves, so doing it afterwards meant writing the record twice for one
     * enrolment - two chances to fail, and the second failure reported "cannot
     * write to the card" after the secret was already safely stored. */
    applock_note_success(&rec);

    /* `applock_store_rewrap` picks the slot, derives, seals the master key under
     * the new secret and writes the record - one call, so the four steps cannot
     * be done in the wrong order or half done. */
    if (!applock_store_rewrap(&rec, enrol_method_, plain.c_str())) { fail("rewrap"); return; }

    /* The plaintext is not kept a moment longer than it takes to hash it. */
    first_entry_.assign(first_entry_.size(), '\0');
    first_entry_.clear();
    password_.assign(password_.size(), '\0');
    password_.clear();

    ui::sfx::play(ui::sfx::Sound::Confirm);
    if (on_unlocked_) on_unlocked_(true);
}

void LockView::applyOutcome()
{
    if (!outcome_->ready.load()) return;
    outcome_->ready.store(false);
    busy_ = false;
    /* Take back the record the worker wrote: it holds the failure counter and
     * the deadline that are already on the card. Keeping our stale copy would
     * make the screen offer attempts the card has already refused. */
    record_ = outcome_->record;

    switch (outcome_->result) {
        case APPLOCK_OK:
            message_.clear();
            /* The secret is not kept a frame longer than it took to check it,
             * and `done_` closes every input path: the host acts on the NEXT
             * frame, and a tap landing in that gap would submit the same buffer
             * again. */
            done_ = true;
            clearEntry();
            ui::sfx::play(ui::sfx::Sound::Confirm);
            if (on_unlocked_) on_unlocked_(true);
            return;
        case APPLOCK_THROTTLED:
            message_ = ui::tr("lock/wait", (long long)outcome_->retry_in);
            message_is_wait_ = true;
            break;
        case APPLOCK_NO_METHOD:
            message_ = ui::tr("lock/no_method");
            break;
        case APPLOCK_ERROR:
            /* The card refused the write. Saying so matters: the attempt was
             * REFUSED, not merely wrong, and retrying will not help until the
             * card does. */
            message_ = ui::tr("lock/card_error");
            break;
        case APPLOCK_WRONG:
        default:
            message_ = outcome_->retry_in > 0
                     ? ui::tr("lock/wrong_wait", (long long)outcome_->retry_in)
                     : ui::tr("lock/wrong");
            message_is_wait_ = outcome_->retry_in > 0;
            break;
    }
    message_bad_ = true;
    shake_pending_ = true;
    ui::sfx::play(ui::sfx::Sound::Failed);
    ui::haptics::play(ui::haptics::Intent::Limit);
    clearEntry();
}

/* ── The system keyboard (the password) ───────────────────────────────────── */

void LockView::openKeyboard()
{
    if (busy_ || done_) return;

#ifdef __SWITCH__
    /* === WHY THIS DOES NOT GO THROUGH brls::ImeManager ===
     *
     * `SwitchImeManager::openForText` always calls
     * `swkbdConfigMakePresetDefault`, and there is NO password masking anywhere
     * in the Borealis stack - not on Switch, not on desktop. On a lock screen
     * that is the whole problem: the software keyboard is a full-screen
     * overlay, so an unmasked field prints the password across the television
     * for exactly the person this lock exists to keep out.
     *
     * `swkbdConfigMakePresetPassword` sets the masking flag (and a QWERTY
     * layout and a blurred background) in one call. Twenty lines of libnx buy
     * the one property that matters here, so we call it directly.
     *
     * This BLOCKS the UI thread until the keyboard closes - which is what the
     * Borealis path does too - and that is acceptable because nothing else is
     * happening: the screen is waiting for exactly this. */
    SwkbdConfig cfg;
    if (R_FAILED(swkbdCreate(&cfg, 0))) {
        message_ = ui::tr("lock/no_keyboard");
        message_bad_ = true;
        return;
    }
    swkbdConfigMakePresetPassword(&cfg);
    swkbdConfigSetGuideText(&cfg, ui::tr("lock/password_sub").c_str());
    swkbdConfigSetHeaderText(&cfg, ui::tr("lock/password").c_str());
    /* The applet counts CHARACTERS; our maximum is in BYTES, and one accented
     * character is two bytes, one CJK character three. Sizing the buffer at the
     * byte maximum would let the applet fill it past the end - it truncates
     * rather than overflowing, so the symptom would be a password silently cut
     * short and impossible to enter again. Four bytes per character covers every
     * UTF-8 sequence; the byte limit is then enforced by
     * `applock_password_valid`, which is where it belongs. */
    swkbdConfigSetStringLenMax(&cfg, APPLOCK_PASSWORD_MAX);

    char buf[APPLOCK_PASSWORD_MAX * 4 + 1] = {0};
    const Result rc = swkbdShow(&cfg, buf, sizeof buf);
    swkbdClose(&cfg);
    if (R_SUCCEEDED(rc) && buf[0]) {
        password_.assign(buf);
        submit();
    }
    /* Wiped whatever happened: a cancelled entry left the password on this
     * stack frame just as surely as an accepted one. */
    memset(buf, 0, sizeof buf);
#else
    brls::ImeManager *ime = brls::Application::getImeManager();
    if (!ime) {
        message_ = ui::tr("lock/no_keyboard");
        message_bad_ = true;
        return;
    }
    /* Desktop: an in-app dialog fed by a PHYSICAL keyboard, and unmasked -
     * Borealis has no on-screen key grid and no password field. It is left as
     * it is because the desktop build is the development one; the console is
     * where the lock has to hold, and the console path above is masked.
     *
     * The callback is not called when the user cancels, so there is no "user
     * aborted" branch to write: nothing happens, which is correct. */
    ime->openForText([this](std::string text) {
        password_ = std::move(text);
        if (!password_.empty()) submit();
    }, ui::tr("lock/password"), ui::tr("lock/password_sub"),
       APPLOCK_PASSWORD_MAX, "");
#endif
}

/* ── Les buttons ──────────────────────────────────────────────────────────── */

bool LockView::up()    { if (mode_ == Mode::Password || busy_ || done_) return true;
                         cursor_ = ui_lock_grid_move(cursor_, 0, -1,
                                   mode_ == Mode::Pattern ? 3 : PAD_COLS,
                                   mode_ == Mode::Pattern ? 3 : PAD_ROWS);
                         return true; }
bool LockView::down()  { if (mode_ == Mode::Password || busy_ || done_) return true;
                         cursor_ = ui_lock_grid_move(cursor_, 0, 1,
                                   mode_ == Mode::Pattern ? 3 : PAD_COLS,
                                   mode_ == Mode::Pattern ? 3 : PAD_ROWS);
                         return true; }
bool LockView::left()  { if (mode_ == Mode::Password || busy_ || done_) return true;
                         cursor_ = ui_lock_grid_move(cursor_, -1, 0,
                                   mode_ == Mode::Pattern ? 3 : PAD_COLS,
                                   mode_ == Mode::Pattern ? 3 : PAD_ROWS);
                         return true; }
bool LockView::right() { if (mode_ == Mode::Password || busy_ || done_) return true;
                         cursor_ = ui_lock_grid_move(cursor_, 1, 0,
                                   mode_ == Mode::Pattern ? 3 : PAD_COLS,
                                   mode_ == Mode::Pattern ? 3 : PAD_ROWS);
                         return true; }

bool LockView::activate()
{
    if (busy_ || done_) return true;
    message_.clear();

    if (mode_ == Mode::Password) { openKeyboard(); return true; }

    if (mode_ == Mode::Pattern) {
        const int before = pattern_len_;
        pattern_len_ = ui_lock_grid_append(pattern_, pattern_len_,
                                           APPLOCK_PATTERN_MAX, cursor_);
        if (pattern_len_ != before) {
            ui::sfx::play(ui::sfx::Sound::Navigation);
            ui::haptics::play(ui::haptics::Intent::Key);
        }
        return true;
    }

    /* PIN pad. */
    if (cursor_ == PAD_OK)    { submit(); return true; }
    if (cursor_ == PAD_ERASE) { if (!pin_.empty()) pin_.pop_back(); return true; }
    {
        const int d = pad_digit(cursor_);
        if (d >= 0 && pin_.size() < (size_t)APPLOCK_PIN_MAX) {
            pin_.push_back((char)('0' + d));
            ui::sfx::play(ui::sfx::Sound::Navigation);
            ui::haptics::play(ui::haptics::Intent::Key);
            /* No auto-submit at the minimum length: a PIN may be longer than
             * four digits, and submitting at four would make every longer PIN
             * impossible to enter. */
        }
    }
    return true;
}

bool LockView::back()
{
    /* ALWAYS true. Returning false lets Borealis pop the activity, and this is
     * the screen that cannot be left. B is given a meaning instead, so it does
     * something rather than appearing broken. */
    if (busy_ || done_) return true;
    if (mode_ == Mode::Pin && !pin_.empty())          { pin_.pop_back(); return true; }
    if (mode_ == Mode::Pattern && pattern_len_)       { pattern_len_ = 0; return true; }
    if (purpose_ == Purpose::Enrol) {
        /* Nothing left to erase: B LEAVES. This screen can be left because
         * refusing to set a lock is a legitimate answer - unlike refusing to
         * open one. Consuming the press here would trap the user on a settings
         * page. */
        if (on_unlocked_) on_unlocked_(false);
        return true;
    }
    message_.clear();
    return true;
}

bool LockView::buttonX()
{
    if (busy_ || done_) return true;
    if (mode_ == Mode::Pattern && pattern_len_ > 0) submit();
    return true;
}

bool LockView::buttonY()
{
    if (busy_ || done_) return true;
    if (purpose_ == Purpose::Enrol) return true;   /* no switching mid-enrolment */
    cycleMode(1);
    return true;
}

/* ── The finger and the mouse ─────────────────────────────────────────────── */

void LockView::pollTouch(float gx, float gy, float side, float px, float py,
                         bool down)
{
    const int cols = (mode_ == Mode::Pattern) ? 3 : PAD_COLS;
    const int rows = (mode_ == Mode::Pattern) ? 3 : PAD_ROWS;
    const float gh = (mode_ == Mode::Pattern) ? side
                                              : side * (float)PAD_ROWS / (float)PAD_COLS;
    const int cell = down ? ui_lock_grid_hit(gx, gy, side, gh, cols, rows,
                                             px, py, HIT_FRAC)
                          : -1;

    if (mode_ == Mode::Pattern) {
        if (down) {
            /* A drag only starts on the PRESS, and only on a node. Two
             * conditions and both earn their place: without the node test, a
             * finger put down outside the grid and slid onto it would begin a
             * pattern in the middle; without the press test, letting go outside
             * the grid and coming back would silently continue the same gesture
             * - the user would have drawn two and entered one. */
            if (!drawing_ && !was_down_ && cell >= 0) drawing_ = true;
            if (drawing_ && cell >= 0) {
                const int before = pattern_len_;
                pattern_len_ = ui_lock_grid_append(pattern_, pattern_len_,
                                                   APPLOCK_PATTERN_MAX, cell);
                if (pattern_len_ != before) {
                    cursor_ = cell;
                    ui::sfx::play(ui::sfx::Sound::Navigation);
                    ui::haptics::play(ui::haptics::Intent::Key);
                }
            }
            if (drawing_) { last_px_ = px; last_py_ = py; have_point_ = true; }
        } else if (was_down_ && drawing_) {
            /* Lifting the finger validates, exactly as on a phone. */
            drawing_ = false;
            have_point_ = false;
            if (pattern_len_ > 0) submit();
        }
    } else if (mode_ == Mode::Pin) {
        /* A key fires on RELEASE, over the same cell it was pressed on. Firing
         * on press makes a mis-aimed touch impossible to take back. */
        if (down && cell >= 0) cursor_ = cell;
        else if (was_down_ && !down && cell < 0) { /* released off the pad */ }
        if (was_down_ && !down) {
            const int c = ui_lock_grid_hit(gx, gy, side, gh, cols, rows,
                                           last_px_, last_py_, HIT_FRAC);
            if (c >= 0 && c == cursor_) activate();
        }
        if (down) { last_px_ = px; last_py_ = py; }
    }
    was_down_ = down;
}

/* ── Le dessin ────────────────────────────────────────────────────────────── */

void LockView::drawPin(NVGcontext *vg, float cx, float top, float w, double t)
{
    (void)t;
    const float side = w;
    const float gh   = side * (float)PAD_ROWS / (float)PAD_COLS;
    const float gx   = cx - side * 0.5f;

    /* The entered digits, as discs. Showing the digits themselves would put the
     * PIN on screen for anyone standing behind the console - which is the
     * situation this lock is for. */
    {
        const float r = 9.0f, gap = 30.0f;
        const int   n = (int)pin_.size();
        const float total = (n > 0 ? (n - 1) * gap : 0.0f);
        for (int i = 0; i < n; i++) {
            nvgBeginPath(vg);
            nvgCircle(vg, cx - total * 0.5f + i * gap, top - 34.0f, r);
            nvgFillColor(vg, ui::paint::accentVif);
            nvgFill(vg);
        }
    }

    for (int cell = 0; cell < PAD_COLS * PAD_ROWS; cell++) {
        float kx, ky;
        ui_lock_grid_center(gx, top, side, gh, PAD_COLS, PAD_ROWS, cell, &kx, &ky);
        const bool focus = (cell == cursor_);
        const float r = (side / PAD_COLS) * 0.34f;

        nvgBeginPath(vg);
        nvgCircle(vg, kx, ky, r);
        nvgFillColor(vg, focus ? ui::paint::cardBgFocusTop : ui::paint::cardBgTop);
        nvgFill(vg);
        nvgStrokeColor(vg, focus ? ui::paint::accent : ui::paint::cardBorder);
        nvgStrokeWidth(vg, focus ? 2.5f : 1.2f);
        nvgStroke(vg);

        char label[8];
        const int d = pad_digit(cell);
        if (d >= 0)                 std::snprintf(label, sizeof label, "%d", d);
        else if (cell == PAD_ERASE) std::snprintf(label, sizeof label, "%s", "<");
        else                        std::snprintf(label, sizeof label, "%s", "OK");

        nvgFontSize(vg, cell == PAD_OK || cell == PAD_ERASE ? 22.0f : 30.0f);
        nvgFontFace(vg, ui::theme::font());
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, ui::theme::value);
        nvgText(vg, kx, ky, label, nullptr);
    }
}

void LockView::drawPattern(NVGcontext *vg, float cx, float top, float w, double t)
{
    (void)t;
    const float side = w;
    const float gx   = cx - side * 0.5f;

    /* The drawn path first, UNDER the nodes: over them it would hide the very
     * node the finger is on. */
    if (pattern_len_ > 0) {
        nvgBeginPath(vg);
        for (int i = 0; i < pattern_len_; i++) {
            float nx, ny;
            ui_lock_grid_center(gx, top, side, side, 3, 3, pattern_[i], &nx, &ny);
            if (i == 0) nvgMoveTo(vg, nx, ny); else nvgLineTo(vg, nx, ny);
        }
        if (have_point_) nvgLineTo(vg, last_px_, last_py_);
        nvgStrokeColor(vg, ui::paint::accentVif);
        nvgStrokeWidth(vg, 5.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        nvgStroke(vg);
    }

    for (int n = 0; n < 9; n++) {
        float nx, ny;
        ui_lock_grid_center(gx, top, side, side, 3, 3, n, &nx, &ny);
        bool used = false;
        for (int i = 0; i < pattern_len_; i++) if (pattern_[i] == n) used = true;
        const bool focus = (n == cursor_);
        const float r = (side / 3.0f) * 0.16f;

        nvgBeginPath(vg);
        nvgCircle(vg, nx, ny, r * (used ? 1.35f : 1.0f));
        nvgFillColor(vg, used ? ui::paint::accentVif : ui::paint::cardBgTop);
        nvgFill(vg);
        nvgStrokeColor(vg, focus ? ui::paint::accent : ui::paint::cardBorder);
        nvgStrokeWidth(vg, focus ? 3.0f : 1.5f);
        nvgStroke(vg);
    }
}

void LockView::drawPassword(NVGcontext *vg, float cx, float top, float w, double t)
{
    (void)t; (void)w;
    nvgFontSize(vg, 22.0f);
    nvgFontFace(vg, ui::theme::font());
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, ui::theme::label);
    nvgText(vg, cx, top + 60.0f, ui::tr("lock/password_prompt").c_str(), nullptr);
}

void LockView::paint(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    const float margin = device::isDocked() ? UI_MARGIN_DOCK : UI_MARGIN_HANDHELD;
    const float px = x + margin;
    const float pw = w - margin * 2.0f;

    ui::paint::shadowBg(vg, x, y, w, h);
    ui::paint::backgroundWaves(vg, x, y, w, h, t);

    /* The answer from the worker is picked up here, on the render thread, never
     * written from the worker itself - the same discipline as every other
     * deferred result in this application. */
    applyOutcome();

    std::string sub = subtitle_;
    int64_t left = 0;
    /* The throttle governs OPENING the lock, never setting one: a wait earned
     * by wrong guesses must not stop the owner from changing their PIN. */
    if (purpose_ == Purpose::Unlock &&
        applock_locked_out(&record_, now_seconds(), &left))
        sub = ui::tr("lock/wait", (long long)left);

    const std::string title = (purpose_ == Purpose::Enrol)
                            ? ui::tr(awaiting_confirm_ ? "lock/enrol_again" : "lock/enrol")
                            : ui::tr("lock/title");
    ui::drawHeader(vg, px, y, pw, title, sub, "", t);

    /* A refusal shakes the entry area. It is the one piece of feedback that
     * needs no reading and no sound - which matters on a console whose volume
     * may be down and whose owner is watching from across the room. */
    /* The footer follows the entry: B says "erase" while there is something to
     * erase and "cancel" when there is not. Rebuilt only when that answer
     * changes, not every frame. */
    {
        const int key = (int)mode_ * 4 + (entryEmpty() ? 0 : 1)
                      + (awaiting_confirm_ ? 2 : 0);
        if (key != hint_key_) { hint_key_ = key; rebuildHints(); }
    }

    if (shake_pending_) { shake_pending_ = false; shake_t_ = t; }
    float shake = 0.0f;
    if (shake_t_ > 0.0 && t - shake_t_ < SHAKE_S) {
        const float u = (float)((t - shake_t_) / SHAKE_S);
        shake = std::sin(u * 28.0f) * 14.0f * (1.0f - u);
    }

    /* The shake moves what is DRAWN, never where the fingers land. Offsetting
     * the hit test too meant a key pressed and released in the same place was
     * dropped, because the target had moved out from under the finger between
     * the two frames. */
    const float cx_hit = x + w * 0.5f;
    const float cx     = cx_hit + shake;
    const float area = (mode_ == Mode::Pattern) ? 300.0f : 270.0f;
    const float top  = y + UI_HEADER_H + 70.0f;

    switch (mode_) {
        case Mode::Pin:      drawPin(vg, cx, top, area, t); break;
        case Mode::Pattern:  drawPattern(vg, cx, top, area, t); break;
        case Mode::Password: drawPassword(vg, cx, top, area, t); break;
    }

    /* The finger and the mouse, read AFTER the geometry is known: the same
     * frame's layout answers the same frame's touch, which is what keeps the
     * drawn line under the finger rather than one frame behind it. */
    if (!busy_ && !done_ && (purpose_ == Purpose::Enrol ||
                   !applock_locked_out(&record_, now_seconds(), nullptr))) {
        const float gx = cx_hit - area * 0.5f;
        ui_touch_t f = ui_touch_current();
        if (f.active) {
            pollTouch(gx, top, area, f.x, f.y, true);
        } else {
            float mx = 0, my = 0; bool pressed = false;
            if (ui::pointer::state(mx, my, pressed))
                pollTouch(gx, top, area, mx, my, pressed);
            else
                pollTouch(gx, top, area, 0, 0, false);
        }
    }

    /* A countdown that has run out stops being displayed: the header already
     * carries the live one, and two numbers disagreeing on the same screen is
     * worse than one. */
    if (message_is_wait_ && !applock_locked_out(&record_, now_seconds(), nullptr)) {
        message_.clear();
        message_is_wait_ = false;
    }
    if (!message_.empty()) {
        nvgFontSize(vg, 19.0f);
        nvgFontFace(vg, ui::theme::font());
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, message_bad_ ? ui::theme::bad : ui::theme::label);
        nvgText(vg, x + w * 0.5f, y + h - UI_FOOTER_H - 34.0f,
                message_.c_str(), nullptr);
    }

    ui::drawFooter(vg, px, y + h - UI_FOOTER_H, pw, UI_FOOTER_H,
                   hints_, busy_ ? ui::tr("lock/checking") : std::string(),
                   busy_, t);
}
