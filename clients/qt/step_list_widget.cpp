/* StepListWidget - see step_list_widget.hpp on why the labels are shared and
 * why the state is drawn rather than written. */
#include "step_list_widget.hpp"

#include "theme.hpp"

#include <cmath>

#include <QEvent>
#include <QFontMetrics>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace theme = halyard::theme;

/* ===================================================================== dot */

StepDot::StepDot(QWidget *parent) : QWidget(parent)
{
    setFixedSize(18, 18);
}

void StepDot::setState(State s)
{
    if (state_ == s) return;
    state_ = s;
    update();
}

void StepDot::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(2.5, 2.5, -2.5, -2.5);

    switch (state_) {
    case State::Pending:
        /* An empty ring: there is something here, and it has not started. */
        p.setPen(QPen(theme::muted(this), 1.6));
        p.drawEllipse(r);
        break;

    case State::Running: {
        /* A three-quarter arc turning once a second. The faint full ring stays
         * behind it so the mark keeps the same footprint as the others and the
         * row does not appear to shrink when a step starts. */
        QColor faint = theme::muted(this);
        faint.setAlphaF(0.35f);
        p.setPen(QPen(faint, 1.6));
        p.drawEllipse(r);

        p.setPen(QPen(theme::accent(this), 2.2, Qt::SolidLine, Qt::RoundCap));
        /* Qt counts angles in 1/16 degree, anticlockwise from 3 o'clock; the
         * minus turns it the way a spinner is expected to turn. */
        p.drawArc(r, int(-phase_ * 360.0 * 16.0), -270 * 16);
        break;
    }

    case State::Done: {
        /* Filled, with the tick drawn in the background colour: a thin tick on
         * a thin ring was unreadable at 18px, and it is the one state people
         * scan the column for. */
        const QColor c = theme::good(this);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(r);

        QPainterPath tick;
        tick.moveTo(r.left() + r.width() * 0.26, r.top() + r.height() * 0.52);
        tick.lineTo(r.left() + r.width() * 0.44, r.top() + r.height() * 0.71);
        tick.lineTo(r.left() + r.width() * 0.76, r.top() + r.height() * 0.30);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(palette().color(QPalette::Window), 2.0, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(tick);
        break;
    }

    case State::Failed: {
        const QColor c = theme::bad(this);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(r);
        p.setPen(QPen(palette().color(QPalette::Window), 2.0, Qt::SolidLine,
                      Qt::RoundCap));
        const qreal m = r.width() * 0.28;
        p.drawLine(r.topLeft() + QPointF(m, m), r.bottomRight() - QPointF(m, m));
        p.drawLine(r.topRight() + QPointF(-m, m), r.bottomLeft() + QPointF(m, -m));
        break;
    }
    }
}

/* ==================================================================== list */

StepListWidget::StepListWidget(QWidget *parent) : QWidget(parent)
{
    /* The same seven as the Borealis client's `connect/step1..7`, word for
     * word, so "it stops at step 2" means the same step in both clients. Not
     * read from Borealis's catalogue (its `ui::tr` is not linked here);
     * QT_TR_NOOP makes them lupdate keys for this client's own catalogue
     * (QT5), and the French entries reuse Borealis's French wording. */
    static const char *kTitles[] = {
        QT_TR_NOOP("Starting the machine"),
        QT_TR_NOOP("Machine address"),
        QT_TR_NOOP("Opening the session"),
        QT_TR_NOOP("Streaming permissions"),
        QT_TR_NOOP("Streaming sessions"),
        QT_TR_NOOP("Waiting for the video server"),
        QT_TR_NOOP("Connecting to the stream"),
    };

    /* UI1 - centred in a card, like the pairing page: the list was pinned to
     * the top-left of a wide window, so on a maximised window the one thing
     * happening was in the corner. */
    auto *outer = new QVBoxLayout(this);
    outer->setAlignment(Qt::AlignCenter);

    auto *card = new QFrame(this);
    card->setProperty("card", true);
    card->setGraphicsEffect(theme::elevation(card, 24));
    card->setMinimumWidth(460);
    card->setMaximumWidth(620);

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(32, 26, 32, 26);
    lay->setSpacing(theme::SpaceTight);

    headline_ = new QLabel(QString(), card);
    headline_->setProperty("h2", true);
    headline_->setWordWrap(true);
    lay->addWidget(headline_);

    subhead_ = new QLabel(QString(), card);
    subhead_->setProperty("dim", true);
    subhead_->setWordWrap(true);
    lay->addWidget(subhead_);
    lay->addSpacing(theme::SpaceGroup);

    for (const char *t : kTitles) {
        Row r;
        r.title = t;

        auto *row = new QWidget(card);
        auto *rl  = new QHBoxLayout(row);
        rl->setContentsMargins(0, 3, 0, 3);
        rl->setSpacing(theme::SpaceRow);

        r.dot = new StepDot(row);
        r.label = new QLabel(row);
        r.detailLabel = new QLabel(row);
        r.detailLabel->setProperty("dim", true);
        r.detailLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        /* The detail elides rather than widening the card: an address or an
         * error body is of unbounded length and must not move the titles. */
        r.detailLabel->setMinimumWidth(0);
        r.detailLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

        rl->addWidget(r.dot);
        rl->addWidget(r.label);
        rl->addStretch(1);
        rl->addWidget(r.detailLabel, 1);

        lay->addWidget(row);
        rows_.append(r);
    }

    /* UI3 - the way out of a failed connection. Hidden until something fails,
     * so the screen is not offering to retry what has not yet gone wrong. */
    footer_ = new QWidget(card);
    auto *fl = new QHBoxLayout(footer_);
    fl->setContentsMargins(0, theme::SpaceGroup, 0, 0);
    fl->setSpacing(theme::SpaceRow);
    back_  = new QPushButton(tr("Back to the machines"), footer_);
    retry_ = new QPushButton(tr("Try again"), footer_);
    retry_->setProperty("accent", true);
    fl->addStretch(1);
    fl->addWidget(back_);
    fl->addWidget(retry_);
    connect(back_,  &QPushButton::clicked, this, &StepListWidget::backRequested);
    connect(retry_, &QPushButton::clicked, this, &StepListWidget::retryRequested);
    footer_->setVisible(false);
    lay->addWidget(footer_);

    outer->addWidget(card);

    /* One timer for the whole column, stopped whenever nothing is running: a
     * 33 ms repaint that nobody can see is still a repaint, and this screen is
     * up while the machine boots - which can be half a minute. */
    anim_ = new QTimer(this);
    anim_->setInterval(33);
    connect(anim_, &QTimer::timeout, this, [this] {
        phase_ = std::fmod(phase_ + 0.033, 1.0);
        for (const Row &r : rows_)
            if (r.state == State::Running) r.dot->setPhase(phase_);
    });

    reset();
}

void StepListWidget::reset()
{
    for (int i = 0; i < rows_.size(); i++) {
        rows_[i].state  = State::Pending;
        rows_[i].detail.clear();
        refresh(i);
    }
    if (subhead_) subhead_->clear();
    setFailed(false);
    retimeAnimation();
}

void StepListWidget::setFailed(bool on)
{
    if (footer_) footer_->setVisible(on);
}

void StepListWidget::setHeadline(const QString &text)
{
    if (headline_) headline_->setText(text);
}

void StepListWidget::setState(int index, State s, const QString &detail)
{
    /* Index -1 is the pre-step (capabilities, turn-servers): it has no row, and
     * its detail belongs under the headline rather than nowhere. */
    if (index < 0) { if (subhead_) subhead_->setText(detail); return; }
    if (index >= rows_.size()) return;
    rows_[index].state = s;
    if (!detail.isEmpty()) rows_[index].detail = detail;
    refresh(index);
    if (s == State::Failed) setFailed(true);
    retimeAnimation();
}

void StepListWidget::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange)
        for (int i = 0; i < rows_.size(); i++) refresh(i);
    QWidget::changeEvent(e);
}

void StepListWidget::retimeAnimation()
{
    bool running = false;
    for (const Row &r : rows_)
        if (r.state == State::Running) { running = true; break; }
    if (running && !anim_->isActive())      anim_->start();
    else if (!running && anim_->isActive()) anim_->stop();
}

void StepListWidget::refresh(int index)
{
    const Row &r = rows_.at(index);

    r.dot->setState(static_cast<StepDot::State>(r.state));
    r.label->setText(tr(r.title));

    /* Only the step in play and the step that failed are coloured. Painting
     * every done step green turned the column into a block of colour with
     * nothing standing out, which is the opposite of what it is for. */
    QColor ink;
    switch (r.state) {
    case State::Pending: ink = theme::muted(this); break;
    case State::Running: ink = theme::accent(this); break;
    case State::Done:    ink = palette().color(QPalette::WindowText); break;
    case State::Failed:  ink = theme::bad(this); break;
    }
    r.label->setStyleSheet(theme::css(ink) +
                           (r.state == State::Running ? QStringLiteral("font-weight:bold;")
                                                      : QString()));

    /* Elided here and not by the layout: a QLabel with word wrap off still
     * reports its full text width as its size hint, and the stretch then
     * cannot shrink it below that. */
    const QString d = r.detail;
    if (d.isEmpty()) {
        r.detailLabel->clear();
    } else {
        const QFontMetrics fm(r.detailLabel->font());
        const int avail = qMax(60, r.detailLabel->width());
        r.detailLabel->setText(fm.elidedText(d, Qt::ElideMiddle, avail));
        r.detailLabel->setToolTip(d);
    }
    if (r.state == State::Failed) r.detailLabel->setStyleSheet(theme::css(theme::bad(this)));
    else                          r.detailLabel->setStyleSheet(QString());
}
