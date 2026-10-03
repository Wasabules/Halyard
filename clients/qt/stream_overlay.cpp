/* stream_overlay - see the header for the two-window design and why. */
#include "stream_overlay.hpp"

#include "theme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QCoreApplication>
#include <QFrame>
#include <QHash>
#include <QMouseEvent>
#include <QToolButton>
#include <QPainter>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include "settings_store.hpp"

#include <cstdlib>

extern "C" {
#include "core/media/audio.h"
#include "core/input/shadow_input.h"
#include "core/services/env_override.h"
#include "core/services/win_compat.h"
#include "core/protocol/eq.h"
#include "core/services/stats.h"
#include "core/protocol/latency.h"
}

namespace halyard {

/* ============================================================ the HUD ===== */

namespace {
/* Borealis's constants, same meaning. Snapping is an INTENT: at 6 px two blocks
 * practically have to overlap, so tidying them side by side does not silently
 * join them. Edge magnetism is wider - aiming at an edge is not as demanding. */
constexpr int kSnapPx   = 6;
/* OV8 - how near a drop has to land to join. Snapping now ALIGNS the block
 * against its neighbour, so it is a deliberate gesture with a visible result;
 * a slightly wider catch than the 6 px seam makes it comfortable without
 * joining things that were merely tidied side by side. */
constexpr int kSnapGrabPx = 18;
constexpr int kMagnetPx = 26;
/* Pull a member this far and it leaves its group. */
constexpr int kDetachPx = 70;
constexpr int kMargin   = 14;
}  // namespace

/* === OV10 2026-10-03 — NO WindowStaysOnTopHint, AND WHY ====================
 *
 * The HUD and the overlay menu used to carry `WindowStaysOnTopHint`. That hint
 * means topmost to the WINDOW MANAGER, i.e. above every non-topmost window on
 * the desktop - including OUR OWN other windows. The file manager, the metrics
 * window and the settings are separate top-levels, so the HUD painted over
 * them whenever they overlapped the video (reported 2026-10-03).
 *
 * The hint was never what kept the HUD above the picture. These windows are
 * PARENTED to the main window and are `Qt::Tool`, which already puts them
 * above their owner and above the video widget's native child surface - the
 * thing that actually had to be beaten. Dropping the hint leaves that intact
 * and restores normal ordering between our own windows: click the file
 * manager and it comes forward, as any window should. */
StreamHud::StreamHud(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool |
                      Qt::WindowTransparentForInput)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    timer_ = new QTimer(this);
    timer_->setInterval(refreshMs_);
    connect(timer_, &QTimer::timeout, this, &StreamHud::refresh);
    rebuild();
}

void StreamHud::setMasks(int sections, int charts)
{
    if (sections == secMask_ && charts == chartMask_) return;
    secMask_ = sections;
    chartMask_ = charts;
    rebuild();
    refresh();
}

void StreamHud::setRefreshMs(int ms)
{
    refreshMs_ = qBound(100, ms, 5000);
    timer_->setInterval(refreshMs_);
}

void StreamHud::setScalePercent(int p)
{
    scalePct_ = qBound(60, p, 200);
    applyStyle();
    layoutBlocks();
}

void StreamHud::setOpacityPercent(int p)
{
    opacityPct_ = qBound(20, p, 100);
    applyStyle();
}

void StreamHud::applyStyle()
{
    const int fs = qRound(11.0 * scalePct_ / 100.0);
    const int alpha = qRound(185.0 * opacityPct_ / 100.0);
    /* OV8 - the card paints NOTHING: the rounded "bubble" behind it does, and
     * it is drawn per GROUP, so two joined blocks read as one panel instead of
     * two cards with a seam. Keeping a background on the card as well would
     * double the alpha exactly where they overlap. */
    /* One placeholder, one argument. It used to read `font-size:%2px` while
     * being given (alpha, fs): QString::arg fills the LOWEST-numbered
     * placeholder present, so %2 took the alpha and every label rendered at
     * 185 px. The alpha is not a placeholder at all - it is the bubble's, and
     * the bubble is painted, not styled. */
    const QString css = QStringLiteral(
        "QFrame#blk { background:transparent; }"
        "QLabel { color:#e8e8e8; font-size:%1px; }").arg(fs);
    bubbleAlpha_ = alpha;
    for (const Blk &b : blocks_) if (b.card) b.card->setStyleSheet(css);
}

/* Edit mode needs input; normal mode must never take any. The flag is part of
 * the native window, so it is set and the window re-shown. */
void StreamHud::setEditing(bool on)
{
    if (editing_ == on) return;
    editing_ = on;
    const bool wasVisible = isVisible();
    setWindowFlag(Qt::WindowTransparentForInput, !on);
    if (wasVisible) show();
    for (const Blk &b : blocks_)
        if (b.card) b.card->setCursor(on ? Qt::OpenHandCursor : Qt::ArrowCursor);
    updateDetachButtons();
    layoutBlocks();
    update();
}

/* --------------------------------------------------------------- build --- */

void StreamHud::rebuild()
{
    /* Keep the anchors of blocks that survive the mask change. */
    QHash<QString, QPair<double, QPair<double, QString>>> keep;
    for (const Blk &b : blocks_) keep.insert(b.id, { b.ax, { b.ay, b.leader } });
    for (const Blk &b : blocks_) delete b.card;
    blocks_.clear();

    auto newCard = [this](const QString &id, const QString &title) {
        Blk b;
        b.id = id;
        b.card = new QFrame(this);
        b.card->setObjectName(QStringLiteral("blk"));
        auto *v = new QVBoxLayout(b.card);
        v->setContentsMargins(10, 7, 10, 7);
        v->setSpacing(2);
        auto *head = new QWidget(b.card);
        auto *hl = new QHBoxLayout(head);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(6);
        auto *t = new QLabel(title, head);
        t->setStyleSheet(QStringLiteral("color:#9fb4c8; font-weight:bold;"));
        /* OV10 - a VISIBLE way out of a group. Right-click worked but nobody
         * can discover it; this button appears in edit mode on a block that is
         * part of a group, and nowhere else. */
        b.detach = new QToolButton(head);
        b.detach->setText(QStringLiteral("✕"));
        b.detach->setToolTip(tr("Detach this block"));
        b.detach->setAutoRaise(true);
        b.detach->setVisible(false);
        b.detach->setFixedSize(16, 16);
        connect(b.detach, &QToolButton::clicked, this, [this, id] {
            const int i = blockIndex(id);
            if (i < 0) return;
            blocks_[i].leader.clear();
            updateDetachButtons();
            emit layoutChanged();
            update();
        });
        hl->addWidget(t);
        hl->addStretch(1);
        hl->addWidget(b.detach);
        v->addWidget(head);
        b.body = v;
        b.card->installEventFilter(this);
        return b;
    };

    static const int kSections[] = { SecPerf, SecLatency, SecNet, SecVideo,
                                     SecInput, SecAudio, SecAdvanced };
    int n = 0;
    for (int sec : kSections) {
        if (!(secMask_ & sec)) continue;
        Blk b = newCard(QStringLiteral("sec%1").arg(sec),
                        QCoreApplication::translate("Hud", hudSectionName(sec)));
        b.section = sec;
        if (sec == SecLatency) {
            auto *host = new QWidget(b.card);
            b.latLay = new QVBoxLayout(host);
            b.latLay->setContentsMargins(0, 0, 0, 0);
            b.latLay->setSpacing(1);
            b.body->addWidget(host);
        } else {
            for (const HudRow &spec : hudRows()) {
                if (spec.section != sec) continue;
                RowW r;
                r.spec = &spec;
                r.host = new QWidget(b.card);
                auto *h = new QHBoxLayout(r.host);
                h->setContentsMargins(0, 0, 0, 0);
                h->setSpacing(10);
                r.name  = new QLabel(QCoreApplication::translate("Hud", spec.label), r.host);
                r.value = new QLabel(QStringLiteral("—"), r.host);
                r.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                h->addWidget(r.name);
                h->addStretch(1);
                h->addWidget(r.value);
                b.body->addWidget(r.host);
                b.rows.append(r);
            }
        }
        b.ax = 1.0; b.ay = qMin(0.92, 0.07 * n++);
        blocks_.append(b);
    }

    for (const HudChartSpec &spec : hudCharts()) {
        if (!(chartMask_ & spec.bit)) continue;
        Blk b = newCard(QStringLiteral("ch%1").arg(spec.bit),
                        QCoreApplication::translate("Hud", spec.label));
        b.chartBit = spec.bit;
        auto *host = new QWidget(b.card);
        auto *h = new QHBoxLayout(host);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        b.chart.spec  = &spec;
        b.chart.value = new QLabel(QStringLiteral("—"), host);
        b.chart.value->setMinimumWidth(62);
        b.chart.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        b.chart.spark = new Sparkline(host);
        b.chart.spark->configure(spec.inverted, spec.warn, spec.good);
        h->addWidget(b.chart.value);
        h->addWidget(b.chart.spark, 1);
        b.body->addWidget(host);
        b.ax = 1.0; b.ay = qMin(0.92, 0.07 * n++);
        blocks_.append(b);
    }

    /* Put back what we knew about the ones that are still here. */
    for (Blk &b : blocks_) {
        auto it = keep.find(b.id);
        if (it != keep.end()) {
            b.ax = it->first; b.ay = it->second.first; b.leader = it->second.second;
        }
    }
    /* OV11 - SHOW THE NEW CARDS.
     *
     * A child created while its parent is already visible stays hidden until it
     * is shown: Qt only cascades visibility when the PARENT is shown. The first
     * build happened in the constructor, before the window appeared, so it
     * worked by accident; a rebuild triggered by toggling a metric created the
     * blocks hidden, and every reading vanished for good - there was no later
     * show() to rescue them. */
    for (Blk &b : blocks_) if (b.card) b.card->show();

    applyStyle();
    updateDetachButtons();
    layoutBlocks();
}

bool StreamHud::isLeader(const QString &id) const
{
    for (const Blk &b : blocks_) if (b.leader == id) return true;
    return false;
}

/* The ✕ shows only while editing, and only on a block that is in a group - a
 * standalone block has nothing to detach from. */
void StreamHud::updateDetachButtons()
{
    for (const Blk &b : blocks_)
        if (b.detach) b.detach->setVisible(editing_ && !b.leader.isEmpty());
}

/* ------------------------------------------------------------- layout --- */

QRect StreamHud::usableRect(const QSize &bs) const
{
    /* The area minus a margin, minus the block's own size: an anchor of 1 then
     * lands the block AT the margin rather than off the edge. */
    const int w = qMax(0, width()  - 2 * kMargin - bs.width());
    const int h = qMax(0, height() - 2 * kMargin - bs.height());
    return QRect(kMargin, kMargin, w, h);
}

void StreamHud::layoutBlocks()
{
    for (Blk &b : blocks_) {
        if (!b.card) continue;
        b.card->adjustSize();
        const QRect u = usableRect(b.card->size());
        b.card->move(u.x() + qRound(b.ax * u.width()),
                     u.y() + qRound(b.ay * u.height()));
    }
    /* OV8 - a translucent top-level does not clear itself: without this the
     * pixels a block used to cover stay on screen and a drag leaves a trail. */
    update();
}

void StreamHud::placeOver(const QRect &r)
{
    lastRect_ = r;
    setGeometry(r);          /* the HUD IS the video area now */
    layoutBlocks();
}

int StreamHud::blockIndex(const QString &id) const
{
    for (int i = 0; i < blocks_.size(); i++) if (blocks_[i].id == id) return i;
    return -1;
}

void StreamHud::moveGroup(const QString &leaderId, double dax, double day)
{
    for (Blk &b : blocks_)
        if (b.id == leaderId || b.leader == leaderId) {
            b.ax = qBound(0.0, b.ax + dax, 1.0);
            b.ay = qBound(0.0, b.ay + day, 1.0);
        }
}

/* On release: snap to a neighbour if the drop was close enough, else magnetise
 * to an edge. Both are intents the user expressed by letting go where they did. */
void StreamHud::snapOrMagnetise(int idx)
{
    Blk &b = blocks_[idx];
    const QRect me = b.card->geometry();

    /* Nearest neighbour whose rectangle we are touching or overlapping. */
    int best = -1; int bestDist = kSnapGrabPx + 1;
    for (int j = 0; j < blocks_.size(); j++) {
        if (j == idx) continue;
        const QRect o = blocks_[j].card->geometry();
        const int dx = qMax(0, qMax(o.left() - me.right(), me.left() - o.right()));
        const int dy = qMax(0, qMax(o.top() - me.bottom(), me.top() - o.bottom()));
        const int d = qMax(dx, dy);
        if (d <= kSnapGrabPx && d < bestDist) { bestDist = d; best = j; }
    }
    if (best >= 0) {
        const QRect o = blocks_[best].card->geometry();
        /* OV8 - ALIGN it, do not merely record the link: a "joined" pair that
         * still overlaps is what made the result look broken. The side chosen
         * is the one the block was dropped nearest to, and the other axis is
         * aligned so the two edges line up exactly. */
        const int dLeft   = qAbs(me.right()  - o.left());
        const int dRight  = qAbs(o.right()   - me.left());
        const int dTop    = qAbs(me.bottom() - o.top());
        const int dBottom = qAbs(o.bottom()  - me.top());
        const int m = qMin(qMin(dLeft, dRight), qMin(dTop, dBottom));
        QPoint p = me.topLeft();
        if (m == dBottom)      p = QPoint(o.left(), o.bottom() + 1);   /* under */
        else if (m == dTop)    p = QPoint(o.left(), o.top() - me.height() - 1);
        else if (m == dRight)  p = QPoint(o.right() + 1, o.top());
        else                   p = QPoint(o.left() - me.width() - 1, o.top());

        const QRect u = usableRect(b.card->size());
        b.ax = u.width()  ? qBound(0.0, double(p.x() - u.x()) / u.width(),  1.0) : 0.0;
        b.ay = u.height() ? qBound(0.0, double(p.y() - u.y()) / u.height(), 1.0) : 0.0;

        const QString lead = blocks_[best].leader.isEmpty() ? blocks_[best].id
                                                            : blocks_[best].leader;
        if (lead != b.id) b.leader = lead;
        return;
    }
    /* Edge magnetism. */
    const QRect u = usableRect(b.card->size());
    QPoint p = me.topLeft();
    if (p.x() - u.left()  < kMagnetPx) p.setX(u.left());
    if (u.right() - p.x() < kMagnetPx) p.setX(u.right());
    if (p.y() - u.top()   < kMagnetPx) p.setY(u.top());
    if (u.bottom() - p.y() < kMagnetPx) p.setY(u.bottom());
    b.ax = u.width()  ? qBound(0.0, double(p.x() - u.x()) / u.width(),  1.0) : 0.0;
    b.ay = u.height() ? qBound(0.0, double(p.y() - u.y()) / u.height(), 1.0) : 0.0;
}

/* ---------------------------------------------------------- edit input --- */

bool StreamHud::eventFilter(QObject *o, QEvent *e)
{
    if (!editing_) return QWidget::eventFilter(o, e);
    int idx = -1;
    for (int i = 0; i < blocks_.size(); i++) if (blocks_[i].card == o) { idx = i; break; }
    if (idx < 0) return QWidget::eventFilter(o, e);

    switch (e->type()) {
    case QEvent::MouseButtonPress: {
        auto *m = static_cast<QMouseEvent *>(e);
        if (m->button() == Qt::RightButton) {
            /* Detach from the group, in place - the way back from a snap. */
            blocks_[idx].leader.clear();
            updateDetachButtons();
            emit layoutChanged();
            update();
            return true;
        }
        if (m->button() == Qt::LeftButton) {
            dragIdx_ = idx;
            dragStart_ = m->globalPosition().toPoint();
            dragOrigin_ = blocks_[idx].card->pos();
            dragDetached_ = false;
            blocks_[idx].card->setCursor(Qt::ClosedHandCursor);
            return true;
        }
        break;
    }
    case QEvent::MouseMove: {
        if (dragIdx_ != idx) break;
        auto *m = static_cast<QMouseEvent *>(e);
        const QPoint d = m->globalPosition().toPoint() - dragStart_;
        /* OV10 - PULL IT OUT. Dragging a member more than a block's width away
         * means "I want this one on its own", which is the gesture people try
         * first; from that moment the drag moves it alone. */
        if (!dragDetached_ && !blocks_[idx].leader.isEmpty()
            && (qAbs(d.x()) > kDetachPx || qAbs(d.y()) > kDetachPx)) {
            blocks_[idx].leader.clear();
            dragDetached_ = true;
            updateDetachButtons();
        }
        const QRect u = usableRect(blocks_[idx].card->size());
        const QPoint want = dragOrigin_ + d;
        const double nax = u.width()  ? double(want.x() - u.x()) / u.width()  : 0.0;
        const double nay = u.height() ? double(want.y() - u.y()) / u.height() : 0.0;
        /* Dragging any member moves the whole group. */
        const QString lead = blocks_[idx].leader.isEmpty() ? blocks_[idx].id
                                                           : blocks_[idx].leader;
        moveGroup(lead, qBound(0.0, nax, 1.0) - blocks_[idx].ax,
                        qBound(0.0, nay, 1.0) - blocks_[idx].ay);
        layoutBlocks();
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (dragIdx_ != idx) break;
        dragIdx_ = -1;
        blocks_[idx].card->setCursor(Qt::OpenHandCursor);
        snapOrMagnetise(idx);
        updateDetachButtons();
        layoutBlocks();
        emit layoutChanged();
        return true;
    }
    default: break;
    }
    return QWidget::eventFilter(o, e);
}

/* In edit mode the window paints a faint wash and a hint, so it is obvious the
 * HUD is grabbing the mouse and how to get out of it. */
void StreamHud::paintEvent(QPaintEvent *e)
{
    QWidget::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    /* OV8 - CLEAR FIRST, always. A translucent top-level keeps whatever was
     * last composited where nothing paints, so a moved block left a copy of
     * itself behind - the "trail". CompositionMode_Source writes the
     * transparency instead of blending into it. */
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    if (editing_) p.fillRect(rect(), QColor(0, 0, 0, 70));

    /* OV10 - ONE BUBBLE PER BLOCK, not per group.
     *
     * The union of a group's rectangles was a giant translucent box with the
     * content stranded in its corners the moment two members were not side by
     * side - reported, and right. Each block gets its own rounded rectangle,
     * inflated a little; because a snap now aligns blocks edge to edge, the
     * inflated rectangles OVERLAP and a joined set still reads as one panel,
     * while blocks that are merely linked but apart look like what they are. */
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(12, 12, 14, bubbleAlpha_));
    for (const Blk &b : blocks_) {
        if (!b.card || !b.card->isVisible()) continue;
        p.drawRoundedRect(b.card->geometry().adjusted(-6, -5, 6, 5), 9, 9);
    }

    if (!editing_) return;

    /* Edit mode: outline each bubble, and say what the gestures are. */
    p.setBrush(Qt::NoBrush);
    for (const Blk &b : blocks_) {
        if (!b.card || !b.card->isVisible()) continue;
        const bool grouped = !b.leader.isEmpty() || isLeader(b.id);
        p.setPen(QPen(QColor(0x7f, 0xb0, 0xff, grouped ? 220 : 130),
                      grouped ? 2 : 1));
        p.drawRoundedRect(b.card->geometry().adjusted(-6, -5, 9, 9), 9, 9);
    }
    p.setPen(QColor(0xff, 0xff, 0xff, 215));
    p.drawText(rect().adjusted(0, 12, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
               tr("Drag a block to move it · drop it against another to join "
                  "them · pull it away (or use ✕) to detach"));
}

/* ---------------------------------------------------------- persistence --- */

QString StreamHud::layoutString() const
{
    QStringList out;
    for (const Blk &b : blocks_)
        out << QStringLiteral("%1:%2,%3,%4").arg(b.id)
                .arg(b.ax, 0, 'f', 4).arg(b.ay, 0, 'f', 4).arg(b.leader);
    return out.join(QLatin1Char(';'));
}

void StreamHud::setLayoutString(const QString &s)
{
    for (const QString &part : s.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const int c = part.indexOf(QLatin1Char(':'));
        if (c <= 0) continue;
        const QString id = part.left(c);
        const QStringList f = part.mid(c + 1).split(QLatin1Char(','));
        if (f.size() < 2) continue;
        const int i = blockIndex(id);
        if (i < 0) continue;
        blocks_[i].ax = qBound(0.0, f[0].toDouble(), 1.0);
        blocks_[i].ay = qBound(0.0, f[1].toDouble(), 1.0);
        blocks_[i].leader = f.size() > 2 ? f[2] : QString();
    }
    layoutBlocks();
}

void StreamHud::resetLayout()
{
    int n = 0;
    for (Blk &b : blocks_) {
        b.leader.clear();
        b.ax = 1.0;
        b.ay = qMin(0.92, 0.07 * n++);
    }
    updateDetachButtons();
    layoutBlocks();
    emit layoutChanged();
}

/* -------------------------------------------------------------- refresh --- */

void StreamHud::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refresh();
    timer_->start();
}

void StreamHud::hideEvent(QHideEvent *e)
{
    timer_->stop();
    QWidget::hideEvent(e);
}

void StreamHud::refresh()
{
    const quint64 presented = presented_ ? presented_() : 0;
    const HudSnap h = hudSample(meters_, presented);

    for (Blk &b : blocks_) {
        for (const RowW &r : b.rows) {
            const bool vis = !r.spec->visible || r.spec->visible(h);
            r.host->setVisible(vis);
            if (!vis) continue;
            r.value->setText(r.spec->text(h));
            const Grade g = r.spec->grade ? r.spec->grade(h) : Grade::Neutral;
            QColor c;
            switch (g) {
            case Grade::Good: c = theme::good(this); break;
            case Grade::Warn: c = theme::warn(this); break;
            case Grade::Bad:  c = theme::bad(this);  break;
            default:          c = QColor(0xe8, 0xe8, 0xe8); break;
            }
            r.value->setStyleSheet(QStringLiteral("color:%1;").arg(c.name()));
        }
        if (b.chart.spec) {
            const double v = b.chart.spec->value(h);
            b.chart.value->setText(QStringLiteral("%1 %2")
                .arg(v, 0, 'f', v < 10 ? 1 : 0)
                .arg(QString::fromUtf8(b.chart.spec->unit)));
            b.chart.spark->push(v);
        }
        if (b.latLay) {
            const auto rows = hudLatencyRows();
            while (b.latLay->count() > rows.size()) {
                QLayoutItem *it = b.latLay->takeAt(b.latLay->count() - 1);
                delete it->widget(); delete it;
            }
            for (int i = 0; i < rows.size(); i++) {
                QWidget *w = (i < b.latLay->count()) ? b.latLay->itemAt(i)->widget() : nullptr;
                QLabel *nm, *vl;
                if (!w) {
                    w = new QWidget(b.card);
                    auto *hl = new QHBoxLayout(w);
                    hl->setContentsMargins(0, 0, 0, 0);
                    hl->setSpacing(10);
                    nm = new QLabel(w);
                    vl = new QLabel(w);
                    vl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    hl->addWidget(nm); hl->addStretch(1); hl->addWidget(vl);
                    b.latLay->addWidget(w);
                } else {
                    const auto ls = w->findChildren<QLabel *>();
                    if (ls.size() < 2) continue;
                    nm = ls[0]; vl = ls[1];
                }
                nm->setText(rows[i].first);
                vl->setText(rows[i].second);
            }
        }
    }
    layoutBlocks();   /* content changes size; anchors keep the placement */
}

/* ========================================================= the menu ======= */

StreamOverlay::StreamOverlay(QWidget *parent)
    /* OV10 - no `WindowStaysOnTopHint`: see the note above StreamHud. */
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool)
{
    setAttribute(Qt::WA_TranslucentBackground);

    /* A rounded dark card so it reads as an overlay, not a window. */
    auto *card = new QWidget(this);
    card->setObjectName(QStringLiteral("card"));
    card->setStyleSheet(QStringLiteral(
        "#card { background:rgba(22,22,26,238); border-radius:12px; }"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(card);

    auto *col = new QVBoxLayout(card);
    col->setContentsMargins(theme::SpaceGroup, theme::SpaceGroup,
                            theme::SpaceGroup, theme::SpaceRow);
    col->setSpacing(theme::SpaceRow);

    /* OV2 - tabs, because one column of every control was a scroll nobody would
     * read mid-game. The order is how often you reach for them. */
    tabs_ = new QTabWidget(card);
    tabs_->addTab(buildSoundTab(),  QString());
    tabs_->addTab(buildVideoTab(),  QString());
    tabs_->addTab(buildInputTab(),  QString());
    tabs_->addTab(buildToolsTab(),  QString());
    tabs_->addTab(buildHudTab(),    QString());
    col->addWidget(tabs_);

    auto *actions = new QHBoxLayout;
    fsBtn_    = new QPushButton(card);
    discBtn_  = new QPushButton(card);
    closeBtn_ = new QPushButton(card);
    actions->addWidget(fsBtn_);
    actions->addWidget(discBtn_);
    actions->addStretch(1);
    actions->addWidget(closeBtn_);
    col->addLayout(actions);
    connect(fsBtn_, &QPushButton::clicked, this,
            [this] { emit requestFullscreenToggle(); });
    connect(discBtn_, &QPushButton::clicked, this,
            [this] { emit requestDisconnect(); hide(); });
    connect(closeBtn_, &QPushButton::clicked, this, [this] { hide(); });

    retranslate();
    setFixedWidth(640);
}

/* === OV2 - writing a SHADOW_* from the overlay ============================
 *
 * The same rules as the settings window: the environment wins (a variable set
 * from outside is refused rather than silently ignored), the value is persisted
 * so it survives a restart, and the toast says whether it applies now or at the
 * next session - a control that looks inert is the defect this avoids. */
void StreamOverlay::writeEnv(const QString &env, const QString &value, bool live)
{
    const QByteArray key = env.toUtf8();
    if (env_override_active(key.constData()) != 0) {
        emit toast(tr("%1 is set outside the application - not changed.").arg(env));
        return;
    }
    if (value.isEmpty()) unsetenv(key.constData());
    else { const QByteArray v = value.toUtf8(); setenv(key.constData(), v.constData(), 1); }
    halyard::store::saveVariable(env, value);
    emit toast(live ? tr("Applied.") : tr("Applied — takes effect on the next session."));
}

/* ------------------------------------------------------------ Sound ------ */

QWidget *StreamOverlay::buildSoundTab()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);
    col->setSpacing(theme::SpaceRow);

    auto *vrow = new QHBoxLayout;
    volume_ = new QSlider(Qt::Horizontal, w);
    volume_->setRange(0, 300);
    volumeVal_ = new QLabel(w);
    volumeVal_->setMinimumWidth(52);
    muteBtn_ = new QPushButton(w);
    muteBtn_->setCheckable(true);
    titleAudio_ = new QLabel(w);
    vrow->addWidget(titleAudio_);
    vrow->addWidget(volume_, 1);
    vrow->addWidget(volumeVal_);
    vrow->addWidget(muteBtn_);
    col->addLayout(vrow);
    connect(volume_, &QSlider::valueChanged, this, [this](int v) {
        audio_set_volume((uint32_t)v);
        volumeVal_->setText(QStringLiteral("%1 %").arg(v));
        if (v > 0) { QSignalBlocker b(muteBtn_); muteBtn_->setChecked(false); }
    });
    connect(muteBtn_, &QPushButton::toggled, this, [this](bool on) {
        if (on) { premute_ = volume_->value(); volume_->setValue(0); }
        else    { volume_->setValue(premute_ > 0 ? premute_ : 100); }
    });

    /* --- equaliser --------------------------------------------------- */
    gEq_ = new QGroupBox(w);
    auto *eqLay = new QVBoxLayout(gEq_);
    auto *eqHead = new QHBoxLayout;
    eqOn_ = new QCheckBox(gEq_);
    eqAutoTrim_ = new QCheckBox(gEq_);
    eqPreset_ = new QComboBox(gEq_);
    eqHead->addWidget(eqOn_);
    eqHead->addWidget(eqAutoTrim_);
    eqHead->addStretch(1);
    eqHead->addWidget(eqPreset_);
    eqLay->addLayout(eqHead);

    eqCurve_ = new EqCurve(gEq_);
    eqLay->addWidget(eqCurve_);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(theme::SpaceRow);
    const char *const typeKeys[] = { QT_TR_NOOP("Off"), QT_TR_NOOP("High-pass"),
        QT_TR_NOOP("Low shelf"), QT_TR_NOOP("Peak"), QT_TR_NOOP("High shelf"),
        QT_TR_NOOP("Low-pass") };
    const char *const colKeys[] = { QT_TR_NOOP("Band"), QT_TR_NOOP("Type"),
        QT_TR_NOOP("Freq (Hz)"), QT_TR_NOOP("Q"), QT_TR_NOOP("Gain (dB)") };
    for (int c = 0; c < 5; c++) {
        auto *h = new QLabel(tr(colKeys[c]), gEq_);
        h->setStyleSheet(theme::css(theme::muted(gEq_)));
        grid->addWidget(h, 0, c);
    }
    const float defFreq[5] = { 80, 250, 1000, 4000, 12000 };
    for (int b = 0; b < 5; b++) {
        grid->addWidget(new QLabel(QString::number(b + 1), gEq_), b + 1, 0);
        bands_[b].type = new QComboBox(gEq_);
        for (const char *k : typeKeys) bands_[b].type->addItem(tr(k));
        bands_[b].freq = new QDoubleSpinBox(gEq_);
        bands_[b].freq->setRange(20, 20000); bands_[b].freq->setValue(defFreq[b]);
        bands_[b].freq->setDecimals(0);
        bands_[b].q = new QDoubleSpinBox(gEq_);
        bands_[b].q->setRange(0.1, 10.0); bands_[b].q->setValue(0.707);
        bands_[b].q->setSingleStep(0.1); bands_[b].q->setDecimals(2);
        bands_[b].gain = new QDoubleSpinBox(gEq_);
        bands_[b].gain->setRange(-18, 18); bands_[b].gain->setDecimals(1);
        bands_[b].gain->setSingleStep(0.5);
        grid->addWidget(bands_[b].type, b + 1, 1);
        grid->addWidget(bands_[b].freq, b + 1, 2);
        grid->addWidget(bands_[b].q,    b + 1, 3);
        grid->addWidget(bands_[b].gain, b + 1, 4);
    }
    eqLay->addLayout(grid);
    col->addWidget(gEq_);
    col->addStretch(1);

    auto applyNow = [this] { applyEq(); };
    connect(eqOn_, &QCheckBox::toggled, this, applyNow);
    connect(eqAutoTrim_, &QCheckBox::toggled, this, applyNow);
    for (int b = 0; b < 5; b++) {
        connect(bands_[b].type, &QComboBox::currentIndexChanged, this, applyNow);
        connect(bands_[b].freq, &QDoubleSpinBox::valueChanged, this, applyNow);
        connect(bands_[b].q,    &QDoubleSpinBox::valueChanged, this, applyNow);
        connect(bands_[b].gain, &QDoubleSpinBox::valueChanged, this, applyNow);
    }
    connect(eqPreset_, &QComboBox::activated, this, [this](int i) {
        auto set = [this](int b, eq_type_t t, double f, double q, double g) {
            QSignalBlocker b1(bands_[b].type), b2(bands_[b].freq),
                           b3(bands_[b].q), b4(bands_[b].gain);
            bands_[b].type->setCurrentIndex((int)t);
            bands_[b].freq->setValue(f); bands_[b].q->setValue(q);
            bands_[b].gain->setValue(g);
        };
        const double keep[5] = { 80, 250, 1000, 4000, 12000 };
        for (int b = 0; b < 5; b++) set(b, EQ_OFF, keep[b], 0.707, 0);
        if (i == 1)      { set(0, EQ_LOWSHELF, 100, 0.707, 6); }
        else if (i == 2) { set(0, EQ_HIGHPASS, 120, 0.707, 0);
                           set(2, EQ_PEAK, 2500, 1.2, 4); }
        else if (i == 3) { set(4, EQ_HIGHSHELF, 8000, 0.707, 5); }
        { QSignalBlocker b(eqOn_); eqOn_->setChecked(i != 0); }
        applyEq();
    });
    return w;
}

/* ------------------------------------------------------------ Video ------ */

QWidget *StreamOverlay::buildVideoTab()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    form->setHorizontalSpacing(theme::SpaceGroup);

    /* A combo bound to a SHADOW_* variable: `values[i]` is written for choice i,
     * an empty entry unsets (the "Automatic" position). */
    auto combo = [&](const QString &caption, const char *env,
                     const QStringList &labels, const QStringList &values,
                     bool live) {
        auto *c = new QComboBox(w);
        c->addItems(labels);
        const char *cur = std::getenv(env);
        const QString cs = cur ? QString::fromUtf8(cur) : QString();
        int idx = values.indexOf(cs);
        if (idx < 0) idx = values.indexOf(QString());
        c->setCurrentIndex(idx >= 0 ? idx : 0);
        const QString e = QString::fromUtf8(env);
        connect(c, &QComboBox::currentIndexChanged, this,
                [this, e, values, live](int i) {
                    if (i >= 0 && i < values.size()) writeEnv(e, values[i], live);
                });
        form->addRow(caption, c);
        return c;
    };
    auto check = [&](const QString &caption, const char *env, bool defOn,
                     bool live, const QString &onVal = QStringLiteral("1"),
                     const QString &offVal = QStringLiteral("0")) {
        auto *c = new QCheckBox(w);
        const char *cur = std::getenv(env);
        c->setChecked(cur ? (QString::fromUtf8(cur) == onVal) : defOn);
        const QString e = QString::fromUtf8(env);
        connect(c, &QCheckBox::toggled, this, [this, e, onVal, offVal, live](bool on) {
            writeEnv(e, on ? onVal : offVal, live);
        });
        form->addRow(caption, c);
        return c;
    };

    combo(tr("Codec"), "SHADOW_CODEC",
          { QStringLiteral("H.264"), QStringLiteral("HEVC"), QStringLiteral("AV1") },
          { QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2") }, false);
    combo(tr("Bitrate"), "SHADOW_BITRATE_MBPS",
          { tr("Automatic"), QStringLiteral("5"), QStringLiteral("10"),
            QStringLiteral("20"), QStringLiteral("30"), QStringLiteral("50"),
            QStringLiteral("75"), QStringLiteral("100") },
          { QString(), QStringLiteral("5"), QStringLiteral("10"),
            QStringLiteral("20"), QStringLiteral("30"), QStringLiteral("50"),
            QStringLiteral("75"), QStringLiteral("100") }, false);
    combo(tr("Frame rate"), "SHADOW_FPS",
          { tr("Automatic"), QStringLiteral("30"), QStringLiteral("60"),
            QStringLiteral("90"), QStringLiteral("120"), QStringLiteral("144") },
          { QString(), QStringLiteral("30"), QStringLiteral("60"),
            QStringLiteral("90"), QStringLiteral("120"), QStringLiteral("144") }, false);
    combo(tr("Hardware decoding"), "SHADOW_HWACCEL",
          { tr("Automatic"), tr("Off"), tr("On") },
          { QString(), QStringLiteral("0"), QStringLiteral("1") }, false);
    check(tr("Full colour (4:4:4)"), "SHADOW_REG_F5", false, false);
    check(tr("Request HDR"), "SHADOW_HDR", false, false, QStringLiteral("1"), QString());
    check(tr("Adapt the bitrate to losses"), "SHADOW_ADAPT_BITRATE", true, false);
    check(tr("Audio only (no video)"), "SHADOW_GUI_NOVIDEO", false, false,
          QStringLiteral("1"), QString());

    auto *note = new QLabel(tr("Most video settings are read when a session "
                               "starts, so they apply the next time you connect."), w);
    note->setWordWrap(true);
    note->setStyleSheet(theme::css(theme::muted(w)));
    form->addRow(note);
    return w;
}

/* ------------------------------------------------------------ Input ------ */

QWidget *StreamOverlay::buildInputTab()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(theme::SpaceGroup);
    col->addLayout(form);

    /* Pointer mode - SHADOW_INPUT_ABS, absent = follow the platform. */
    {
        auto *c = new QComboBox(w);
        c->addItems({ tr("Automatic"), tr("Always relative"), tr("Always absolute") });
        const QStringList vals = { QString(), QStringLiteral("0"), QStringLiteral("1") };
        const char *cur = std::getenv("SHADOW_INPUT_ABS");
        int idx = vals.indexOf(cur ? QString::fromUtf8(cur) : QString());
        c->setCurrentIndex(idx < 0 ? 0 : idx);
        connect(c, &QComboBox::currentIndexChanged, this, [this, vals](int i) {
            if (i >= 0 && i < vals.size())
                writeEnv(QStringLiteral("SHADOW_INPUT_ABS"), vals[i], false);
        });
        form->addRow(tr("Pointer mode"), c);
    }
    /* OV6 - which pointer is shown: nothing, the VM's own bitmap (sent on
     * :base+20 and used as the real mouse cursor), or this desktop's arrow. */
    {
        auto *c = new QComboBox(w);
        c->addItems({ tr("None"), tr("The VM's pointer"), tr("Local pointer") });
        c->setCurrentIndex(2);
        connect(c, &QComboBox::currentIndexChanged, this,
                [this](int i) { emit cursorSourceChanged(i); });
        form->addRow(tr("Pointer shown"), c);
        cursorSrc_ = c;
    }
    /* Rumble - read per packet by core, so it really is live. */
    {
        auto *row = new QHBoxLayout;
        auto *s = new QSlider(Qt::Horizontal, w);
        s->setRange(0, 100);
        const char *cur = std::getenv("SHADOW_RUMBLE_PCT");
        s->setValue(cur ? QString::fromUtf8(cur).toInt() : 100);
        auto *val = new QLabel(QStringLiteral("%1 %").arg(s->value()), w);
        row->addWidget(s, 1); row->addWidget(val);
        connect(s, &QSlider::valueChanged, this, [this, val](int v) {
            val->setText(QStringLiteral("%1 %").arg(v));
            writeEnv(QStringLiteral("SHADOW_RUMBLE_PCT"), QString::number(v), true);
        });
        form->addRow(tr("Rumble intensity"), row);
    }
    {
        auto *c = new QComboBox(w);
        c->addItems({ QStringLiteral("Xbox 360"), QStringLiteral("Xbox One"),
                      QStringLiteral("DualShock 4"), tr("Joy-Con (left)"),
                      tr("Joy-Con (right)"), tr("Generic") });
        const char *cur = std::getenv("SHADOW_GAMEPAD_TYPE");
        c->setCurrentIndex(cur ? QString::fromUtf8(cur).toInt() : 0);
        connect(c, &QComboBox::currentIndexChanged, this, [this](int i) {
            writeEnv(QStringLiteral("SHADOW_GAMEPAD_TYPE"), QString::number(i), false);
        });
        form->addRow(tr("Controller type"), c);
    }
    {
        auto *c = new QCheckBox(w);
        const char *cur = std::getenv("SHADOW_KBD_ETENDU");
        c->setChecked(cur ? QString::fromUtf8(cur) != QStringLiteral("0") : true);
        connect(c, &QCheckBox::toggled, this, [this](bool on) {
            writeEnv(QStringLiteral("SHADOW_KBD_ETENDU"),
                     on ? QStringLiteral("1") : QStringLiteral("0"), false);
        });
        form->addRow(tr("Extended keys"), c);
    }

    /* Special combinations the local OS would otherwise eat, sent as evdev
     * scancodes straight down core's input queue. */
    gCombos_ = new QGroupBox(w);
    auto *cl = new QHBoxLayout(gCombos_);
    struct Combo { const char *label; int mods[2]; int key; };
    static const Combo kCombos[] = {
        { QT_TR_NOOP("Ctrl+Alt+Del"), { 29, 56 }, 111 },
        { QT_TR_NOOP("Windows"),      { 0, 0 },   125 },
        { QT_TR_NOOP("Alt+F4"),       { 56, 0 },  62  },
        { QT_TR_NOOP("Alt+Tab"),      { 56, 0 },  15  },
        { QT_TR_NOOP("Esc"),          { 0, 0 },   1   },
    };
    for (const Combo &c : kCombos) {
        auto *b = new QPushButton(tr(c.label), gCombos_);
        const int m0 = c.mods[0], m1 = c.mods[1], k = c.key;
        connect(b, &QPushButton::clicked, this, [this, m0, m1, k] {
            if (m0) shadow_input_post_scancode((uint16_t)m0, true);
            if (m1) shadow_input_post_scancode((uint16_t)m1, true);
            shadow_input_post_scancode((uint16_t)k, true);
            shadow_input_post_scancode((uint16_t)k, false);
            if (m1) shadow_input_post_scancode((uint16_t)m1, false);
            if (m0) shadow_input_post_scancode((uint16_t)m0, false);
            emit toast(tr("Sent to the VM."));
        });
        cl->addWidget(b);
    }
    col->addWidget(gCombos_);
    col->addStretch(1);
    return w;
}

/* ------------------------------------------------------------ Tools ------ */

QWidget *StreamOverlay::buildToolsTab()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);

    auto *form = new QFormLayout;
    {
        auto *c = new QComboBox(w);
        c->addItems({ tr("Off"), tr("Both ways"), tr("This PC to the VM only"),
                      tr("The VM to this PC only") });
        const char *cur = std::getenv("SHADOW_CLIPBOARD");
        c->setCurrentIndex(cur ? QString::fromUtf8(cur).toInt() : 1);
        connect(c, &QComboBox::currentIndexChanged, this, [this](int i) {
            writeEnv(QStringLiteral("SHADOW_CLIPBOARD"), QString::number(i), true);
        });
        form->addRow(tr("Clipboard sharing"), c);
    }
    col->addLayout(form);

    auto *row1 = new QHBoxLayout;
    shotBtn_  = new QPushButton(w);
    filesBtn_ = new QPushButton(w);
    metBtn_   = new QPushButton(w);
    setBtn_   = new QPushButton(w);
    diagBtn_  = new QPushButton(w);
    for (QPushButton *b : { shotBtn_, filesBtn_, metBtn_ }) row1->addWidget(b);
    auto *row2 = new QHBoxLayout;
    for (QPushButton *b : { setBtn_, diagBtn_ }) row2->addWidget(b);
    row2->addStretch(1);
    col->addLayout(row1);
    col->addLayout(row2);
    col->addStretch(1);

    connect(shotBtn_,  &QPushButton::clicked, this, [this] { emit requestScreenshot(); });
    connect(filesBtn_, &QPushButton::clicked, this, [this] { emit requestFiles(); });
    connect(metBtn_,   &QPushButton::clicked, this, [this] { emit requestMetrics(); });
    connect(setBtn_,   &QPushButton::clicked, this, [this] { emit requestSettings(); });
    connect(diagBtn_,  &QPushButton::clicked, this, [this] { emit requestCopyDiagnostics(); });
    return w;
}

/* -------------------------------------------------------------- HUD ------ */

QWidget *StreamOverlay::buildHudTab()
{
    auto *w = new QWidget(this);
    auto *root = new QVBoxLayout(w);

    static const int kSecs[7]   = { SecPerf, SecLatency, SecNet, SecVideo,
                                    SecInput, SecAudio, SecAdvanced };
    static const int kCharts[8] = { ChDecoded, ChPresented, ChBitrate, ChPackets,
                                    ChLoss, ChRtt, ChAudio, ChLatency };

    auto emitMasks = [this] {
        static const int secs[7] = { SecPerf, SecLatency, SecNet, SecVideo,
                                     SecInput, SecAudio, SecAdvanced };
        static const int chs[8]  = { ChDecoded, ChPresented, ChBitrate, ChPackets,
                                     ChLoss, ChRtt, ChAudio, ChLatency };
        int sec = 0, ch = 0;
        for (int i = 0; i < 7; i++)
            if (secBox_[i] && secBox_[i]->isChecked()) sec |= secs[i];
        for (int i = 0; i < 8; i++)
            if (chartBox_[i] && chartBox_[i]->isChecked()) ch |= chs[i];
        emit hudMasksChanged(sec, ch);
    };

    auto *cols = new QHBoxLayout;
    auto *secCol = new QVBoxLayout;
    auto *secHead = new QLabel(tr("Sections"), w);
    secHead->setStyleSheet(theme::css(theme::muted(w)));
    secCol->addWidget(secHead);
    for (int i = 0; i < 7; i++) {
        secBox_[i] = new QCheckBox(
            QCoreApplication::translate("Hud", hudSectionName(kSecs[i])), w);
        connect(secBox_[i], &QCheckBox::toggled, this, emitMasks);
        secCol->addWidget(secBox_[i]);
    }
    secCol->addStretch(1);

    auto *chCol = new QVBoxLayout;
    auto *chHead = new QLabel(tr("Charts"), w);
    chHead->setStyleSheet(theme::css(theme::muted(w)));
    chCol->addWidget(chHead);
    for (int i = 0; i < 8; i++) {
        chartBox_[i] = new QCheckBox(
            QCoreApplication::translate("Hud", hudChartName(kCharts[i])), w);
        connect(chartBox_[i], &QCheckBox::toggled, this, emitMasks);
        chCol->addWidget(chartBox_[i]);
    }
    chCol->addStretch(1);
    cols->addLayout(secCol, 1);
    cols->addLayout(chCol, 1);
    root->addLayout(cols);

    /* --- presentation and the layout editor (OV7) ---------------------- */
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(theme::SpaceGroup);

    refreshBox_ = new QComboBox(w);
    refreshBox_->addItem(tr("4 per second (250 ms)"), 250);
    refreshBox_->addItem(tr("2 per second (500 ms)"), 500);
    refreshBox_->addItem(tr("Once a second"), 1000);
    refreshBox_->addItem(tr("Every 2 seconds"), 2000);
    connect(refreshBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
        emit hudRefreshChanged(refreshBox_->itemData(i).toInt());
    });
    form->addRow(tr("Refresh"), refreshBox_);

    scaleSlider_ = new QSlider(Qt::Horizontal, w);
    scaleSlider_->setRange(60, 200);
    auto *scaleVal = new QLabel(w);
    auto *srow = new QHBoxLayout;
    srow->addWidget(scaleSlider_, 1); srow->addWidget(scaleVal);
    connect(scaleSlider_, &QSlider::valueChanged, this, [this, scaleVal](int v) {
        scaleVal->setText(QStringLiteral("%1 %").arg(v));
        emit hudScaleChanged(v);
    });
    form->addRow(tr("Size"), srow);

    opacitySlider_ = new QSlider(Qt::Horizontal, w);
    opacitySlider_->setRange(20, 100);
    auto *opVal = new QLabel(w);
    auto *orow = new QHBoxLayout;
    orow->addWidget(opacitySlider_, 1); orow->addWidget(opVal);
    connect(opacitySlider_, &QSlider::valueChanged, this, [this, opVal](int v) {
        opVal->setText(QStringLiteral("%1 %").arg(v));
        emit hudOpacityChanged(v);
    });
    form->addRow(tr("Opacity"), opacitySlider_->parentWidget() ? orow : orow);
    root->addLayout(form);

    auto *actions = new QHBoxLayout;
    editBtn_ = new QPushButton(w);
    editBtn_->setCheckable(true);
    resetLayoutBtn_ = new QPushButton(w);
    actions->addWidget(editBtn_);
    actions->addWidget(resetLayoutBtn_);
    actions->addStretch(1);
    root->addLayout(actions);
    connect(editBtn_, &QPushButton::toggled, this, [this](bool on) {
        emit hudEditToggled(on);
        if (on) hide();   /* the menu would sit on top of what you are arranging */
    });
    connect(resetLayoutBtn_, &QPushButton::clicked, this,
            [this] { emit hudLayoutReset(); });

    auto *note = new QLabel(tr("Blocks are placed on the stream. In edit mode, "
                               "drag one onto another to join them into a panel, "
                               "and right-click a block to detach it."), w);
    note->setWordWrap(true);
    note->setStyleSheet(theme::css(theme::muted(w)));
    root->addWidget(note);
    root->addStretch(1);
    return w;
}

void StreamOverlay::setInitialHud(int sections, int charts)
{
    static const int kSecs[7]   = { SecPerf, SecLatency, SecNet, SecVideo,
                                    SecInput, SecAudio, SecAdvanced };
    static const int kCharts[8] = { ChDecoded, ChPresented, ChBitrate, ChPackets,
                                    ChLoss, ChRtt, ChAudio, ChLatency };
    for (int i = 0; i < 7; i++) {
        QSignalBlocker b(secBox_[i]);
        secBox_[i]->setChecked(sections & kSecs[i]);
    }
    for (int i = 0; i < 8; i++) {
        QSignalBlocker b(chartBox_[i]);
        chartBox_[i]->setChecked(charts & kCharts[i]);
    }
}

void StreamOverlay::setHudPresentation(int refreshMs, int scalePct, int opacityPct)
{
    { QSignalBlocker b(refreshBox_);
      const int i = refreshBox_->findData(refreshMs);
      refreshBox_->setCurrentIndex(i >= 0 ? i : 1); }
    { QSignalBlocker b(scaleSlider_);   scaleSlider_->setValue(scalePct); }
    { QSignalBlocker b(opacitySlider_); opacitySlider_->setValue(opacityPct); }
}

void StreamOverlay::setHudEditing(bool on)
{
    QSignalBlocker b(editBtn_);
    editBtn_->setChecked(on);
}

void StreamOverlay::showEvent(QShowEvent *e)
{
    loadFromCore();    /* reflect the live volume and EQ state into the controls */
    QWidget::showEvent(e);
}

void StreamOverlay::placeOver(const QRect &r)
{
    adjustSize();
    move(r.center().x() - width() / 2, r.center().y() - height() / 2);
}

void StreamOverlay::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::LanguageChange) retranslate();
    else if (e->type() == QEvent::ActivationChange && isVisible() && !isActiveWindow())
        hide();   /* click away = close, like a pause menu */
    QWidget::changeEvent(e);
}

void StreamOverlay::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) { hide(); emit closed(); return; }
    QWidget::keyPressEvent(e);
}

void StreamOverlay::retranslate()
{
    tabs_->setTabText(0, tr("Sound"));
    tabs_->setTabText(1, tr("Video"));
    tabs_->setTabText(2, tr("Input"));
    tabs_->setTabText(3, tr("Tools"));
    tabs_->setTabText(4, tr("HUD"));
    titleAudio_->setText(tr("Volume"));
    muteBtn_->setText(tr("Mute"));
    gCombos_->setTitle(tr("Send to the VM"));
    shotBtn_->setText(tr("Screenshot"));
    filesBtn_->setText(tr("File transfer"));
    metBtn_->setText(tr("Metrics"));
    setBtn_->setText(tr("Settings"));
    diagBtn_->setText(tr("Copy diagnostics"));
    editBtn_->setText(tr("Edit the layout"));
    resetLayoutBtn_->setText(tr("Reset the layout"));
    gEq_->setTitle(tr("Equaliser"));
    eqOn_->setText(tr("On"));
    eqAutoTrim_->setText(tr("Auto-trim"));
    fsBtn_->setText(tr("Fullscreen"));
    discBtn_->setText(tr("Disconnect"));
    closeBtn_->setText(tr("Close"));
    if (eqPreset_->count() == 0)
        eqPreset_->addItems({ tr("Flat"), tr("Bass boost"), tr("Voice"),
                              tr("Treble boost") });
}

void StreamOverlay::applyEq()
{
    eq_band_t bands[5];
    for (int b = 0; b < 5; b++) {
        bands[b].type    = eqOn_->isChecked() ? (eq_type_t)bands_[b].type->currentIndex()
                                              : EQ_OFF;
        bands[b].freq    = (float)bands_[b].freq->value();
        bands[b].q       = (float)bands_[b].q->value();
        bands[b].gain_db = (float)bands_[b].gain->value();
    }
    audio_eq_configure(bands, 5, eqAutoTrim_->isChecked());
    if (eqCurve_) eqCurve_->setBands(bands, 5);   /* the drawn curve follows */
}

void StreamOverlay::loadFromCore()
{
    volume_->setValue((int)audio_get_volume());
    volumeVal_->setText(QStringLiteral("%1 %").arg(volume_->value()));
    eqOn_->setChecked(audio_eq_active());
}

}  // namespace halyard
