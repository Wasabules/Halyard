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
#include <QProgressBar>
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

    /* UI7 - the two numbers that answer "should I wait". Determinate, because
     * the total is known: seven, always. A busy indicator here would say only
     * that something is happening, which the spinning dot already says. */
    {
        auto *pr = new QHBoxLayout;
        pr->setSpacing(theme::SpaceRow);
        progress_ = new QProgressBar(card);
        progress_->setRange(0, 7);
        progress_->setTextVisible(false);
        progress_->setFixedHeight(6);
        elapsed_ = new QLabel(card);
        elapsed_->setProperty("dim", true);
        pr->addWidget(progress_, 1);
        pr->addWidget(elapsed_);
        lay->addLayout(pr);
    }

    /* UI7 - one line standing in for the steps that are done. Flat, so it
     * reads as a summary and not as another button to press. */
    collapsed_ = new QPushButton(card);
    collapsed_->setFlat(true);
    collapsed_->setCursor(Qt::PointingHandCursor);
    collapsed_->setVisible(false);
    connect(collapsed_, &QPushButton::clicked, this, [this] {
        expanded_ = true;
        refreshCollapse();
    });
    lay->addWidget(collapsed_);

    lay->addSpacing(theme::SpaceGroup);

    for (const char *t : kTitles) {
        Row r;
        r.title = t;

        auto *row = new QWidget(card);
        /* UI7 - named, so the highlight below can SELECT it. */
        row->setObjectName(QStringLiteral("stepRow"));
        auto *rl  = new QHBoxLayout(row);
        /* UI7 - 8 px of left margin on EVERY row, lit or not, so the live
         * row's 2 px bar has somewhere to sit and nothing moves sideways when
         * a step starts. A row that shifts under the eye looks broken. */
        rl->setContentsMargins(8, 3, 0, 3);
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
    cancel_ = new QPushButton(tr("Cancel"), card);
    retry_ = new QPushButton(tr("Try again"), footer_);
    retry_->setProperty("accent", true);
    fl->addStretch(1);
    fl->addWidget(back_);
    fl->addWidget(retry_);
    connect(back_,  &QPushButton::clicked, this, &StepListWidget::backRequested);
    connect(retry_, &QPushButton::clicked, this, &StepListWidget::retryRequested);
    footer_->setVisible(false);

    /* UI7 - the failure's detail, in full and selectable.
     *
     * The detail used to be elided into the step's own row, which is right
     * while things are going well (a long address must not move the titles)
     * and wrong the moment one fails: an HTTP body cut at 40 characters with
     * an ellipsis in the middle is the one thing someone needs to read. */
    error_ = new QLabel(card);
    error_->setWordWrap(true);
    error_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    error_->setFont(theme::monoFont(card));
    error_->setVisible(false);
    lay->addWidget(error_);

    lay->addWidget(footer_);

    /* UI7 - Cancel while it runs. Its own row above the footer, because the
     * footer is the failed state and these two are never up together. */
    {
        auto *cl = new QHBoxLayout;
        cl->setContentsMargins(0, theme::SpaceRow, 0, 0);
        cl->addStretch(1);
        cl->addWidget(cancel_);
        lay->addLayout(cl);
        connect(cancel_, &QPushButton::clicked, this,
                &StepListWidget::cancelRequested);
    }

    /* The elapsed seconds, ticking once a second - not at the animation's 33
     * ms, which would repaint a label 30 times to change it once. */
    clock_ = new QTimer(this);
    clock_->setInterval(1000);
    connect(clock_, &QTimer::timeout, this, &StepListWidget::refreshProgress);

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
    expanded_ = false;
    since_.start();
    if (clock_) clock_->start();
    if (cancel_) cancel_->setVisible(true);
    refreshProgress();
    refreshCollapse();
    retimeAnimation();
}

void StepListWidget::setFailed(bool on)
{
    if (footer_) footer_->setVisible(on);
    if (cancel_) cancel_->setVisible(!on);
    if (on && clock_) clock_->stop();

    if (!error_) return;
    if (!on) { error_->setVisible(false); error_->clear(); return; }

    /* UI7 - the detail of whichever step failed, verbatim. */
    for (const Row &r : rows_)
        if (r.state == State::Failed && !r.detail.isEmpty()) {
            error_->setText(r.detail);
            error_->setStyleSheet(theme::css(theme::bad(this)));
            error_->setVisible(true);
            /* UI7 - a failure is also a reason to show the steps again: the
             * collapsed summary hides which ones had passed. */
            expanded_ = true;
            refreshCollapse();
            return;
        }
    error_->setVisible(false);
}

/* UI7 - "4 of 7" and the seconds. */
void StepListWidget::refreshProgress()
{
    int done = 0;
    for (const Row &r : rows_) if (r.state == State::Done) done++;
    if (progress_) progress_->setValue(done);
    if (!elapsed_) return;

    const qint64 sec = since_.isValid() ? since_.elapsed() / 1000 : 0;
    elapsed_->setText(tr("%1/7 \u00b7 %2 s").arg(done).arg(sec));
}

/* UI7 - fold the finished steps away once there are enough of them to be in
 * the way. Two is the threshold: one tick is not clutter, and collapsing from
 * the first would make the list jump on every step. */
void StepListWidget::refreshCollapse()
{
    int done = 0;
    for (const Row &r : rows_) if (r.state == State::Done) done++;

    const bool fold = !expanded_ && done >= 2;
    for (const Row &r : rows_) {
        QWidget *row = r.label ? r.label->parentWidget() : nullptr;
        if (row) row->setVisible(!(fold && r.state == State::Done));
    }
    if (collapsed_) {
        collapsed_->setVisible(fold);
        collapsed_->setText(tr("%n step(s) completed", "", done)
                            + QStringLiteral("  \u25be"));
        collapsed_->setStyleSheet(theme::css(theme::muted(this)));
    }
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
    refreshProgress();
    refreshCollapse();
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

    /* === UI7 2026-10-03 - THE HIGHLIGHT NEEDS A SELECTOR ==================
     *
     * The first version set `border-left: 2px solid ...` on the row widget
     * with no selector. A selector-less stylesheet applies to the widget AND
     * EVERY DESCENDANT, so the row, the dot, the title and the detail each
     * drew their own left border - three stripes per step, reported as
     * "3 bordures gauche a chaque texte". It is the same Qt trap as a
     * selector-less `background`, and it is invisible until a rule happens to
     * be one that looks wrong when repeated.
     *
     * `QWidget#stepRow` matches the row only. The tint spans the dot and the
     * detail because it is on their PARENT, which is what was wanted - a
     * stripe behind one of three widgets reads as a highlight gone wrong. The
     * accent at 22/255 is enough to find and not enough to compete with the
     * text on it. */
    if (QWidget *row = r.label->parentWidget()) {
        if (r.state == State::Running) {
            const QColor a = theme::accent(this);
            row->setStyleSheet(
                QStringLiteral("QWidget#stepRow {"
                               "  background: rgba(%1,%2,%3,22);"
                               "  border-left: 2px solid %4;"
                               "  border-top-right-radius: 4px;"
                               "  border-bottom-right-radius: 4px;"
                               "}")
                    .arg(a.red()).arg(a.green()).arg(a.blue()).arg(a.name()));
        } else {
            row->setStyleSheet(QString());
        }
    }

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

/* ============================================================ SignInSteps */

SignInSteps::SignInSteps(QWidget *parent) : QWidget(parent)
{
    /* The three phases that can take time. Worded as what is being waited
     * for, not as what the code is doing: "contacting the data centre" is
     * something a person can act on (their network), "tinag_get_datacenter"
     * is not. */
    titles_ = { QT_TR_NOOP("Data centre"), QT_TR_NOOP("Endpoints"),
                QT_TR_NOOP("Code") };

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(theme::SpaceGroup);
    row->addStretch(1);
    for (const char *t : titles_) {
        Cell c;
        c.dot   = new StepDot(this);
        c.label = new QLabel(tr(t), this);
        auto *pair = new QHBoxLayout;
        pair->setContentsMargins(0, 0, 0, 0);
        pair->setSpacing(theme::SpaceTight);
        pair->addWidget(c.dot);
        pair->addWidget(c.label);
        row->addLayout(pair);
        cells_.append(c);
    }
    row->addStretch(1);

    /* Same rule as the column: the timer runs only while a dot is spinning. */
    anim_ = new QTimer(this);
    anim_->setInterval(33);
    connect(anim_, &QTimer::timeout, this, [this] {
        phase_ = std::fmod(phase_ + 0.033, 1.0);
        for (const Cell &c : cells_) c.dot->setPhase(phase_);
    });
    refresh();
}

void SignInSteps::setStage(int index)
{
    stage_ = index;
    failed_ = false;
    done_ = false;
    refresh();
}

void SignInSteps::setFailed() { failed_ = true; refresh(); }
void SignInSteps::setComplete() { done_ = true; refresh(); }
void SignInSteps::reset() { stage_ = 0; failed_ = false; done_ = false; refresh(); }

void SignInSteps::refresh()
{
    for (int i = 0; i < cells_.size(); i++) {
        StepDot::State st;
        if (done_)             st = StepDot::State::Done;
        else if (i <  stage_)  st = StepDot::State::Done;
        else if (i == stage_)  st = failed_ ? StepDot::State::Failed
                                            : StepDot::State::Running;
        else                   st = StepDot::State::Pending;
        cells_[i].dot->setState(st);

        /* Only the live one is emphasised. The labels are short and three of
         * them in accent would read as a title rather than as progress. */
        const bool live = (i == stage_ && !done_ && !failed_);
        cells_[i].label->setStyleSheet(
            live ? theme::css(theme::accent(this)) + QStringLiteral("font-weight:bold;")
                 : theme::css(theme::muted(this)));
        cells_[i].label->setText(tr(titles_.at(i)));
    }

    const bool spinning = !done_ && !failed_ && stage_ < cells_.size();
    if (spinning && !anim_->isActive())      anim_->start();
    else if (!spinning && anim_->isActive()) anim_->stop();
}
