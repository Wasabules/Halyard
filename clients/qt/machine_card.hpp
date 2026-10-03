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
 */
#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;

class MachineCard : public QFrame
{
    Q_OBJECT

public:
    MachineCard(const QString &id, const QString &name, const QString &state,
                const QString &datacentre, QWidget *parent = nullptr);

    QString machineId() const { return id_; }
    void    setBusy(bool busy);     /* a session is running: nothing to connect to */

signals:
    void connectRequested(const QString &id);

protected:
    void mouseDoubleClickEvent(QMouseEvent *e) override;

private:
    QString      id_;
    QLabel      *pill_ = nullptr;
    QPushButton *connect_ = nullptr;
};
