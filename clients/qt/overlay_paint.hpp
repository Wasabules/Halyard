/* overlay_paint - the two drawn widgets the overlay needs: a sparkline for the
 * HUD, and the equaliser's response curve.
 *
 * === OV2 2026-10-03 — WHY DRAWN, NOT A CHART LIBRARY =======================
 *
 * A HUD line and an EQ curve are a ring of floats and a painter; pulling in Qt
 * Charts (a separate module, a separate dependency to ship beside the binary -
 * which the CA-bundle and multimedia-plugin episodes showed is where a Windows
 * build breaks) buys nothing for two shapes. Both take the FOREGROUND colour
 * from the palette through theme.hpp, so they are correct in light and dark.
 *
 * Both are self-contained QWidgets: feed Sparkline a value per tick, feed EqCurve
 * the five bands, and they paint. No timers, no I/O.
 */
#pragma once

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QVector>
#include <QWidget>

#include <cmath>
#include <vector>

#include "theme.hpp"

extern "C" {
#include "core/protocol/eq.h"
}

namespace halyard {

/* A rolling line of the last N samples, with an optional coloured threshold. */
class Sparkline : public QWidget {
    Q_OBJECT
public:
    explicit Sparkline(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(22);
        setMinimumWidth(90);
    }

    /* `good`/`warn` colour the latest point: value >= good is good, >= warn is
     * warn, below is bad. Set inverted=true when LOWER is better (loss, RTT). */
    void configure(bool inverted, double warn, double good)
    {
        inverted_ = inverted; warn_ = warn; good_ = good;
    }

    void push(double v)
    {
        last_ = v;
        ring_.append(v);
        while (ring_.size() > kN) ring_.removeFirst();
        if (v > max_) max_ = v;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const double w = width(), h = height();
        if (ring_.size() < 2 || max_ <= 0.0) return;

        QColor line = colourFor(last_);
        QPainterPath path;
        for (int i = 0; i < ring_.size(); i++) {
            const double x = w * i / (kN - 1);
            const double y = h - 2 - (h - 4) * (ring_[i] / max_);
            if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        QColor fill = line; fill.setAlphaF(0.18f);
        QPainterPath area = path;
        area.lineTo(w * (ring_.size() - 1) / (kN - 1), h);
        area.lineTo(0, h);
        area.closeSubpath();
        p.fillPath(area, fill);
        p.setPen(QPen(line, 1.5));
        p.drawPath(path);
    }

private:
    QColor colourFor(double v) const
    {
        const double hi = inverted_ ? -v : v;
        const double g = inverted_ ? -good_ : good_;
        const double wa = inverted_ ? -warn_ : warn_;
        if (hi >= g) return theme::good(this);
        if (hi >= wa) return theme::warn(this);
        return theme::bad(this);
    }

    static const int kN = 90;
    QVector<double> ring_;
    double last_ = 0, max_ = 0;
    bool   inverted_ = false;
    double warn_ = 0, good_ = 0;
};

/* The equaliser cascade's magnitude response, 20 Hz..20 kHz on a log axis,
 * -18..+18 dB. Redrawn whenever setBands() is called. */
class EqCurve : public QWidget {
    Q_OBJECT
public:
    explicit EqCurve(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(90);
    }
    void setBands(const eq_band_t *b, int n)
    {
        bands_.assign(b, b + n);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    std::vector<eq_band_t> bands_;
};

}  // namespace halyard
