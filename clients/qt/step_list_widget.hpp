/* StepListWidget - the seven bootstrap steps, and which one you are on.
 *
 * === QT2 2026-10-03 — THE STEP NAMES ARE SHARED ON PURPOSE =================
 *
 * The same seven labels as the Borealis client's connecting screen. Not for
 * consistency's own sake: when a user reports "it stops at step 2", both
 * clients must mean the same step, and a log line from one must be readable
 * against the other. The vocabulary belongs to the protocol, not to a UI.
 *
 * A step shows its DETAIL as well as its state, because that is where the
 * useful part lives — the address for step 2, the HTTP code for a failure, and
 * the elapsed seconds while the machine boots. `17/30` of an internal retry
 * count is what UX12 removed from the Borealis client for saying nothing.
 *
 * === UI1 2026-10-03 — A DRAWN DOT, NOT A GLYPH =============================
 *
 * The state was a leading `·` / `…` / `✓` / `✗` inside the row's text. Three
 * problems, all of them visible: the four glyphs have four different widths so
 * the titles did not line up; `✓` and `✗` are missing from several fonts and
 * came out as a box; and nothing moved, so a step that takes thirty seconds
 * (the machine booting) looked identical to one that had hung.
 *
 * Each row now paints its own 18px mark - a ring, a rotating arc, a check, a
 * cross - at a fixed width, and ONE timer drives the arc, running only while a
 * step is running. The detail moves to its own right-aligned label, so a long
 * address cannot push the title around.
 */
#pragma once

#include <QElapsedTimer>
#include <QWidget>
#include <QVector>

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class QVBoxLayout;

/* The mark at the left of a step. Separate so it can repaint without the row's
 * two labels relaying out on every animation frame. */
class StepDot : public QWidget
{
    Q_OBJECT

public:
    enum class State { Pending, Running, Done, Failed };

    explicit StepDot(QWidget *parent = nullptr);

    void setState(State s);
    State state() const { return state_; }
    void setPhase(qreal t) { phase_ = t; if (state_ == State::Running) update(); }

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    State state_ = State::Pending;
    qreal phase_ = 0.0;   /* turns, [0,1) - the arc's start angle */
};

class StepListWidget : public QWidget
{
    Q_OBJECT

public:
    enum class State { Pending, Running, Done, Failed };

    explicit StepListWidget(QWidget *parent = nullptr);

    void reset();
    void setState(int index, State s, const QString &detail = QString());

    /* A line above the steps, for what has no step of its own: the data centre,
     * the capability probe, a refused credential. */
    void setHeadline(const QString &text);

    /* UI3 - a failed bootstrap left the screen on a red step with no way out
     * but the window's close button. The two buttons appear only once a step
     * has failed, and `reset()` takes them away again. */
    void setFailed(bool on);

signals:
    void retryRequested();
    void backRequested();
    /* UI7 - give up while it is still going, not only after it has failed. */
    void cancelRequested();

protected:
    void changeEvent(QEvent *e) override;

private:
    void refresh(int index);
    void retimeAnimation();   /* the timer runs only while something is running */

    struct Row {
        QLabel  *label  = nullptr;
        QLabel  *detailLabel = nullptr;
        StepDot *dot    = nullptr;
        /* The SOURCE string, translated at every refresh rather than once at
         * construction - so a language switch re-labels the steps (QT5). */
        const char *title = nullptr;
        State   state = State::Pending;
        QString detail;
    };
    QVector<Row> rows_;
    QLabel *headline_ = nullptr;
    QLabel *subhead_  = nullptr;
    QTimer *anim_     = nullptr;
    /* === UI7 2026-10-03 - PROGRESS, AND WHAT IS WORTH LOOKING AT =========
     *
     * Seven lines with no sense of how far along they are: someone watching a
     * machine boot cannot tell whether to wait or to give up. So:
     *
     *  - `progress_`/`elapsed_` say "4 of 7" and the seconds, which together
     *    answer that question;
     *  - the LIVE row gets a tinted background and an accent bar, because bold
     *    text in a column of seven is not enough to find at a glance;
     *  - the finished rows collapse into one line after a moment. Borealis
     *    does the same, and for the same reason: four ticks are the part that
     *    is over, and they were taking four sevenths of the eye's attention.
     */
    QProgressBar *progress_ = nullptr;
    QLabel       *elapsed_  = nullptr;
    QPushButton  *cancel_   = nullptr;
    QPushButton  *collapsed_ = nullptr;   /* "4 completed" - click to expand */
    QLabel       *error_     = nullptr;   /* the detail, in full, on failure */
    QTimer       *clock_     = nullptr;
    QElapsedTimer since_;
    bool          expanded_  = false;
    void refreshProgress();
    void refreshCollapse();
    QPushButton *retry_  = nullptr;
    QPushButton *back_   = nullptr;
    QWidget     *footer_ = nullptr;
    qreal   phase_    = 0.0;
};

/* === UI4 2026-10-03 — THE SIGN-IN'S OWN THREE STEPS ========================
 *
 * The same idea as StepListWidget and the same dot, for the three phases of a
 * sign-in (`AuthWorker::SignInStage`). A separate class and not a parameter of
 * StepListWidget because the two differ in what they are driven BY: the
 * bootstrap reports each step's state independently, while a sign-in only ever
 * says "I am now in phase N" - everything below is done by construction, and
 * encoding that rule once here is what keeps the caller from having to mark
 * three rows on every signal.
 *
 * Compact on purpose: one line of three dots with their labels, not a column.
 * It sits above a code the user is meant to read, and a column of three would
 * push the code down for information that is only interesting while it moves.
 */
class SignInSteps : public QWidget
{
    Q_OBJECT

public:
    explicit SignInSteps(QWidget *parent = nullptr);

    /* Phase `index` is now running; everything before it is done. */
    void setStage(int index);
    /* The phase that was running failed; the rest stay as they are. */
    void setFailed();
    /* Every phase done - the row then has nothing left to say, so the caller
     * normally hides it. */
    void setComplete();
    void reset();

private:
    void refresh();

    struct Cell { StepDot *dot = nullptr; QLabel *label = nullptr; };
    QVector<Cell> cells_;
    QVector<const char *> titles_;
    int  stage_  = 0;
    bool failed_ = false;
    bool done_   = false;
    QTimer *anim_ = nullptr;
    qreal   phase_ = 0.0;
};
