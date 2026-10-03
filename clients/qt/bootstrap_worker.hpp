/* BootstrapWorker - the seven steps from a bearer to a running stream.
 *
 * === QT2 2026-10-03 — THE SEQUENCE IS THE PROTOCOL =========================
 *
 * Mirrors `clients/borealis/activity/connecting_activity.cpp` step for step,
 * with its own step names, because the ORDER is not arbitrary — each step
 * produces what the next one needs, and the KB records what happens when one
 * is skipped (§3.37: without BOTH SSE streams the control port never opens).
 *
 *   0. capabilities + turn-servers       non-fatal, and kept. The official
 *                                        clients call them before /vm/start,
 *                                        possibly as a "modern client" signal
 *                                        for server-side QoS; removing them
 *                                        changes the sequence the server sees
 *                                        on a path that works.
 *   1. `launcher_start_vm`               -> the machine boots
 *   2. `launcher_get_vm_ip`              -> ip, port, proximus_url, slot.
 *                                        Answers HTTP 470 `vm not on a slot`
 *                                        while it boots: THAT is the signal to
 *                                        keep waiting, not a retry count. A
 *                                        cold VM took 285 s in one measurement
 *                                        (UX12), which is why the budget is
 *                                        time and the loop follows the answer.
 *   3. `launcher_auth_login`             -> the session token
 *   4. `launcher_proximus_credentials`   -> launcher_jwt, main_jwt, usb_jwt
 *   5. `proximus_create_launcher_client` -> a client id
 *      `proximus_create_main_client`     -> THE STREAMING TOKEN
 *   6. both SSE streams                  -> without them the control port on
 *                                        `:base+11` never opens (§3.37)
 *   7. `ctrl_session_glue_run`           -> handed to SessionWorker
 *
 * === WHAT THIS WORKER DOES NOT DO ==========================================
 *
 * It does not run the session. Step 7's inputs — vm_host, port_base, the
 * streaming token, the client id, the bearer — are emitted as a `Ready`, and
 * `SessionWorker` takes it from there on its own thread. Two workers because
 * they have two lifetimes: the bootstrap ends, the session lasts.
 *
 * The SSE keepalives are the exception: they must OUTLIVE this worker, because
 * the session needs them for its whole duration. So they are handed over too,
 * and stopping them is the receiver's job.
 */
#pragma once

#include <QObject>
#include <QString>

#include <atomic>

extern "C" {
#include "core/services/proximus.h"
}

class BootstrapWorker : public QObject
{
    Q_OBJECT

public:
    /* What step 7 needs, and the two SSE streams that must stay alive. */
    struct Ready {
        QString vmHost;
        int     portBase = 0;
        QString streamingToken;
        QString clientId;
        QString bearer;
        /* Owned by the receiver once `ready` is emitted: `proximus_sse_stop()`
         * on both when the session ends. They are raw because they are
         * explicitly a handover, and wrapping them in a guard here would free
         * them at the end of this function - which is the bug this comment
         * exists to prevent. */
        proximus_sse_keepalive *sseLauncher = nullptr;
        proximus_sse_keepalive *sseMain     = nullptr;
    };

    /* === CAPS2 2026-10-03 — WHAT THE ACCOUNT IS ALLOWED ====================
     *
     * `/vms/{id}/capabilities` was fetched, summarised into one step line and
     * dropped. Printed whole for the first time (`--probe caps`) it turned
     * out to carry the two things the client most visibly lacked: the
     * per-session time ceiling the official client counts down from, and the
     * number of monitors the VM allows.
     *
     * Passed as a plain struct rather than the core one: `VmCapabilities`
     * owns heap strings freed by `vmcaps_free`, and a queued signal copies
     * its argument - which would hand the GUI thread pointers the worker is
     * about to free. */
    struct Caps {
        int     maxSessionLength = 0;   /* seconds, 0 = unsaid */
        int     maxDuration = 0;        /* the period's allowance */
        int     fairUseUsage = 0;       /* spent so far */
        double  fairUseAlert = 0.0;
        QString fairUseRenew;
        int     maxMonitors = 0;
        int     maxWidth = 0, maxHeight = 0, maxFps = 0;
        QString videoCodecs, videoChroma, audioCodecs;
        bool    microAllowed = false, clipboardAllowed = false;
        bool    fileTransferAllowed = false, gamepadAllowed = false;
    };

    explicit BootstrapWorker(QObject *parent = nullptr);

    void requestStop();

public slots:
    void start(const QString &launcherUrl, const QString &bearer,
               const QString &vmId);

signals:
    void capabilities(const BootstrapWorker::Caps &c);

    /* `index` is 0..6 and matches the seven step names the UI shows, so the
     * two clients report the same vocabulary. `detail` carries the HTTP code or
     * the address, whichever the step produced. */
    void stepRunning(int index, const QString &detail);
    void stepDone(int index, const QString &detail);
    void stepFailed(int index, const QString &detail);

    void ready(const BootstrapWorker::Ready &r);

private:
    bool stopped() const { return stop_.load(); }
    std::atomic<bool> stop_{false};
};

Q_DECLARE_METATYPE(BootstrapWorker::Ready)
Q_DECLARE_METATYPE(BootstrapWorker::Caps)
