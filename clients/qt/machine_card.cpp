/* MachineCard - see the header for why a card. */
#include "machine_card.hpp"

#include "theme.hpp"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace theme = halyard::theme;

namespace {

/* The server's own words for a VM's state, mapped onto the three the card
 * shows. Anything unrecognised reads as "off" rather than being invented into
 * a green pill - the colour is a claim, and a wrong one here sends someone
 * clicking Connect on a machine that is not there. */
const char *pillClassFor(const QString &state)
{
    const QString s = state.toLower();
    if (s.contains(QStringLiteral("start")) || s.contains(QStringLiteral("boot")) ||
        s.contains(QStringLiteral("pending")))
        return "busy";
    if (s.contains(QStringLiteral("run")) || s.contains(QStringLiteral("ready")) ||
        s.contains(QStringLiteral("active")) || s.contains(QStringLiteral("started")))
        return "ok";
    return "off";
}

}  // namespace

MachineCard::MachineCard(const QString &id, const QString &name,
                         const QString &state, const QString &datacentre,
                         QWidget *parent)
    : QFrame(parent), id_(id)
{
    setProperty("card", true);
    setProperty("cardHover", true);
    setGraphicsEffect(theme::elevation(this, 16));

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(theme::SpaceGroup, theme::SpaceRow,
                            theme::SpaceGroup, theme::SpaceRow);
    row->setSpacing(theme::SpaceGroup);

    auto *texts = new QVBoxLayout;
    texts->setSpacing(theme::SpaceTight);

    auto *title = new QLabel(name.isEmpty() ? id : name, this);
    title->setProperty("h2", true);
    texts->addWidget(title);

    auto *sub = new QHBoxLayout;
    sub->setSpacing(theme::SpaceRow);
    pill_ = new QLabel(state.isEmpty() ? tr("unknown") : state, this);
    pill_->setProperty("pill", QString::fromUtf8(pillClassFor(state)));
    sub->addWidget(pill_);
    if (!datacentre.isEmpty()) {
        auto *dc = new QLabel(datacentre, this);
        dc->setProperty("dim", true);
        sub->addWidget(dc);
    }
    sub->addStretch(1);
    texts->addLayout(sub);

    connect_ = new QPushButton(tr("Connect"), this);
    connect_->setProperty("accent", true);
    QObject::connect(connect_, &QPushButton::clicked, this,
                     [this] { emit connectRequested(id_); });

    row->addLayout(texts, 1);
    row->addWidget(connect_, 0, Qt::AlignVCenter);
}

void MachineCard::setBusy(bool busy)
{
    connect_->setEnabled(!busy);
}

void MachineCard::mouseDoubleClickEvent(QMouseEvent *e)
{
    Q_UNUSED(e);
    if (connect_->isEnabled()) emit connectRequested(id_);
}
