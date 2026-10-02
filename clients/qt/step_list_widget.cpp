/* StepListWidget - see step_list_widget.hpp on why the labels are shared. */
#include "step_list_widget.hpp"

#include "theme.hpp"

#include <QEvent>
#include <QLabel>
#include <QVBoxLayout>
#include <QFont>

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

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 24, 24, 24);
    lay->setSpacing(8);

    headline_ = new QLabel(QString(), this);
    QFont hf = headline_->font();
    hf.setPointSize(hf.pointSize() + 2);
    headline_->setFont(hf);
    lay->addWidget(headline_);
    lay->addSpacing(12);

    for (const char *t : kTitles) {
        Row r;
        r.title = t;
        r.label = new QLabel(this);
        lay->addWidget(r.label);
        rows_.append(r);
    }
    lay->addStretch(1);
    reset();
}

void StepListWidget::reset()
{
    for (int i = 0; i < rows_.size(); i++) {
        rows_[i].state  = State::Pending;
        rows_[i].detail.clear();
        refresh(i);
    }
}

void StepListWidget::setHeadline(const QString &text)
{
    if (headline_) headline_->setText(text);
}

void StepListWidget::setState(int index, State s, const QString &detail)
{
    /* Index -1 is the pre-step (capabilities, turn-servers): it has no row, and
     * its detail belongs on the headline rather than nowhere. */
    if (index < 0) { setHeadline(detail); return; }
    if (index >= rows_.size()) return;
    rows_[index].state = s;
    if (!detail.isEmpty()) rows_[index].detail = detail;
    refresh(index);
}

void StepListWidget::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange)
        for (int i = 0; i < rows_.size(); i++) refresh(i);
    QWidget::changeEvent(e);
}

void StepListWidget::refresh(int index)
{
    const Row &r = rows_.at(index);
    /* Colours from the theme (QT4): the fixed hex values that were here were
     * chosen against a dark window and washed out on a light one. */
    QString mark;
    QColor colour;
    switch (r.state) {
    case State::Pending: mark = QStringLiteral("·"); colour = halyard::theme::muted(this); break;
    case State::Running: mark = QStringLiteral("…"); colour = halyard::theme::warn(this);  break;
    case State::Done:    mark = QStringLiteral("✓"); colour = halyard::theme::good(this);  break;
    case State::Failed:  mark = QStringLiteral("✗"); colour = halyard::theme::bad(this);   break;
    }
    QString text = QStringLiteral("%1  %2").arg(mark, tr(r.title));
    if (!r.detail.isEmpty())
        text += QStringLiteral("   —   %1").arg(r.detail);
    r.label->setText(text);
    r.label->setStyleSheet(halyard::theme::css(colour));
}
