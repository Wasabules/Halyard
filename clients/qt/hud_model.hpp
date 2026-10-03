/* hud_model - WHAT the stream HUD shows, as data, separate from the widgets.
 *
 * === OV5 2026-10-03 — PARITY WITH THE BOREALIS PANEL =======================
 *
 * The first Qt HUD showed six one-line numbers. The Borealis panel
 * (`stream_view.cpp::setupHud`) shows SEVEN SECTIONS and SEVEN CHARTS, each row
 * graded, each selectable, the whole selection persisted as a bitmask. This
 * file carries the same inventory so the two clients say the same thing, and so
 * a row is a line in a table rather than a line in a paint routine.
 *
 * The sections and their contents follow Borealis's, mapped onto what core
 * publishes to a client (`session_stats_get`, `latency_read`,
 * `shadow_input_get_debug`, the glue's codec getters). Where Borealis reads a
 * number only its own draw loop can know - its render cadence, its "frames
 * received" before the view - this client substitutes the equivalent it can
 * measure honestly (the frames the video widget has presented) rather than
 * inventing a second counter that would mean something else.
 *
 * GRADING is Borealis's, value for value: the same reading must carry the same
 * colour in both clients, or "it is orange here and green there" becomes a bug
 * report about the protocol.
 *
 * A row is {section, label, text(snapshot), grade(snapshot)}; a chart is
 * {bit, label, value(snapshot), how to colour it}. The widget layer builds
 * whatever the masks select and asks for text once per tick.
 */
#pragma once

#include <QString>
#include <QVector>

#include <functional>

#include "rate_meter.hpp"

extern "C" {
#include "core/services/stats.h"
#include "core/input/shadow_input.h"
}

namespace halyard {

/* The seven sections, as a persisted bitmask. The bit index is part of the
 * stored value: inserting one in the middle silently re-maps every saved
 * selection, so new sections go on the end. */
enum HudSection {
    SecPerf     = 1 << 0,
    SecLatency  = 1 << 1,
    SecNet      = 1 << 2,
    SecVideo    = 1 << 3,
    SecInput    = 1 << 4,
    SecAudio    = 1 << 5,
    SecAdvanced = 1 << 6,
    SecDefault  = SecPerf | SecNet,          /* what a first run shows */
};

/* The charts, same rule. */
enum HudChart {
    ChDecoded   = 1 << 0,
    ChPresented = 1 << 1,
    ChBitrate   = 1 << 2,
    ChPackets   = 1 << 3,
    ChLoss      = 1 << 4,
    ChRtt       = 1 << 5,
    ChAudio     = 1 << 6,
    ChLatency   = 1 << 7,
    ChDefault   = ChDecoded | ChBitrate,
};

enum class Grade { Neutral, Good, Warn, Bad };

/* The meters that turn core's cumulative counters into live rates. Owned by the
 * widget (GUI thread only) and handed to hudSample each tick. */
struct HudMeters {
    RateMeter decoded, presented, vbytes, vpkts, adecoded, abytes;
};

/* One tick's reading of everything the rows and charts can need. Taken in one
 * place so every row of a tick describes the SAME instant - rows that each
 * polled core separately would disagree with each other by a frame. */
struct HudSnap {
    session_stats_t s{};
    shadow_input_debug dbg{};

    double fpsDecoded = 0, fpsPresented = 0;
    double mbps = 0, pps = 0;
    double audioFps = 0, audioKbps = 0;
    double lossPct = 0, rttMs = 0;
    double dispWaitMs = 0;      /* LAT_VID_DISP_QUEUE p50, the "display wait" */
    bool   dispWaitFed = false;

    quint64 presented = 0;
    bool    padActive = false;
    int     padProbe = -1;
    QString codec, audioCodec;
    bool    hw = false;
    uint32_t volume = 100;
    bool    eqOn = false;
};

/* Fills a snapshot. `presented` is the video widget's own count of pictures put
 * on screen - the one number core cannot know. */
HudSnap hudSample(HudMeters &m, quint64 presented);

struct HudRow {
    int          section;
    const char  *label;                               /* QT_TR_NOOP'd */
    std::function<QString(const HudSnap &)> text;
    std::function<Grade(const HudSnap &)>   grade;    /* may be null = Neutral */
    std::function<bool(const HudSnap &)>    visible;  /* null = always */
};

struct HudChartSpec {
    int          bit;
    const char  *label;
    const char  *unit;
    std::function<double(const HudSnap &)> value;
    bool         inverted;        /* true when LOWER is better */
    double       warn, good;
};

/* The full inventories. The widget filters by mask; keeping them whole here is
 * what lets the configuration UI list every row that exists. */
const QVector<HudRow>       &hudRows();
const QVector<HudChartSpec> &hudCharts();

/* Section and chart names for the configuration UI, by bit. */
const char *hudSectionName(int bit);
const char *hudChartName(int bit);

/* The latency section is special: its rows are one per FED stage, discovered at
 * runtime, so it is produced rather than declared. Returns {label, "p50 / p99 /
 * max"} pairs. */
QVector<QPair<QString, QString>> hudLatencyRows();

}  // namespace halyard
