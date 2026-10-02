/* MetricsWindow - see metrics_window.hpp on grants versus counters. */
#include "metrics_window.hpp"

#include <QEvent>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

extern "C" {
#include "core/protocol/ctrl_session.h"
#include "core/protocol/session_caps.h"
}

MetricsWindow::MetricsWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    resize(640, 420);

    summary_ = new QLabel(this);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_->setWordWrap(true);

    channels_ = new QTableWidget(0, 4, this);
    channels_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    channels_->verticalHeader()->setVisible(false);
    channels_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    channels_->setSelectionMode(QAbstractItemView::NoSelection);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(summary_);
    lay->addWidget(channels_, 1);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &MetricsWindow::refresh);
    retranslate();
}

/* QT5 - the static texts, re-set on a language switch. The dynamic ones are
 * re-produced by refresh(), which runs from here too. */
void MetricsWindow::retranslate()
{
    setWindowTitle(tr("Halyard metrics"));
    channels_->setHorizontalHeaderLabels(
        { tr("Channel"), tr("Granted"), tr("Transport"), tr("Port") });
    refresh();
}

void MetricsWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange) retranslate();
    QWidget::changeEvent(e);
}

void MetricsWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refresh();
    timer_->start();
}

void MetricsWindow::hideEvent(QHideEvent *e)
{
    timer_->stop();
    QWidget::hideEvent(e);
}

void MetricsWindow::refresh()
{
    shadow_session_caps c;
    const bool have = ctrl_session_caps(&c);

    if (!have) {
        summary_->setText(tr("No session has completed its bootstrap yet.\n\n"
                             "The grant snapshot appears once the server has "
                             "answered the eight channel announcements."));
        channels_->setRowCount(0);
        return;
    }

    /* The server's own build, from the Capabilities reply. Every byte-exact
     * decision in this client is dated against ONE build, so a client talking
     * to a different one should be able to see the number rather than guess. */
    summary_->setText(
        tr("Session %1   ·   server %2.%3.%4   ·   port base %5\n"
           "%6 of 8 channels granted\n\n"
           "Video, AS GRANTED: %7×%8 @ %9 fps, codec %10, %11 Mb/s\n"
           "Audio, AS GRANTED: %12 Hz, %13 bits, codec %14\n\n"
           "\"As granted\" and not \"as asked\": the server restates these and "
           "may not honour what was requested (KB §3.37).")
            .arg(c.generation)
            .arg(c.srv_major).arg(c.srv_minor).arg(c.srv_patch)
            .arg(c.port_base)
            .arg(c.n_granted)
            .arg(c.video_width).arg(c.video_height)
            .arg(QString::number(c.video_fps, 'f', 2))
            .arg(c.video_codec == 0 ? QStringLiteral("H.264")
                 : c.video_codec == 1 ? QStringLiteral("H.265")
                 : c.video_codec == 2 ? QStringLiteral("AV1")
                                      : QString::number(c.video_codec))
            .arg(c.video_bitrate_bps / 1000000)
            .arg(c.audio_sample_rate).arg(c.audio_bits)
            .arg(c.audio_codec == 1 ? QStringLiteral("Opus")
                 : c.audio_codec == 2 ? QStringLiteral("FLAC")
                                      : QString::number(c.audio_codec)));

    /* The body order, which is what `SHADOW_CHAN_IDX_*` indexes - NOT the
     * server's channel numbers. Mixing the two is the mistake that caused a
     * wrong-channel bug (SRV5), so the names are spelled out here in the one
     * order this array uses. QT_TR_NOOP so lupdate can extract them: a
     * `tr()` of a variable is invisible to it, and the names stayed English
     * in every language until this was spotted. */
    static const char *kNames[8] = {
        QT_TR_NOOP("video"), QT_TR_NOOP("cursor"), QT_TR_NOOP("input"),
        QT_TR_NOOP("audio"), QT_TR_NOOP("controller"), QT_TR_NOOP("clipboard"),
        QT_TR_NOOP("microphone"), QT_TR_NOOP("file transfer"),
    };

    channels_->setRowCount(8);
    for (int i = 0; i < 8; i++) {
        const shadow_chan_caps &ch = c.chan[i];
        channels_->setItem(i, 0, new QTableWidgetItem(tr(kNames[i])));
        channels_->setItem(i, 1, new QTableWidgetItem(
            ch.granted ? tr("yes") : tr("no")));
        channels_->setItem(i, 2, new QTableWidgetItem(
            ch.granted ? (ch.tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"))
                       : QStringLiteral("—")));
        channels_->setItem(i, 3, new QTableWidgetItem(
            ch.granted ? QString::number(ch.port) : QStringLiteral("—")));
    }
}
