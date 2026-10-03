#include "theme.hpp"

#include <QFontDatabase>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif
#include <QGraphicsDropShadowEffect>
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

/* ================================================================ surfaces */

QColor surface(const QWidget *context)
{
    const QPalette pal = basePalette(context);
    /* A card lifts off the window: toward the text colour on a dark theme,
     * toward white on a light one. Derived, so it tracks the system. */
    const QColor win = pal.color(QPalette::Window);
    return win.lightnessF() < 0.5 ? mix(win, pal.color(QPalette::WindowText), 0.07)
                                  : mix(win, QColor(255, 255, 255), 0.65);
}

QColor surfaceAlt(const QWidget *context)
{
    const QPalette pal = basePalette(context);
    const QColor win = pal.color(QPalette::Window);
    return win.lightnessF() < 0.5 ? mix(win, pal.color(QPalette::WindowText), 0.13)
                                  : mix(win, QColor(255, 255, 255), 0.85);
}

QColor border(const QWidget *context)
{
    const QPalette pal = basePalette(context);
    return mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.18);
}

QColor accent(const QWidget *context)
{
    /* The sail's blue. Lightness from the window, like every other accent here,
     * so it stays legible on both themes. */
    const QPalette pal = basePalette(context);
    const bool dark = pal.color(QPalette::Window).lightnessF() < 0.5;
    return QColor::fromHsl(210, 160, dark ? 160 : 110);
}

bool animationsEnabled()
{
    static const bool kOn = [] {
        if (qgetenv("SHADOW_QT_ANIM") == "0") return false;
#ifdef Q_OS_WIN
        BOOL on = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0))
            return on != FALSE;
#endif
        return true;
    }();
    return kOn;
}

/* See the header (D2). */
bool applyThemeMode(ThemeMode mode)
{
    if (mode == ThemeMode::Auto) return false;

    const bool dark = (mode == ThemeMode::Dark);
    /* A palette built from two anchors and nothing else: the window colour and
     * the text colour. Everything this file paints is derived from those two
     * (surface, surfaceAlt, border, accent, muted), so naming more of them
     * here would be inventing values the derivation would then ignore - and
     * the two would disagree the first time one of them was edited. */
    const QColor win  = dark ? QColor(0x1e, 0x1f, 0x24) : QColor(0xf4, 0xf5, 0xf7);
    const QColor text = dark ? QColor(0xe9, 0xea, 0xee) : QColor(0x1b, 0x1c, 0x20);
    const QColor base = dark ? QColor(0x17, 0x18, 0x1c) : QColor(0xff, 0xff, 0xff);

    QPalette pal;
    pal.setColor(QPalette::Window,          win);
    pal.setColor(QPalette::WindowText,      text);
    pal.setColor(QPalette::Base,            base);
    pal.setColor(QPalette::AlternateBase,   win);
    pal.setColor(QPalette::Text,            text);
    pal.setColor(QPalette::Button,          win);
    pal.setColor(QPalette::ButtonText,      text);
    pal.setColor(QPalette::ToolTipBase,     base);
    pal.setColor(QPalette::ToolTipText,     text);
    /* Disabled text must be derived too, or it is black on dark. */
    pal.setColor(QPalette::Disabled, QPalette::WindowText, mix(text, win, 0.55));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, mix(text, win, 0.55));
    pal.setColor(QPalette::Disabled, QPalette::Text,       mix(text, win, 0.55));
    QGuiApplication::setPalette(pal);
    return true;
}

QString appStyleSheet(const QWidget *context)
{
    const QColor sf = surface(context), sa = surfaceAlt(context);
    const QColor bd = border(context),  ac = accent(context);
    const QColor tx = basePalette(context).color(QPalette::WindowText);
    const QColor mu = muted(context);

    return QStringLiteral(R"(
QWidget[card="true"] {
    background: %1; border: 1px solid %2; border-radius: 12px;
}
QWidget[cardHover="true"]:hover { border-color: %3; }

QPushButton {
    background: %4; border: 1px solid %2; border-radius: 7px;
    padding: 7px 16px; color: %5;
}
QPushButton:hover    { border-color: %3; }
QPushButton:pressed  { background: %1; }
QPushButton:disabled { color: %6; border-color: %2; }
QPushButton[accent="true"] {
    background: %3; border: 1px solid %3; color: #ffffff; font-weight: bold;
}
QPushButton[accent="true"]:hover   { background: %7; border-color: %7; }
QPushButton[accent="true"]:disabled{ background: %4; color: %6; border-color: %2; }

QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QKeySequenceEdit {
    background: %4; border: 1px solid %2; border-radius: 6px; padding: 5px 8px;
    selection-background-color: %3;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border-color: %3;
}

QLabel[pill="ok"], QLabel[pill="off"], QLabel[pill="busy"] {
    border-radius: 9px; padding: 2px 10px; font-size: 11px; font-weight: bold;
}
QLabel[pill="ok"]   { background: rgba(80,180,120,55);  color: %5; }
QLabel[pill="busy"] { background: rgba(230,180,80,55);  color: %5; }
QLabel[pill="off"]  { background: rgba(140,140,150,45); color: %6; }

QLabel[dim="true"] { color: %6; }
QLabel[h1="true"]  { font-size: 26px; font-weight: bold; }
QLabel[h2="true"]  { font-size: 15px; font-weight: bold; }

QTabWidget::pane { border: 1px solid %2; border-radius: 8px; }
QTabBar::tab {
    background: transparent; padding: 7px 14px; border: none; color: %6;
}
QTabBar::tab:selected { color: %5; border-bottom: 2px solid %3; }

QGroupBox {
    border: 1px solid %2; border-radius: 9px; margin-top: 10px; padding-top: 8px;
}
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: %6; }

QProgressBar {
    border: 1px solid %2; border-radius: 6px; text-align: center; background: %4;
}
QProgressBar::chunk { background: %3; border-radius: 5px; }
)")
        .arg(sf.name(), bd.name(), ac.name(), sa.name(), tx.name(), mu.name(),
             ac.lighter(115).name());
}

QGraphicsDropShadowEffect *elevation(QWidget *on, int radius)
{
    auto *e = new QGraphicsDropShadowEffect(on);
    e->setBlurRadius(radius);
    e->setOffset(0, 3);
    e->setColor(QColor(0, 0, 0, 90));
    return e;
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

/* See the header (ICON1). */
QIcon appTileIcon()
{
    /* The brand blue, fixed and NOT palette-derived: this icon is composited
     * by the desktop onto surfaces we cannot see, so there is no palette whose
     * answer would be the right one. The hue is the same 210 the in-app accent
     * uses, at a lightness that holds up on both a light and a dark taskbar. */
    const QColor top = QColor::fromHsl(210, 170, 120);
    const QColor bot = QColor::fromHsl(210, 175,  86);

    QIcon icon;
    for (int px : { 16, 20, 24, 32, 40, 48, 64, 128, 256 }) {
        QPixmap pm(px, px);
        pm.fill(Qt::transparent);

        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal s = px;
        /* A hair of inset so the antialiased corner is not clipped by the
         * pixmap edge, and 22% radius - the squircle proportion Windows 11 and
         * macOS both land near, so the tile does not look foreign in a row of
         * system icons. */
        const QRectF tile = QRectF(0.5, 0.5, s - 1, s - 1);
        QLinearGradient g(tile.topLeft(), tile.bottomLeft());
        g.setColorAt(0.0, top);
        g.setColorAt(1.0, bot);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(tile, s * 0.22, s * 0.22);

        /* === THE MARK, IN TILE FRACTIONS ==================================
         *
         * Two shapes only - a mast and a hoisted sail. `appIcon` draws three,
         * the third being the halyard itself, and that one is dropped here:
         * at 16px, which is the size the taskbar actually uses, a hairline
         * curve running down the mast antialiases into a smudge, and the
         * smudge is most of what made the old icon look like scratches.
         *
         * The numbers are chosen so the INK is centred, not the box: the sail
         * is all on one side of the mast, so a mark laid out symmetrically
         * about the tile's middle sits visibly to the right. Left edge of the
         * mast 0.30, right edge of the sail 0.74, so the ink spans 0.30..0.74
         * and its centre is 0.52 - shifted by -0.02 below to land on 0.50. */
        /* Below 24px the mast and the sail are drawn TOUCHING, as one glyph.
         * Separated, the gap between them is one pixel, antialiasing turns
         * that pixel into grey, and the result is the soft smear this whole
         * icon exists to get away from. Joined, it is a solid white shape
         * that survives 16 pixels - the detail is lost either way, so the
         * choice is between losing it cleanly and losing it as mud. */
        const bool tiny = px < 24;

        const qreal dx   = -0.02 * s;
        const qreal mastX = 0.33 * s + dx;
        const qreal mastW = qMax<qreal>(1.5, (tiny ? 0.10 : 0.065) * s);
        const qreal topY  = 0.18 * s;
        const qreal botY  = 0.82 * s;

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255));

        /* The sail: a filled triangle, not an outline. At 16px an outlined
         * triangle is three grey lines and a filled one is a shape - and a
         * shape is the only thing that survives being 16 pixels wide. */
        const qreal gap = tiny ? 0.0 : qMax<qreal>(1.0, 0.045 * s);
        QPainterPath sail;
        sail.moveTo(mastX + mastW / 2 + gap, topY + 0.03 * s);
        sail.lineTo(mastX + mastW / 2 + gap, botY - 0.05 * s);
        sail.lineTo(0.76 * s + dx,           botY - 0.05 * s);
        sail.closeSubpath();
        p.drawPath(sail);

        /* The mast, drawn after the sail so its cap is not cut by it. A
         * rounded rectangle rather than a line: a line's round cap at 16px
         * rounds to a square anyway, and a rect is exact at every size. */
        p.drawRoundedRect(QRectF(mastX - mastW / 2, topY, mastW, botY - topY),
                          mastW / 2, mastW / 2);

        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

}  // namespace halyard::theme
