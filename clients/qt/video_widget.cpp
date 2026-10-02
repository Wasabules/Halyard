/* VideoWidget - see video_widget.hpp for what this gives up and why. */
#include "video_widget.hpp"

#include <QVBoxLayout>
#include <QVideoWidget>
#include <QVideoSink>
#include <QLabel>

VideoWidget::VideoWidget(QWidget *parent) : QWidget(parent)
{
    video_ = new QVideoWidget(this);
    sink_  = video_->videoSink();

    status_ = new QLabel(tr("idle"), this);
    status_->setStyleSheet(QStringLiteral(
        "color: #e8e8e8; background: rgba(0,0,0,140); padding: 6px;"));
    status_->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(status_, 0);
    lay->addWidget(video_, 1);

    setMinimumSize(640, 360);
}

void VideoWidget::presentFrame(const QVideoFrame &frame)
{
    if (!sink_ || !frame.isValid()) return;
    sink_->setVideoFrame(frame);
    presented_++;
}

void VideoWidget::setStatus(const QString &text)
{
    if (status_)
        status_->setText(QStringLiteral("%1   |   %2 frame(s) presented")
                             .arg(text).arg(presented_));
}
