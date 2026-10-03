/* See pairing_widgets.hpp for why each of these is painted. */
#include "pairing_widgets.hpp"

#include "theme.hpp"

#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>

extern "C" {
#include "core/services/qr_helper.h"
}

namespace theme = halyard::theme;

namespace halyard {

namespace {
/* Four modules of quiet zone is the standard minimum, and it is not optional:
 * a QR flush against other content fails to scan on roughly half the readers
 * tried. `qr_matrix` deliberately leaves it out, so it is added here. */
constexpr int kQuietModules = 4;
}  // namespace

/* ================================================================== QrView */

QrView::QrView(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

bool QrView::setText(const QString &text)
{
    modules_.clear();
    width_ = 0;
    if (text.isEmpty()) { update(); return false; }

    /* 177 modules is the largest code libqrencode can produce (version 40), so
     * this covers anything it can return and the call cannot be the reason a
     * QR is refused. */
    QVector<unsigned char> buf(177 * 177);
    int w = 0;
    const QByteArray t = text.toUtf8();
    if (!qr_matrix(t.constData(), buf.data(), buf.size(), &w) || w <= 0) {
        update();
        return false;
    }
    buf.resize(w * w);
    modules_ = buf;
    width_ = w;
    updateGeometry();
    update();
    return true;
}

QSize QrView::sizeHint() const
{
    if (width_ <= 0) return QSize(0, 0);
    /* A fixed 170px box whatever the version, so the card does not change size
     * when a longer URL pushes the code up a version. The module size is then
     * whatever divides into it. */
    return QSize(170, 170);
}

void QrView::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);
    if (width_ <= 0) return;

    QPainter p(this);
    const int total = width_ + 2 * kQuietModules;
    /* INTEGER module size, floored. A fractional one makes QPainter put some
     * modules on a half pixel and antialias them to grey, and a grey module is
     * a module a reader has to guess at - which is how a QR that looks right
     * on screen fails to scan. The leftover is spent on centring. */
    const int unit = qMax(1, qMin(width(), height()) / total);
    const int side = unit * total;
    const int ox = (width() - side) / 2, oy = (height() - side) / 2;

    /* The quiet zone must be LIGHT, so the background is painted here rather
     * than inherited: on a dark theme the card behind is dark, and a dark
     * quiet zone is the same defect as no quiet zone at all. */
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255));
    p.drawRoundedRect(QRect(ox, oy, side, side), 8, 8);

    p.setBrush(QColor(0, 0, 0));
    for (int y = 0; y < width_; y++)
        for (int x = 0; x < width_; x++)
            if (modules_.at(y * width_ + x))
                p.drawRect(ox + (x + kQuietModules) * unit,
                           oy + (y + kQuietModules) * unit, unit, unit);
}

/* =============================================================== CodeCells */

CodeCells::CodeCells(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void CodeCells::setCode(const QString &code)
{
    code_ = code;
    updateGeometry();
    update();
}

QSize CodeCells::sizeHint() const
{
    if (code_.isEmpty()) return QSize(0, 0);
    return QSize(code_.size() * 44 + (code_.size() - 1) * 6, 58);
}

void CodeCells::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);
    if (code_.isEmpty()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int n = code_.size();
    const int gap = 6;
    const int cw = qMax(18, (width() - gap * (n - 1)) / n);
    const int ch = height();
    const int used = cw * n + gap * (n - 1);
    int x = (width() - used) / 2;

    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPixelSize(int(ch * 0.52));
    f.setBold(true);
    p.setFont(f);

    for (int i = 0; i < n; i++) {
        const QRect cell(x, 0, cw, ch);
        p.setPen(QPen(theme::border(this), 1));
        p.setBrush(theme::surfaceAlt(this));
        p.drawRoundedRect(QRectF(cell).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);

        const QChar c = code_.at(i);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(cell, Qt::AlignCenter, QString(c));

        /* The zero gets a slash. This is the whole reason the code is painted
         * rather than set on a label: the platform's fixed font may or may not
         * distinguish O from 0, we cannot choose the font on every machine,
         * and a misread zero costs the user the whole sign-in. */
        if (c == QLatin1Char('0')) {
            const QFontMetrics fm(f);
            const int h = fm.ascent();
            const QPointF mid(cell.center().x(), cell.center().y());
            p.setPen(QPen(palette().color(QPalette::WindowText), 1.4));
            p.drawLine(QPointF(mid.x() + h * 0.22, mid.y() - h * 0.34),
                       QPointF(mid.x() - h * 0.22, mid.y() + h * 0.34));
        }
        x += cw + gap;
    }
}

/* ============================================================= ValidityBar */

ValidityBar::ValidityBar(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ValidityBar::setRemaining(int left, int total)
{
    left_  = qMax(0, left);
    total_ = qMax(1, total);
    update();
}

QSize ValidityBar::sizeHint() const { return QSize(0, 6); }

void ValidityBar::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);
    if (total_ <= 0) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal r = height() / 2.0;

    p.setPen(Qt::NoPen);
    p.setBrush(theme::surfaceAlt(this));
    p.drawRoundedRect(QRectF(0, 0, width(), height()), r, r);

    const qreal frac = qBound(0.0, double(left_) / double(total_), 1.0);
    const qreal w = width() * frac;
    if (w <= 0) return;

    /* Amber under a minute, matching the text beside it: two things saying the
     * same thing in two ways beats one of them saying it twice. */
    p.setBrush(left_ <= 60 ? theme::warn(this) : theme::accent(this));
    p.drawRoundedRect(QRectF(0, 0, qMax(w, 2.0 * r), height()), r, r);
}

}  // namespace halyard
