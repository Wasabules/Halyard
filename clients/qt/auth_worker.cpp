/* AuthWorker - see auth_worker.hpp for the sequence and why it is a worker. */
#include "auth_worker.hpp"

#include "core_scope.hpp"

#include <QThread>

extern "C" {
#include "core/services/config.h"
}

using halyard::str;

AuthWorker::AuthWorker(QObject *parent) : QObject(parent) {}

void AuthWorker::requestStop() { stop_ = true; }

void AuthWorker::signIn()
{
    stop_ = false;

    /* --- 1. the data centre, which yields the one URL everything needs --- */
    emit stage(StageDatacentre);
    emit progress(tr("resolving the data centre"));
    halyard::ScopedGapInfo gap;
    long http = 0;
    /* A CONSTANT address, exactly as the Borealis client does: the answer is
     * per data centre, not per account, and asking for the user's e-mail here
     * would collect something we have no use for. */
    if (!tinag_get_datacenter("test@example.com", gap.out(), &http)) {
        emit failed(tr("the data centre could not be resolved (HTTP %1)").arg(http));
        return;
    }
    const QString launcherUrl = str(gap->launcher_api_url);
    if (launcherUrl.isEmpty()) {
        emit failed(tr("the data centre gave no launcher URL"));
        return;
    }
    emit datacentre(str(gap->name), launcherUrl);

    /* --- 2. the OIDC endpoints ------------------------------------------- */
    emit stage(StageEndpoints);
    emit progress(tr("discovering the sign-in endpoints"));
    halyard::ScopedDiscovery disc;
    if (!oauth_discover(disc.out(), &http)) {
        emit failed(tr("OIDC discovery failed (HTTP %1)").arg(http));
        return;
    }

    halyard::ScopedAuthState auth;

    /* --- 3. the silent path: a refresh token on disk --------------------- */
    char *saved = nullptr;
    if (oauth_load_refresh(&saved) && saved && *saved) {
        emit progress(tr("refreshing the saved session"));
        auth.get().refresh_token = saved;   /* oauth_state_free owns it now */
        if (oauth_refresh(disc.out(), SHADOW_OAUTH_CLIENT_ID, auth.out())
            && auth->access_token) {
            (void)oauth_save_refresh(auth.out());
            emit succeeded(str(auth->access_token), launcherUrl);
            return;
        }
        emit progress(tr("the saved session is no longer valid"));
        /* Falls through to the device grant. `auth` still owns what it has and
         * frees it at the end of the function; the fields below are overwritten
         * by `oauth_device_poll`, which is why this is not a leak. */
    } else {
        free(saved);
    }

    /* --- 4. the device grant -------------------------------------------- */
    emit stage(StageCode);
    emit progress(tr("asking for a device code"));
    halyard::ScopedDeviceInit grant;
    if (!oauth_device_init(disc.out(), grant.out(), &http) || !grant->user_code) {
        emit failed(tr("no device code could be obtained (HTTP %1)").arg(http));
        return;
    }

    const int expires  = grant->expires_in > 0 ? grant->expires_in : 600;
    const int interval = grant->interval   > 0 ? grant->interval   : 5;

    emit pairingNeeded(str(grant->user_code),
                       str(grant->verification_uri),
                       str(grant->verification_uri_complete),
                       expires);

    /* The poll. `interval` comes from the server and SLOW_DOWN raises it —
     * ignoring either is how a client gets rate-limited into a failure that
     * looks like a refusal. */
    int waited = 0;
    int step = interval;
    while (waited < expires) {
        if (stop_) { emit failed(tr("cancelled")); return; }

        /* Slept in short slices so a cancel is honoured promptly rather than
         * at the end of the interval. */
        for (int s = 0; s < step * 10 && !stop_; s++)
            QThread::msleep(100);
        waited += step;

        const OAuthPollResult r =
            oauth_device_poll(disc.out(), grant->device_code,
                              SHADOW_OAUTH_CLIENT_ID, auth.out());
        switch (r) {
        case OAUTH_POLL_SUCCESS:
            if (!auth->access_token) {
                emit failed(tr("the server reported success with no token"));
                return;
            }
            (void)oauth_save_refresh(auth.out());
            emit succeeded(str(auth->access_token), launcherUrl);
            return;
        case OAUTH_POLL_SLOW_DOWN:
            step += interval;    /* the server asked; obeying is the point */
            break;
        case OAUTH_POLL_PENDING:
            emit pairingProgress(expires - waited);
            break;
        case OAUTH_POLL_DENIED:
            emit failed(tr("the request was refused in the browser"));
            return;
        case OAUTH_POLL_EXPIRED:
            emit failed(tr("the device code expired"));
            return;
        case OAUTH_POLL_NETWORK_ERROR:
        case OAUTH_POLL_OTHER_ERROR:
        default:
            /* NOT fatal: a single failed poll during a browser round trip is
             * ordinary, and giving up on it would make a flaky network look
             * like a refusal. The overall expiry is the real deadline. */
            emit pairingProgress(expires - waited);
            break;
        }
    }
    emit failed(tr("no authorisation within %1 s").arg(expires));
}
