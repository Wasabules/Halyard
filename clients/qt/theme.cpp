#include "theme.hpp"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QWidget>

namespace halyard::theme {

namespace {

QPalette basePalette(const QWidget *context)
{
    return context ? context->palette() : QGuiApplication::palette();
}

/* `a` moved `t` of the way toward `b`, in sRGB.
 *
 * Not perceptually uniform, and it does not need to be: the only requirement
 * is monotonic contrast, so that the result sits between the two and never
 * crosses either. A Lab blend would be more correct and would not change a
 * single decision here. */
QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF()  + (b.blueF()  - a.blueF())  * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

/* An accent that stays legible on THIS background.
 *
 * A fixed green is the defect this function exists to avoid: #2e7d32 on a dark
 * window is barely distinguishable from the window, and the same green on a
 * light one is fine. So the hue is fixed and the LIGHTNESS is chosen from the
 * window's own lightness - dark window, light ink. */
QColor accent(const QWidget *context, int hue, int sat)
{
    const QPalette pal = basePalette(context);
    const bool dark = pal.color(QPalette::Window).lightnessF() < 0.5;
    return QColor::fromHsl(hue, sat, dark ? 185 : 95);
}

}  // namespace

QColor muted(const QWidget *context)
{
    const QPalette pal = basePalette(context);
    /* 45% of the way from the text colour to the background. Deliberately not
     * `QPalette::Mid` or `PlaceholderText`: `Mid` is a FRAME shade with no
     * guaranteed contrast against the window (that is the dark-grey-on-dark
     * defect), and `PlaceholderText` is unset on several styles, where it
     * resolves to plain text and is not muted at all. */
    return mix(pal.color(QPalette::WindowText), pal.color(QPalette::Window), 0.45);
}

QColor overridden(const QWidget *context) { return accent(context, 35,  160); }
QColor good(const QWidget *context)       { return accent(context, 135, 120); }
QColor warn(const QWidget *context)       { return accent(context, 35,  160); }
QColor bad(const QWidget *context)        { return accent(context, 0,   140); }

QString css(const QColor &c)
{
    return QStringLiteral("color: %1;").arg(c.name(QColor::HexRgb));
}

QFont titleFont(const QWidget *context)
{
    QFont f = context ? context->font() : QGuiApplication::font();
    f.setPointSizeF(f.pointSizeF() * 1.45);
    f.setBold(true);
    return f;
}

QFont headingFont(const QWidget *context)
{
    QFont f = context ? context->font() : QGuiApplication::font();
    f.setBold(true);
    return f;
}

QFont monoFont(const QWidget *context)
{
    /* The style hint and not a font name: "Consolas" does not exist on Linux,
     * "DejaVu Sans Mono" does not exist on Windows, and a missing family
     * resolves to the proportional default - which silently defeats the only
     * reason to ask for mono (an identifier that must not be mistaken for
     * prose). `systemFont` asks the platform for the one it has. */
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QFont base = context ? context->font() : QGuiApplication::font();
    f.setPointSizeF(base.pointSizeF() * 0.88);
    return f;
}

/* ==================================================================== mark */

QIcon appIcon(const QColor &ink)
{
    QIcon icon;
    /* The sizes Windows and the Linux desktops actually ask for. Drawing each
     * rather than scaling one keeps the strokes from blurring at 16px, which is
     * the size the taskbar uses and the only one most people ever see. */
    for (int px : { 16, 24, 32, 48, 64, 128, 256 }) {
        QPixmap pm(px, px);
        pm.fill(Qt::transparent);

        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal s = px;
        const qreal w = qMax<qreal>(1.0, s * 0.075);   /* stroke, scaled */

        /* The sail, hoisted: a triangle off the mast. Filled at partial alpha
         * so the mark reads as one shape at 16px rather than three lines. */
        QColor fill = ink;
        fill.setAlphaF(0.30f);
        QPainterPath sail;
        sail.moveTo(s * 0.42, s * 0.17);
        sail.lineTo(s * 0.42, s * 0.74);
        sail.lineTo(s * 0.84, s * 0.74);
        sail.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(sail);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ink, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        /* The mast. */
        p.drawLine(QPointF(s * 0.34, s * 0.10), QPointF(s * 0.34, s * 0.90));

        /* The halyard itself - the line that did the hoisting, slack from the
         * masthead down to where it is made fast. This is the one stroke that
         * names the application, so it is drawn last and at full weight. */
        QPainterPath line;
        line.moveTo(s * 0.34, s * 0.11);
        line.quadTo(s * 0.17, s * 0.52, s * 0.23, s * 0.88);
        p.drawPath(line);

        /* The sail's leading edge, against the mast. */
        p.drawLine(QPointF(s * 0.42, s * 0.17), QPointF(s * 0.42, s * 0.74));

        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QIcon appIcon()
{
    return appIcon(QGuiApplication::palette().color(QPalette::WindowText));
}

}  // namespace halyard::theme
