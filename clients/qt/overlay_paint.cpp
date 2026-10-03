#include "overlay_paint.hpp"

#include <QPainter>

#include <cmath>

extern "C" {
#include "core/media/audio.h"
}

namespace halyard {

void EqCurve::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double w = width(), h = height();
    const double kDbMax = 18.0;

    /* Grid: 0 dB, and the decade lines. Muted so the curve reads above it. */
    QColor grid = theme::muted(this);
    grid.setAlphaF(0.35f);
    p.setPen(QPen(grid, 1));
    const double zero = h / 2.0;
    p.drawLine(QPointF(0, zero), QPointF(w, zero));
    for (double f : { 100.0, 1000.0, 10000.0 }) {
        const double x = w * (std::log10(f) - std::log10(20.0))
                           / (std::log10(20000.0) - std::log10(20.0));
        p.drawLine(QPointF(x, 0), QPointF(x, h));
    }

    /* The cascade's response, from core - the same function the sound goes
     * through, so the curve cannot drift from what is heard. */
    QPainterPath path;
    const int N = 160;
    for (int i = 0; i < N; i++) {
        const double t = (double)i / (N - 1);
        const double f = std::pow(10.0, std::log10(20.0) + t * (std::log10(20000.0) - std::log10(20.0)));
        double db = audio_eq_reponse_db((float)f);
        if (db >  kDbMax) db =  kDbMax;
        if (db < -kDbMax) db = -kDbMax;
        const double x = w * t;
        const double y = zero - (db / kDbMax) * (h / 2.0 - 3.0);
        if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
    }
    p.setPen(QPen(theme::good(this), 2.0));
    p.drawPath(path);
}

}  // namespace halyard
