/* hud_model - see the header: the HUD's inventory, as data. */
#include "hud_model.hpp"

#include <QCoreApplication>

extern "C" {
#include "core/protocol/latency.h"
#include "core/protocol/ctrl_gamepad.h"
#include "core/media/audio.h"
#include "core/session/ctrl_session_glue.h"
}

namespace halyard {

namespace {
QString n(double v, int dec = 0) { return QString::number(v, 'f', dec); }
QString tr_h(const char *s) { return QCoreApplication::translate("Hud", s); }
}  // namespace

HudSnap hudSample(HudMeters &m, quint64 presented)
{
    HudSnap h;
    session_stats_get(&h.s);
    shadow_input_get_debug(&h.dbg);
    const int64_t now = latency_now_us();

    m.decoded.sample(h.s.h264_frames_decoded, now);
    m.presented.sample(presented, now);
    m.vbytes.sample(h.s.rtp_video_bytes, now);
    m.vpkts.sample(h.s.rtp_video_packets, now);
    m.adecoded.sample(h.s.opus_decoded, now);
    m.abytes.sample(h.s.rtp_audio_bytes, now);

    h.fpsDecoded   = m.decoded.value();
    h.fpsPresented = m.presented.value();
    h.mbps         = m.vbytes.value() * 8.0 / 1e6;
    h.pps          = m.vpkts.value();
    h.audioFps     = m.adecoded.value();
    h.audioKbps    = m.abytes.value() * 8.0 / 1e3;
    h.lossPct      = h.s.chunks_expected
                   ? 100.0 * h.s.chunks_missing / h.s.chunks_expected : 0.0;
    h.rttMs        = h.s.ctrl_rtt_us / 1000.0;
    h.presented    = presented;

    latency_report_t r;
    h.dispWaitFed = latency_read(LAT_VID_DISP_QUEUE, &r) != 0;
    h.dispWaitMs  = h.dispWaitFed ? r.p50_us / 1000.0 : 0.0;

    h.padActive  = ctrl_gamepad_active();
    h.padProbe   = ctrl_gamepad_axis_probing();
    h.codec      = QString::fromUtf8(ctrl_session_glue_codec());
    h.audioCodec = QString::fromUtf8(ctrl_session_glue_codec_audio());
    h.hw         = ctrl_session_glue_hw() != 0;
    h.volume     = audio_get_volume();
    h.eqOn       = audio_eq_active();
    return h;
}

/* Grading lives in hud_grade.hpp (pure, and tested there). */

const QVector<HudRow> &hudRows()
{
    static const QVector<HudRow> kRows = {
    /* ------------------------------------------------------- perf ------ */
    { SecPerf, QT_TRANSLATE_NOOP("Hud", "Decoded"),
      [](const HudSnap &h) { return n(h.fpsDecoded) + QStringLiteral(" fps"); },
      [](const HudSnap &h) { return gradeHi(h.fpsDecoded, 24, 50); }, nullptr },
    { SecPerf, QT_TRANSLATE_NOOP("Hud", "Presented"),
      [](const HudSnap &h) { return n(h.fpsPresented) + QStringLiteral(" fps"); },
      [](const HudSnap &h) { return gradeHi(h.fpsPresented, 24, 50); }, nullptr },
    { SecPerf, QT_TRANSLATE_NOOP("Hud", "Display wait"),
      [](const HudSnap &h) {
          return h.dispWaitFed ? n(h.dispWaitMs, 1) + QStringLiteral(" ms")
                               : tr_h("—"); },
      [](const HudSnap &h) {
          return h.dispWaitFed ? gradeLo(h.dispWaitMs, grade::kDispWaitWarnMs, grade::kDispWaitGoodMs) : Grade::Neutral; },
      nullptr },
    { SecPerf, QT_TRANSLATE_NOOP("Hud", "Frames decoded"),
      [](const HudSnap &h) { return QString::number(h.s.h264_frames_decoded); },
      nullptr, nullptr },
    { SecPerf, QT_TRANSLATE_NOOP("Hud", "Frames presented"),
      [](const HudSnap &h) { return QString::number(h.presented); },
      nullptr, nullptr },
    { SecPerf, QT_TRANSLATE_NOOP("Hud", "Decode errors"),
      [](const HudSnap &h) { return QString::number(h.s.h264_decode_errors); },
      [](const HudSnap &h) { return badIfAny(h.s.h264_decode_errors); },
      [](const HudSnap &h) { return h.s.h264_decode_errors > 0; } },
    { SecPerf, QT_TRANSLATE_NOOP("Hud", "Decoder drops"),
      [](const HudSnap &h) { return QString::number(h.s.dec_queue_dropped); },
      [](const HudSnap &h) { return warnIfAny(h.s.dec_queue_dropped); },
      [](const HudSnap &h) { return h.s.dec_queue_dropped > 0; } },
    { SecPerf, QT_TRANSLATE_NOOP("Hud", "Session"),
      [](const HudSnap &h) {
          return QStringLiteral("%1:%2").arg(h.s.session_seconds / 60)
                     .arg(h.s.session_seconds % 60, 2, 10, QLatin1Char('0')); },
      nullptr, nullptr },
    { SecPerf, QT_TRANSLATE_NOOP("Hud", "Video frozen"),
      [](const HudSnap &h) { return QStringLiteral("%1 s").arg(h.s.rtp_video_stuck_secs); },
      [](const HudSnap &) { return Grade::Bad; },
      [](const HudSnap &h) { return h.s.rtp_video_stuck_secs > 0; } },

    /* -------------------------------------------------------- net ------ */
    { SecNet, QT_TRANSLATE_NOOP("Hud", "Bitrate"),
      [](const HudSnap &h) { return n(h.mbps, 1) + QStringLiteral(" Mb/s"); },
      [](const HudSnap &h) { return gradeHi(h.mbps, 1, 4); }, nullptr },
    { SecNet, QT_TRANSLATE_NOOP("Hud", "Packets"),
      [](const HudSnap &h) { return QString::number(h.s.rtp_video_packets); },
      nullptr, nullptr },
    { SecNet, QT_TRANSLATE_NOOP("Hud", "Packet rate"),
      [](const HudSnap &h) { return n(h.pps) + QStringLiteral(" /s"); },
      nullptr, nullptr },
    { SecNet, QT_TRANSLATE_NOOP("Hud", "Loss"),
      [](const HudSnap &h) {
          return h.s.chunks_expected
              ? QStringLiteral("%1 %  (%2/%3)").arg(n(h.lossPct, 2))
                    .arg(h.s.chunks_missing).arg(h.s.chunks_expected)
              : tr_h("—"); },
      [](const HudSnap &h) {
          return h.s.chunks_expected ? gradeLo(h.lossPct, grade::kLossWarnPct, grade::kLossGoodPct) : Grade::Neutral; },
      nullptr },
    { SecNet, QT_TRANSLATE_NOOP("Hud", "Lost pictures"),
      [](const HudSnap &h) { return QString::number(h.s.chunks_orphan_lost); },
      [](const HudSnap &h) { return warnIfAny(h.s.chunks_orphan_lost); },
      [](const HudSnap &h) { return h.s.chunks_orphan_lost > 0; } },
    { SecNet, QT_TRANSLATE_NOOP("Hud", "Truncated"),
      [](const HudSnap &h) { return QString::number(h.s.frames_trunc); },
      [](const HudSnap &h) { return warnIfAny(h.s.frames_trunc); },
      [](const HudSnap &h) { return h.s.frames_trunc > 0; } },
    { SecNet, QT_TRANSLATE_NOOP("Hud", "Kernel drops"),
      [](const HudSnap &h) { return QString::number(h.s.kernel_drops); },
      [](const HudSnap &h) { return warnIfAny(h.s.kernel_drops); },
      [](const HudSnap &h) { return h.s.kernel_drops_valid && h.s.kernel_drops > 0; } },
    { SecNet, QT_TRANSLATE_NOOP("Hud", "RTT"),
      [](const HudSnap &h) {
          return h.s.ctrl_rtt_us ? n(h.rttMs, 1) + QStringLiteral(" ms") : tr_h("—"); },
      [](const HudSnap &h) {
          return h.s.ctrl_rtt_us ? gradeLo(h.rttMs, grade::kRttWarnMs, grade::kRttGoodMs) : Grade::Neutral; },
      nullptr },
    { SecNet, QT_TRANSLATE_NOOP("Hud", "RTT avg / p90 / jitter"),
      [](const HudSnap &h) {
          return QStringLiteral("%1 / %2 / %3 ms")
              .arg(n(h.s.ctrl_rtt_avg_us / 1000.0, 1))
              .arg(n(h.s.ctrl_rtt_p90_us / 1000.0, 1))
              .arg(n(h.s.ctrl_rtt_jitter_us / 1000.0, 1)); },
      nullptr,
      [](const HudSnap &h) { return h.s.ctrl_rtt_avg_us > 0; } },

    /* ------------------------------------------------------ video ------ */
    { SecVideo, QT_TRANSLATE_NOOP("Hud", "Resolution"),
      [](const HudSnap &h) {
          return h.s.h264_width > 0
              ? QStringLiteral("%1 × %2").arg(h.s.h264_width).arg(h.s.h264_height)
              : tr_h("—"); },
      nullptr, nullptr },
    { SecVideo, QT_TRANSLATE_NOOP("Hud", "Codec"),
      [](const HudSnap &h) {
          return QStringLiteral("%1 (%2)").arg(h.codec,
              h.hw ? tr_h("hardware") : tr_h("software")); },
      nullptr, nullptr },

    /* ------------------------------------------------------ audio ------ */
    { SecAudio, QT_TRANSLATE_NOOP("Hud", "Audio codec"),
      [](const HudSnap &h) { return h.audioCodec; }, nullptr, nullptr },
    { SecAudio, QT_TRANSLATE_NOOP("Hud", "Audio rate"),
      [](const HudSnap &h) { return n(h.audioFps) + QStringLiteral(" /s"); },
      nullptr, nullptr },
    { SecAudio, QT_TRANSLATE_NOOP("Hud", "Audio bitrate"),
      [](const HudSnap &h) { return n(h.audioKbps) + QStringLiteral(" kb/s"); },
      nullptr, nullptr },
    { SecAudio, QT_TRANSLATE_NOOP("Hud", "Audio lost"),
      [](const HudSnap &h) { return QString::number(h.s.opus_lost); },
      [](const HudSnap &h) { return warnIfAny(h.s.opus_lost); },
      [](const HudSnap &h) { return h.s.opus_lost > 0; } },
    { SecAudio, QT_TRANSLATE_NOOP("Hud", "Audio duplicates"),
      [](const HudSnap &h) { return QString::number(h.s.opus_dup_skipped); },
      nullptr,
      [](const HudSnap &h) { return h.s.opus_dup_skipped > 0; } },
    { SecAudio, QT_TRANSLATE_NOOP("Hud", "Audio ring full"),
      [](const HudSnap &h) { return QString::number(h.s.opus_ring_full); },
      [](const HudSnap &h) { return warnIfAny(h.s.opus_ring_full); },
      [](const HudSnap &h) { return h.s.opus_ring_full > 0; } },
    { SecAudio, QT_TRANSLATE_NOOP("Hud", "Audio errors"),
      [](const HudSnap &h) { return QString::number(h.s.opus_errors); },
      [](const HudSnap &h) { return badIfAny(h.s.opus_errors); },
      [](const HudSnap &h) { return h.s.opus_errors > 0; } },

    /* ------------------------------------------------------ input ------ */
    { SecInput, QT_TRANSLATE_NOOP("Hud", "Queue"),
      [](const HudSnap &h) { return QString::number(h.dbg.queue_depth); },
      nullptr, nullptr },
    { SecInput, QT_TRANSLATE_NOOP("Hud", "Mouse moves"),
      [](const HudSnap &h) { return QString::number(h.dbg.n_mouse_moves); },
      nullptr, nullptr },
    { SecInput, QT_TRANSLATE_NOOP("Hud", "Clicks L / R"),
      [](const HudSnap &h) {
          return QStringLiteral("%1 / %2").arg(h.dbg.n_clicks_left)
                                          .arg(h.dbg.n_clicks_right); },
      nullptr, nullptr },
    { SecInput, QT_TRANSLATE_NOOP("Hud", "Keys"),
      [](const HudSnap &h) { return QString::number(h.dbg.n_keypress); },
      nullptr, nullptr },
    { SecInput, QT_TRANSLATE_NOOP("Hud", "Last action"),
      [](const HudSnap &h) { return QString::fromUtf8(h.dbg.last_action); },
      nullptr, nullptr },

    /* --------------------------------------------------- advanced ------ */
    { SecAdvanced, QT_TRANSLATE_NOOP("Hud", "Gamepad"),
      [](const HudSnap &h) { return h.padActive ? tr_h("connected") : tr_h("absent"); },
      nullptr, nullptr },
    { SecAdvanced, QT_TRANSLATE_NOOP("Hud", "Axis probe"),
      [](const HudSnap &h) { return QStringLiteral("#%1").arg(h.padProbe); },
      [](const HudSnap &) { return Grade::Warn; },
      [](const HudSnap &h) { return h.padProbe >= 0; } },
    { SecAdvanced, QT_TRANSLATE_NOOP("Hud", "Volume"),
      [](const HudSnap &h) { return QStringLiteral("%1 %").arg(h.volume); },
      nullptr, nullptr },
    { SecAdvanced, QT_TRANSLATE_NOOP("Hud", "Equaliser"),
      [](const HudSnap &h) { return h.eqOn ? tr_h("on") : tr_h("off"); },
      nullptr, nullptr },
    };
    return kRows;
}

const QVector<HudChartSpec> &hudCharts()
{
    static const QVector<HudChartSpec> kCharts = {
        { ChDecoded,   QT_TRANSLATE_NOOP("Hud", "Decoded"),   "fps",
          [](const HudSnap &h) { return h.fpsDecoded; },   false, 24, 50 },
        { ChPresented, QT_TRANSLATE_NOOP("Hud", "Presented"), "fps",
          [](const HudSnap &h) { return h.fpsPresented; }, false, 24, 50 },
        { ChBitrate,   QT_TRANSLATE_NOOP("Hud", "Bitrate"),   "Mb/s",
          [](const HudSnap &h) { return h.mbps; },         false, 1,  4  },
        { ChPackets,   QT_TRANSLATE_NOOP("Hud", "Packets"),   "/s",
          [](const HudSnap &h) { return h.pps; },          false, 50, 200 },
        { ChLoss,      QT_TRANSLATE_NOOP("Hud", "Loss"),      "%",
          [](const HudSnap &h) { return h.lossPct; },      true,  1.0, 0.3 },
        { ChRtt,       QT_TRANSLATE_NOOP("Hud", "RTT"),       "ms",
          [](const HudSnap &h) { return h.rttMs; },        true,  80, 30 },
        { ChAudio,     QT_TRANSLATE_NOOP("Hud", "Audio"),     "/s",
          [](const HudSnap &h) { return h.audioFps; },     false, 10, 40 },
        { ChLatency,   QT_TRANSLATE_NOOP("Hud", "Display wait"), "ms",
          [](const HudSnap &h) { return h.dispWaitMs; },   true,  33, 16 },
    };
    return kCharts;
}

const char *hudSectionName(int bit)
{
    switch (bit) {
    case SecPerf:     return QT_TRANSLATE_NOOP("Hud", "Performance");
    case SecLatency:  return QT_TRANSLATE_NOOP("Hud", "Latency");
    case SecNet:      return QT_TRANSLATE_NOOP("Hud", "Network");
    case SecVideo:    return QT_TRANSLATE_NOOP("Hud", "Video");
    case SecInput:    return QT_TRANSLATE_NOOP("Hud", "Input");
    case SecAudio:    return QT_TRANSLATE_NOOP("Hud", "Audio");
    case SecAdvanced: return QT_TRANSLATE_NOOP("Hud", "Advanced");
    default:          return "";
    }
}

const char *hudChartName(int bit)
{
    for (const HudChartSpec &c : hudCharts()) if (c.bit == bit) return c.label;
    return "";
}

/* The latency stages that have been fed, one row each. Produced rather than
 * declared: which stages exist depends on the transport (the receive-loop pass
 * is silent in TCP video mode by design) and on what has run yet. */
QVector<QPair<QString, QString>> hudLatencyRows()
{
    static const char *kNames[] = {
        QT_TRANSLATE_NOOP("Hud", "Video burst"), QT_TRANSLATE_NOOP("Hud", "Picture hold"),
        QT_TRANSLATE_NOOP("Hud", "Decoder wait"), QT_TRANSLATE_NOOP("Hud", "Decode"),
        QT_TRANSLATE_NOOP("Hud", "Display wait"), QT_TRANSLATE_NOOP("Hud", "Render"),
        QT_TRANSLATE_NOOP("Hud", "Draw interval"), QT_TRANSLATE_NOOP("Hud", "End to end"),
        QT_TRANSLATE_NOOP("Hud", "Input send"), QT_TRANSLATE_NOOP("Hud", "Gamepad send"),
        QT_TRANSLATE_NOOP("Hud", "Gamepad interval"), QT_TRANSLATE_NOOP("Hud", "Audio queue"),
        QT_TRANSLATE_NOOP("Hud", "Receive pass"),
    };
    QVector<QPair<QString, QString>> out;
    for (int e = 0; e < LAT_NB && e < (int)(sizeof kNames / sizeof *kNames); e++) {
        latency_report_t r;
        const int fed = latency_read((latency_stage_t)e, &r);
        if (!fed && r.n_session == 0) continue;
        const QString v = fed
            ? QStringLiteral("%1 / %2 / %3").arg(n(r.p50_us / 1000.0, 1))
                  .arg(n(r.p99_us / 1000.0, 1)).arg(n(r.worst_session_us / 1000.0, 1))
            : QStringLiteral("— / — / %1").arg(n(r.worst_session_us / 1000.0, 1));
        out.append({ tr_h(kNames[e]), v });
    }
    return out;
}

}  // namespace halyard
