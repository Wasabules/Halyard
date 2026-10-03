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

#include <QWidget>
#include <QVector>

class QLabel;
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
    QPushButton *retry_  = nullptr;
    QPushButton *back_   = nullptr;
    QWidget     *footer_ = nullptr;
    qreal   phase_    = 0.0;
};
