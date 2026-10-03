/* VideoWidget - see video_widget.hpp for what this gives up and why, and for
 * how input is forwarded (IN1). */
#include "video_widget.hpp"

#include "qt_input_map.hpp"

#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVideoSink>
#include <QVideoWidget>
#include <QElapsedTimer>
#include <QPainter>
#include <QStackedLayout>
#include <QTimer>

#include <cmath>

#include "theme.hpp"
#include <QWheelEvent>
#include <QCursor>
#include <QPixmap>
#include <QTimer>
#include <cstring>

extern "C" {
#include "core/input/shadow_input.h"
#include "core/protocol/cursor_state.h"
}

/* === VID1 — THE WAITING PANEL ============================================
 *
 * Painted rather than assembled from labels, for two reasons. The spinner has
 * to be drawn anyway, and everything here sits on a near-black field that is
 * NOT the palette's window colour - a QLabel on it would need its own colour
 * override, which is three stylesheets for what is four drawText calls.
 *
 * It paints its own background, so there is never a frame where the area is
 * undefined; that blank frame is what the report called a black screen. */
class VideoWidget::Placeholder : public QWidget
{
public:
    explicit Placeholder(QWidget *parent) : QWidget(parent)
    {
        setAutoFillBackground(true);
        spin_ = new QTimer(this);
        spin_->setInterval(33);
        connect(spin_, &QTimer::timeout, this, [this] {
            phase_ = std::fmod(phase_ + 0.025, 1.0);
            update();
        });
    }

    void setBusy(const QString &title, const QString &detail)
    {
        title_ = title;
        detail_ = detail;
        busy_ = true;
        since_.start();
        if (!spin_->isActive() && halyard::theme::animationsEnabled()) spin_->start();
        update();
    }

    void setIdle(const QString &title, const QString &detail)
    {
        title_ = title;
        detail_ = detail;
        busy_ = false;
        spin_->stop();
        update();
    }

    /* The line the session's own progress writes, under the title. */
    void setDetail(const QString &detail) { detail_ = detail; update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(0x10, 0x10, 0x14));

        const QPointF c(width() / 2.0, height() / 2.0 - 26);

        if (busy_) {
            /* A ring and a three-quarter arc, the same gesture as the
             * connecting screen's dots, at 22 px radius. */
            const QRectF r(c.x() - 22, c.y() - 22, 44, 44);
            QColor faint = halyard::theme::accent(this);
            faint.setAlphaF(0.25f);
            p.setPen(QPen(faint, 3));
            p.drawArc(r, 0, 360 * 16);
            p.setPen(QPen(halyard::theme::accent(this), 3, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(r, int(-phase_ * 360.0 * 16.0), -270 * 16);
        } else {
            /* Not busy: a plain dot, so the layout does not jump between the
             * two states and nothing suggests work is still going on. */
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x55, 0x58, 0x62));
            p.drawEllipse(c, 7, 7);
        }

        QFont f = font();
        f.setPixelSize(17);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(0xec, 0xec, 0xf0));
        p.drawText(QRect(0, int(c.y()) + 40, width(), 26),
                   Qt::AlignHCenter | Qt::AlignTop, title_);

        f.setPixelSize(12);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(0x9a, 0x9d, 0xa8));
        QString sub = detail_;
        /* The seconds, because "is it stuck" is the only question being asked
         * at this point and it is the one thing the panel can answer. */
        if (busy_ && since_.isValid()) {
            const qint64 sec = since_.elapsed() / 1000;
            if (sec >= 2)
                sub = sub.isEmpty() ? tr("%1 s").arg(sec)
                                    : QStringLiteral("%1  \u00b7  %2 s").arg(sub).arg(sec);
        }
        p.drawText(QRect(24, int(c.y()) + 70, width() - 48, 40),
                   Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, sub);
    }

private:
    QTimer *spin_ = nullptr;
    QElapsedTimer since_;
    QString title_, detail_;
    qreal phase_ = 0.0;
    bool busy_ = true;
};

VideoWidget::VideoWidget(QWidget *parent) : QWidget(parent)
{
    video_ = new QVideoWidget(this);
    sink_  = video_->videoSink();

    /* === IN3 - the control bar =========================================== *
     *
     * A real row in the layout ABOVE the video, not a widget floating over it.
     * On Windows QVideoWidget is a NATIVE child: it paints over any non-native
     * sibling whatever the Qt stacking order, so a floated button sat half under
     * the video and its clicks reached the video, not the button - exactly what
     * was reported. A layout sibling never overlaps the video's rectangle, so it
     * is always visible and always clickable, in a window and in fullscreen
     * (where the menu bar is gone and this bar is the only control surface). */
    bar_ = new QWidget(this);
    QWidget *bar = bar_;
    bar->setStyleSheet(QStringLiteral("background: rgba(20,20,22,235);"));
    auto *barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(8, 4, 8, 4);
    barLay->setSpacing(8);

    status_ = new QLabel(tr("idle"), bar);
    status_->setStyleSheet(QStringLiteral("color: #e8e8e8;"));
    barLay->addWidget(status_, 1);

    fsBtn_ = new QPushButton(tr("Fullscreen"), bar);
    auto *setBtn  = new QPushButton(tr("Settings"), bar);
    auto *filBtn  = new QPushButton(tr("File transfer"), bar);
    auto *discBtn = new QPushButton(tr("Disconnect"), bar);
    for (QPushButton *b : { fsBtn_, setBtn, filBtn, discBtn }) barLay->addWidget(b);

    /* VID1 - one of the two, never both. */
    placeholder_ = new Placeholder(this);
    auto *area = new QWidget(this);
    stack_ = new QStackedLayout(area);
    stack_->setContentsMargins(0, 0, 0, 0);
    stack_->addWidget(placeholder_);
    stack_->addWidget(video_);
    stack_->setCurrentWidget(placeholder_);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(bar, 0);
    lay->addWidget(area, 1);

    placeholder_->setBusy(tr("Waiting for the picture"),
                          tr("The video server is starting."));

    setMinimumSize(640, 360);

    /* IN1 - take the keyboard, and watch the child for the mouse. The video
     * widget is a real child window that grabs mouse events first, so filtering
     * it is the only way to see them without subclassing a widget we create. */
    setFocusPolicy(Qt::StrongFocus);
    video_->setFocusPolicy(Qt::NoFocus);     /* keys stay with us */
    video_->setMouseTracking(true);
    video_->installEventFilter(this);

    connect(fsBtn_,  &QPushButton::clicked, this,
            [this] { emit requestFullscreenToggle(); });
    connect(setBtn,  &QPushButton::clicked, this,
            [this] { emit requestSettings(); });
    connect(filBtn,  &QPushButton::clicked, this,
            [this] { emit requestFiles(); });
    connect(discBtn, &QPushButton::clicked, this,
            [this] { emit requestDisconnect(); });
}

void VideoWidget::setFullscreenState(bool on)
{
    if (fsBtn_) fsBtn_->setText(on ? tr("Leave fullscreen") : tr("Fullscreen"));
}

/* OV4 - immersive fullscreen: the control bar goes away with the rest of the
 * chrome, so the picture is the whole screen. The way back is F11 (consumed
 * locally, never forwarded) and the overlay hotkey - both still reach us
 * because this widget keeps the keyboard focus. */
void VideoWidget::setChromeVisible(bool on)
{
    if (bar_) bar_->setVisible(on);
}

QImage VideoWidget::currentFrameImage() const
{
    if (!sink_) return QImage();
    QVideoFrame f = sink_->videoFrame();
    if (!f.isValid()) return QImage();
    /* toImage() maps and converts; it returns a detached QImage, so the frame
     * may be unmapped and recycled right after. */
    return f.toImage();
}

void VideoWidget::setLocalCursorHidden(bool hidden)
{
    setCursorSource(hidden ? CursorNone : CursorLocal);
}

void VideoWidget::setCursorSource(int src)
{
    cursorSrc_ = src;
    lastCursorSeq_ = 0;          /* force a refetch of the VM shape */
    if (!cursorTimer_) {
        /* The VM sends a shape every few seconds, not per frame, so polling the
         * image counter at 1 Hz is enough and costs nothing. */
        cursorTimer_ = new QTimer(this);
        cursorTimer_->setInterval(1000);
        connect(cursorTimer_, &QTimer::timeout, this, &VideoWidget::applyCursor);
    }
    if (src == CursorVmImage) cursorTimer_->start();
    else                      cursorTimer_->stop();
    applyCursor();
}

/* OV6 - turn the VM's BGRA bitmap into the widget's actual mouse cursor. Using
 * a real QCursor rather than painting a sprite is what keeps it glued to the
 * pointer: there is no second position to track, and no lag of our own. */
void VideoWidget::applyCursor()
{
    if (!video_) return;
    if (cursorSrc_ == CursorNone)  { video_->setCursor(Qt::BlankCursor); return; }
    if (cursorSrc_ == CursorLocal) { video_->setCursor(Qt::ArrowCursor); return; }

    uint32_t received = 0, hidden = 0;
    cursor_state_get_image_stats(&received, &hidden);
    if (received == lastCursorSeq_ && lastCursorSeq_ != 0) return;  /* unchanged */

    cursor_image_t ci;
    if (!cursor_state_get_image(&ci) || ci.format != 2 /* BGRA32 */
        || ci.width == 0 || ci.height == 0 || !ci.pixels) {
        /* Nothing usable yet: the local arrow rather than no pointer at all. */
        video_->setCursor(Qt::ArrowCursor);
        return;
    }
    /* QImage::Format_ARGB32 is 0xAARRGGBB in a uint32, i.e. B,G,R,A in memory
     * on a little-endian machine - the wire's byte order exactly, so the rows
     * copy straight across with no channel swap. */
    QImage img((int)ci.width, (int)ci.height, QImage::Format_ARGB32);
    for (uint32_t y = 0; y < ci.height; y++)
        memcpy(img.scanLine((int)y), ci.pixels + (size_t)y * ci.stride,
               (size_t)ci.width * 4);
    video_->setCursor(QCursor(QPixmap::fromImage(img),
                              (int)ci.hot_x, (int)ci.hot_y));
    lastCursorSeq_ = received;
}

void VideoWidget::presentFrame(const QVideoFrame &frame)
{
    if (!sink_ || !frame.isValid()) return;
    sink_->setVideoFrame(frame);
    presented_++;

    /* VID1 - the first frame is what ends the waiting state. Done on the
     * frame and not on a protocol event on purpose: a channel can be granted,
     * announced and connected and still produce no picture, and in that case
     * the panel staying up with its seconds ticking is the true report. */
    if (!showing_video_) {
        showing_video_ = true;
        stack_->setCurrentWidget(video_);
    }

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
    /* VID1 - the same words under the spinner while there is no picture. The
     * bar is a strip of 12px text at the top of a 700px window; someone
     * staring at the middle of the screen was not reading it. */
    if (placeholder_ && !showing_video_) placeholder_->setDetail(text);
}

void VideoWidget::beginSession()
{
    presented_ = 0;
    showing_video_ = false;
    frameW_ = frameH_ = 0;
    stack_->setCurrentWidget(placeholder_);
    placeholder_->setBusy(tr("Waiting for the picture"),
                          tr("The video server is starting."));
}

void VideoWidget::showMessage(const QString &title, const QString &detail)
{
    showing_video_ = false;
    stack_->setCurrentWidget(placeholder_);
    placeholder_->setIdle(title, detail);
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

/* KEY1 - is this event the fullscreen key?
 *
 * The key used to be written here as a literal `Qt::Key_F11` AND in the menu
 * action, so changing one left the other swallowing the old key. The sequence
 * now arrives from `MainWindow::applyShortcuts`, which reads the one table.
 *
 * Compared as key + modifiers rather than through `QKeySequence::matches`:
 * matches() wants a sequence built from the event, and building one per key
 * press on the streaming path - which is every key the VM receives - is work
 * for nothing when a single chord is all that can be configured. */
bool VideoWidget::isFullscreenKey(const QKeyEvent *e) const
{
    if (fs_key_ == 0) return false;
    const int mods = int(e->modifiers() & ~Qt::KeypadModifier);
    return (fs_key_ & ~Qt::KeyboardModifierMask) == e->key()
        && (fs_key_ &  Qt::KeyboardModifierMask) == mods;
}

void VideoWidget::setFullscreenKey(const QKeySequence &seq)
{
    fs_key_ = seq.isEmpty() ? 0 : seq[0].toCombined();
}

void VideoWidget::keyPressEvent(QKeyEvent *e)
{
    /* IN3 - the fullscreen key is a LOCAL command, never forwarded.
     * Intercepting it here is also what keeps a hook-proof way out of
     * fullscreen: it is not one of the keys IN2's hook swallows, so it always
     * reaches us. */
    if (isFullscreenKey(e)) { emit requestFullscreenToggle(); return; }
    postKey(e, true);
}

void VideoWidget::keyReleaseEvent(QKeyEvent *e)
{
    if (isFullscreenKey(e)) return;   /* its press was consumed locally */
    postKey(e, false);
}
