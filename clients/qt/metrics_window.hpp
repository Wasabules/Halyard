/* MetricsWindow - what the session is actually doing, in a window of its own.
 *
 * === QT3 2026-10-03 — WHY THE NUMBERS DESERVE A WINDOW =====================
 *
 * The Borealis client carries a panel over the stream, drawn by its own
 * framework, because a console has one screen. A desktop does not, and these
 * numbers are read WHILE watching the picture — so they go on a second window
 * that can sit on another monitor. That is the first thing Qt buys that the
 * console UI could not.
 *
 * It shows two things, and the distinction matters:
 *
 *   THE GRANT SNAPSHOT (`session_caps.h`), which is what the SERVER decided:
 *   which of the eight channels exist, the transport it chose, the ABSOLUTE
 *   port for each, the resolution and codec as granted rather than as asked.
 *   Asking is not getting - KB §3.37 - and this is the only place a client can
 *   see the difference.
 *
 *   THE COUNTERS, which are what actually arrived: pictures decoded, packets
 *   lost over how many, NACKs, the audio gap. `lost=0/6393` is the number that
 *   says a session is healthy, and it is meaningless without its denominator -
 *   which is why both halves are shown and not a percentage.
 *
 * Polled, not pushed. A session emits nothing for this; `ctrl_session_caps()`
 * is a snapshot read under no lock (it is published once at bootstrap), and a
 * timer at 1 Hz costs nothing. Pushing would mean a signal per counter change,
 * which is per packet.
 */
#pragma once

#include <QWidget>

class QLabel;
class QTableWidget;
class QTimer;

class MetricsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MetricsWindow(QWidget *parent = nullptr);

protected:
    /* Polling stops when the window is not visible: these numbers are only
     * interesting to somebody looking at them. */
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;

private slots:
    void refresh();
    void retranslate();

private:
    QLabel       *summary_  = nullptr;
    QTableWidget *channels_ = nullptr;
    QTimer       *timer_    = nullptr;
};
