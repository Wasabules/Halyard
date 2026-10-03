/* VideoWidget - the decoded picture on screen, converted on the GPU, and the
 * surface the keyboard and mouse are forwarded from.
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
 *
 * === IN1 2026-10-03 — INPUT FORWARDING =====================================
 *
 * The keyboard and mouse reach the VM through core's `shadow_input` queue, the
 * same one the Borealis stream view posts to; core's session thread drains it.
 * This widget posts and never touches the socket.
 *
 *   - Mouse coordinates are mapped into the DECODED resolution, because that is
 *     the space core clamps to (`shadow_input_set_bounds`, set here from each
 *     frame). The video is letterboxed inside the widget, so the map subtracts
 *     the black bars - a click on a bar is dropped rather than sent at the edge.
 *   - Keys go through `qt_input_map.hpp` (physical, not the character - see
 *     there). Auto-repeat is dropped: the VM runs its own.
 *   - The mouse events arrive on the child `QVideoWidget`, which takes them
 *     first, so this installs an event filter on it rather than overriding the
 *     handlers of a widget it does not own.
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

    /* The overlay reflects the window's state so its label is right. */
    void setFullscreenState(bool on);

signals:
    /* IN3 - the overlay's actions. The main window owns the window state and the
     * session, so it does the fullscreen toggle and the disconnect; this widget
     * only asks. The menu is always reachable with the mouse, which is the
     * guaranteed way out when the key hook (IN2) is swallowing Alt+Tab. */
    void requestFullscreenToggle();
    void requestDisconnect();
    void requestSettings();
    void requestFiles();

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    /* Widget point -> decoded-frame point, accounting for the letterbox. Returns
     * false when the point is on a black bar (outside the video). */
    bool mapToFrame(const QPointF &widgetPt, int &fx, int &fy) const;
    void postKey(QKeyEvent *e, bool pressed);
    void placeOverlay();

    QVideoWidget *video_ = nullptr;
    QVideoSink   *sink_  = nullptr;
    QLabel       *status_ = nullptr;

    /* IN3 - the overlay. `menuBtn_` is pinned top-right, always clickable;
     * `overlay_` is the panel it shows. Children of `this`, stacked ABOVE the
     * video child, so they are never hidden by it. */
    class QWidget     *overlay_ = nullptr;
    class QToolButton *menuBtn_ = nullptr;
    class QPushButton *fsBtn_   = nullptr;

    int frameW_ = 0, frameH_ = 0;   /* last decoded size, for the coord map */

    /* Counted, because "the window is black" and "no picture ever arrived" are
     * different problems and the first thing to ask is which one it is. */
    quint64 presented_ = 0;
};
