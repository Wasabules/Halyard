/* BootstrapWorker - see bootstrap_worker.hpp for the sequence and why order
 * matters at every step. */
#include "bootstrap_worker.hpp"

#include "core_scope.hpp"

#include <QThread>

using halyard::str;

/* How long to wait for the machine's address. The server answers HTTP 470
 * `{"err":"vm not on a slot"}` while it boots, and a cold VM took 285 s in one
 * measurement - so the budget is TIME and the loop follows the server's own
 * answer rather than a retry count (UX12, which is the same fix made in the
 * Borealis client). Erring long costs nothing: the stop flag is polled every
 * 100 ms. */
static constexpr int kVmIpBudgetSeconds = 180;
static constexpr int kVmIpStepSeconds   = 2;

BootstrapWorker::BootstrapWorker(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<BootstrapWorker::Ready>();
}

void BootstrapWorker::requestStop() { stop_ = true; }

void BootstrapWorker::start(const QString &launcherUrl, const QString &bearer,
                            const QString &vmId)
{
    stop_ = false;

    const QByteArray base = launcherUrl.toUtf8();
    const QByteArray tok  = bearer.toUtf8();
    const QByteArray vm   = vmId.toUtf8();
    long http = 0;

    /* --- 0. capabilities and turn-servers, non-fatal but KEPT ------------ */
    {
        halyard::ScopedVmCaps caps;
        if (launcher_get_capabilities(base, tok, vm, caps.out(), &http)) {
            emit stepDone(-1, tr("video %1, up to %2x%3 @ %4, codecs %5")
                                  .arg(caps->video_allowed ? tr("allowed") : tr("refused"))
                                  .arg(caps->max_width).arg(caps->max_height)
                                  .arg(caps->max_frame_rate)
                                  .arg(str(caps->video_codecs)));
        } else {
            emit stepDone(-1, tr("capabilities: HTTP %1, continuing").arg(http));
        }
        halyard::ScopedTurnServers turn;
        (void)launcher_get_turn_servers(base, tok, vm, turn.out(), &http);
    }
    if (stopped()) return;

    /* --- 1. start the machine -------------------------------------------- */
    emit stepRunning(0, QString());
    if (!launcher_start_vm(base, tok, vm, &http)) {
        emit stepFailed(0, tr("HTTP %1").arg(http));
        return;
    }
    emit stepDone(0, QString());
    if (stopped()) return;

    /* --- 2. the machine's address ---------------------------------------- */
    emit stepRunning(1, QString());
    halyard::ScopedVmConn conn;
    bool got = false;
    int waited = 0;
    long lastHttp = 0;
    for (; waited < kVmIpBudgetSeconds && !stopped(); waited += kVmIpStepSeconds) {
        long st = 0;
        /* A FRESH guard per attempt: `launcher_get_vm_ip` fills the struct, and
         * reusing a filled one across attempts would leak the previous answer.
         * The Borealis client calls `vmconn_free` by hand in the loop for the
         * same reason. */
        halyard::ScopedVmConn attempt;
        if (launcher_get_vm_ip(base, tok, vm, attempt.out(), &st)
            && attempt->ip && attempt->port) {
            /* Copy out what step 7 needs before the guard goes out of scope. */
            conn.get() = attempt.get();
            memset(attempt.out(), 0, sizeof(VmConnectionInfo));  /* ownership moved */
            got = true;
            break;
        }
        lastHttp = st;
        /* 470 = "vm not on a slot" = still booting. Anything else is not
         * something waiting will fix, so it stops at once instead of being
         * retried for three minutes. */
        if (st != 470 && st != 0) break;
        emit stepRunning(1, tr("the machine is starting - %1 s / %2 s")
                                .arg(waited + kVmIpStepSeconds)
                                .arg(kVmIpBudgetSeconds));
        for (int s = 0; s < kVmIpStepSeconds * 10 && !stopped(); s++)
            QThread::msleep(100);
    }
    if (stopped()) return;
    if (!got) {
        emit stepFailed(1, tr("HTTP %1 after %2 s").arg(lastHttp).arg(waited));
        return;
    }
    const QString vmHost = str(conn->ip);
    const int portBase   = QString(str(conn->port)).toInt() + 7000;
    const QString proximusUrl = str(conn->proximus_url);
    emit stepDone(1, QStringLiteral("%1:%2").arg(vmHost).arg(portBase));

    /* --- 3. open the session --------------------------------------------- */
    emit stepRunning(2, QString());
    halyard::ScopedSessionToken sess;
    if (!launcher_auth_login(base, tok, vm, sess.out(), &http) || !sess->token) {
        emit stepFailed(2, tr("HTTP %1").arg(http));
        return;
    }
    emit stepDone(2, QString());
    if (stopped()) return;

    /* --- 4. streaming permissions ---------------------------------------- */
    emit stepRunning(3, QString());
    halyard::ScopedProxCreds creds;
    if (!launcher_proximus_credentials(base, tok, vm, creds.out(), &http)
        || !creds->launcher_jwt || !creds->main_jwt) {
        emit stepFailed(3, tr("HTTP %1").arg(http));
        return;
    }
    emit stepDone(3, QString());
    if (stopped()) return;

    /* --- 5. the two streaming clients ------------------------------------ */
    emit stepRunning(4, QString());
    const QByteArray prox = proximusUrl.toUtf8();
    halyard::ScopedProxLauncher pl;
    if (!proximus_create_launcher_client(prox, creds->launcher_jwt, pl.out(), &http)) {
        emit stepFailed(4, tr("launcher client: HTTP %1").arg(http));
        return;
    }
    halyard::ScopedProxMain pm;
    if (!proximus_create_main_client(prox, creds->main_jwt, pm.out(), &http)
        || !pm->streaming_token || !pm->id) {
        emit stepFailed(4, tr("main client: HTTP %1").arg(http));
        return;
    }
    emit stepDone(4, QString());
    if (stopped()) return;

    /* --- 6. both SSE streams, WITHOUT WHICH THE CONTROL PORT NEVER OPENS - */
    emit stepRunning(5, QString());
    proximus_sse_keepalive *sseL = proximus_sse_start(prox, creds->launcher_jwt);
    proximus_sse_keepalive *sseM = proximus_sse_start(prox, creds->main_jwt);
    if (!sseL || !sseM) {
        if (sseL) proximus_sse_stop(sseL);
        if (sseM) proximus_sse_stop(sseM);
        /* Not a soft failure: KB §3.37 says the server opens `:base+11` only
         * once BOTH are up, so continuing would spend the whole retry ladder
         * on a port that will never answer. */
        emit stepFailed(5, tr("one of the two event streams did not start - "
                              "the control port stays shut without both"));
        return;
    }
    emit stepDone(5, QString());

    /* --- 7. handed over -------------------------------------------------- */
    emit stepRunning(6, QString());
    Ready r;
    r.vmHost         = vmHost;
    r.portBase       = portBase;
    r.streamingToken = str(pm->streaming_token);
    r.clientId       = str(pm->id);
    r.bearer         = bearer;
    r.sseLauncher    = sseL;   /* the receiver stops them - see the header */
    r.sseMain        = sseM;
    emit ready(r);
}
