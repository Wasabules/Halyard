/* VideoWidget - the decoded picture on screen, converted on the GPU.
 *
 * === WHY QVideoSink AND NOT A SHADER OF OUR OWN (YET) ======================
 *
 * The Borealis client carries its own NV12->RGB fragment shader, with the
 * colour matrix and range as uniforms (COL1, and e72d02d2's half-landed
 * `u_range`/`u_kr`/`u_kb`). That shader is correct and measured, and it can be
 * reused here through a `QOpenGLWidget` the day it is needed.
 *
 * It is not what this starts with, because the first question to answer is
 * whether the PATH holds — a 187 MB/s copy out of the decode thread, across a
 * queued signal, onto the GPU — and `QVideoWidget` answers it in thirty lines
 * instead of three hundred. Qt accepts `Format_NV12` and `Format_YUV420P` and
 * does the conversion itself.
 *
 * WHAT WE GIVE UP UNTIL THEN, stated so it is a choice and not an oversight:
 * Qt picks the colour matrix from the frame format, so a stream whose range or
 * primaries the VM reports oddly cannot be corrected the way SHADOW_COLOR_MATRIX
 * corrects it today. If that turns out to matter, the fix is the existing
 * shader in a QOpenGLWidget, not a patch here.
 */
#pragma once

#include <QWidget>
#include <QVideoFrame>

class QVideoWidget;
class QVideoSink;
class QLabel;

class VideoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget *parent = nullptr);

public slots:
    /* The frame arrives already copied and owned by us - see
     * SessionWorker::onFrame. Connected with Qt::QueuedConnection. */
    void presentFrame(const QVideoFrame &frame);

    /* A line of text over the video, for the bootstrap steps. */
    void setStatus(const QString &text);

private:
    QVideoWidget *video_ = nullptr;
    QVideoSink   *sink_  = nullptr;
    QLabel       *status_ = nullptr;

    /* Counted, because "the window is black" and "no picture ever arrived" are
     * different problems and the first thing to ask is which one it is. */
    quint64 presented_ = 0;
};
