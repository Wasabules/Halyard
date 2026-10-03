/* VideoWidget - see video_widget.hpp for what this gives up and why, and for
 * how input is forwarded (IN1). */
#include "video_widget.hpp"

#include "qt_input_map.hpp"

#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QVideoSink>
#include <QVideoWidget>
#include <QWheelEvent>

extern "C" {
#include "core/input/shadow_input.h"
}

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

    /* IN1 - take the keyboard, and watch the child for the mouse. The video
     * widget is a real child window that grabs mouse events first, so filtering
     * it is the only way to see them without subclassing a widget we create. */
    setFocusPolicy(Qt::StrongFocus);
    video_->setFocusPolicy(Qt::NoFocus);     /* keys stay with us */
    video_->setMouseTracking(true);
    video_->installEventFilter(this);
}

void VideoWidget::presentFrame(const QVideoFrame &frame)
{
    if (!sink_ || !frame.isValid()) return;
    sink_->setVideoFrame(frame);
    presented_++;

    /* IN1 - the coordinate space core clamps to is the DECODED size, and the
     * server may not have honoured what we asked for, so it is learned here
     * from the frame and pushed to core. Cheap, and it follows a mid-session
     * resolution change (PM2 in the Borealis client). */
    const int w = frame.width(), h = frame.height();
    if (w > 0 && h > 0 && (w != frameW_ || h != frameH_)) {
        frameW_ = w; frameH_ = h;
        shadow_input_set_bounds(w, h);
    }
}

void VideoWidget::setStatus(const QString &text)
{
    if (status_)
        status_->setText(QStringLiteral("%1   |   %2 frame(s) presented")
                             .arg(text).arg(presented_));
}

/* ---------------------------------------------------------------- mapping */

bool VideoWidget::mapToFrame(const QPointF &p, int &fx, int &fy) const
{
    if (frameW_ <= 0 || frameH_ <= 0) return false;
    const QSize vs = video_->size();
    if (vs.width() <= 0 || vs.height() <= 0) return false;

    /* QVideoWidget keeps the aspect ratio and letterboxes. Recompute the drawn
     * rectangle the same way: fit the frame into the widget, centre it. */
    const double fa = (double)frameW_ / frameH_;
    const double wa = (double)vs.width() / vs.height();
    double dw, dh;
    if (wa > fa) { dh = vs.height();        dw = dh * fa; }   /* bars left/right */
    else         { dw = vs.width();         dh = dw / fa; }   /* bars top/bottom */
    const double ox = (vs.width()  - dw) / 2.0;
    const double oy = (vs.height() - dh) / 2.0;

    const double rx = p.x() - ox, ry = p.y() - oy;
    if (rx < 0 || ry < 0 || rx >= dw || ry >= dh) return false;   /* on a bar */

    fx = (int)(rx / dw * frameW_);
    fy = (int)(ry / dh * frameH_);
    if (fx < 0) fx = 0; else if (fx >= frameW_) fx = frameW_ - 1;
    if (fy < 0) fy = 0; else if (fy >= frameH_) fy = frameH_ - 1;
    return true;
}

/* ------------------------------------------------------------------ mouse */

bool VideoWidget::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj != video_ || !shadow_input_session_active())
        return QWidget::eventFilter(obj, ev);

    switch (ev->type()) {
    case QEvent::MouseMove: {
        auto *m = static_cast<QMouseEvent *>(ev);
        int fx, fy;
        if (mapToFrame(m->position(), fx, fy))
            shadow_input_post_mouse_move_abs(fx, fy);
        return false;   /* let the widget keep painting its own cursor logic */
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        auto *m = static_cast<QMouseEvent *>(ev);
        const bool pressed = ev->type() == QEvent::MouseButtonPress;
        /* Place the cursor first, then click: a click carries the position in
         * core's templates, and a press without a preceding move (a tap on a
         * freshly focused window) would otherwise land at the last known point. */
        int fx, fy;
        if (mapToFrame(m->position(), fx, fy))
            shadow_input_post_mouse_move_abs(fx, fy);
        int btn = -1;
        switch (m->button()) {
        case Qt::LeftButton:   btn = 0; break;
        case Qt::RightButton:  btn = 1; break;
        case Qt::MiddleButton: btn = 2; break;
        default: break;
        }
        if (btn >= 0) shadow_input_post_mouse_button(btn, pressed);
        if (pressed) setFocus(Qt::MouseFocusReason);   /* so keys come to us */
        return false;
    }
    case QEvent::Wheel: {
        auto *w = static_cast<QWheelEvent *>(ev);
        const int dy = w->angleDelta().y();
        if (dy != 0) {
            /* One notch per 120 units (Qt's standard step); send the sign per
             * notch so a fast flick is several notches, not one big jump. */
            int notches = dy / 120;
            if (notches == 0) notches = dy > 0 ? 1 : -1;
            const int dir = notches > 0 ? 1 : -1;
            for (int i = 0, n = notches > 0 ? notches : -notches; i < n; i++)
                shadow_input_post_mouse_wheel(dir);
        }
        return false;
    }
    default:
        return QWidget::eventFilter(obj, ev);
    }
}

/* --------------------------------------------------------------- keyboard */

void VideoWidget::postKey(QKeyEvent *e, bool pressed)
{
    if (!shadow_input_session_active()) return;
    if (e->isAutoRepeat()) return;   /* the VM runs its own repeat */
    const int ev = halyard::evdevForKeyEvent(e->key(), e->nativeScanCode());
    if (ev) shadow_input_post_scancode((uint16_t)ev, pressed);
}

void VideoWidget::keyPressEvent(QKeyEvent *e)   { postKey(e, true); }
void VideoWidget::keyReleaseEvent(QKeyEvent *e) { postKey(e, false); }
