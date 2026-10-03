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

#include <QKeySequence>
#include <QWidget>
#include <QImage>
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

    /* OV2 - the picture currently on screen, as an image, for the screenshot
     * tool. Taken from the sink's own frame, so what is saved is the DECODED
     * picture: no overlay, no HUD, no local cursor in the file. Null before the
     * first frame. */
    QImage currentFrameImage() const;

    /* OV5 - pictures this widget has put on screen, for the HUD. */
    quint64 framesPresented() const { return presented_; }

    /* OV6 - which pointer is drawn over the video.
     *   CursorNone  : nothing (the VM paints its own inside the picture)
     *   CursorVmImage: the bitmap the VM sends on :base+20, used as the real
     *                  mouse cursor so it follows the pointer with no lag and
     *                  no second position to track
     *   CursorLocal : this desktop's arrow
     * The VM image needs the cursor channel to have delivered a shape; until it
     * does, this falls back to the local arrow rather than leaving nothing. */
public:
    enum CursorSource { CursorNone = 0, CursorVmImage = 1, CursorLocal = 2 };

public slots:
    void setCursorSource(int src);
    int  cursorSource() const { return cursorSrc_; }

    /* IN4 - hide the LOCAL system cursor over the video. On a remote desktop the
     * VM draws its own cursor inside the picture, so the local one is a second,
     * lagging cursor; hiding it leaves only the VM's. */
    /* KEY1 - the fullscreen chord, from the one shortcut table. Empty unbinds
     * it, in which case the key is forwarded to the VM like any other. */
    void setFullscreenKey(const QKeySequence &seq);

    void setLocalCursorHidden(bool hidden);

    /* OV4 - show/hide the control bar for immersive fullscreen. */
    void setChromeVisible(bool on);

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

private:
    bool isFullscreenKey(const QKeyEvent *e) const;
    int  fs_key_ = 0;   /* KEY1 - key|modifiers, 0 = unbound */

    /* Widget point -> decoded-frame point, accounting for the letterbox. Returns
     * false when the point is on a black bar (outside the video). */
    bool mapToFrame(const QPointF &widgetPt, int &fx, int &fy) const;
    void postKey(QKeyEvent *e, bool pressed);

    QVideoWidget *video_ = nullptr;
    QVideoSink   *sink_  = nullptr;
    QLabel       *status_ = nullptr;
    QWidget      *bar_ = nullptr;
    int           cursorSrc_ = CursorLocal;
    class QTimer *cursorTimer_ = nullptr;
    quint32       lastCursorSeq_ = 0;
    void applyCursor();

    /* IN3 - the Fullscreen button, kept so its label flips in fullscreen. The
     * rest of the control bar is wired in place. */
    class QPushButton *fsBtn_ = nullptr;

    int frameW_ = 0, frameH_ = 0;   /* last decoded size, for the coord map */

    /* Counted, because "the window is black" and "no picture ever arrived" are
     * different problems and the first thing to ask is which one it is. */
    quint64 presented_ = 0;
};
