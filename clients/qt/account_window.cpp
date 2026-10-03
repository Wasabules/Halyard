/* AccountWindow - see the header for why these figures live here and nowhere
 * else. */
#include "account_window.hpp"

#include "core_scope.hpp"
#include "session_limit.hpp"
#include "theme.hpp"

#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QtConcurrent>

extern "C" {
#include "core/version.h"
}

namespace theme = halyard::theme;
using halyard::str;

namespace {

/* What the two replies give, copied out of the core structs while they are
 * alive: the scope guards free them the moment the worker lambda returns, so
 * anything the GUI thread reads later has to be its own. Deliberately not
 * `BootstrapWorker::Caps`: this window is not part of a bootstrap and a
 * shared struct would tie the two together for no reason beyond the fields
 * happening to match today. */
struct Caps {
    int     maxSessionLength = 0, maxDuration = 0, fairUseUsage = 0;
    double  fairUseAlert = 0.0;
    QString fairUseRenew;
    int     maxMonitors = 0, maxWidth = 0, maxHeight = 0, maxFps = 0;
    QString videoCodecs, videoChroma, audioCodecs;
    bool    microAllowed = false, clipboardAllowed = false;
    bool    fileTransferAllowed = false, gamepadAllowed = false;
};

/* LIM1 - the one formatter, shared with the stream's warnings. It used to be
 * a copy here, and two copies of "hours and minutes" is how this window and
 * the banner end up disagreeing about the separator. */
QString hhmm(int seconds)
{
    return seconds <= 0 ? QStringLiteral("-") : halyard::fmtHours(seconds);
}

/* An ISO 8601 instant in the reader's own locale, or the raw string when it
 * does not parse - a date we cannot read is still information, and replacing
 * it with a dash would hide a server that changed its format. */
QString whenISO(const QString &iso)
{
    if (iso.isEmpty()) return QStringLiteral("-");
    const QDateTime t = QDateTime::fromString(iso, Qt::ISODate);
    if (!t.isValid()) return iso;
    return QLocale().toString(t.toLocalTime(), QLocale::ShortFormat);
}

QString whenUnix(long ts)
{
    if (ts <= 0) return QStringLiteral("-");
    return QLocale().toString(
        QDateTime::fromSecsSinceEpoch(qint64(ts)).toLocalTime(),
        QLocale::ShortFormat);
}

QString yesNo(bool b) { return b ? AccountWindow::tr("yes") : AccountWindow::tr("no"); }

}  // namespace

AccountWindow::AccountWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Account"));
    setWindowIcon(theme::appTileIcon());
    resize(560, 680);
    build();
}

QLabel *AccountWindow::addRow(QFormLayout *f, const QString &label)
{
    auto *v = new QLabel(QStringLiteral("-"), this);
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    f->addRow(label, v);
    return v;
}

void AccountWindow::build()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *host = new QWidget(scroll);
    auto *col = new QVBoxLayout(host);
    col->setContentsMargins(theme::SpacePage, theme::SpaceGroup,
                            theme::SpacePage, theme::SpaceGroup);
    col->setSpacing(theme::SpaceGroup);

    const auto group = [&](const QString &title) {
        auto *g = new QGroupBox(title, host);
        auto *f = new QFormLayout(g);
        f->setHorizontalSpacing(theme::SpaceGroup);
        col->addWidget(g);
        return f;
    };

    {
        auto *f = group(tr("Subscription"));
        v_plan_    = addRow(f, tr("Plan"));
        v_status_  = addRow(f, tr("Status"));
        v_since_   = addRow(f, tr("Active since"));
        v_payment_ = addRow(f, tr("Last payment"));
        v_hold_    = addRow(f, tr("On hold"));
    }

    {
        auto *f = group(tr("Sessions"));
        v_session_len_ = addRow(f, tr("Maximum session length"));
        v_hard_stop_   = addRow(f, tr("Hard stop"));
        v_slots_       = addRow(f, tr("Time slots"));
    }

    {
        /* === ACC1 — fair use, with the SERVER's own alert threshold =======
         *
         * The bar turns amber at `fair_use_alert_threshold`, which the reply
         * carries (0.8 on the account measured). Inventing 80 % here would
         * have been indistinguishable in practice and wrong in principle:
         * the number is the server's to choose and it is already in hand. */
        auto *g = new QGroupBox(tr("Fair use"), host);
        auto *gl = new QVBoxLayout(g);
        gl->setSpacing(theme::SpaceRow);
        v_fair_bar_ = new QProgressBar(g);
        v_fair_bar_->setRange(0, 1000);
        v_fair_bar_->setTextVisible(false);
        v_fair_bar_->setFixedHeight(10);
        v_fair_text_ = new QLabel(QStringLiteral("-"), g);
        v_renew_ = new QLabel(QStringLiteral("-"), g);
        v_renew_->setProperty("dim", true);
        gl->addWidget(v_fair_text_);
        gl->addWidget(v_fair_bar_);
        gl->addWidget(v_renew_);
        col->addWidget(g);
    }

    {
        auto *f = group(tr("What this account may do"));
        v_channels_   = addRow(f, tr("Channels"));
        v_monitors_   = addRow(f, tr("Screens"));
        v_resolution_ = addRow(f, tr("Maximum resolution"));
        v_fps_        = addRow(f, tr("Maximum frame rate"));
        v_vcodecs_    = addRow(f, tr("Video codecs"));
        v_chroma_     = addRow(f, tr("Chroma"));
        v_acodecs_    = addRow(f, tr("Audio codecs"));
    }

    {
        auto *f = group(tr("Service"));
        v_dc_  = addRow(f, tr("Data centre"));
        v_api_ = addRow(f, tr("Launcher API"));
        v_app_ = addRow(f, tr("This client"));
    }

    v_note_ = new QLabel(host);
    v_note_->setWordWrap(true);
    v_note_->setProperty("dim", true);
    col->addWidget(v_note_);

    auto *row = new QHBoxLayout;
    auto *refreshBtn = new QPushButton(tr("Refresh"), host);
    connect(refreshBtn, &QPushButton::clicked, this, &AccountWindow::refresh);
    row->addStretch(1);
    row->addWidget(refreshBtn);
    col->addLayout(row);

    col->addStretch(1);
    scroll->setWidget(host);
    outer->addWidget(scroll);
}

void AccountWindow::setSession(const QString &launcherUrl, const QString &bearer,
                               const QString &vmId, const QString &dataCentre,
                               const QString &launcherApiVersion)
{
    launcher_ = launcherUrl;
    bearer_ = bearer;
    vm_ = vmId;
    datacentre_ = dataCentre;
    apiVersion_ = launcherApiVersion;
}

void AccountWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refresh();
}

void AccountWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange) setWindowTitle(tr("Account"));
    QWidget::changeEvent(e);
}

void AccountWindow::refresh()
{
    v_dc_->setText(datacentre_.isEmpty() ? QStringLiteral("-") : datacentre_);
    v_api_->setText(apiVersion_.isEmpty() ? QStringLiteral("-") : apiVersion_);
    v_app_->setText(str(SHADOW_VERSION));

    if (bearer_.isEmpty() || launcher_.isEmpty()) {
        v_note_->setText(tr("Sign in to see what this account is entitled to."));
        return;
    }
    if (busy_) return;
    busy_ = true;
    v_note_->setText(tr("asking Shadow..."));

    const QByteArray base = launcher_.toUtf8();
    const QByteArray tok  = bearer_.toUtf8();
    const QByteArray vm   = vm_.toUtf8();

    /* Two HTTP calls on a pool thread. Both block, and this window is opened
     * from a menu while a stream may be running - a frozen main thread would
     * stutter the picture to fetch a billing date. */
    (void)QtConcurrent::run([this, base, tok, vm] {
        halyard::ScopedVmCaps caps;
        long chttp = 0;
        bool gotCaps = false;
        if (!vm.isEmpty())
            gotCaps = launcher_get_capabilities(base.constData(), tok.constData(),
                                                vm.constData(), caps.out(), &chttp);

        halyard::ScopedSubscription sub;
        long shttp = 0;
        /* Account-wide: it takes the bearer and nothing else - no launcher
         * base and no machine, because a subscription is not a property of
         * either. */
        const bool gotSub = launcher_get_subscription_status(
            tok.constData(), sub.out(), &shttp);

        /* Everything the GUI needs, copied out while the structs are alive:
         * they are freed by their guards the moment this lambda returns. */
        Caps c;
        if (gotCaps) {
            c.maxSessionLength = caps->usage.max_session_length;
            c.maxDuration      = caps->usage.max_duration;
            c.fairUseUsage     = caps->usage.fair_use_usage;
            c.fairUseAlert     = caps->usage.fair_use_alert_threshold;
            c.fairUseRenew     = str(caps->usage.fair_use_renew_date);
            c.maxMonitors      = caps->max_monitor_count;
            c.maxWidth         = caps->max_width;
            c.maxHeight        = caps->max_height;
            c.maxFps           = caps->max_frame_rate;
            c.videoCodecs      = str(caps->video_codecs);
            c.videoChroma      = str(caps->video_chroma);
            c.audioCodecs      = str(caps->audio_codecs);
            c.microAllowed        = caps->micro_allowed;
            c.clipboardAllowed    = caps->clipboard_allowed;
            c.fileTransferAllowed = caps->filetransfer_allowed;
            c.gamepadAllowed      = caps->gamepad_allowed;
        }
        const QString hardStop = gotCaps ? str(caps->usage.end_of_streaming_session)
                                         : QString();
        /* NOT `slots`: Qt defines that as a macro expanding to nothing, so a
         * local called `slots` becomes `const bool = ...` and the error
         * points at the `=`. Same trap as `signals` and `emit`. */
        const bool slotsOn = gotCaps && caps->usage.time_slots_enabled;
        const QString slotsTz = gotCaps ? str(caps->usage.time_slots_timezone)
                                        : QString();

        const QString plan   = gotSub ? str(sub->plan_short) : QString();
        const QString status = gotSub ? str(sub->status) : QString();
        const QString pay    = gotSub ? str(sub->last_payment_status) : QString();
        const QString reason = gotSub ? str(sub->on_hold_reason) : QString();
        const bool onHold    = gotSub && sub->on_hold;
        const long since     = gotSub ? sub->started_at : 0;
        const long payDate   = gotSub ? sub->last_payment_date : 0;

        QMetaObject::invokeMethod(this, [=, this] {
            busy_ = false;

            /* --- subscription --- */
            v_plan_->setText(plan.isEmpty() ? QStringLiteral("-") : plan);
            v_status_->setText(status.isEmpty() ? QStringLiteral("-") : status);
            v_since_->setText(whenUnix(since));
            v_payment_->setText(pay.isEmpty()
                                    ? QStringLiteral("-")
                                    : tr("%1 (%2)").arg(pay, whenUnix(payDate)));
            v_hold_->setText(onHold ? (reason.isEmpty() ? tr("yes") : reason)
                                    : tr("no"));
            v_hold_->setStyleSheet(onHold ? theme::css(theme::bad(this)) : QString());

            /* --- sessions --- */
            v_session_len_->setText(hhmm(c.maxSessionLength));
            v_hard_stop_->setText(hardStop.isEmpty() ? tr("none") : whenISO(hardStop));
            v_slots_->setText(slotsOn ? tr("enabled (%1)").arg(slotsTz)
                                      : tr("disabled"));

            /* --- fair use --- */
            if (c.maxDuration > 0) {
                const double frac = double(c.fairUseUsage) / double(c.maxDuration);
                v_fair_bar_->setValue(int(qBound(0.0, frac, 1.0) * 1000));
                /* LIM1 - what is LEFT first. "41 h of 210 h used" makes the
                 * reader do the subtraction; the question being asked is how
                 * much is still there. */
                const int left = c.maxDuration - c.fairUseUsage;
                v_fair_text_->setText(
                    left > 0 ? tr("%1 left of %2 this period (%3 % used)")
                                   .arg(hhmm(left), hhmm(c.maxDuration))
                                   .arg(frac * 100.0, 0, 'f', 1)
                             : tr("the %1 for this period is used up")
                                   .arg(hhmm(c.maxDuration)));
                /* The SERVER's threshold, not ours. */
                const bool warn = c.fairUseAlert > 0.0 && frac >= c.fairUseAlert;
                v_fair_bar_->setStyleSheet(
                    warn ? QStringLiteral("QProgressBar::chunk { background: %1; }")
                               .arg(theme::warn(this).name())
                         : QString());
                v_fair_text_->setStyleSheet(warn ? theme::css(theme::warn(this))
                                                 : QString());
            } else {
                v_fair_bar_->setValue(0);
                v_fair_text_->setText(tr("the server gave no allowance"));
            }
            v_renew_->setText(c.fairUseRenew.isEmpty()
                                  ? QString()
                                  : tr("renews on %1").arg(whenISO(c.fairUseRenew)));

            /* --- entitlements --- */
            QStringList ch;
            if (c.clipboardAllowed)    ch << tr("clipboard");
            if (c.fileTransferAllowed) ch << tr("file transfer");
            if (c.gamepadAllowed)      ch << tr("gamepad");
            if (c.microAllowed)        ch << tr("microphone");
            v_channels_->setText(ch.isEmpty() ? QStringLiteral("-")
                                              : ch.join(QStringLiteral(", ")));
            /* DISP1 - the number the server enforces. Said plainly because a
             * client that cannot yet ask for a second screen should at least
             * say whether one would be allowed. */
            v_monitors_->setText(
                c.maxMonitors > 0
                    ? tr("%n allowed (this client uses one)", "", c.maxMonitors)
                    : QStringLiteral("-"));
            v_resolution_->setText(c.maxWidth > 0
                                       ? QStringLiteral("%1 x %2")
                                             .arg(c.maxWidth).arg(c.maxHeight)
                                       : QStringLiteral("-"));
            v_fps_->setText(c.maxFps > 0 ? tr("%1 Hz").arg(c.maxFps)
                                         : QStringLiteral("-"));
            v_vcodecs_->setText(c.videoCodecs.isEmpty() ? QStringLiteral("-")
                                                        : c.videoCodecs);
            v_chroma_->setText(c.videoChroma.isEmpty() ? QStringLiteral("-")
                                                       : c.videoChroma);
            v_acodecs_->setText(c.audioCodecs.isEmpty() ? QStringLiteral("-")
                                                        : c.audioCodecs);

            QStringList notes;
            if (!gotSub) notes << tr("the subscription could not be read (HTTP %1)")
                                      .arg(shttp);
            if (!gotCaps && !vm_.isEmpty())
                notes << tr("the machine's entitlements could not be read (HTTP %1)")
                             .arg(chttp);
            if (vm_.isEmpty())
                notes << tr("no machine yet, so only the subscription is shown");
            v_note_->setText(notes.join(QStringLiteral("  ")));
        }, Qt::QueuedConnection);
    });
}
