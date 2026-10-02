/* SessionWorker - see session_worker.hpp for the three contracts. */
#include "session_worker.hpp"

#include <QDebug>
#include <QVideoFrameFormat>
#include <QThread>

#include "plane_copy.hpp"

extern "C" {
#include "core/session/ctrl_session_glue.h"
#include "core/protocol/ctrl_session.h"
#include "core/protocol/session_caps.h"
}

/* The depth of the frame pool. Three is the Borealis client's own queue depth
 * for the same reason: in steady state decoding keeps up and the pool sits at
 * one, and the slack is for bursts (G25 measured bursts of 2-3 pictures under
 * 16 ms). More than that only delays the picture. */
static constexpr int kPoolDepth = 3;

SessionWorker *SessionWorker::s_instance = nullptr;

SessionWorker::SessionWorker(QObject *parent) : QObject(parent)
{
    /* ONE session per process (contract 1). Constructing a second worker is a
     * programming error, not a runtime condition, so it is said loudly and the
     * trampoline keeps pointing at the first. */
    if (s_instance) {
        qCritical("SessionWorker: a second instance was constructed. The core's "
                  "session API is global (ctrl_session_set_bitrate, "
                  "request_idr, active take no handle), so only one session per "
                  "process is possible. The first instance keeps the frame sink.");
    } else {
        s_instance = this;
    }
}

SessionWorker::~SessionWorker()
{
    requestStop();
    if (s_instance == this) {
        /* Unregister before dying: core would otherwise call a sink whose
         * object is gone, on the decode thread, with no way to notice. */
        ctrl_session_glue_set_frame_sink(nullptr);
        s_instance = nullptr;
    }
}

void SessionWorker::requestStop()
{
    abort_flag_ = 1;
}

/* ---------------------------------------------------------------- the pool */

QVideoFrame SessionWorker::takeFromPool(int width, int height, int format)
{
    const QVideoFrameFormat::PixelFormat pf =
        /* core passes an AVPixelFormat: 0 = YUV420P, 23 = NV12
         * (core/media/h264_decoder.h says so at the callback's declaration). */
        (format == 23) ? QVideoFrameFormat::Format_NV12
                       : QVideoFrameFormat::Format_YUV420P;

    {
        QMutexLocker lock(&pool_mutex_);
        for (int i = 0; i < pool_.size(); i++) {
            const QVideoFrame &f = pool_.at(i);
            if (f.width() == width && f.height() == height
                && f.surfaceFormat().pixelFormat() == pf) {
                QVideoFrame taken = pool_.takeAt(i);
                return taken;
            }
        }
    }
    /* Nothing of the right shape: allocate. Happens on the first pictures and
     * after a resolution change, which is where an allocation is affordable. */
    return QVideoFrame(QVideoFrameFormat(QSize(width, height), pf));
}

void SessionWorker::returnToPool(const QVideoFrame &f)
{
    QMutexLocker lock(&pool_mutex_);
    if (pool_.size() < kPoolDepth)
        pool_.append(f);
    /* Over the depth, it is simply dropped - Qt frames are reference-counted,
     * so this is a free, not a leak. */
}

/* ------------------------------------------------------------- the sink */

void SessionWorker::frameSinkTrampoline(int width, int height,
                                        const uint8_t *y, int ly,
                                        const uint8_t *u, int lu,
                                        const uint8_t *v, int lv,
                                        int format, int64_t pts, void *user)
{
    (void)user;   /* belongs to the session's params, not to us */
    if (s_instance)
        s_instance->onFrame(width, height, y, ly, u, lu, v, lv, format, pts);
}

void SessionWorker::onFrame(int width, int height,
                            const uint8_t *y, int ly,
                            const uint8_t *u, int lu,
                            const uint8_t *v, int lv,
                            int format, int64_t pts)
{
    if (width <= 0 || height <= 0 || !y) return;

    QVideoFrame frame = takeFromPool(width, height, format);
    if (!frame.isValid()) return;

    /* THE COPY, and it happens HERE, on the decode thread, because the planes
     * die when this function returns (contract 2). */
    if (!frame.map(QVideoFrame::WriteOnly)) return;

    const bool nv12 = frame.surfaceFormat().pixelFormat()
                      == QVideoFrameFormat::Format_NV12;

    /* QT1: the stride-aware copy lives in `plane_copy.hpp`, pure, with 25
     * checks of its own (tests/test_qt_planes.cpp). It is the part that goes
     * wrong invisibly - a single memcpy of a whole plane shears every row
     * against the last, and the result looks like a decoder fault. */
    using halyard::copyPlane;
    using halyard::planeShapeNv12;
    using halyard::planeShapeYuv420p;

    bool copied = true;
    auto plane = [&](int i) {
        return nv12 ? planeShapeNv12(width, height, i)
                    : planeShapeYuv420p(width, height, i);
    };
    copied = copied && copyPlane(frame.bits(0), frame.bytesPerLine(0),
                                 y, ly, plane(0));
    copied = copied && copyPlane(frame.bits(1), frame.bytesPerLine(1),
                                 u, lu, plane(1));
    if (!nv12)
        copied = copied && copyPlane(frame.bits(2), frame.bytesPerLine(2),
                                     v, lv, plane(2));

    frame.unmap();

    /* A refused copy means the destination could not hold the row - see
     * plane_copy.hpp. The frame is dropped rather than shown cropped, and said
     * once, because a silently cropped picture is not a thing anybody reports. */
    if (!copied) {
        static bool said = false;
        if (!said) {
            said = true;
            qWarning("SessionWorker: plane copy refused for %dx%d (%s) - the "
                     "surface stride cannot hold the row. Frames dropped.",
                     width, height, nv12 ? "NV12" : "YUV420P");
        }
        returnToPool(frame);
        return;
    }

    if (pts >= 0) frame.setStartTime(pts);

    /* Queued by the connection, so this returns at once and the decode thread
     * goes back to decoding. */
    emit frameReady(frame);
}

/* ------------------------------------------------------------- the session */

void SessionWorker::run(const QString &vmHost, int portBase)
{
    if (running_.exchange(true)) {
        emit finished(false, QStringLiteral("a session is already running"));
        return;
    }
    abort_flag_ = 0;

    /* Registered BEFORE the session, or every picture is decoded and dropped -
     * core says so once in the log, which is how a client author finds out. */
    ctrl_session_glue_set_frame_sink(&SessionWorker::frameSinkTrampoline);

    const QByteArray host = vmHost.toUtf8();

    ctrl_session_glue_params p{};
    p.vm_host        = host.constData();
    p.display_width  = 1920;
    p.display_height = 1080;
    p.port_base      = portBase;
    p.abort_flag     = &abort_flag_;

    /* NOT filled here: streaming_token, client_id, bearer_jwt. They come from
     * the REST bootstrap (OAuth device grant, VM start, service tokens) which
     * this skeleton does not do yet - `clients/borealis/activity/
     * connecting_activity.cpp` is the worked example, seven steps long.
     *
     * So this call is expected to fail at the control channel, and that is the
     * point: it proves the wiring, the thread and the frame sink without an
     * account. */
    emit progress(QStringLiteral("M0.skeleton"),
                  QStringLiteral("no credentials: the session will stop at the "
                                 "control channel, by design"));

    ctrl_session_glue_stats st{};
    const bool ok = ctrl_session_glue_run(&p, &st);

    running_ = false;
    emit finished(ok, ok ? QStringLiteral("session ended")
                         : QStringLiteral("bootstrap failed (expected without "
                                          "credentials), exit_reason=%1")
                               .arg(st.exit_reason));
}
