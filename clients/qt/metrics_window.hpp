/* MetricsWindow - what the session is actually doing, live, in its own window.
 *
 * === QT3 / MET1 — TWO TABS, GRANT AND LIVE =================================
 *
 * A console has one screen, so the Borealis HUD is an overlay on the stream. A
 * desktop does not: these numbers are read WHILE watching the picture, so they
 * get a second window that can sit on another monitor. It carries, in two tabs:
 *
 *   LIVE (MET1) - the counters that move: video/decode rate and bitrate, the
 *   loss fraction and the control-channel RTT, the audio rates and drops, the
 *   input counters, and the per-stage latency percentiles. This is the Borealis
 *   HUD's content (its perf/net/video/audio/input/latence sections), rebuilt by
 *   polling the SAME core snapshot it reads (`session_stats_get`, `latency_read`)
 *   and deriving rates with the SAME helper (`RateMeter`, ported in
 *   rate_meter.hpp).
 *
 *   SESSION - the grant snapshot (`ctrl_session_caps`): what the SERVER decided
 *   per channel - transport, absolute port, resolution and codec AS GRANTED.
 *   Asking is not getting (KB §3.37), and this is the only place the difference
 *   shows.
 *
 * Polled, not pushed: a push would mean a signal per counter change, which is
 * per packet. Every getter it calls is documented thread-safe for a reader off
 * the session threads (stats.h, latency.h, session_caps.h). Polling stops when
 * the window is hidden - these numbers interest only someone looking at them.
 */
#pragma once

#include <QWidget>

#include "rate_meter.hpp"

class QLabel;
class QTableWidget;
class QTimer;

class MetricsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MetricsWindow(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;

private slots:
    void refresh();

private:
    void build();          /* (re)creates the whole UI; also the retranslate path */
    void refreshLive();
    void refreshGrant();

    QTimer *timer_ = nullptr;

    /* ---- the live tab's value labels, set in build(), filled in refresh ---- */
    /* Performance */
    QLabel *v_decoded_ = nullptr, *v_dec_total_ = nullptr,
           *v_session_ = nullptr, *v_freeze_ = nullptr;
    /* Network */
    QLabel *v_bitrate_ = nullptr, *v_vpkts_ = nullptr, *v_prate_ = nullptr,
           *v_loss_ = nullptr, *v_orphan_ = nullptr, *v_trunc_ = nullptr,
           *v_kernel_ = nullptr, *v_rtt_ = nullptr, *v_rtt_spread_ = nullptr;
    /* Video */
    QLabel *v_res_ = nullptr, *v_codec_ = nullptr, *v_decerr_ = nullptr,
           *v_dropped_ = nullptr;
    /* Audio */
    QLabel *v_acodec_ = nullptr, *v_arate_ = nullptr, *v_abitrate_ = nullptr,
           *v_alost_ = nullptr, *v_adup_ = nullptr, *v_aring_ = nullptr,
           *v_aerr_ = nullptr;
    /* Input */
    QLabel *v_moves_ = nullptr, *v_clicks_ = nullptr, *v_keys_ = nullptr,
           *v_queue_ = nullptr, *v_last_ = nullptr, *v_pad_ = nullptr;
    /* Latency table */
    QTableWidget *latency_ = nullptr;
    /* When no session: a single line shown instead of stale zeros. */
    QLabel *live_idle_ = nullptr;
    QWidget *live_body_ = nullptr;

    /* ---- the grant tab ---- */
    QLabel       *summary_  = nullptr;
    QTableWidget *channels_ = nullptr;

    /* ---- derived rates (MET1), fed from the cumulative core counters ---- */
    halyard::RateMeter rm_decoded_, rm_vbytes_, rm_vpkts_, rm_adecoded_, rm_abytes_;
};
