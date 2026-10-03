/* MachineCard - see the header for why a card. */
#include "machine_card.hpp"

#include "machine_state.hpp"
#include "theme.hpp"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QPropertyAnimation>
#include <QPainter>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace theme = halyard::theme;

MachineCard::MachineCard(const Info &info, QWidget *parent)
    : QFrame(parent), id_(info.id), maintenance_(info.maintenance)
{
    const QString &name = info.name;
    const QString &state = info.state;
    const QString &datacentre = info.datacentre;
    setProperty("card", true);
    setProperty("cardHover", true);
    /* UI5 - focusable, and reachable by Tab as well as by the arrows. */
    setFocusPolicy(Qt::StrongFocus);
    shadow_ = theme::elevation(this, 16);
    setGraphicsEffect(shadow_);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(theme::SpaceGroup, theme::SpaceRow,
                            theme::SpaceGroup, theme::SpaceRow);
    row->setSpacing(theme::SpaceGroup);

    auto *texts = new QVBoxLayout;
    texts->setSpacing(theme::SpaceTight);

    auto *title = new QLabel(name.isEmpty() ? info.id : name, this);
    title->setProperty("h2", true);
    texts->addWidget(title);

    auto *sub = new QHBoxLayout;
    sub->setSpacing(theme::SpaceRow);
    /* UI1 - NO pill when the launcher gave no state.
     *
     * It does so routinely: `launcher: the server sends `status: null` - the
     * VM list carries no run state; it arrives on the SSE stream`. A grey
     * "unknown" chip on every single card is noise that says nothing, and
     * worse, it reads as a claim - someone scanning the list sees a dull pill
     * and concludes the machine is asleep. Saying nothing is the honest
     * rendering of knowing nothing. */
    if (!state.isEmpty()) {
        pill_ = new QLabel(state, this);
        pill_->setProperty("pill", QString::fromUtf8(halyard::pillClassFor(state)));
        sub->addWidget(pill_);
    }
    /* === VMK1 — MAINTENANCE FIRST ========================================
      *
      * The server tells us a machine is unavailable and the first version
      * never asked. Connecting to one succeeds through the launcher and then
      * fails at step 5 or 6, which reads as our bug rather than as the
      * machine being down. A pill, before everything else, and Connect is
      * disabled below. */
    if (info.maintenance) {
        auto *mt = new QLabel(tr("maintenance"), this);
        mt->setProperty("pill", QStringLiteral("busy"));
        sub->addWidget(mt);
    }

    /* UI6/VMK1 - where it runs, and on what. The data centre is the
     * machine's own now; the hardware tier used to be invisible because the
     * parser only used it as a fallback for an empty name. */
    has_datacentre_ = !datacentre.isEmpty();
    if (!datacentre.isEmpty()) {
        auto *dc = new QLabel(datacentre, this);
        dc->setProperty("dim", true);
        sub->addWidget(dc);
    }
    if (!info.hwconfig.isEmpty() && info.hwconfig != name) {
        auto *hw = new QLabel(info.hwconfig, this);
        hw->setProperty("dim", true);
        sub->addWidget(hw);
    }

    /* UI6 - the local record of the last connection, filled by setLastUsed.
     * Created empty rather than conditionally, so the row does not change
     * height the first time a machine is used. */
    last_used_ = new QLabel(this);
    last_used_->setProperty("dim", true);
    sub->addWidget(last_used_);

    sub->addStretch(1);
    texts->addLayout(sub);

    connect_ = new QPushButton(tr("Connect"), this);
    connect_->setProperty("accent", true);
    if (info.maintenance) {
        connect_->setEnabled(false);
        connect_->setToolTip(tr("Shadow reports this machine as under "
                                "maintenance. Connecting would fail partway "
                                "through the bootstrap."));
    }
    QObject::connect(connect_, &QPushButton::clicked, this,
                     [this] { emit connectRequested(id_); });

    row->addLayout(texts, 1);
    row->addWidget(connect_, 0, Qt::AlignVCenter);
}

/* UI6 - "today", "yesterday", "4 days ago", then a date.
 *
 * Relative and not a timestamp, because the question is "is this the one I was
 * on" and not "what was the hour". Past a week the relative form stops helping
 * ("23 days ago" is not a thing anyone pictures) and the date is shorter. */
void MachineCard::setLastUsed(const QDateTime &when)
{
    if (!last_used_) return;
    if (!when.isValid()) { last_used_->clear(); return; }

    const qint64 days = when.date().daysTo(QDate::currentDate());
    QString text;
    if (days <= 0)     text = tr("used today");
    else if (days == 1) text = tr("used yesterday");
    else if (days <= 7) text = tr("used %n day(s) ago", "", int(days));
    else                text = tr("used on %1")
                                   .arg(when.date().toString(QLocale().dateFormat(
                                       QLocale::ShortFormat)));
    /* UI8 - the separator only when there IS something to separate from. It
     * was unconditional, and the server sends no state and often no data
     * centre, so the row routinely opened with an orphaned middle dot. */
    const bool alone = !pill_ && !has_datacentre_;
    last_used_->setText(alone ? text : QStringLiteral("\u00b7  ") + text);
}

/* UI6 - 16 -> 26 of blur over 120 ms. The card does not MOVE: a translation
 * would have to come out of the layout's spacing, and a row that shifts under
 * the pointer makes a mis-click more likely, not less. Depth alone reads as
 * "this one is under the cursor". */
void MachineCard::animateElevation(int to)
{
    if (!shadow_) return;
    auto *a = new QPropertyAnimation(shadow_, "blurRadius", this);
    a->setDuration(120);
    a->setEndValue(to);
    a->setEasingCurve(QEasingCurve::OutCubic);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void MachineCard::enterEvent(QEnterEvent *e)
{
    QFrame::enterEvent(e);
    animateElevation(26);
}

void MachineCard::leaveEvent(QEvent *e)
{
    QFrame::leaveEvent(e);
    animateElevation(16);
}

void MachineCard::setBusy(bool busy)
{
    /* VMK1 - a machine under maintenance stays unavailable whatever the
     * session state: `setBusy(false)` at the end of a session would otherwise
     * quietly hand back a button that must not be pressed. */
    connect_->setEnabled(!busy && !maintenance_);
}

void MachineCard::activate()
{
    if (connect_->isEnabled()) emit connectRequested(id_);
}

void MachineCard::mouseDoubleClickEvent(QMouseEvent *e)
{
    Q_UNUSED(e);
    setFocus(Qt::MouseFocusReason);
    activate();
}

void MachineCard::keyPressEvent(QKeyEvent *e)
{
    /* Return, Enter and Space: the three the platform conventions disagree
     * about, so all three. Space is what a focused control responds to on
     * Windows, Enter is what a list row responds to everywhere, and the keypad
     * Enter is a different key code from the main one. */
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        activate();
        return;
    default:
        break;
    }
    QFrame::keyPressEvent(e);
}

void MachineCard::paintEvent(QPaintEvent *e)
{
    QFrame::paintEvent(e);
    if (!hasFocus()) return;

    /* The ring is drawn and not left to the style: the card's border comes
     * from the stylesheet (`QWidget[card="true"]`), and a stylesheet border
     * cannot be changed on `:focus` for a QFrame without restyling the whole
     * rule - which would also lose the rounded corner. Two pixels inside the
     * border so the two do not sit on the same pixels and alias. */
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(theme::accent(this), 2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 10, 10);
}

void MachineCard::focusInEvent(QFocusEvent *e)  { QFrame::focusInEvent(e);  update(); }
void MachineCard::focusOutEvent(QFocusEvent *e) { QFrame::focusOutEvent(e); update(); }
