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
 */
#pragma once

#include <QWidget>
#include <QVector>

class QLabel;
class QVBoxLayout;

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

protected:
    void changeEvent(QEvent *e) override;

private:
    void refresh(int index);

    struct Row {
        QLabel *label = nullptr;
        /* The SOURCE string, translated at every refresh rather than once at
         * construction - so a language switch re-labels the steps (QT5). */
        const char *title = nullptr;
        State   state = State::Pending;
        QString detail;
    };
    QVector<Row> rows_;
    QLabel *headline_ = nullptr;
};
