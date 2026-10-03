/* The account: what it is entitled to, and where it stands.
 *
 * === ACC1 2026-10-03 — A CENSUS SAID THIS WAS ALL ALREADY ARRIVING =========
 *
 * An audit of every reply the core parses against every widget the Qt client
 * draws turned up one pattern over and over: the data is fetched, kept in a
 * struct and never read. The biggest single block of it was `VmUsage` - the
 * session ceiling, the fair-use allowance, how much is spent, when it renews
 * - parsed with a comment saying it exists so a client can tell someone where
 * they stand, and then read by nothing.
 *
 * `/v1/subscription/status` was worse: the core can fetch it, the console
 * client does, and the Qt client never called it at all. Plan, active or
 * expired, on hold and why, since when.
 *
 * This window is where those belong. Not a header line and not a toast: the
 * figures are THE ACCOUNT'S, not the machine's - a monthly allowance and how
 * much of it is gone - and the one place they are appropriate is a window
 * someone opened on purpose. They must not end up in an overlay that survives
 * a screen share, which is why none of this is wired to the HUD.
 *
 * === WHAT IT DELIBERATELY DOES NOT SHOW ==================================
 *
 * No token of any kind. `plan_short` rather than `plan_id`, because the
 * derived form is the display-safe one. No account id, no login name, no
 * device UUID. The census flagged each of those; a window about entitlements
 * does not need an identifier to say what someone is entitled to.
 */
#pragma once

#include <QWidget>

class QLabel;
class QProgressBar;
class QFormLayout;

class AccountWindow : public QWidget
{
    Q_OBJECT

public:
    AccountWindow(QWidget *parent = nullptr);

    /* The bearer and the launcher URL the session signed in with, plus a
     * machine to ask about. Without a machine the capabilities half is
     * skipped and the subscription half still works - which is the state
     * right after signing in on an account whose list is empty. */
    void setSession(const QString &launcherUrl, const QString &bearer,
                    const QString &vmId, const QString &dataCentre,
                    const QString &launcherApiVersion);

public slots:
    void refresh();

protected:
    void showEvent(QShowEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void build();
    QLabel *addRow(QFormLayout *f, const QString &label);

    QString launcher_, bearer_, vm_, datacentre_, apiVersion_;
    bool    busy_ = false;

    QLabel *v_plan_ = nullptr, *v_status_ = nullptr, *v_since_ = nullptr,
           *v_payment_ = nullptr, *v_hold_ = nullptr;
    QLabel *v_session_len_ = nullptr, *v_hard_stop_ = nullptr,
           *v_slots_ = nullptr;
    QLabel *v_fair_text_ = nullptr, *v_renew_ = nullptr;
    QProgressBar *v_fair_bar_ = nullptr;
    QLabel *v_monitors_ = nullptr, *v_resolution_ = nullptr, *v_fps_ = nullptr,
           *v_vcodecs_ = nullptr, *v_chroma_ = nullptr, *v_acodecs_ = nullptr,
           *v_channels_ = nullptr;
    QLabel *v_dc_ = nullptr, *v_api_ = nullptr, *v_app_ = nullptr;
    QLabel *v_note_ = nullptr;
};
