/* SessionWorker - a Halyard session, on a thread of its own.
 *
 * === WHY A WORKER AND NOT A TIMER ==========================================
 *
 * `ctrl_session_glue_run()` BLOCKS for the whole session: it opens the control
 * channel, the eight media channels, and then loops until the abort flag is
 * raised. It cannot be driven from an event loop, and nothing it calls back is
 * on the GUI thread.
 *
 * So this object lives on a `QThread`, runs the session there, and the only
 * thing that crosses back to the GUI is a Qt signal — queued, which is what
 * makes it safe.
 *
 * === THE FRAME SINK, AND WHY IT COPIES =====================================
 *
 * `ctrl_session_glue_set_frame_sink()` is called on the DECODE thread and the
 * YUV planes are valid only for the duration of the call. So the sink copies
 * into a `QVideoFrame` there and hands that over; keeping the pointers would be
 * a use-after-free a frame later.
 *
 * It copies into a frame taken from a small POOL rather than allocating one per
 * picture. 1080p NV12 is 3.1 MB and this runs sixty times a second — 187 MB/s
 * of allocation is what the Borealis client already measured and fixed (VI2:
 * push copy p50 362 -> 115 us once the plane buffers were recycled). Starting
 * without the pool would be repeating a measurement we already have.
 *
 * === ONE SESSION PER PROCESS ===============================================
 *
 * The core's session API is global - `ctrl_session_set_bitrate()`,
 * `request_idr()`, `active()` take no handle. So this class refuses to start a
 * second session rather than letting two worker threads into the same module
 * state, and says so.
 */
#pragma once

#include <QObject>
#include <QVideoFrame>
#include <QMutex>
#include <QList>
#include <QString>

#include <atomic>

class SessionWorker : public QObject
{
    Q_OBJECT

public:
    explicit SessionWorker(QObject *parent = nullptr);
    ~SessionWorker() override;

    /* Raises the abort flag the session polls. Safe from any thread, and the
     * session leaves within ~100 ms - the bound the consoles impose and which
     * this client inherits for free. */
    void requestStop();

public slots:
    /* Runs one session. Call it on the worker thread (a queued invocation from
     * the GUI), never directly. */
    void run(const QString &vmHost, int portBase);

signals:
    /* A decoded picture, already copied and owned by the receiver. Connect with
     * Qt::QueuedConnection - the signal is emitted from the decode thread. */
    void frameReady(const QVideoFrame &frame);

    /* Bootstrap progress, straight from core's own step names ("M8.connect",
     * "M9.cap"), so the two clients report the same vocabulary. */
    void progress(const QString &step, const QString &detail);

    void finished(bool ok, const QString &why);

private:
    /* The C callback core invokes on the decode thread. It finds `this` through
     * the single-instance pointer below, because the sink signature carries a
     * `user` pointer that belongs to the SESSION's params, not to us. */
    static void frameSinkTrampoline(int width, int height,
                                    const uint8_t *y, int ly,
                                    const uint8_t *u, int lu,
                                    const uint8_t *v, int lv,
                                    int format, int64_t pts, void *user);
    void onFrame(int width, int height,
                 const uint8_t *y, int ly, const uint8_t *u, int lu,
                 const uint8_t *v, int lv, int format, int64_t pts);

    QVideoFrame takeFromPool(int width, int height, int format);
    void returnToPool(const QVideoFrame &f);

    /* The abort flag core polls. `volatile int` because that is the type
     * `ctrl_session_params` declares; the atomic is ours, for the GUI side. */
    volatile int abort_flag_ = 0;
    std::atomic<bool> running_{false};

    QMutex pool_mutex_;
    QList<QVideoFrame> pool_;

    /* ONE session per process - see the header comment. The trampoline needs to
     * find the instance, and there can only be one. */
    static SessionWorker *s_instance;
};
