/* MachineCard - one machine, as a card you click rather than a list row.
 *
 * === UI1 2026-10-03 — WHY A CARD AND NOT A QListWidget ROW =================
 *
 * The machine list had one line of text per machine, with the state crammed in
 * parentheses. That is a debug view: the three things a person needs before
 * connecting - which machine, is it awake, where does it run - were one string
 * they had to parse.
 *
 * A card gives each its own place: the name reads as a title, the state is a
 * coloured pill (awake / asleep / starting), the data centre sits underneath,
 * and Connect is a real button on the card rather than a shared one at the
 * bottom whose target you infer from the selection. Double-clicking the card
 * connects too, because the old list taught that gesture.
 *
 * === UI5 2026-10-03 - THE KEYBOARD, WHICH THE LIST GAVE FOR FREE ==========
 *
 * The `QListWidget` this replaced came with arrow keys, Enter, Home and End,
 * and nobody had to write a line of it. A `QFrame` has none of that, so the
 * redesign quietly made the machine list mouse-only - a regression, and the
 * kind that does not show up in a screenshot.
 *
 * So the card takes focus (`StrongFocus`), draws its own focus ring, and acts
 * on Enter, Return and Space. The ARROWS are not handled here: moving between
 * siblings is the container's business, and a card that reached for its
 * neighbours would have to know how they are laid out. The window does it.
 */
#pragma once

#include <QDateTime>
#include <QFrame>
#include <QString>

class QEnterEvent;
class QGraphicsDropShadowEffect;
class QKeyEvent;
class QLabel;
class QPushButton;

class MachineCard : public QFrame
{
    Q_OBJECT

public:
    /* VMK1 - what the server says about THIS machine. `datacentre` is now the
     * machine's own (`/vms.datacenter`) and not the account-wide name from
     * TINAG, which the first version painted onto every card. */
    struct Info {
        QString id, name, state, datacentre, hwconfig, tags;
        bool    maintenance = false;
    };

    MachineCard(const Info &info, QWidget *parent = nullptr);

    /* === UI6 2026-10-03 - WHAT THE CARD IS FOR ============================
     *
     * The card carried a name and a button, which is a list row with rounded
     * corners. The three things that actually decide which machine to pick are
     * where it runs, when it was last used, and whether it is awake - and the
     * launcher only answers the first two (it sends `status: null` and the run
     * state arrives later on the SSE stream).
     *
     * `setLastUsed` takes the local record, not a server field: nothing in the
     * protocol remembers which machine THIS computer connected to, and that is
     * the one ordering question a person actually has. */
    void setLastUsed(const QDateTime &when);

    QString machineId() const { return id_; }
    /* UI5 - fire the same action the button does, for the container's Enter. */
    void    activate();
    void    setBusy(bool busy);     /* a session is running: nothing to connect to */
    bool    underMaintenance() const { return maintenance_; }

signals:
    void connectRequested(const QString &id);

protected:
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    /* UI6 - the lift on hover. `enterEvent`/`leaveEvent` and not a `:hover`
     * rule, because a stylesheet cannot animate: the shadow's blur radius has
     * to be driven by a QPropertyAnimation. */
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;

private:
    QString      id_;
    bool         maintenance_ = false;
    QLabel      *pill_ = nullptr;
    QLabel      *last_used_ = nullptr;
    class QGraphicsDropShadowEffect *shadow_ = nullptr;
    void animateElevation(int to);
    QPushButton *connect_ = nullptr;
};
