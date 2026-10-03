/* shots.cpp - see the header for why the UI renders itself to disk. */
#include "shots.hpp"

#include "hud_theme.hpp"
#include "stream_overlay.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QLayout>
#include <QTabWidget>
#include <QTextStream>
#include <QWidget>

namespace halyard::shots {

namespace {

/* A stand-in for the stream. Not noise and not a flat colour: the glass has
 * to be judged against something with large soft areas of DIFFERENT
 * brightness, because the question the blur answers is whether the panel
 * stays readable when the picture behind it goes light. */
QImage fakeFrame(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(img.rect(), QColor(0x15, 0x1A, 0x23));
    p.setPen(Qt::NoPen);
    /* A bright patch on the right, under where the HUD sits: the worst case
     * for a translucent panel, and therefore the one worth looking at. */
    p.setBrush(QColor(0xC8, 0xD4, 0xE4));
    p.drawEllipse(QRectF(w * 0.62, h * 0.05, w * 0.42, h * 0.52));
    p.setBrush(QColor(0x5A, 0x74, 0x96));
    p.drawEllipse(QRectF(w * 0.05, h * 0.42, w * 0.46, h * 0.5));
    p.setBrush(QColor(0x2A, 0x33, 0x44));
    p.drawEllipse(QRectF(w * 0.30, h * 0.10, w * 0.34, h * 0.38));
    return img;
}

bool save(QWidget *w, const QString &dir, const QString &name, QTextStream &out)
{
    if (!w) { out << "  MISSING " << name << "\n"; return false; }
    w->adjustSize();
    const QPixmap pm = w->grab();
    const QString path = dir + QLatin1Char('/') + name + QStringLiteral(".png");
    const bool ok = !pm.isNull() && pm.save(path, "PNG");
    out << (ok ? "  wrote " : "  FAILED ") << name << ".png  "
        << pm.width() << "x" << pm.height() << "\n";
    return ok;
}

}  // namespace

int run(const QString &dir)
{
    QTextStream out(stdout);
    QDir().mkpath(dir);
    out << "rendering the UI into " << dir << "\n";

    int bad = 0;

    /* ---- the overlay menu, one PNG per tab --------------------------- */
    {
        auto *ov = new StreamOverlay(nullptr);
        ov->resize(560, 760);
        /* The panel is a frameless tool window; grabbing it before it has
         * ever been shown gives an unlaid-out widget, so it is laid out
         * explicitly rather than shown (showing would need a display). */
        ov->ensurePolished();
        if (ov->layout()) ov->layout()->activate();

        auto *tabs = ov->findChild<QTabWidget *>();
        if (!tabs) {
            out << "  MISSING the overlay's tab widget\n";
            bad++;
        } else {
            static const char *names[] = { "overlay-sound", "overlay-video",
                                           "overlay-input", "overlay-tools",
                                           "overlay-hud" };
            for (int i = 0; i < tabs->count() && i < 5; i++) {
                tabs->setCurrentIndex(i);
                ov->ensurePolished();
                if (ov->layout()) ov->layout()->activate();
                if (!save(ov, dir, QString::fromUtf8(names[i]), out)) bad++;
            }
        }
        delete ov;
    }

    /* ---- the HUD, over a stand-in picture ----------------------------- */
    {
        auto *hud = new StreamHud(nullptr);
        const QImage frame = fakeFrame(1280, 720);
        hud->setFrameSource([frame] { return frame; });
        hud->setPresentedCounter([] { return quint64(0); });
        hud->setMasks(SecPerf | SecNet | SecLatency, ChDecoded | ChBitrate);
        /* The quota figures measured on a real account (6 h a session, 210 h
         * a period), at a point where the session bar is amber and the month
         * is green - the two states worth seeing side by side. */
        hud->setQuotas(6 * 3600, 5 * 3600 + 12 * 60, 210 * 3600, 42 * 3600);
        hud->resize(1280, 720);
        hud->ensurePolished();
        /* SHOT1 - the HUD lays its blocks out and computes its glass on the
         * refresh tick, and the tick only starts on `showEvent`. Offscreen
         * there is no show, so the first render came out with every block
         * stacked at the origin and no backdrop at all - which is precisely
         * the kind of thing this harness exists to make visible, but here it
         * would be the harness's fault rather than the HUD's. Drive it. */
        QMetaObject::invokeMethod(hud, "refresh", Qt::DirectConnection);
        QCoreApplication::processEvents();

        /* The HUD paints its blocks against a transparent window, so the
         * grab is composited onto the stand-in frame here - otherwise the
         * PNG shows the panels on a checkerboard and the one thing being
         * judged, the glass, is invisible. */
        QImage canvas = frame;
        QPixmap pm = hud->grab();
        {
            QPainter p(&canvas);
            p.drawPixmap(0, 0, pm);
        }
        const QString path = dir + QStringLiteral("/hud-over-stream.png");
        const bool ok = canvas.save(path, "PNG");
        out << (ok ? "  wrote " : "  FAILED ") << "hud-over-stream.png  "
            << canvas.width() << "x" << canvas.height() << "\n";
        if (!ok) bad++;
        delete hud;
    }

    out << (bad ? "some surfaces did not render\n" : "done\n");
    return bad ? 1 : 0;
}

}  // namespace halyard::shots
