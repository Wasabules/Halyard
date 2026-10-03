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
#include "core/services/proximus.h"   /* S51/QT - deleting our clients */
}

#include <cstdlib>

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

void SessionWorker::runSession(const BootstrapWorker::Ready &r)
{
    /* The keepalives are stopped on EVERY exit from this function, including
     * the early one below - see the header. */
    struct SseGuard {
        proximus_sse_keepalive *l, *m;
        ~SseGuard() {
            if (l) proximus_sse_stop(l);
            if (m) proximus_sse_stop(m);
        }
    } sse{r.sseLauncher, r.sseMain};

    /* === S51/QT 2026-10-03 — AND DELETE THE TWO CLIENTS ===================
     *
     * `proximus.h`: the official client deletes its client registrations at
     * the end of a session, and "without it the clients pile up on the VM".
     * K14 wrote the call in 2026-08 and wired it only into the headless test
     * binary; Borealis caught up (S51); this client was still leaving a pair
     * behind on every single session.
     *
     * The order is the official client's: MAIN first, then launcher. Each
     * with its OWN jwt - they are different tokens and the server checks.
     *
     * A guard beside the SSE one, and for the same reason: this function has
     * several exits and a cleanup written at one of them is a cleanup that
     * runs on one of them. Failures are ignored on purpose - we are closing,
     * and a VM that has already gone does not need telling. It blocks for up
     * to a couple of seconds (measured ~2 s of a ~3 s close on console),
     * which is acceptable HERE because this is the session thread; it would
     * not be on the GUI one.
     *
     * `SHADOW_DELETE_CLIENT=0` restores the old behaviour, same name as the
     * Borealis toggle so one setting covers both clients. */
    struct ClientGuard {
        BootstrapWorker::Ready r;
        ~ClientGuard() {
            const char *e = getenv("SHADOW_DELETE_CLIENT");
            if (e && atoi(e) == 0) return;
            if (r.proximusUrl.isEmpty()) return;
            const QByteArray url = r.proximusUrl.toUtf8();
            long st = 0;
            if (!r.mainClientId.isEmpty() && !r.mainJwt.isEmpty()) {
                const QByteArray j = r.mainJwt.toUtf8(), id = r.mainClientId.toUtf8();
                (void)proximus_delete_client(url, j, id, &st);
                qInfo("[S51] main client deleted (HTTP %ld)", st);
            }
            if (!r.launcherClientId.isEmpty() && !r.launcherJwt.isEmpty()) {
                const QByteArray j = r.launcherJwt.toUtf8(),
                                 id = r.launcherClientId.toUtf8();
                (void)proximus_delete_client(url, j, id, &st);
                qInfo("[S51] launcher client deleted (HTTP %ld)", st);
            }
        }
    } clients{r};

    if (running_.exchange(true)) {
        emit finished(false, QStringLiteral("a session is already running"));
        return;
    }
    abort_flag_ = 0;

    /* Registered BEFORE the session, or every picture is decoded and dropped -
     * core says so once in the log, which is how a client author finds out. */
    ctrl_session_glue_set_frame_sink(&SessionWorker::frameSinkTrampoline);

    /* The byte arrays must outlive the call: `ctrl_session_glue_params` holds
     * `const char *` into them, and a QString temporary would be gone before
     * the session read it. */
    const QByteArray host  = r.vmHost.toUtf8();
    const QByteArray token = r.streamingToken.toUtf8();
    const QByteArray cid   = r.clientId.toUtf8();
    const QByteArray jwt   = r.bearer.toUtf8();

    ctrl_session_glue_params p{};
    p.vm_host         = host.constData();
    p.streaming_token = token.constData();
    p.client_id       = cid.constData();
    p.bearer_jwt      = jwt.constData();
    p.display_width   = 1920;
    p.display_height  = 1080;
    p.port_base       = r.portBase;
    p.abort_flag      = &abort_flag_;

    emit progress(QStringLiteral("M7.stream"),
                  QStringLiteral("opening the control channel on :%1")
                      .arg(r.portBase + 11));

    ctrl_session_glue_stats st{};
    const bool ok = ctrl_session_glue_run(&p, &st);

    running_ = false;
    emit finished(ok, ok
        ? QStringLiteral("%1 s, %2 pictures decoded")
              .arg(st.session_seconds).arg(st.frames_displayed)
        : QStringLiteral("exit_reason=%1 after %2 s")
              .arg(st.exit_reason).arg(st.session_seconds));
}
