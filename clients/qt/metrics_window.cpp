/* MetricsWindow - see the header: a live HUD tab + the grant snapshot tab. */
#include "metrics_window.hpp"

#include <QtConcurrent>
#include <QPushButton>

#include "theme.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

extern "C" {
#include "core/services/netpath.h"
#include "core/services/stats.h"
#include "core/protocol/latency.h"
#include "core/protocol/session_caps.h"
#include "core/protocol/ctrl_gamepad.h"
#include "core/session/ctrl_session_glue.h"
#include "core/input/shadow_input.h"
}

namespace theme = halyard::theme;

namespace {

/* A friendly label per latency stage, in the enum's order. The enum's own
 * `latency_stage_name` returns the log token (parsed by tooling); this is for a
 * person. Indexed by latency_stage_t. */
const char *stageLabel(int e)
{
    switch (e) {
    case LAT_VID_BURST:     return QT_TRANSLATE_NOOP("Metrics", "Video burst (UDP chunks)");
    case LAT_VID_HOLD:      return QT_TRANSLATE_NOOP("Metrics", "Picture hold");
    case LAT_VID_DEC_QUEUE: return QT_TRANSLATE_NOOP("Metrics", "Wait for the decoder");
    case LAT_VID_DECODE:    return QT_TRANSLATE_NOOP("Metrics", "Decode");
    case LAT_VID_DISP_QUEUE:return QT_TRANSLATE_NOOP("Metrics", "Display wait");
    case LAT_VID_UPLOAD:    return QT_TRANSLATE_NOOP("Metrics", "Render / upload");
    case LAT_VID_CADENCE:   return QT_TRANSLATE_NOOP("Metrics", "Draw interval");
    case LAT_VID_E2E:       return QT_TRANSLATE_NOOP("Metrics", "End to end (variation)");
    case LAT_IN_SEND:       return QT_TRANSLATE_NOOP("Metrics", "Input send");
    case LAT_IN_PAD:        return QT_TRANSLATE_NOOP("Metrics", "Gamepad send");
    case LAT_IN_PAD_RATE:   return QT_TRANSLATE_NOOP("Metrics", "Gamepad read interval");
    case LAT_AUD_QUEUE:     return QT_TRANSLATE_NOOP("Metrics", "Audio queue depth");
    case LAT_RX_PASS:       return QT_TRANSLATE_NOOP("Metrics", "Receive-loop pass");
    default:                return "";
    }
}

QString tr_m(const char *s) { return QCoreApplication::translate("Metrics", s); }

}  // namespace

MetricsWindow::MetricsWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowIcon(theme::appTileIcon());
    resize(560, 680);
    timer_ = new QTimer(this);
    timer_->setInterval(500);   /* MET1 - the Borealis HUD's cadence */
    connect(timer_, &QTimer::timeout, this, &MetricsWindow::refresh);
    build();
}

/* A value label added to a form, with a muted caption on the left. */
static QLabel *addRow(QFormLayout *form, const QString &caption, QWidget *parent)
{
    auto *v = new QLabel(QStringLiteral("—"), parent);
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *cap = new QLabel(caption, parent);
    cap->setStyleSheet(theme::css(theme::muted(parent)));
    form->addRow(cap, v);
    return v;
}

void MetricsWindow::build()
{
    setWindowTitle(tr("Halyard metrics"));

    /* Tear down a previous build (language switch). */
    if (auto *old = layout()) {
        QLayoutItem *it;
        while ((it = old->takeAt(0))) { delete it->widget(); delete it; }
        delete old;
    }

    auto *tabs = new QTabWidget(this);

    /* ================================================= the LIVE tab ===== */
    auto *liveTab = new QWidget(tabs);
    auto *liveOuter = new QVBoxLayout(liveTab);
    liveOuter->setContentsMargins(0, 0, 0, 0);

    live_idle_ = new QLabel(
        tr("No session is streaming yet. The live counters appear once a "
           "stream is running."), liveTab);
    live_idle_->setWordWrap(true);
    live_idle_->setStyleSheet(theme::css(theme::muted(liveTab)));
    liveOuter->addWidget(live_idle_);

    auto *scroll = new QScrollArea(liveTab);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    live_body_ = new QWidget(scroll);
    auto *col = new QVBoxLayout(live_body_);
    col->setContentsMargins(theme::SpaceRow, theme::SpaceRow,
                            theme::SpaceRow, theme::SpaceRow);
    col->setSpacing(theme::SpaceGroup);

    auto group = [&](const QString &title) {
        auto *g = new QGroupBox(title, live_body_);
        auto *f = new QFormLayout(g);
        f->setLabelAlignment(Qt::AlignLeft);
        f->setHorizontalSpacing(theme::SpaceGroup);
        col->addWidget(g);
        return f;
    };

    QFormLayout *f;
    f = group(tr("Performance"));
    v_decoded_   = addRow(f, tr("Decoded frame rate"), live_body_);
    v_dec_total_ = addRow(f, tr("Frames decoded"), live_body_);
    v_session_   = addRow(f, tr("Session time"), live_body_);
    v_freeze_    = addRow(f, tr("Video freeze"), live_body_);

    f = group(tr("Network"));
    v_bitrate_    = addRow(f, tr("Video bitrate"), live_body_);
    v_vpkts_      = addRow(f, tr("Video packets"), live_body_);
    v_prate_      = addRow(f, tr("Packet rate"), live_body_);
    v_loss_       = addRow(f, tr("Chunk loss"), live_body_);
    v_orphan_     = addRow(f, tr("Lost pictures (orphan)"), live_body_);
    v_trunc_      = addRow(f, tr("Truncated pictures"), live_body_);
    v_kernel_     = addRow(f, tr("Kernel drops"), live_body_);
    v_rtt_        = addRow(f, tr("Control RTT"), live_body_);
    v_rtt_spread_ = addRow(f, tr("RTT avg / p90 / jitter"), live_body_);

    /* NET1 - the path, as the official client draws it: this machine, the
     * router, then everything else lumped together. Only the local hop is
     * measured; the rest is the session's round trip minus it, which is not a
     * traceroute and does not claim to be. */
    v_path_ = addRow(f, tr("Path (you / router / Shadow)"), live_body_);
    v_path_age_ = addRow(f, tr("Path measured"), live_body_);
    {
        auto *btn = new QPushButton(tr("Re-run the test"), live_body_);
        connect(btn, &QPushButton::clicked, this, &MetricsWindow::probeNetworkPath);
        f->addRow(QString(), btn);
    }

    f = group(tr("Video"));
    v_res_     = addRow(f, tr("Resolution"), live_body_);
    v_codec_   = addRow(f, tr("Codec"), live_body_);
    v_decerr_  = addRow(f, tr("Decode errors"), live_body_);
    v_dropped_ = addRow(f, tr("Decoder drops"), live_body_);

    f = group(tr("Audio"));
    v_acodec_   = addRow(f, tr("Codec"), live_body_);
    v_arate_    = addRow(f, tr("Frame rate"), live_body_);
    v_abitrate_ = addRow(f, tr("Bitrate"), live_body_);
    v_alost_    = addRow(f, tr("Lost"), live_body_);
    v_adup_     = addRow(f, tr("Duplicates dropped"), live_body_);
    v_aring_    = addRow(f, tr("Ring overflow"), live_body_);
    v_aerr_     = addRow(f, tr("Errors"), live_body_);

    f = group(tr("Input"));
    v_moves_  = addRow(f, tr("Mouse moves"), live_body_);
    v_clicks_ = addRow(f, tr("Clicks (L / R)"), live_body_);
    v_keys_   = addRow(f, tr("Keys"), live_body_);
    v_queue_  = addRow(f, tr("Queue depth"), live_body_);
    v_last_   = addRow(f, tr("Last action"), live_body_);
    v_pad_    = addRow(f, tr("Gamepad"), live_body_);

    auto *latGroup = new QGroupBox(tr("Latency (last 10 s window, ms)"), live_body_);
    auto *latLay = new QVBoxLayout(latGroup);
    latency_ = new QTableWidget(0, 5, latGroup);
    latency_->setHorizontalHeaderLabels(
        { tr("Stage"), QStringLiteral("p50"), QStringLiteral("p90"),
          QStringLiteral("p99"), tr("max") });
    latency_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    latency_->verticalHeader()->setVisible(false);
    latency_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    latency_->setSelectionMode(QAbstractItemView::NoSelection);
    latLay->addWidget(latency_);
    col->addWidget(latGroup);
    col->addStretch(1);

    scroll->setWidget(live_body_);
    liveOuter->addWidget(scroll, 1);
    tabs->addTab(liveTab, tr("Live"));

    /* ================================================ the SESSION tab ==== */
    auto *grantTab = new QWidget(tabs);
    auto *gl = new QVBoxLayout(grantTab);
    summary_ = new QLabel(grantTab);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_->setWordWrap(true);
    channels_ = new QTableWidget(0, 4, grantTab);
    channels_->setHorizontalHeaderLabels(
        { tr("Channel"), tr("Granted"), tr("Transport"), tr("Port") });
    channels_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    channels_->verticalHeader()->setVisible(false);
    channels_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    channels_->setSelectionMode(QAbstractItemView::NoSelection);
    gl->addWidget(summary_);
    gl->addWidget(channels_, 1);
    tabs->addTab(grantTab, tr("Session"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(theme::SpaceRow, theme::SpaceRow,
                             theme::SpaceRow, theme::SpaceRow);
    root->addWidget(tabs);

    refresh();
}

void MetricsWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange) build();
    QWidget::changeEvent(e);
}

void MetricsWindow::showEvent(QShowEvent *e)
{
    /* NET1 - measure on open, then every two minutes. Not on the metrics
     * tick: a ping a second would be a packet storm aimed at the user's own
     * router to answer a question whose answer changes with the wiring. */
    if (!path_timer_) {
        path_timer_ = new QTimer(this);
        path_timer_->setInterval(120000);
        connect(path_timer_, &QTimer::timeout, this,
                &MetricsWindow::probeNetworkPath);
    }
    path_timer_->start();
    probeNetworkPath();

    QWidget::showEvent(e);
    refresh();
    timer_->start();
}

void MetricsWindow::hideEvent(QHideEvent *e)
{
    if (path_timer_) path_timer_->stop();

    timer_->stop();
    QWidget::hideEvent(e);
}

void MetricsWindow::refresh()
{
    refreshLive();
    refreshGrant();
}

/* ======================================================== the live tab === */

/* NET1 - the probe, on a worker, with its result posted back.
 *
 * `QtConcurrent::run` and not a QThread: this is one blocking call with no
 * state, which is exactly the shape that pool is for. The guard is not
 * paranoia - the timer and the button can both fire, and two pings in flight
 * would race to write the same labels. */
void MetricsWindow::probeNetworkPath()
{
    if (path_running_) return;
    path_running_ = true;
    v_path_age_->setText(tr("measuring..."));

    const int64_t total = []() -> int64_t {
        session_stats_t st;
        session_stats_get(&st);
        return (int64_t)st.ctrl_rtt_us;
    }();

    (void)QtConcurrent::run([this, total] {
        netpath_split sp;
        netpath_measure(total, 1200, &sp);
        /* Queued: the lambda runs on a pool thread and these are widgets. */
        QMetaObject::invokeMethod(this, [this, sp] {
            path_running_ = false;
            path_age_.start();

            auto ms = [](int64_t us) {
                return us < 0 ? QStringLiteral("?")
                              : QStringLiteral("%1").arg(us / 1000.0, 0, 'f', 1);
            };
            if (!sp.have_local) {
                /* Said plainly rather than shown as a dash: a gateway that
                 * drops ICMP is the common case on a corporate network, and
                 * "unknown" with no reason reads as a bug in us. */
                v_path_->setText(tr("%1 ms total - the router did not answer, "
                                    "so the split is unavailable")
                                     .arg(ms(sp.total_us)));
            } else if (!sp.ordered) {
                v_path_->setText(tr("you \u2192 %1 ms \u2192 router \u2192 ? \u2192 Shadow "
                                    "(the local hop measured longer than the "
                                    "whole round trip)")
                                     .arg(ms(sp.local_us)));
            } else {
                v_path_->setText(tr("you \u2192 %1 ms \u2192 router (%2) \u2192 %3 ms \u2192 Shadow")
                                     .arg(ms(sp.local_us),
                                          QString::fromUtf8(sp.gateway),
                                          ms(sp.remote_us)));
            }
            v_path_age_->setText(tr("just now"));
        }, Qt::QueuedConnection);
    });
}

void MetricsWindow::refreshLive()
{
    shadow_session_caps caps;
    const bool have = ctrl_session_caps(&caps);
    live_idle_->setVisible(!have);
    live_body_->setVisible(have);
    if (!have) return;

    session_stats_t s;
    session_stats_get(&s);
    const int64_t now = latency_now_us();

    rm_decoded_.sample(s.h264_frames_decoded, now);
    rm_vbytes_.sample(s.rtp_video_bytes, now);
    rm_vpkts_.sample(s.rtp_video_packets, now);
    rm_adecoded_.sample(s.opus_decoded, now);
    rm_abytes_.sample(s.rtp_audio_bytes, now);

    /* Performance */
    v_decoded_->setText(QStringLiteral("%1 fps").arg(rm_decoded_.value(), 0, 'f', 1));
    v_dec_total_->setText(QString::number(s.h264_frames_decoded));
    v_session_->setText(QStringLiteral("%1:%2")
                            .arg(s.session_seconds / 60)
                            .arg(s.session_seconds % 60, 2, 10, QLatin1Char('0')));
    v_freeze_->setText(s.rtp_video_stuck_secs > 0
                           ? tr("%1 s without a frame").arg(s.rtp_video_stuck_secs)
                           : tr("live"));

    /* NET1 - how old the split is. Shown because a two-minute-old reading of
     * a network that has since changed is worse than none, and the official
     * client says it too ("Mesure il y a 2 minutes"). */
    if (v_path_age_ && path_age_.isValid() && !path_running_) {
        const qint64 sec = path_age_.elapsed() / 1000;
        v_path_age_->setText(sec < 10 ? tr("just now")
                                      : tr("%n second(s) ago", "", int(sec)));
    }

    /* Network */
    v_bitrate_->setText(QStringLiteral("%1 Mb/s")
                            .arg(rm_vbytes_.value() * 8.0 / 1e6, 0, 'f', 1));
    v_vpkts_->setText(QString::number(s.rtp_video_packets));
    v_prate_->setText(QStringLiteral("%1 /s").arg(rm_vpkts_.value(), 0, 'f', 0));
    if (s.chunks_expected > 0)
        v_loss_->setText(QStringLiteral("%1 %  (%2 / %3)")
                             .arg(100.0 * s.chunks_missing / s.chunks_expected, 0, 'f', 2)
                             .arg(s.chunks_missing).arg(s.chunks_expected));
    else
        v_loss_->setText(QStringLiteral("—"));
    v_orphan_->setText(QString::number(s.chunks_orphan_lost));
    v_trunc_->setText(QString::number(s.frames_trunc));
    v_kernel_->setText(s.kernel_drops_valid ? QString::number(s.kernel_drops)
                                            : tr("n/a on this platform"));
    v_rtt_->setText(s.ctrl_rtt_us > 0
                        ? QStringLiteral("%1 ms").arg(s.ctrl_rtt_us / 1000.0, 0, 'f', 1)
                        : QStringLiteral("—"));
    v_rtt_spread_->setText(QStringLiteral("%1 / %2 / %3 ms")
                               .arg(s.ctrl_rtt_avg_us / 1000.0, 0, 'f', 1)
                               .arg(s.ctrl_rtt_p90_us / 1000.0, 0, 'f', 1)
                               .arg(s.ctrl_rtt_jitter_us / 1000.0, 0, 'f', 1));

    /* Video */
    v_res_->setText(s.h264_width > 0
                        ? QStringLiteral("%1 × %2").arg(s.h264_width).arg(s.h264_height)
                        : QStringLiteral("—"));
    v_codec_->setText(QStringLiteral("%1  (%2)")
                          .arg(QString::fromUtf8(ctrl_session_glue_codec()),
                               ctrl_session_glue_hw() ? tr("hardware") : tr("software")));
    v_decerr_->setText(QString::number(s.h264_decode_errors));
    v_dropped_->setText(QString::number(s.dec_queue_dropped));

    /* Audio */
    v_acodec_->setText(QString::fromUtf8(ctrl_session_glue_codec_audio()));
    v_arate_->setText(QStringLiteral("%1 /s").arg(rm_adecoded_.value(), 0, 'f', 0));
    v_abitrate_->setText(QStringLiteral("%1 kb/s")
                             .arg(rm_abytes_.value() * 8.0 / 1e3, 0, 'f', 0));
    v_alost_->setText(QString::number(s.opus_lost));
    v_adup_->setText(QString::number(s.opus_dup_skipped));
    v_aring_->setText(QString::number(s.opus_ring_full));
    v_aerr_->setText(QString::number(s.opus_errors));

    /* Input */
    shadow_input_debug dbg;
    shadow_input_get_debug(&dbg);
    v_moves_->setText(QString::number(dbg.n_mouse_moves));
    v_clicks_->setText(QStringLiteral("%1 / %2").arg(dbg.n_clicks_left).arg(dbg.n_clicks_right));
    v_keys_->setText(QString::number(dbg.n_keypress));
    v_queue_->setText(QString::number(dbg.queue_depth));
    v_last_->setText(QString::fromUtf8(dbg.last_action));
    v_pad_->setText(ctrl_gamepad_active() ? tr("connected") : tr("absent"));

    /* Latency: one row per stage ever fed, with the last window's percentiles. */
    int rows = 0;
    for (int e = 0; e < LAT_NB; e++) {
        latency_report_t r;
        const int fed = latency_read((latency_stage_t)e, &r);
        if (!fed && r.n_session == 0) continue;   /* never fed: skip */
        if (latency_->rowCount() <= rows) latency_->insertRow(rows);
        auto put = [&](int c, const QString &t) {
            auto *it = latency_->item(rows, c);
            if (!it) { it = new QTableWidgetItem; latency_->setItem(rows, c, it); }
            it->setText(t);
        };
        put(0, tr_m(stageLabel(e)));
        if (!fed) {
            put(1, QStringLiteral("—")); put(2, QStringLiteral("—"));
            put(3, QStringLiteral("—"));
            put(4, QStringLiteral("%1").arg(r.worst_session_us / 1000.0, 0, 'f', 1));
        } else {
            put(1, QStringLiteral("%1").arg(r.p50_us / 1000.0, 0, 'f', 1));
            put(2, QStringLiteral("%1").arg(r.p90_us / 1000.0, 0, 'f', 1));
            put(3, QStringLiteral("%1").arg(r.p99_us / 1000.0, 0, 'f', 1));
            put(4, QStringLiteral("%1").arg(r.worst_session_us / 1000.0, 0, 'f', 1));
        }
        rows++;
    }
    latency_->setRowCount(rows);
    if (!latency_enabled() && rows == 0)
        latency_->setRowCount(0);
}

/* ======================================================= the grant tab === */

void MetricsWindow::refreshGrant()
{
    shadow_session_caps c;
    if (!ctrl_session_caps(&c)) {
        summary_->setText(tr("No session has completed its bootstrap yet.\n\n"
                             "The grant snapshot appears once the server has "
                             "answered the eight channel announcements."));
        channels_->setRowCount(0);
        return;
    }

    summary_->setText(
        tr("Session %1   ·   server %2.%3.%4   ·   port base %5\n"
           "%6 of 8 channels granted\n\n"
           "Video, AS GRANTED: %7×%8 @ %9 fps, codec %10, %11 Mb/s\n"
           "Audio, AS GRANTED: %12 Hz, %13 bits, codec %14\n\n"
           "\"As granted\" and not \"as asked\": the server restates these and "
           "may not honour what was requested (KB §3.37).")
            .arg(c.generation)
            .arg(c.srv_major).arg(c.srv_minor).arg(c.srv_patch)
            .arg(c.port_base)
            .arg(c.n_granted)
            .arg(c.video_width).arg(c.video_height)
            .arg(QString::number(c.video_fps, 'f', 2))
            .arg(c.video_codec == 0 ? QStringLiteral("H.264")
                 : c.video_codec == 1 ? QStringLiteral("H.265")
                 : c.video_codec == 2 ? QStringLiteral("AV1")
                                      : QString::number(c.video_codec))
            .arg(c.video_bitrate_bps / 1000000)
            .arg(c.audio_sample_rate).arg(c.audio_bits)
            .arg(c.audio_codec == 1 ? QStringLiteral("Opus")
                 : c.audio_codec == 2 ? QStringLiteral("FLAC")
                                      : QString::number(c.audio_codec)));

    static const char *kNames[8] = {
        QT_TR_NOOP("video"), QT_TR_NOOP("cursor"), QT_TR_NOOP("input"),
        QT_TR_NOOP("audio"), QT_TR_NOOP("controller"), QT_TR_NOOP("clipboard"),
        QT_TR_NOOP("microphone"), QT_TR_NOOP("file transfer"),
    };
    channels_->setRowCount(8);
    for (int i = 0; i < 8; i++) {
        const shadow_chan_caps &ch = c.chan[i];
        channels_->setItem(i, 0, new QTableWidgetItem(tr(kNames[i])));
        channels_->setItem(i, 1, new QTableWidgetItem(ch.granted ? tr("yes") : tr("no")));
        channels_->setItem(i, 2, new QTableWidgetItem(
            ch.granted ? (ch.tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"))
                       : QStringLiteral("—")));
        channels_->setItem(i, 3, new QTableWidgetItem(
            ch.granted ? QString::number(ch.port) : QStringLiteral("—")));
    }
}
