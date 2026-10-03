/* MachineCard - see the header for why a card. */
#include "machine_card.hpp"

#include "machine_state.hpp"
#include "theme.hpp"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace theme = halyard::theme;

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
