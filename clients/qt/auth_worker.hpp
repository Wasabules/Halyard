/* AuthWorker - sign in, on a thread of its own.
 *
 * === QT2 2026-10-03 — THE SEQUENCE, WHICH IS NOT OBVIOUS ===================
 *
 * Mirrors `clients/borealis/activity/boot_activity.cpp` rather than inventing
 * one, because the order is load-bearing:
 *
 *   1. `tinag_get_datacenter(email)` -> `launcher_api_url`. Everything after
 *      this needs that URL; the Borealis client calls it with a CONSTANT
 *      address ("test@example.com") because the answer is per data-centre, not
 *      per account.
 *   2. `oauth_discover()` -> the OIDC endpoints.
 *   3. If a refresh token is on disk: `oauth_load_refresh` + `oauth_refresh`.
 *      Silent, and the common case - the device grant is for a first run or an
 *      expired token.
 *   4. Otherwise `oauth_device_init()` -> a user code to show, then
 *      `oauth_device_poll()` every `interval` seconds until the user has
 *      authorised it in a browser.
 *   5. `oauth_save_refresh()` so the next start is silent.
 *
 * All of it is BLOCKING HTTP, which is why this is a worker and not a slot on
 * the window.
 *
 * === THE USER CODE GOES ON THE CLIPBOARD ===================================
 *
 * `core/services/local_clipboard.h` already does this for the Borealis client
 * (CLIP5): the code is eight characters to retype in a browser on the same
 * machine, so it is copied and the screen says so. The copy belongs to the UI
 * thread, so this worker only EMITS the code and the window copies it - a
 * worker thread calling `OpenClipboard` is the Win32 mistake that CLIP3
 * documents.
 */
#pragma once

#include <QObject>
#include <QString>

#include <atomic>

class AuthWorker : public QObject
{
    Q_OBJECT

public:
    explicit AuthWorker(QObject *parent = nullptr);

    void requestStop();

public slots:
    /* Runs the whole sequence. Emits `pairingNeeded` when a browser visit is
     * required, then `succeeded` once a token is in hand. */
    void signIn();

signals:
    /* The data centre, resolved first because everything needs its URL. */
    void datacentre(const QString &name, const QString &launcherUrl);

    /* A browser visit is needed. `userCode` is what the person types,
     * `verificationUri` where, and `verificationUriComplete` the one-click
     * form. The window copies the code to the clipboard - see the header. */
    void pairingNeeded(const QString &userCode,
                       const QString &verificationUri,
                       const QString &verificationUriComplete,
                       int expiresInSeconds);

    /* Still waiting for the browser. `secondsLeft` so the screen can say how
     * long the code is good for rather than spinning silently. */
    void pairingProgress(int secondsLeft);

    void progress(const QString &what);

    /* `bearer` is the access token every launcher call needs. It is a
     * CREDENTIAL: the window keeps it for the session and never logs it. */
    void succeeded(const QString &bearer, const QString &launcherUrl);
    void failed(const QString &why);

private:
    std::atomic<bool> stop_{false};
};
