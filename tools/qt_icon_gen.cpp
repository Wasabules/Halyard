/* qt_icon_gen - writes the Windows .ico from the drawn application tile.
 *
 * === ICON1 2026-10-03 — WHY GENERATE IT RATHER THAN COMMIT IT =============
 *
 * `setWindowIcon` fixes the taskbar button of a RUNNING application, and
 * nothing else. Explorer, a pinned shortcut, the alt-tab panel before the
 * window exists and the file's own thumbnail all read the icon resource
 * compiled into the executable - which this binary did not have, so next to
 * other applications it showed whatever Windows uses for "no icon".
 *
 * The resource could be a committed .ico. It is generated instead, at build
 * time, from `theme::appTileIcon()`: a committed binary would be a second
 * source of truth for the mark, and the first time someone adjusted the
 * drawing the two would disagree with nothing to notice it. The mark is code,
 * so the icon is built from the code.
 *
 * === WHY THE FILE IS ASSEMBLED BY HAND ====================================
 *
 * Qt lists "ico" among `QImageWriter::supportedImageFormats()`, which is
 * true and misleading: calling `write()` repeatedly on one writer does not
 * add entries to one icon, it concatenates whole single-image .ico files.
 * The first attempt produced 381 KB whose header said "1 image, 16x16", so
 * Windows would have scaled the 16px tile up to fill every other size - a
 * blurrier result than the placeholder it replaced.
 *
 * ICO is a directory of images and nothing more, so it is written directly.
 * Each entry's payload is a PNG: Windows has accepted PNG-in-ICO since Vista,
 * it is what every size above 48px uses in practice anyway, and it avoids
 * hand-rolling the upside-down BMP with its separate AND mask that the older
 * form requires.
 */
#include <QBuffer>
#include <QByteArray>
#include <QDataStream>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QList>

#include "theme.hpp"

namespace {

QByteArray pngOf(const QImage &img)
{
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

}  // namespace

int main(int argc, char **argv)
{
    /* QGuiApplication and not QCoreApplication: QPixmap and the image
     * handlers need a GUI application object. The offscreen platform so this
     * runs on a build machine with no display. */
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    if (argc < 2) {
        qWarning("usage: qt_icon_gen <out.ico>");
        return 2;
    }

    /* The sizes Windows asks for: 16 in the taskbar and the title bar, 32 in
     * Explorer's small view and alt-tab, 48 in medium icons, 256 for the
     * extra-large view and the Start menu's tile. The rest are the DPI steps
     * in between - cheaper to include than to let the shell interpolate. */
    static const int kSizes[] = { 16, 20, 24, 32, 40, 48, 64, 128, 256 };

    const QIcon tile = halyard::theme::appTileIcon();
    QList<QByteArray> payloads;
    for (int s : kSizes) payloads << pngOf(tile.pixmap(s, s).toImage());

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("cannot open %s: %s", argv[1], qPrintable(f.errorString()));
        return 1;
    }
    QDataStream ds(&f);
    ds.setByteOrder(QDataStream::LittleEndian);   /* ICO is little-endian */

    const int n = payloads.size();
    ds << quint16(0) << quint16(1) << quint16(n);   /* reserved, type=icon */

    /* The directory comes before every payload, so the first offset is past
     * the whole of it. */
    quint32 offset = quint32(6 + 16 * n);
    for (int i = 0; i < n; i++) {
        const int s = kSizes[i];
        /* 0 means 256 in a single byte - the format's own escape, and the
         * reason 256 is the largest size an .ico can hold. */
        ds << quint8(s >= 256 ? 0 : s) << quint8(s >= 256 ? 0 : s)
           << quint8(0)                 /* palette size: none, it is truecolour */
           << quint8(0)                 /* reserved */
           << quint16(1)                /* colour planes */
           << quint16(32)               /* bits per pixel */
           << quint32(payloads.at(i).size())
           << offset;
        offset += quint32(payloads.at(i).size());
    }
    for (const QByteArray &p : payloads)
        if (ds.writeRawData(p.constData(), int(p.size())) != int(p.size())) {
            qWarning("short write to %s", argv[1]);
            return 1;
        }

    f.close();
    return f.error() == QFile::NoError ? 0 : 1;
}
